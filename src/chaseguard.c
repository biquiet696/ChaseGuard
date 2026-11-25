/*
 * ChaseGuard - crash protection for Need for Speed: Most Wanted (2005) v1.3
 * Copyright (C) 2026 biquiet696 and contributors
 * Based on MWCrashFix by x0reaxeax (https://github.com/x0reaxeax/MWCrashFix)
 * License: GPL-3.0-or-later (see LICENSE)
 *
 * chaseguard.c - plugin entry point: patching speed.exe, the vectored exception handler,
 * configuration and logging.
 */
#include "win32.h"
#include "core.h"
#include "version.h"

/* ------------------------------------------------------------------------------------------ */
/* Patch table                                                                                 */
/* ------------------------------------------------------------------------------------------ */

/*
 * 434 instructions "mov dword ptr [reg], 0x890970" in speed.exe v1.3. 0x890970 is the vtable of
 * the base class of the game's event-subscriber objects; it is stored in an object only while
 * its constructor registers it (0x5D7930) or its destructor unregisters it (0x5D5A50). A virtual
 * call that reaches such an object in that window ends in "R6025 pure virtual function call".
 * MWCrashFix replaces the value with 0x8AA828, a table whose entries are harmless accessors.
 * The table and both values are unchanged from MWCrashFix.
 */
typedef struct site { DWORD addr; BYTE size; BYTE modrm; } site;
static const site k_sites[] = {
#include "sites.inc"
};
#define SITE_COUNT  (sizeof(k_sites) / sizeof(k_sites[0]))
#define VALUE_OLD   0x00890970u
#define VALUE_NEW   0x008AA828u
#define GAME_BASE   0x00400000u
_Static_assert(SITE_COUNT == 434, "patch table must have 434 entries");

/* ------------------------------------------------------------------------------------------ */
/* Small helpers (no C runtime)                                                                */
/* ------------------------------------------------------------------------------------------ */

static DWORD str_len(const char *s) { DWORD n = 0; while (s[n]) n++; return n; }

