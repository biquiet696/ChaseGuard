/*
 * ChaseGuard - tests for the decision logic (src/core.c) against the real speed.exe v1.3 code.
 * Build and run: tests/run_tests.sh <path to speed.exe>
 * License: GPL-3.0-or-later (see LICENSE)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/core.h"
#include "../third_party/zydis/Zydis.h"

/* ---- fake address space: speed.exe sections + a stack page + a heap page ---- */
typedef struct { uint32_t va, size; const uint8_t *data; } region;
static region g_map[8];
static int g_nmap;
static uint8_t g_stack[0x1000];   /* at 0x0019F000 */
static uint8_t g_heap[0x1000];    /* at 0x02000000 */
#define STACK_VA 0x0019F000u
#define HEAP_VA  0x02000000u
#define BAD_CODE 0x42652EE0u      /* float stored after the one-slot vtable at 0x890970 */

static int fake_read(void *u, cg_u32 addr, void *dst, cg_u32 len)
{
    int i;
    (void)u;
    for (i = 0; i < g_nmap; i++) {
        region *r = &g_map[i];
        if (addr >= r->va && (uint64_t)addr + len <= (uint64_t)r->va + r->size) {
            memcpy(dst, r->data + (addr - r->va), len);
            return 1;
        }
    }
    return 0;
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t g_text_begin, g_text_end;

static void load_exe(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *d;
    uint32_t nt, nsec, sec, i, base;
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) exit(2);
    fclose(f);
    nt = rd32le(d + 0x3C);
    base = rd32le(d + nt + 24 + 28);
    nsec = d[nt + 6] | d[nt + 7] << 8;
    sec = nt + 24 + (d[nt + 20] | d[nt + 21] << 8);
    for (i = 0; i < nsec && g_nmap < 6; i++, sec += 40) {
        uint32_t vs = rd32le(d + sec + 8), va = rd32le(d + sec + 12);
        uint32_t rs = rd32le(d + sec + 16), ra = rd32le(d + sec + 20);
        g_map[g_nmap].va = base + va;
        g_map[g_nmap].size = vs < rs ? vs : rs;
        g_map[g_nmap].data = d + ra;
        if (!memcmp(d + sec, ".text", 6)) { g_text_begin = base + va; g_text_end = base + va + vs; }
        g_nmap++;
    }
    g_map[g_nmap++] = (region){ STACK_VA, sizeof(g_stack), g_stack };
    g_map[g_nmap++] = (region){ HEAP_VA, sizeof(g_heap), g_heap };
}

/* ---- helpers ---- */
static int g_fail, g_pass;
static cg_env env_default(void)
{
    cg_env e = { g_text_begin, g_text_end, 1, 1, 1, fake_read, 0 };
    return e;
}
static cg_regs regs_with(uint32_t eip)
{
    cg_regs r;
    memset(&r, 0, sizeof(r));
    r.eax = 0x11111111; r.ecx = 0x22222222; r.edx = 0x33333333; r.ebx = 0x44444444;
    r.esi = 0x55555555; r.edi = 0x66666666; r.ebp = 0x77777777;
    r.esp = STACK_VA + 0x800; r.eip = eip;
    return r;
}
static void put32(uint8_t *mem, uint32_t base_va, uint32_t va, uint32_t v)
{
    memcpy(mem + (va - base_va), &v, 4);
}

#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static cg_result run(const char *name, cg_env *e, uint32_t access, uint32_t fault, cg_regs *r, cg_verdict want)
{
    cg_result res;
    cg_regs before = *r;
    cg_verdict v = cg_decide(e, access, fault, r, &res);
    CHECK(v == want, "%s: verdict %d, expected %d (%s)", name, v, want, res.reason);
    if (v != CG_RECOVERED)
        CHECK(!memcmp(&before, r, sizeof(before)), "%s: registers modified without recovery", name);
    printf("  %-58s -> %-9s %s\n", name, v == CG_RECOVERED ? "RECOVERED" : v == CG_GIVE_UP ? "GIVE_UP" : "FOREIGN",
           res.reason);
    return res;
}

