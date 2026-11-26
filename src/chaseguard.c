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
    int recover_pure_calls; /* "R6025 pure virtual function call": return 0 instead of exiting */
    int correct_stack;  /* remove the arguments of a skipped callee from the stack */
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
    g_cfg.recover_pure_calls = ini_int("RecoverPureCalls", 1, 0, 1);
    g_cfg.correct_stack     = ini_int("CorrectStack", 1, 0, 1);
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
        if (worth_logging(st->total)) {
            char stack_note[48];
            stack_note[0] = 0;
            if (res.kind == CG_KIND_SKIP_CALL || res.kind == CG_KIND_RETURN_BAD_CALL)
                fmt(stack_note, sizeof(stack_note), res.arg_known ? ", %lu arg bytes removed" :
                    ", arg size unknown", (DWORD)res.arg_bytes);
            cg_log("recovered #%lu at %08lX: %s (%s %s %08lX%s%s) [%lu at this address]",
                 g_recovered, key, res.reason, cg_kind_name(res.kind), access_name(access), fault,
                 res.zeroed ? ", register cleared" : "", stack_note, st->total);
        }
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
/* Exit diagnostics: every way the game can end is logged with its origin.                     */
/*  - R6025: the static CRT of speed.exe calls the handler stored at 0x9BA9F8 (the slot of     */
/*    _set_purecall_handler) before it shows the error; ChaseGuard installs itself there.       */
/*  - R60xx: _amsg_exit (0x7C4240) is hooked to log the runtime error number.                  */
/*  - ExitProcess: the import slot of speed.exe is hooked to log the exit code and the caller.  */
/*  - Unhandled exceptions: a top-level filter logs the module and registers, then chains to   */
/*    the filter the game installs.                                                            */
/* ------------------------------------------------------------------------------------------ */

#define CRT_PURECALL        0x007C5200u   /* _purecall                                       */
#define CRT_PURECALL_SLOT   0x009BA9F8u   /* handler pointer used by _purecall               */
#define CRT_AMSG_EXIT       0x007C4240u   /* _amsg_exit(int rterrnum)                        */
#define CRT_AMSG_EXIT_CONT  0x007C424Au   /* first instruction after the 10 replaced bytes   */
#define IAT_EXIT_PROCESS    0x008901C4u   /* speed.exe import slot of kernel32!ExitProcess   */
#define IAT_SET_UEF         0x0089022Cu   /* ... of kernel32!SetUnhandledExceptionFilter     */

static DWORD g_real_exit_process;
static PTOP_LEVEL_EXCEPTION_FILTER g_game_filter;
static DWORD g_last_rt_error, g_pure_calls, g_pure_recovered;
static volatile LONG g_exit_logged;

static void module_of(DWORD addr, char *out, DWORD cap, DWORD *offset)
{
    HMODULE m = 0;
    char path[300];
    DWORD n, i, cut = 0;
    out[0] = 0;
    *offset = addr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (const char *)addr, &m) || !m) {
        str_copy(out, cap, "?");
        return;
    }
    n = GetModuleFileNameA(m, path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) { str_copy(out, cap, "?"); return; }
    for (i = 0; i < n; i++) if (path[i] == '\\' || path[i] == '/') cut = i + 1;
    str_copy(out, cap, path + cut);
    *offset = addr - (DWORD)m;
}

/* Logs up to 8 return addresses into game code found on the stack (each preceded by a CALL). */
static void log_game_stack(DWORD esp)
{
    DWORD i, v, found = 0;
    cg_u32 site;
    char line[200], item[24];
    line[0] = 0;
    for (i = 0; i < 512 && found < 8; i++) {
        if (!safe_read(0, esp + i * 4, &v, 4)) break;
        if (cg_call_before(&g_env, v, &site)) {
            fmt(item, sizeof(item), found ? " <- %08lX" : "%08lX", site);
            str_cat(line, sizeof(line), item);
            found++;
        }
    }
    cg_log("  game call stack (newest first): %s", found ? line : "none found");
}