static void str_copy(char *dst, DWORD cap, const char *src)
{
    DWORD i = 0;
    if (!cap) return;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void str_cat(char *dst, DWORD cap, const char *src)
{
    DWORD n = str_len(dst);
    if (n < cap) str_copy(dst + n, cap - n, src);
}

static int fmt(char *out, DWORD cap, const char *f, ...)
{
    char tmp[1100];  /* wvsprintfA writes at most 1024 characters */
    __builtin_va_list ap;
    int n;
    __builtin_va_start(ap, f);
    n = wvsprintfA(tmp, f, (va_list_t)ap);
    __builtin_va_end(ap);
    str_copy(out, cap, tmp);
    return n;
}

/* ------------------------------------------------------------------------------------------ */
/* Configuration (ChaseGuard.ini next to ChaseGuard.asi; every key is optional)                */
/* ------------------------------------------------------------------------------------------ */

typedef struct config {
    int enable_patch, enable_handler, recover_faults, recover_bad_calls, zero_loaded_reg;
    int loop_limit;     /* recoveries of one address per second before giving up on it */
    int max_log_kb;
    int self_test;      /* exercise the handler once, 10 seconds after start */
} config;

static config g_cfg;
static char   g_ini_path[300];
static char   g_log_path[300];

static int ini_int(const char *key, int def, int lo, int hi)
{
    int v = (int)GetPrivateProfileIntA("ChaseGuard", key, def, g_ini_path);
    return v < lo ? lo : v > hi ? hi : v;
}

static void load_config(void)
{
    g_cfg.enable_patch      = ini_int("EnablePatch", 1, 0, 1);
    g_cfg.enable_handler    = ini_int("EnableHandler", 1, 0, 1);
    g_cfg.recover_faults    = ini_int("RecoverFaults", 1, 0, 1);
    g_cfg.recover_bad_calls = ini_int("RecoverBadCalls", 1, 0, 1);
    g_cfg.zero_loaded_reg   = ini_int("ZeroLoadedRegister", 1, 0, 1);
    g_cfg.loop_limit        = ini_int("LoopLimit", 500, 10, 1000000);
    g_cfg.max_log_kb        = ini_int("MaxLogKB", 1024, 16, 65536);
    g_cfg.self_test         = ini_int("SelfTest", 0, 0, 1);
}

/* Directory of a module path including the trailing backslash. */
static void module_dir(HMODULE m, char *out, DWORD cap)
{
    DWORD n = GetModuleFileNameA(m, out, cap), i, cut = 0;
    if (n == 0 || n >= cap) { out[0] = 0; return; }
    for (i = 0; i < n; i++) if (out[i] == '\\' || out[i] == '/') cut = i + 1;
    out[cut] = 0;
}

static void build_paths(HMODULE self)
{
    DWORD n, i, dot = 0, slash = 0;
    module_dir(0, g_log_path, sizeof(g_log_path));
    str_cat(g_log_path, sizeof(g_log_path), "chaseguard.log");

    n = GetModuleFileNameA(self, g_ini_path, sizeof(g_ini_path) - 8);
    if (n == 0 || n >= sizeof(g_ini_path) - 8) { str_copy(g_ini_path, sizeof(g_ini_path), "ChaseGuard.ini"); return; }
    for (i = 0; i < n; i++) {
        if (g_ini_path[i] == '\\' || g_ini_path[i] == '/') slash = i;
        if (g_ini_path[i] == '.') dot = i;
    }
    if (dot > slash) g_ini_path[dot] = 0;
    str_cat(g_ini_path, sizeof(g_ini_path), ".ini");
}

/* ------------------------------------------------------------------------------------------ */
/* Log                                                                                         */
/* ------------------------------------------------------------------------------------------ */

static HANDLE g_log = INVALID_HANDLE_VALUE;
static DWORD  g_log_written, g_log_cap;
static int    g_log_full;

static void log_open(void)
{
    DWORD size;
    g_log_cap = (DWORD)g_cfg.max_log_kb * 1024u;
    g_log = CreateFileA(g_log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (g_log == INVALID_HANDLE_VALUE) return;          /* logging is optional */
    size = GetFileSize(g_log, 0);
    if (size == 0xFFFFFFFFu || size > g_log_cap) {      /* start over instead of growing forever */
        CloseHandle(g_log);
        g_log = CreateFileA(g_log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    } else {
        SetFilePointer(g_log, 0, 0, FILE_END);
    }
}

static void log_write(const char *s, DWORD n)
{
    DWORD w = 0;
    if (g_log == INVALID_HANDLE_VALUE || g_log_full) return;
    if (g_log_written + n > g_log_cap) {
        static const char full[] = "[ChaseGuard] log size limit reached (MaxLogKB), further messages dropped\r\n";
        WriteFile(g_log, full, sizeof(full) - 1, &w, 0);
        g_log_full = 1;
        return;
    }
    WriteFile(g_log, s, n, &w, 0);
    g_log_written += w;
}

static void cg_log(const char *f, ...)
{
    char body[1100], line[1200];
    SYSTEMTIME t;
    __builtin_va_list ap;
    if (g_log == INVALID_HANDLE_VALUE || g_log_full) return;
    __builtin_va_start(ap, f);
    wvsprintfA(body, f, (va_list_t)ap);
    __builtin_va_end(ap);
    GetLocalTime(&t);
    fmt(line, sizeof(line), "%02u:%02u:%02u.%03u [ChaseGuard] %s\r\n",
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, body);
    log_write(line, str_len(line));
}

static void log_close(void)
{
    if (g_log != INVALID_HANDLE_VALUE) { CloseHandle(g_log); g_log = INVALID_HANDLE_VALUE; }
}

/* ------------------------------------------------------------------------------------------ */
/* Memory access                                                                               */
/* ------------------------------------------------------------------------------------------ */

static int readable_protect(DWORD p)
{
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* Copies memory only after VirtualQuery confirmed every byte is committed and readable. */
static int safe_read(void *user, cg_u32 addr, void *dst, cg_u32 len)
{
    cg_u32 cur = addr, end = addr + len;
    MEMORY_BASIC_INFORMATION mbi;
    (void)user;
    if (len == 0 || end < addr) return 0;
    while (cur < end) {
        cg_u32 rend;
        if (!VirtualQuery((const void *)cur, &mbi, sizeof(mbi))) return 0;
        if (mbi.State != MEM_COMMIT || !readable_protect(mbi.Protect)) return 0;
        rend = (cg_u32)mbi.BaseAddress + (cg_u32)mbi.RegionSize;
        if (rend <= cur) return 0;
        cur = rend;
    }
    {
        const volatile BYTE *s = (const volatile BYTE *)addr;
        BYTE *d = (BYTE *)dst;
        cg_u32 i;
        for (i = 0; i < len; i++) d[i] = s[i];
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------ */
/* speed.exe identification and patching                                                       */
/* ------------------------------------------------------------------------------------------ */

static DWORD g_text_begin, g_text_end, g_exe_stamp, g_exe_size;

static DWORD rd32(DWORD a) { return *(const DWORD *)a; }
static WORD  rd16(DWORD a) { return *(const WORD *)a; }

/* Finds speed.exe .text from its PE header. Returns 0 if the main module looks wrong. */
static int locate_game(void)
{
    DWORD base = (DWORD)GetModuleHandleA(0), nt, sec, nsec, i;
    if (base != GAME_BASE || rd16(base) != 0x5A4D) return 0;
    nt = base + rd32(base + 0x3C);
    if (rd32(nt) != 0x00004550 || rd16(nt + 4) != 0x014C) return 0;
    nsec = rd16(nt + 6);
    g_exe_stamp = rd32(nt + 8);
    g_exe_size = rd32(nt + 24 + 56);
    sec = nt + 24 + rd16(nt + 20);
    for (i = 0; i < nsec; i++, sec += 40) {
        const char *name = (const char *)sec;
        if (name[0] == '.' && name[1] == 't' && name[2] == 'e' && name[3] == 'x' &&
            name[4] == 't' && name[5] == 0) {
            g_text_begin = base + rd32(sec + 12);
            g_text_end = g_text_begin + rd32(sec + 8);
            return g_text_end > g_text_begin;
        }
    }
    return 0;
}

static void site_bytes(const site *s, DWORD value, BYTE out[7])
{
    out[0] = 0xC7;
    out[1] = s->modrm;
    if (s->size == 7) out[2] = 0x00;
    *(DWORD *)(out + s->size - 4) = value;
}

static int site_equals(const site *s, DWORD value)
{
    BYTE b[7];
    DWORD i;
    site_bytes(s, value, b);
    for (i = 0; i < s->size; i++)
        if (((const BYTE *)s->addr)[i] != b[i]) return 0;
    return 1;
}

static int write_site(const site *s, DWORD value)
{
    BYTE b[7];
    DWORD old, tmp, i;
    site_bytes(s, value, b);
    if (!VirtualProtect((void *)s->addr, s->size, PAGE_EXECUTE_READWRITE, &old)) return 0;
    for (i = 0; i < s->size; i++) ((volatile BYTE *)s->addr)[i] = b[i];
    VirtualProtect((void *)s->addr, s->size, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void *)s->addr, s->size);
    return 1;
}

typedef enum patch_state {
    PATCH_APPLIED, PATCH_ALREADY, PATCH_DISABLED, PATCH_UNSUPPORTED, PATCH_FOREIGN, PATCH_FAILED
} patch_state;

static DWORD g_bad_site;

static patch_state apply_patch(void)
{
    DWORD i, n_old = 0, n_new = 0;
    for (i = 0; i < SITE_COUNT; i++) {
        const site *s = &k_sites[i];
        if (s->addr < g_text_begin || s->addr + s->size > g_text_end) { g_bad_site = s->addr; return PATCH_UNSUPPORTED; }
        if (site_equals(s, VALUE_OLD)) n_old++;
        else if (site_equals(s, VALUE_NEW)) n_new++;
        else if (!g_bad_site) g_bad_site = s->addr;
    }
    if (n_new == SITE_COUNT) return PATCH_ALREADY;
    if (n_old + n_new != SITE_COUNT) return (n_old + n_new == 0) ? PATCH_UNSUPPORTED : PATCH_FOREIGN;
    if (!g_cfg.enable_patch) return PATCH_DISABLED;

    /* Every site is verified before the first byte is written; a failure rolls back. */
    for (i = 0; i < SITE_COUNT; i++) {
        if (site_equals(&k_sites[i], VALUE_NEW)) continue;
        if (!write_site(&k_sites[i], VALUE_NEW)) {
            g_bad_site = k_sites[i].addr;
            while (i-- > 0) write_site(&k_sites[i], VALUE_OLD);
            return PATCH_FAILED;
        }
    }
    return PATCH_APPLIED;
}

/* ------------------------------------------------------------------------------------------ */
/* Exception handler                                                                           */
/* ------------------------------------------------------------------------------------------ */

typedef struct addr_stat {
    DWORD addr, window_start, window_count, total;
    int   given_up;
} addr_stat;

#define STAT_SLOTS 64
static addr_stat     g_stats[STAT_SLOTS];
static cg_env        g_env;
static void         *g_veh;
static volatile LONG g_lock;
static volatile DWORD g_owner_tid;
static DWORD         g_recovered, g_given_up, g_loops;

static void lock(void)   { while (__sync_lock_test_and_set(&g_lock, 1)) Sleep(0); }
static void unlock(void) { __sync_lock_release(&g_lock); }

static addr_stat *stat_for(DWORD addr)
{
    DWORD i, victim = 0;
    for (i = 0; i < STAT_SLOTS; i++) {
        if (g_stats[i].addr == addr) return &g_stats[i];
        if (g_stats[i].total < g_stats[victim].total) victim = i;
    }
    g_stats[victim].addr = addr;
    g_stats[victim].window_start = GetTickCount();
    g_stats[victim].window_count = 0;
    g_stats[victim].total = 0;
    g_stats[victim].given_up = 0;
    return &g_stats[victim];
}

static int worth_logging(DWORD n)
{
    return n <= 10 || n == 100 || n == 1000 || n == 10000 || n == 100000 || n == 1000000;
}

static const char *access_name(DWORD a)
{
    return a == CG_ACCESS_READ ? "read" : a == CG_ACCESS_WRITE ? "write" :
           a == CG_ACCESS_EXECUTE ? "execute" : "access";
}

static void dump_context(const CONTEXT *c)
{
    DWORD st[8], i;
    BYTE code[16];
    char hex[64];
    cg_log("  EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX",
         c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
    cg_log("  EBP=%08lX ESP=%08lX EIP=%08lX EFLAGS=%08lX", c->Ebp, c->Esp, c->Eip, c->EFlags);
    if (safe_read(0, c->Esp, st, sizeof(st)))
        cg_log("  stack: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
             st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
    if (safe_read(0, c->Eip, code, sizeof(code))) {
        hex[0] = 0;
        for (i = 0; i < sizeof(code); i++) {
            char b[4];
            fmt(b, sizeof(b), "%02X", code[i]);
            str_cat(hex, sizeof(hex), b);
        }
        cg_log("  code at EIP: %s", hex);
    }
}

static LONG WINAPI veh(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    DWORD tid, access, fault, key, now;
    cg_regs r;
    cg_result res;
    cg_verdict v;
    addr_stat *st;
    LONG ret = EXCEPTION_CONTINUE_SEARCH;

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || (er->ExceptionFlags & EXCEPTION_NONCONTINUABLE)
        || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    tid = GetCurrentThreadId();
    if (g_owner_tid == tid)                 /* fault inside this handler: never handle it */
        return EXCEPTION_CONTINUE_SEARCH;

    lock();
    g_owner_tid = tid;

    access = er->ExceptionInformation[0];
    fault = er->ExceptionInformation[1];
    r.eax = c->Eax; r.ecx = c->Ecx; r.edx = c->Edx; r.ebx = c->Ebx;
    r.esp = c->Esp; r.ebp = c->Ebp; r.esi = c->Esi; r.edi = c->Edi; r.eip = c->Eip;

    v = cg_decide(&g_env, access, fault, &r, &res);
    if (v == CG_FOREIGN) goto out;

    key = res.call_site ? res.call_site : c->Eip;
    st = stat_for(key);
    st->total++;

    if (v == CG_RECOVERED) {
        now = GetTickCount();
        if (now - st->window_start >= 1000) { st->window_start = now; st->window_count = 0; }
        if (st->given_up || ++st->window_count > (DWORD)g_cfg.loop_limit) {
            if (!st->given_up) {
                g_loops++;
                cg_log("game code at %08lX faults over and over (more than %d times per second): "
                     "stopping recovery there, the game will crash as it would without ChaseGuard",
                     key, g_cfg.loop_limit);
                dump_context(c);
            }
            st->given_up = 1;
            g_given_up++;
            goto out;
        }
        g_recovered++;
        if (worth_logging(st->total))
            cg_log("recovered #%lu at %08lX: %s (%s %s %08lX%s) [%lu at this address]",
                 g_recovered, key, res.reason, cg_kind_name(res.kind), access_name(access), fault,
                 res.zeroed ? ", register cleared" : "", st->total);
        c->Eax = r.eax; c->Ecx = r.ecx; c->Edx = r.edx; c->Ebx = r.ebx;
        c->Esp = r.esp; c->Ebp = r.ebp; c->Esi = r.esi; c->Edi = r.edi; c->Eip = r.eip;
        ret = EXCEPTION_CONTINUE_EXECUTION;
    } else {
        g_given_up++;
        if (st->total <= 3) {
            cg_log("Faulting address %08lX in game code, not recovered: %s (%s %08lX)",
                 c->Eip, res.reason, access_name(access), fault);
            dump_context(c);
        }
    }
out:
    g_owner_tid = 0;
    unlock();
    return ret;
}

/* ------------------------------------------------------------------------------------------ */
/* Self-test (SelfTest=1): runs three tiny pieces of speed.exe code with broken pointers, one   */
/* for each recovery path, and logs whether the handler brought them back.                     */
/* ------------------------------------------------------------------------------------------ */

#define GADGET_LOAD 0x00402510u   /* mov eax, [ecx+8]       ; ret */
#define GADGET_CALL 0x00402567u   /* call dword [eax+disp8] ; ret */
#define UNMAPPED    0x00000012u   /* inside the never-mapped first 64 KB */

static DWORD g_fake_vtable[64];

/* cdecl: calls target with EAX/ECX preset, returns EAX. Must not be inlined or reordered. */
__declspec(naked) static DWORD call_gadget(DWORD target, DWORD eax_value, DWORD ecx_value)
{
    __asm__ volatile (
        "movl 8(%esp), %eax\n\t"
        "movl 12(%esp), %ecx\n\t"
        "movl 4(%esp), %edx\n\t"
        "call *%edx\n\t"
        "ret\n\t");
}

static int gadget_ok(DWORD addr, const BYTE *pattern, DWORD n, DWORD wildcard)
{
    DWORD i;
    for (i = 0; i < n; i++)
        if (i != wildcard && ((const BYTE *)addr)[i] != pattern[i]) return 0;
    return 1;
}

static DWORD WINAPI self_test_thread(void *arg)
{
    static const BYTE load[] = { 0x8B, 0x41, 0x08, 0xC3 };
    static const BYTE call[] = { 0xFF, 0x50, 0x00, 0xC3 };
    DWORD disp, r, passed = 0;
    (void)arg;
    Sleep(10000);
    if (!gadget_ok(GADGET_LOAD, load, 4, 99) || !gadget_ok(GADGET_CALL, call, 4, 2)) {
        cg_log("self-test: game code differs from v1.3, test skipped");
        return 0;
    }
    disp = ((const BYTE *)GADGET_CALL)[2];
    cg_log("self-test: starting (3 deliberate access violations in game code follow)");

    r = call_gadget(GADGET_LOAD, 0xDEADBEEF, UNMAPPED);
    cg_log("self-test 1/3 load from bad pointer:   %s (EAX=%08lX)", r == 0 ? "PASS" : "FAIL", r);
    passed += r == 0;

    r = call_gadget(GADGET_CALL, UNMAPPED, 0);
    cg_log("self-test 2/3 call via bad pointer:    %s (EAX=%08lX)", r == 0 ? "PASS" : "FAIL", r);
    passed += r == 0;

    g_fake_vtable[disp / 4] = UNMAPPED;
    r = call_gadget(GADGET_CALL, (DWORD)g_fake_vtable, 0xDEADBEEF);
    cg_log("self-test 3/3 call into bad code:      %s (EAX=%08lX)", r == 0 ? "PASS" : "FAIL", r);
    passed += r == 0;

    cg_log("self-test finished: %lu/3 passed%s", passed,
           passed == 3 ? "" : " - please report this with chaseguard.log");
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Entry point                                                                                 */
/* ------------------------------------------------------------------------------------------ */

static void message(const char *text, unsigned icon)
{
    MessageBoxA(0, text, "ChaseGuard " CG_VERSION_STR, icon | MB_SETFOREGROUND | MB_TOPMOST);
}

static void startup(HMODULE self)
{
    char msg[512];
    patch_state ps;

    build_paths(self);
    load_config();
    log_open();
    cg_log("=== ChaseGuard " CG_VERSION_STR " started ===");
    cg_log("config: %s", g_ini_path);
    cg_log("EnablePatch=%d EnableHandler=%d RecoverFaults=%d RecoverBadCalls=%d ZeroLoadedRegister=%d "
           "LoopLimit=%d MaxLogKB=%d SelfTest=%d",
         g_cfg.enable_patch, g_cfg.enable_handler, g_cfg.recover_faults, g_cfg.recover_bad_calls,
         g_cfg.zero_loaded_reg, g_cfg.loop_limit, g_cfg.max_log_kb, g_cfg.self_test);

    if (GetModuleHandleA("MWCrashFix.asi") || GetModuleHandleA("MWCrashFix.dll")) {
        cg_log("MWCrashFix is loaded as well - remove MWCrashFix.asi, only one of them must be installed");
        message("MWCrashFix is installed as well.\n\nPlease delete MWCrashFix.asi from the scripts "
                "folder: ChaseGuard replaces it and both must not run together.", MB_ICONWARNING);
    }

    if (!locate_game()) {
        cg_log("main module is not speed.exe v1.3 (unexpected image base or PE header) - nothing changed");
        message("Unsupported game executable.\n\nChaseGuard needs NFS Most Wanted speed.exe v1.3. "
                "Nothing was changed.", MB_ICONERROR);
        return;
    }
    cg_log("speed.exe: TimeDateStamp=%08lX SizeOfImage=%08lX .text=%08lX-%08lX",
         g_exe_stamp, g_exe_size, g_text_begin, g_text_end);

    ps = apply_patch();
    switch (ps) {
    case PATCH_APPLIED:
        cg_log("patched %u references (%08X -> %08X)", (unsigned)SITE_COUNT, VALUE_OLD, VALUE_NEW);
        break;
    case PATCH_ALREADY:
        cg_log("all %u references are already patched by another plugin - patch skipped", (unsigned)SITE_COUNT);
        break;
    case PATCH_DISABLED:
        cg_log("patch disabled in the ini (EnablePatch=0)");
        break;
    case PATCH_UNSUPPORTED:
        cg_log("unsupported speed.exe: instruction check failed at %08lX - nothing changed", g_bad_site);
        fmt(msg, sizeof(msg), "Unsupported speed.exe.\n\nChaseGuard needs NFS Most Wanted v1.3 "
            "(instruction check failed at 0x%08lX). Nothing was changed.", g_bad_site);
        message(msg, MB_ICONERROR);
        return;
    case PATCH_FOREIGN:
        cg_log("speed.exe was modified by another plugin at %08lX - nothing changed", g_bad_site);
        fmt(msg, sizeof(msg), "Another plugin changed game code that ChaseGuard needs "
            "(at 0x%08lX). Nothing was changed.\n\nSee chaseguard.log.", g_bad_site);
        message(msg, MB_ICONERROR);
        return;
    case PATCH_FAILED:
        cg_log("VirtualProtect failed at %08lX (error %lu) - all changes were rolled back",
             g_bad_site, GetLastError());
        message("Could not write to game memory. All changes were rolled back.", MB_ICONERROR);
        return;
    }

    if (!g_cfg.enable_handler) {
        cg_log("exception handler disabled in the ini (EnableHandler=0)");
        return;
    }
    if (!cg_init()) {
        cg_log("instruction decoder failed to initialise - exception handler not installed");
        return;
    }
    g_env.text_begin = g_text_begin;
    g_env.text_end = g_text_end;
    g_env.recover_faults = g_cfg.recover_faults;
    g_env.recover_bad_calls = g_cfg.recover_bad_calls;
    g_env.zero_loaded_reg = g_cfg.zero_loaded_reg;
    g_env.read = safe_read;
    g_env.read_user = 0;
    g_veh = AddVectoredExceptionHandler(1, veh);
    if (!g_veh) {
        cg_log("AddVectoredExceptionHandler failed (error %lu)", GetLastError());
        return;
    }
    cg_log("exception handler installed");
    if (g_cfg.self_test && CreateThread(0, 0, self_test_thread, 0, 0, 0))
        cg_log("self-test scheduled in 10 seconds (SelfTest=1)");
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        startup(self);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_veh && !reserved) { RemoveVectoredExceptionHandler(g_veh); g_veh = 0; }
        cg_log("=== session end: %lu recovered, %lu not recovered, %lu loop stops ===",
             g_recovered, g_given_up, g_loops);
        log_close();
    }
    return TRUE;
}