/* ---- tests ---- */
static void test_in_game_code(void)
{
    cg_env e = env_default();
    cg_regs r;
    cg_result res;
    puts("A) faults inside speed.exe .text");

    r = regs_with(0x402510); r.ecx = 0;                       /* mov eax, [ecx+8]; ecx = NULL */
    res = run("mov eax,[ecx+8] with ecx=0", &e, CG_ACCESS_READ, 8, &r, CG_RECOVERED);
    CHECK(r.eip == 0x402513 && r.eax == 0 && res.zeroed, "eip=%08x eax=%08x", r.eip, r.eax);
    CHECK(r.esp == STACK_VA + 0x800, "esp changed");

    e.zero_loaded_reg = 0;
    r = regs_with(0x402510); r.ecx = 0;
    run("same, ZeroLoadedRegister=0", &e, CG_ACCESS_READ, 8, &r, CG_RECOVERED);
    CHECK(r.eax == 0x11111111, "eax must be kept");
    e.zero_loaded_reg = 1;

    r = regs_with(0x401e94); r.eax = 0;                       /* mov cl, byte ptr [eax] */
    run("mov cl,[eax] clears only CL", &e, CG_ACCESS_READ, 0, &r, CG_RECOVERED);
    CHECK(r.ecx == 0x22222200 && r.eip == 0x401e96, "ecx=%08x eip=%08x", r.ecx, r.eip);

    r = regs_with(0x42c0c1);                                  /* movzx edx, word [esi+ecx+4] */
    run("movzx edx,word [esi+ecx+4]", &e, CG_ACCESS_READ, 0x77777779, &r, CG_RECOVERED);
    CHECK(r.edx == 0, "edx=%08x", r.edx);

    r = regs_with(0x4016f9); r.eax = 0;                       /* mov [eax+4], ecx (write) */
    run("mov [eax+4],ecx (write) skipped, nothing cleared", &e, CG_ACCESS_WRITE, 4, &r, CG_RECOVERED);
    CHECK(r.ecx == 0x22222222 && r.eip == 0x4016fc, "ecx/eip");

    r = regs_with(0x5ac091); r.eax = 0;                       /* cmp dword [eax+0x30], 0 */
    run("cmp [eax+0x30],0 skipped", &e, CG_ACCESS_READ, 0x30, &r, CG_RECOVERED);

    r = regs_with(0x402699); r.ecx = 0;                       /* fld dword [ecx+0x14] */
    run("fld [ecx+0x14] skipped (as MWCrashFix did)", &e, CG_ACCESS_READ, 0x14, &r, CG_RECOVERED);

    r = regs_with(0x5d5aff); r.eax = 0x10;                    /* call dword [eax+4] */
    res = run("call [eax+4] with bad eax -> skip call, EAX=0", &e, CG_ACCESS_READ, 0x14, &r, CG_RECOVERED);
    CHECK(res.kind == CG_KIND_SKIP_CALL && r.eip == 0x5d5b02 && r.eax == 0 && r.esp == STACK_VA + 0x800,
          "eip=%08x eax=%08x esp=%08x", r.eip, r.eax, r.esp);

    r = regs_with(0x5d5aff); r.eax = 0x10;
    run("call [eax+4], fault address does not match", &e, CG_ACCESS_READ, 0x5000, &r, CG_GIVE_UP);

    r = regs_with(0x5d5aff); r.eax = 0x10;
    run("call [eax+4], write fault (stack broken)", &e, CG_ACCESS_WRITE, STACK_VA, &r, CG_GIVE_UP);

    r = regs_with(0x40a9b5); r.eax = 0x100;                   /* call dword [eax+0x140] */
    run("call [eax+0x140] with bad eax", &e, CG_ACCESS_READ, 0x240, &r, CG_RECOVERED);

    r = regs_with(0x7ec3f4); r.eax = 0;                       /* push dword [eax+4] */
    run("push [eax+4] must not be skipped", &e, CG_ACCESS_READ, 4, &r, CG_GIVE_UP);

    r = regs_with(0x628bd5); r.edx = 0;                       /* mov esp, [edx] */
    run("mov esp,[edx] must not be skipped", &e, CG_ACCESS_READ, 0, &r, CG_GIVE_UP);

    r = regs_with(0x401000);
    run("ret must not be skipped", &e, CG_ACCESS_READ, STACK_VA, &r, CG_GIVE_UP);

    r = regs_with(0x40175f);                                  /* rep movsd */
    run("rep movsd with bad esi skipped", &e, CG_ACCESS_READ, 0x55555555, &r, CG_RECOVERED);

    r = regs_with(0x402510);
    run("execute fault inside .text", &e, CG_ACCESS_EXECUTE, 0x402510, &r, CG_GIVE_UP);

    e.recover_faults = 0;
    r = regs_with(0x402510); r.ecx = 0;
    run("RecoverFaults=0", &e, CG_ACCESS_READ, 8, &r, CG_GIVE_UP);
    e.recover_faults = 1;
    e.recover_bad_calls = 0;
    r = regs_with(0x5d5aff); r.eax = 0x10;
    run("RecoverBadCalls=0 (call [bad])", &e, CG_ACCESS_READ, 0x14, &r, CG_GIVE_UP);
}