static const char *rt_error_text(DWORD n)
{
    switch (n) {
    case 2:  return "R6002 floating point support not loaded";
    case 8:  return "R6008 not enough space for arguments";
    case 9:  return "R6009 not enough space for environment";
    case 16: return "R6016 not enough space for thread data";
    case 17: return "R6017 unexpected multithread lock error";
    case 18: return "R6018 unexpected heap error";
    case 19: return "R6019 unable to open console device";
    case 24: return "R6024 not enough space for _onexit/atexit table";
    case 25: return "R6025 pure virtual function call";
    case 26: return "R6026 not enough space for stdio initialization";
    case 27: return "R6027 not enough space for lowio initialization";
    case 28: return "R6028 unable to initialize heap";
    default: return "runtime error";
    }
}

/* Called from purecall_stub. frame = EBP of _purecall, this_ptr = ECX of the virtual call.
   Returns the number of argument bytes to remove when the pure virtual call should return 0 to
   its caller, or -1 to let R6025 happen. */
__attribute__((used)) int __cdecl on_pure_call(DWORD frame, DWORD this_ptr)
{
    DWORD ret = 0, vptr = 0, mod_off;
    cg_u32 site = 0, arg_bytes = 0;
    char mod[64], note[40];
    int recover, arg_known = 0;
    addr_stat *st;

    safe_read(0, frame + 4, &ret, 4);
    safe_read(0, this_ptr, &vptr, 4);
    cg_call_before(&g_env, ret, &site);
    module_of(site ? site : ret, mod, sizeof(mod), &mod_off);
    g_pure_calls++;

    lock();
    st = stat_for(site ? site : ret);
    st->total++;
    recover = g_cfg.recover_pure_calls && site && !st->given_up;
    if (recover) {
        DWORD now = GetTickCount();
        if (now - st->window_start >= 1000) { st->window_start = now; st->window_count = 0; }
        if (++st->window_count > (DWORD)g_cfg.loop_limit) {
            st->given_up = 1;
            recover = 0;
            g_loops++;
        }
    }
    if (recover && g_cfg.correct_stack)
        arg_known = cg_call_arg_bytes(&g_env, site, &arg_bytes);
    unlock();

    note[0] = 0;
    if (recover)
        fmt(note, sizeof(note), arg_known ? ", %lu arg bytes removed" : ", arg size unknown", (DWORD)arg_bytes);
    if (st->total <= 10 || worth_logging(st->total) || !recover)
        cg_log("pure virtual function call (R6025) from %08lX (%s+%lX), this=%08lX vtable=%08lX: %s%s [%lu at this address]",
               site ? site : ret, mod, mod_off, this_ptr, vptr,
               recover ? "returned 0 to the caller" :
               !g_cfg.recover_pure_calls ? "RecoverPureCalls=0, the game shows R6025 and exits" :
               !site ? "caller unknown, the game shows R6025 and exits" :
                       "repeating too often, the game shows R6025 and exits",
               note, st->total);
    if (st->total <= 3 || !recover)
        log_game_stack(frame + 8);
    if (!recover) return -1;
    g_pure_recovered++;
    return (int)arg_bytes;
}

/* Installed in the CRT purecall slot. On entry: [esp] = return into _purecall, EBP = frame of
   _purecall ([ebp+4] = caller of the pure virtual function), ECX = this. */
__declspec(naked) static void purecall_stub(void)
{
    __asm__ volatile (
        "pushl %ecx\n\t"
        "pushl %ebp\n\t"
        "call  _on_pure_call\n\t"
        "addl  $8, %esp\n\t"
        "cmpl  $-1, %eax\n\t"
        "je    1f\n\t"
        "movl  %ebp, %esp\n\t"     /* leave _purecall's frame ...                         */
        "popl  %ebp\n\t"
        "popl  %ecx\n\t"           /* return address of the virtual call                  */
        "addl  %eax, %esp\n\t"     /* remove the arguments the callee would have removed  */
        "pushl %ecx\n\t"
        "xorl  %eax, %eax\n\t"     /* ... and return 0 to the code that made the call     */
        "ret\n"
        "1:\n\t"
        "ret\n\t");
}

__attribute__((used)) void __cdecl on_rt_error(DWORD code, DWORD esp_at_entry)
{
    g_last_rt_error = code;
    cg_log("C runtime error %lu: %s - the game will show an error box and exit", code, rt_error_text(code));
    log_game_stack(esp_at_entry);
}

