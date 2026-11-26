/*
 * ChaseGuard - crash protection for Need for Speed: Most Wanted (2005) v1.3
 * Copyright (C) 2026 biquiet696 and contributors
 * Based on MWCrashFix by x0reaxeax (https://github.com/x0reaxeax/MWCrashFix)
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version. See LICENSE.
 *
 * core.h - platform-independent decision logic of the exception handler.
 * It is compiled into the Windows plugin and into the Linux test suite (tests/).
 */
#ifndef CHASEGUARD_CORE_H
#define CHASEGUARD_CORE_H

typedef unsigned int  cg_u32;
typedef unsigned char cg_u8;

/* Access type as reported in EXCEPTION_RECORD.ExceptionInformation[0]. */
#define CG_ACCESS_READ    0u
#define CG_ACCESS_WRITE   1u
#define CG_ACCESS_EXECUTE 8u

/* Integer registers of the faulting thread (in/out). */
typedef struct cg_regs {
    cg_u32 eax, ecx, edx, ebx, esp, ebp, esi, edi, eip;
} cg_regs;

/* Copies len bytes at addr into dst. Returns 1 only if the whole range is readable. */
typedef int (*cg_read_fn)(void *user, cg_u32 addr, void *dst, cg_u32 len);

typedef struct cg_env {
    cg_u32     text_begin;        /* speed.exe .text, [begin, end) */
    cg_u32     text_end;
    int        recover_faults;    /* skip a game instruction that faults on a data access   */
    int        recover_bad_calls; /* return from a call that jumped to non-executable memory */
    int        zero_loaded_reg;   /* "mov reg, [bad]" -> reg = 0 when it is skipped          */
    int        correct_stack;     /* remove the arguments a skipped callee would have removed */
    cg_read_fn read;
    void      *read_user;
    const cg_u8 *text_image;      /* optional: bytes of .text (fast path for code reads)     */
} cg_env;

typedef enum cg_verdict {
    CG_FOREIGN   = 0,  /* not caused by game code: leave it alone, do not log           */
    CG_GIVE_UP   = 1,  /* game fault, but recovery is not safe/allowed: let it crash    */
    CG_RECOVERED = 2   /* registers were changed, continue execution                    */
} cg_verdict;

typedef enum cg_kind {
    CG_KIND_NONE = 0,
    CG_KIND_SKIP_INSTRUCTION,   /* game instruction with a bad memory operand skipped        */
    CG_KIND_SKIP_CALL,          /* "call [bad]" skipped, EAX = 0                             */
    CG_KIND_RETURN_BAD_CALL     /* call landed on non-executable memory, returned, EAX = 0   */
} cg_kind;

typedef struct cg_result {
    cg_verdict  verdict;
    cg_kind     kind;
    cg_u8       insn_len;      /* length of the skipped / calling instruction            */
    cg_u32      call_site;     /* address of the CALL instruction (bad-call paths)       */
    cg_u32      arg_bytes;     /* stack bytes removed for the skipped callee             */
    int         arg_known;     /* 1 if arg_bytes was determined, 0 if it is a guess (0)  */
    int         zeroed;        /* 1 if a destination register was cleared                */
    const char *reason;        /* short human-readable explanation, never NULL           */
} cg_result;

/* Must be called once before cg_decide(). Returns 0 on failure. */
int cg_init(void);

/*
 * Decides what to do with an access violation.
 *   access     - ExceptionInformation[0]
 *   fault_addr - ExceptionInformation[1] (the inaccessible data/code address)
 *   regs       - thread registers; modified only when CG_RECOVERED is returned
 */
cg_verdict cg_decide(const cg_env *env, cg_u32 access, cg_u32 fault_addr,
                     cg_regs *regs, cg_result *out);

const char *cg_kind_name(cg_kind kind);

/*
 * If the bytes right before ret (a return address in .text) decode to a CALL instruction,
 * stores its address in *call_site and returns 1. Used for logging call stacks.
 */
int cg_call_before(const cg_env *env, cg_u32 ret, cg_u32 *call_site);

/*
 * Estimates how many bytes of arguments the callee of the CALL at call_site would remove from
 * the stack (__thiscall/__stdcall "ret N"). Works on the caller's code: it decodes the calling
 * function from its start and counts the argument pushes that belong to this call. Returns 1
 * and sets *bytes on success; returns 0 if the code could not be analysed (then *bytes = 0).
 * A call followed by "add esp, N" is __cdecl and yields 0.
 * Measured on speed.exe v1.3: exact for 88.9 % of the 22,640 callee-cleaned direct calls.
 */
int cg_call_arg_bytes(const cg_env *env, cg_u32 call_site, cg_u32 *bytes);

/*
 * Returns 1 if the code at ret (the return address of a call) uses a float result of the callee
 * in ST(0) before loading anything onto the x87 stack. A substitute that skips such a callee
 * must push 0.0 or the caller pops an empty FPU register.
 */
int cg_call_returns_float(const cg_env *env, cg_u32 ret);

/*
 * Removes every {type, interface} pair whose interface equals iface from an array of count
 * pairs (what the game's 0x5D5A50 should do). Returns the new count; *found = pairs removed,
 * *type = type of the last one removed (unchanged if none).
 */
cg_u32 cg_iface_remove(cg_u32 *pairs, cg_u32 count, cg_u32 iface, cg_u32 *found, cg_u32 *type);

#endif