static void test_bad_calls(void)
{
    cg_env e = env_default();
    cg_regs r;
    cg_result res;
    uint32_t vt = HEAP_VA + 0x100;
    puts("B) execution outside .text");

    /* call dword ptr [edx+4] at 0x5d8355 jumped to BAD_CODE */
    put32(g_heap, HEAP_VA, vt + 4, BAD_CODE);
    r = regs_with(BAD_CODE); r.edx = vt; r.esp = STACK_VA + 0x7FC;
    put32(g_stack, STACK_VA, r.esp, 0x5d8358);
    res = run("call [edx+4] into garbage (execute fault)", &e, CG_ACCESS_EXECUTE, BAD_CODE, &r, CG_RECOVERED);
    CHECK(r.eip == 0x5d8358 && r.esp == STACK_VA + 0x800 && r.eax == 0 && res.call_site == 0x5d8355,
          "eip=%08x esp=%08x eax=%08x site=%08x", r.eip, r.esp, r.eax, res.call_site);

    r = regs_with(BAD_CODE); r.edx = vt; r.esp = STACK_VA + 0x7FC;
    run("same as a read fault (no DEP, unmapped)", &e, CG_ACCESS_READ, BAD_CODE, &r, CG_RECOVERED);

    put32(g_heap, HEAP_VA, vt + 4, 0x12345678);
    r = regs_with(BAD_CODE); r.edx = vt; r.esp = STACK_VA + 0x7FC;
    run("call [edx+4] does not point to EIP", &e, CG_ACCESS_EXECUTE, BAD_CODE, &r, CG_FOREIGN);
    put32(g_heap, HEAP_VA, vt + 4, BAD_CODE);

    r = regs_with(BAD_CODE); r.edx = vt; r.esp = STACK_VA + 0x7FC;
    run("data fault in foreign code (fault != EIP)", &e, CG_ACCESS_READ, 0x10, &r, CG_FOREIGN);

    r = regs_with(BAD_CODE); r.edx = vt; r.esp = STACK_VA + 0x7FC;
    put32(g_stack, STACK_VA, r.esp, 0x402513);                /* after "mov eax,[ecx+8]" */
    run("return address not preceded by a call", &e, CG_ACCESS_EXECUTE, BAD_CODE, &r, CG_FOREIGN);

    r = regs_with(BAD_CODE); r.esp = STACK_VA + 0x7FC;
    put32(g_stack, STACK_VA, r.esp, 0x008F0000);              /* pointer into .data */
    run("top of stack points into game .data", &e, CG_ACCESS_EXECUTE, BAD_CODE, &r, CG_FOREIGN);

    r = regs_with(BAD_CODE); r.esp = 0x0BAD0000;
    run("unreadable ESP", &e, CG_ACCESS_EXECUTE, BAD_CODE, &r, CG_FOREIGN);

    r = regs_with(0x6B001234); r.esp = STACK_VA + 0x7FC;     /* fault in some DLL, code readable */
    put32(g_stack, STACK_VA, r.esp, 0x5d8358);
    run("access violation inside another DLL", &e, CG_ACCESS_READ, 0, &r, CG_FOREIGN);

    r = regs_with(0); r.eax = 0; r.esp = STACK_VA + 0x7FC;   /* call eax with eax = 0 */
    put32(g_stack, STACK_VA, r.esp, 0x454b7e);
    run("call eax with eax=0 (EIP=0)", &e, CG_ACCESS_EXECUTE, 0, &r, CG_RECOVERED);
    CHECK(r.eip == 0x454b7e, "eip=%08x", r.eip);

    r = regs_with(BAD_CODE); r.eax = 0; r.esp = STACK_VA + 0x7FC;
    put32(g_stack, STACK_VA, r.esp, 0x454b7e);
    run("call eax but eax != EIP", &e, CG_ACCESS_EXECUTE, BAD_CODE, &r, CG_FOREIGN);

    /* call [0x891584] -> import address slot in .rdata, does not hold EIP */
    r = regs_with(BAD_CODE); r.esp = STACK_VA + 0x7FC;
    put32(g_stack, STACK_VA, r.esp, 0x42c9de);
    run("call [abs] whose slot does not hold EIP", &e, CG_ACCESS_EXECUTE, BAD_CODE, &r, CG_FOREIGN);

    e.recover_bad_calls = 0;
    r = regs_with(BAD_CODE); r.edx = vt; r.esp = STACK_VA + 0x7FC;
    put32(g_stack, STACK_VA, r.esp, 0x5d8358);
    run("RecoverBadCalls=0", &e, CG_ACCESS_EXECUTE, BAD_CODE, &r, CG_GIVE_UP);
}