/* Replaces the first 10 bytes of _amsg_exit (push ebp; mov ebp,esp; cmp dword [0x9BA9EC],2). */
__declspec(naked) static void amsg_exit_stub(void)
{
    __asm__ volatile (
        "movl  %esp, %eax\n\t"
        "pushl %eax\n\t"                /* esp at entry: [eax] = return address, [eax+4] = code */
        "pushl 4(%eax)\n\t"
        "call  _on_rt_error\n\t"
        "addl  $8, %esp\n\t"
        "pushl %ebp\n\t"                /* the replaced instructions                          */
        "movl  %esp, %ebp\n\t"
        "pushl $0x007C424A\n\t"
        "cmpl  $2, 0x009BA9EC\n\t"
        "ret\n\t");                     /* continue in _amsg_exit with the flags of the cmp   */
}

static void WINAPI hook_exit_process(DWORD code)
{
    if (!__sync_lock_test_and_set(&g_exit_logged, 1)) {
        if (code == 0)
            cg_log("game exits normally (exit code 0)");
        else
            cg_log("game exits with code %lu%s%s", code,
                   g_last_rt_error ? " after " : "", g_last_rt_error ? rt_error_text(g_last_rt_error) : "");
        log_game_stack((DWORD)&code);
    }
    ((void (WINAPI *)(DWORD))g_real_exit_process)(code);
}