/* Cross-check: Zydis instruction lengths against llvm-objdump for the whole .text. */
static int is_artifact(const char *text)
{
    /* objdump prints lone prefixes as instructions, "<unknown>" for undecodable bytes and
       "..." for zero runs; such spots are data inside .text, not code. */
    static const char *const bad[] = { "<unknown>", "xrelease", "xacquire", "\tlock", "\trep",
                                       "data16", "addr16", "(bad)", 0 };
    int i;
    for (i = 0; bad[i]; i++) if (strstr(text, bad[i])) return 1;
    return 0;
}

/* Cross-check: Zydis instruction lengths against llvm-objdump for the whole .text. */
static void test_lengths(const char *listing)
{
    FILE *f = fopen(listing, "r");
    char line[512];
    uint32_t prev = 0, checked = 0, bad = 0;
    int prev_ok = 0;
    ZydisDecoder dec;
    if (!f) { printf("(no listing, length cross-check skipped)\n"); return; }
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    while (fgets(line, sizeof(line), f)) {
        unsigned a;
        if (strstr(line, "...")) { prev_ok = 0; continue; }
        if (sscanf(line, " %x:", &a) != 1 || !strchr(line, ':')) continue;
        if (prev_ok && a > prev && a - prev <= 15 && !is_artifact(line)) {
            uint8_t buf[16];
            ZydisDecodedInstruction insn;
            if (fake_read(0, prev, buf, 15)
                && ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&dec, 0, buf, 15, &insn))) {
                checked++;
                if (insn.length != a - prev && bad++ < 5)
                    printf("  length mismatch at %08x: %u vs %u\n", prev, insn.length, a - prev);
            }
        }
        prev = a;
        prev_ok = !is_artifact(line);
    }
    fclose(f);
    printf("C) decoder cross-check: %u instructions, %u length mismatches\n", checked, bad);
    CHECK(checked > 1000000 && bad == 0, "decoder mismatches: %u", bad);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s speed.exe [objdump listing]\n", argv[0]); return 2; }
    load_exe(argv[1]);
    printf(".text %08x-%08x\n", g_text_begin, g_text_end);
    if (!cg_init()) { puts("cg_init failed"); return 1; }
    test_in_game_code();
    test_bad_calls();
    if (argc > 2) test_lengths(argv[2]);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