static LONG WINAPI top_filter(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD off;
    char mod[64];
    module_of((DWORD)er->ExceptionAddress, mod, sizeof(mod), &off);
    cg_log("UNHANDLED EXCEPTION %08lX at %08lX (%s+%lX)%s%s %08lX - the game crashes here",
           er->ExceptionCode, (DWORD)er->ExceptionAddress, mod, off,
           er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 ? ", " : "",
           er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2
               ? access_name(er->ExceptionInformation[0]) : "",
           er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0);
    dump_context(ep->ContextRecord);
    log_game_stack(ep->ContextRecord->Esp);
    g_exit_logged = 1;
    return g_game_filter ? g_game_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

/* The game's own SetUnhandledExceptionFilter calls: keep ChaseGuard's filter first in line. */
static PTOP_LEVEL_EXCEPTION_FILTER WINAPI hook_set_uef(PTOP_LEVEL_EXCEPTION_FILTER f)
{
    PTOP_LEVEL_EXCEPTION_FILTER prev = g_game_filter;
    g_game_filter = (f == top_filter) ? 0 : f;
    return prev;
}

static int write_code(DWORD addr, const void *src, DWORD n)
{
    DWORD old, tmp, i;
    if (!VirtualProtect((void *)addr, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    for (i = 0; i < n; i++) ((volatile BYTE *)addr)[i] = ((const BYTE *)src)[i];
    VirtualProtect((void *)addr, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, n);
    return 1;
}

static int bytes_equal(DWORD addr, const BYTE *b, DWORD n)
{
    DWORD i;
    for (i = 0; i < n; i++) if (((const BYTE *)addr)[i] != b[i]) return 0;
    return 1;
}

static void install_quit_diagnostics(void);

static void install_diagnostics(void)
{
    static const BYTE purecall[] = { 0x55, 0x8B, 0xEC, 0x83, 0x3D, 0xF8, 0xA9, 0x9B, 0x00, 0x00 };
    static const BYTE amsg[]     = { 0x55, 0x8B, 0xEC, 0x83, 0x3D, 0xEC, 0xA9, 0x9B, 0x00, 0x02 };
    BYTE jmp[10];
    DWORD v, i;
    int ok_pure = 0, ok_rt = 0, ok_exit = 0, ok_uef = 0;

    if (bytes_equal(CRT_PURECALL, purecall, sizeof(purecall)) && *(DWORD *)CRT_PURECALL_SLOT == 0) {
        v = (DWORD)purecall_stub;
        ok_pure = write_code(CRT_PURECALL_SLOT, &v, 4);   /* plain data slot, as _set_purecall_handler */
    }
    if (bytes_equal(CRT_AMSG_EXIT, amsg, sizeof(amsg))) {
        jmp[0] = 0xE9;
        *(DWORD *)(jmp + 1) = (DWORD)amsg_exit_stub - (CRT_AMSG_EXIT + 5);
        for (i = 5; i < 10; i++) jmp[i] = 0xCC;
        ok_rt = write_code(CRT_AMSG_EXIT, jmp, sizeof(jmp));
    }
    if (safe_read(0, IAT_EXIT_PROCESS, &v, 4) && v && (v < g_text_begin || v >= g_text_end)) {
        g_real_exit_process = v;
        v = (DWORD)hook_exit_process;
        ok_exit = write_code(IAT_EXIT_PROCESS, &v, 4);
    }
    g_game_filter = SetUnhandledExceptionFilter(top_filter);
    if (g_game_filter == top_filter) g_game_filter = 0;
    v = (DWORD)hook_set_uef;
    ok_uef = write_code(IAT_SET_UEF, &v, 4);
    cg_log("diagnostics: pure-call handler %s, runtime-error hook %s, exit hook %s, crash filter %s",
           ok_pure ? "on" : "OFF", ok_rt ? "on" : "OFF", ok_exit ? "on" : "OFF", ok_uef ? "on" : "OFF");
    install_quit_diagnostics();
}

/* ------------------------------------------------------------------------------------------ */
/* Quit diagnostics: who asked the game to close?                                              */
/* The main loop ends the game when the flag at 0x9257EC is set. Five places set it; each one  */
/* ("mov dword ptr [0x9257EC], 1", 10 bytes) is replaced by a call to quit_flag_stub, which     */
/* logs the reason and sets the flag. The window procedure (0x6DB6C0) is hooked to remember    */
/* recent window messages (focus, Alt+F4, close requests) for that log entry.                  */
/* ------------------------------------------------------------------------------------------ */

#define QUIT_FLAG     0x009257ECu
#define GAME_WNDPROC  0x006DB6C0u
#define WNDPROC_CONT  0x006DB6C6u

typedef struct quit_site { DWORD addr; const char *reason; } quit_site;
static const quit_site k_quit_sites[] = {
    { 0x005A4C87u, "Quit confirmed in the main menu (Q -> Yes)" },
    { 0x0057E667u, "quit requested by a game menu message (handler 0x57E630)" },
    { 0x0055BE9Bu, "quit requested by a game menu message (handler 0x55BE90)" },
    { 0x006DB7D4u, "the game window received WM_CLOSE" },
    { 0x006C2206u, "the game received WM_QUIT (its window was destroyed or PostQuitMessage)" },
};
#define QUIT_SITE_COUNT (sizeof(k_quit_sites) / sizeof(k_quit_sites[0]))

typedef struct wnd_event { DWORD tick, msg, wparam, lparam, sent; } wnd_event;
#define WND_RING 16
static wnd_event g_wnd_ring[WND_RING];
static DWORD     g_wnd_count, g_focus_logs;
static volatile LONG g_quit_logged;

static const char *wnd_msg_name(DWORD m)
{
    switch (m) {
    case 0x0002: return "WM_DESTROY";
    case 0x0005: return "WM_SIZE";
    case 0x0006: return "WM_ACTIVATE";
    case 0x0008: return "WM_KILLFOCUS";
    case 0x0010: return "WM_CLOSE";
    case 0x0011: return "WM_QUERYENDSESSION";
    case 0x0016: return "WM_ENDSESSION";
    case 0x001C: return "WM_ACTIVATEAPP";
    case 0x0104: return "WM_SYSKEYDOWN";
    case 0x0112: return "WM_SYSCOMMAND";
    default:     return 0;
    }
}

static void describe_event(const wnd_event *e, char *out, DWORD cap)
{
    const char *extra = "";
    if (e->msg == 0x0112 && (e->wparam & 0xFFF0) == 0xF060) extra = " = SC_CLOSE (Alt+F4, title bar or taskbar close)";
    else if (e->msg == 0x0112 && (e->wparam & 0xFFF0) == 0xF020) extra = " = SC_MINIMIZE";
    else if (e->msg == 0x0104 && e->wparam == 0x73) extra = " = Alt+F4 pressed";
    else if (e->msg == 0x001C) extra = e->wparam ? " = game got focus" : " = game lost focus";
    else if (e->msg == 0x0005 && e->wparam == 1) extra = " = minimized";
    fmt(out, cap, "%s wParam=%08lX lParam=%08lX%s%s", wnd_msg_name(e->msg), e->wparam, e->lparam,
        e->sent ? " (sent by another thread/program)" : "", extra);
}

/* Called for every message of the game window, before the game handles it. */
__attribute__((used)) void __cdecl on_wnd_msg(const DWORD *args)   /* hwnd, msg, wparam, lparam */
{
    wnd_event e;
    char text[200];
    if (!wnd_msg_name(args[1])) return;
    if (args[1] == 0x0104 && args[2] != 0x73) return;          /* only Alt+F4 among sys keys */
    e.tick = GetTickCount();
    e.msg = args[1]; e.wparam = args[2]; e.lparam = args[3];
    e.sent = (InSendMessageEx(0) & 0x3) == 0x1;                  /* ISMEX_SEND (other thread) */
    lock();
    g_wnd_ring[g_wnd_count++ % WND_RING] = e;
    unlock();
    switch (e.msg) {
    case 0x0010: case 0x0002: case 0x0011: case 0x0016:
        describe_event(&e, text, sizeof(text));
        cg_log("window: %s", text);
        break;
    case 0x0112:
        if ((e.wparam & 0xFFF0) == 0xF060) { describe_event(&e, text, sizeof(text)); cg_log("window: %s", text); }
        break;
    case 0x0104:
        describe_event(&e, text, sizeof(text)); cg_log("window: %s", text);
        break;
    case 0x001C:
        if (g_focus_logs++ < 20) { describe_event(&e, text, sizeof(text)); cg_log("window: %s", text); }
        break;
    default:
        break;
    }
}

__declspec(naked) static void wndproc_stub(void)
{
    __asm__ volatile (
        "pushfl\n\t"
        "pushal\n\t"
        "leal  40(%esp), %eax\n\t"      /* args: [esp at entry + 4] */
        "pushl %eax\n\t"
        "call  _on_wnd_msg\n\t"
        "addl  $4, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "pushl %ebx\n\t"                /* the replaced instructions */
        "pushl %esi\n\t"
        "movl  16(%esp), %esi\n\t"
        "pushl $0x006DB6C6\n\t"
        "ret\n\t");
}

__attribute__((used)) void __cdecl on_quit_flag(DWORD ret, DWORD esp_after)
{
    DWORD site = ret - 5, i, now = GetTickCount(), n;
    const char *reason = "unknown place";
    char text[200];
    for (i = 0; i < QUIT_SITE_COUNT; i++) if (k_quit_sites[i].addr == site) reason = k_quit_sites[i].reason;
    if (__sync_lock_test_and_set(&g_quit_logged, 1)) return;
    cg_log("game asked to quit at %08lX: %s", site, reason);
    log_game_stack(esp_after);
    lock();
    n = g_wnd_count < WND_RING ? g_wnd_count : WND_RING;
    unlock();
    if (n) cg_log("  last window events (oldest first):");
    for (i = 0; i < n; i++) {
        const wnd_event *e = &g_wnd_ring[(g_wnd_count - n + i) % WND_RING];
        describe_event(e, text, sizeof(text));
        cg_log("    %lu ms ago: %s", now - e->tick, text);
    }
}

/* Replaces "mov dword ptr [0x9257EC], 1" at the five quit sites (call + 5 nops). */
__declspec(naked) static void quit_flag_stub(void)
{
    __asm__ volatile (
        "pushfl\n\t"
        "pushal\n\t"
        "leal  40(%esp), %eax\n\t"      /* esp of the game code after our return */
        "pushl %eax\n\t"
        "pushl 40(%esp)\n\t"            /* return address (after the 5-byte call) */
        "call  _on_quit_flag\n\t"
        "addl  $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "movl  $1, 0x009257EC\n\t"
        "ret\n\t");
}

static void install_quit_diagnostics(void)
{
    static const BYTE flag_insn[] = { 0xC7, 0x05, 0xEC, 0x57, 0x92, 0x00, 0x01, 0x00, 0x00, 0x00 };
    static const BYTE wndproc[]   = { 0x53, 0x56, 0x8B, 0x74, 0x24, 0x10 };
    BYTE patch[10];
    DWORD i, j, hooked = 0;
    int ok_wnd = 0;
    for (i = 0; i < QUIT_SITE_COUNT; i++) {
        DWORD a = k_quit_sites[i].addr;
        if (!bytes_equal(a, flag_insn, sizeof(flag_insn))) continue;
        patch[0] = 0xE8;
        *(DWORD *)(patch + 1) = (DWORD)quit_flag_stub - (a + 5);
        for (j = 5; j < 10; j++) patch[j] = 0x90;
        hooked += write_code(a, patch, sizeof(patch));
    }
    if (bytes_equal(GAME_WNDPROC, wndproc, sizeof(wndproc))) {
        patch[0] = 0xE9;
        *(DWORD *)(patch + 1) = (DWORD)wndproc_stub - (GAME_WNDPROC + 5);
        patch[5] = 0x90;
        ok_wnd = write_code(GAME_WNDPROC, patch, 6);
    }
    cg_log("diagnostics: %lu/%u quit paths watched, window messages %s",
           hooked, (unsigned)QUIT_SITE_COUNT, ok_wnd ? "watched" : "NOT watched");
}

/* ------------------------------------------------------------------------------------------ */
/* Self-test (SelfTest=1): runs three tiny pieces of speed.exe code with broken pointers, one   */
/* for each recovery path, and logs whether the handler brought them back.                     */
/* ------------------------------------------------------------------------------------------ */

#define GADGET_LOAD 0x00402510u   /* mov eax, [ecx+8]       ; ret */
#define GADGET_CALL 0x00402567u   /* call dword [eax+disp8] ; ret */
#define UNMAPPED    0x00000012u   /* inside the never-mapped first 64 KB */

static DWORD g_fake_vtable[64];
static volatile DWORD g_st_esp_after, g_st_esp_expected;

/* cdecl: calls target with EAX/ECX preset, returns EAX (for code that ends in "ret"). */
__declspec(naked) static DWORD call_gadget(DWORD target, DWORD eax_value, DWORD ecx_value)
{
    __asm__ volatile (
        "movl 8(%esp), %eax\n\t"
        "movl 12(%esp), %ecx\n\t"
        "movl 4(%esp), %edx\n\t"
        "call *%edx\n\t"
        "ret\n\t");
}

/*
 * cdecl: enters game code at a CALL instruction the way its own function would: the stack holds
 * a return address and then arg_bytes of (dummy) arguments. If ChaseGuard removes exactly the
 * arguments of the failed call, the game's "ret" after the call comes back here with ESP where
 * it was before the arguments were pushed; that ESP is recorded for the check.
 */
__declspec(naked) static DWORD call_gadget_args(DWORD target, DWORD eax_value, DWORD ecx_value, DWORD arg_bytes)
{
    __asm__ volatile (
        "pushl %ebp\n\t"
        "movl  %esp, %ebp\n\t"
        "pushl %ebx\n\t"
        "pushl %esi\n\t"
        "pushl %edi\n\t"
        "pushl $2f\n\t"                 /* where the game's "ret" must come back to */
        "movl  %esp, %eax\n\t"
        "addl  $4, %eax\n\t"
        "movl  %eax, _g_st_esp_expected\n\t"
        "movl  20(%ebp), %ecx\n\t"
        "shrl  $2, %ecx\n"
        "1:\n\t"
        "testl %ecx, %ecx\n\t"
        "jz    3f\n\t"
        "pushl $0xA5A5A5A5\n\t"         /* dummy argument */
        "decl  %ecx\n\t"
        "jmp   1b\n"
        "3:\n\t"
        "movl  12(%ebp), %eax\n\t"
        "movl  16(%ebp), %ecx\n\t"
        "movl  8(%ebp), %edx\n\t"
        "jmp   *%edx\n"
        "2:\n\t"
        "movl  %esp, _g_st_esp_after\n\t"
        "leal  -12(%ebp), %esp\n\t"
        "popl  %edi\n\t"
        "popl  %esi\n\t"
        "popl  %ebx\n\t"
        "popl  %ebp\n\t"
        "ret\n\t");
}

static int gadget_ok(DWORD addr, const BYTE *pattern, DWORD n, DWORD wildcard)
{
    DWORD i;
    for (i = 0; i < n; i++)
        if (i != wildcard && ((const BYTE *)addr)[i] != pattern[i]) return 0;
    return 1;
}

static int st_check(const char *name, DWORD r)
{
    int stack_ok = g_st_esp_after == g_st_esp_expected;
    int pass = r == 0 && stack_ok;
    cg_log("%s %s (EAX=%08lX, stack %s)", name, pass ? "PASS" : "FAIL", r,
           stack_ok ? "balanced" : "NOT balanced");
    return pass;
}

static DWORD WINAPI self_test_thread(void *arg)
{
    static const BYTE load[] = { 0x8B, 0x41, 0x08, 0xC3 };
    static const BYTE call[] = { 0xFF, 0x50, 0x00, 0xC3 };
    DWORD disp, r, passed = 0;
    cg_u32 args = 0;
    (void)arg;
    Sleep(10000);
    if (!gadget_ok(GADGET_LOAD, load, 4, 99) || !gadget_ok(GADGET_CALL, call, 4, 2)) {
        cg_log("self-test: game code differs from v1.3, test skipped");
        return 0;
    }
    disp = ((const BYTE *)GADGET_CALL)[2];
    if (g_cfg.correct_stack && !cg_call_arg_bytes(&g_env, GADGET_CALL, &args)) args = 0;
    cg_log("self-test: starting (deliberate faults in game code follow; the call at %08X takes %lu arg bytes)",
           GADGET_CALL, (DWORD)args);

    r = call_gadget(GADGET_LOAD, 0xDEADBEEF, UNMAPPED);
    cg_log("self-test 1/4 load from bad pointer:   %s (EAX=%08lX)", r == 0 ? "PASS" : "FAIL", r);
    passed += r == 0;

    r = call_gadget_args(GADGET_CALL, UNMAPPED, 0, args);
    passed += st_check("self-test 2/4 call via bad pointer:   ", r);

    g_fake_vtable[disp / 4] = UNMAPPED;
    r = call_gadget_args(GADGET_CALL, (DWORD)g_fake_vtable, 0xDEADBEEF, args);
    passed += st_check("self-test 3/4 call into bad code:     ", r);

    if (g_cfg.recover_pure_calls && *(DWORD *)CRT_PURECALL_SLOT == (DWORD)purecall_stub) {
        /* the game's "call [eax+disp]" reaches _purecall, exactly like a real R6025 */
        g_fake_vtable[disp / 4] = CRT_PURECALL;
        r = call_gadget_args(GADGET_CALL, (DWORD)g_fake_vtable, (DWORD)g_fake_vtable, args);
        passed += st_check("self-test 4/4 pure virtual call:      ", r);
    } else {
        cg_log("self-test 4/4 pure virtual call:       skipped (RecoverPureCalls=0)");
        passed++;
    }
    cg_log("self-test finished: %lu/4 passed%s", passed,
           passed == 4 ? "" : " - please report this with chaseguard.log");
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
    cg_log("EnablePatch=%d EnableHandler=%d RecoverFaults=%d RecoverBadCalls=%d RecoverPureCalls=%d "
           "CorrectStack=%d ZeroLoadedRegister=%d LoopLimit=%d MaxLogKB=%d SelfTest=%d",
         g_cfg.enable_patch, g_cfg.enable_handler, g_cfg.recover_faults, g_cfg.recover_bad_calls,
         g_cfg.recover_pure_calls, g_cfg.correct_stack, g_cfg.zero_loaded_reg, g_cfg.loop_limit,
         g_cfg.max_log_kb, g_cfg.self_test);

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

    if (!cg_init()) {
        cg_log("instruction decoder failed to initialise - exception handler not installed");
        return;
    }
    g_env.text_begin = g_text_begin;
    g_env.text_end = g_text_end;
    g_env.recover_faults = g_cfg.recover_faults;
    g_env.recover_bad_calls = g_cfg.recover_bad_calls;
    g_env.zero_loaded_reg = g_cfg.zero_loaded_reg;
    g_env.correct_stack = g_cfg.correct_stack;
    g_env.read = safe_read;
    g_env.read_user = 0;
    g_env.text_image = (const cg_u8 *)g_text_begin;   /* speed.exe .text is mapped and readable */
    install_diagnostics();

    if (!g_cfg.enable_handler) {
        cg_log("exception handler disabled in the ini (EnableHandler=0)");
        return;
    }
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
        if (!g_exit_logged)
            cg_log("game is closing (exit path not seen by ChaseGuard)");
        cg_log("=== session end: %lu recovered, %lu not recovered, %lu pure calls (%lu returned), %lu loop stops ===",
             g_recovered, g_given_up, g_pure_calls, g_pure_recovered, g_loops);
        log_close();
    }
    return TRUE;
}
