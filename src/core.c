/*
 * ChaseGuard - crash protection for Need for Speed: Most Wanted (2005) v1.3
 * Copyright (C) 2026 biquiet696 and contributors
 * Based on MWCrashFix by x0reaxeax (https://github.com/x0reaxeax/MWCrashFix)
 * License: GPL-3.0-or-later (see LICENSE)
 *
 * core.c - decides whether an access violation can be recovered from.
 *
 * Two situations are handled, and only when they can be proven from the instruction stream:
 *
 *  A) The faulting instruction is inside speed.exe .text and accesses bad data
 *     ("mov eax, [ecx+8]" with a dangling ecx). The instruction is skipped. A load into a
 *     general-purpose register clears that register, so the game's own null checks take over.
 *     Instructions that change control flow or the stack pointer are never skipped, because
 *     skipping them would desynchronise the stack. A "call [bad]" is skipped as a whole and
 *     returns 0 in EAX.
 *
 *  B) Execution jumped to non-executable memory from a CALL in speed.exe (e.g. a call through a
 *     corrupted virtual table). This is accepted only if the fault is an execute fault exactly at
 *     EIP, [ESP] is a return address in .text, the bytes before it decode to a CALL, and that CALL
 *     really targets EIP (its register/memory operand is re-evaluated). Then the call "returns"
 *     with EAX = 0.
 *
 * Everything else is reported as foreign (not touched, not logged) or as give-up (logged, then
 * passed on so the game crashes exactly as it would without the plugin).
 */
#include "core.h"
#include "../third_party/zydis/Zydis.h"

static ZydisDecoder g_decoder;
static int g_ready;

int cg_init(void)
{
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&g_decoder, ZYDIS_MACHINE_MODE_LEGACY_32,
                                       ZYDIS_STACK_WIDTH_32)))
        return 0;
    g_ready = 1;
    return 1;
}

const char *cg_kind_name(cg_kind kind)
{
    switch (kind) {
    case CG_KIND_SKIP_INSTRUCTION: return "skip-instruction";
    case CG_KIND_SKIP_CALL:        return "skip-call";
    case CG_KIND_RETURN_BAD_CALL:  return "return-from-bad-call";
    default:                       return "none";
    }
}

static int in_text(const cg_env *env, cg_u32 a)
{
    return a >= env->text_begin && a < env->text_end;
}

/* Pointer to the 32-bit register slot in cg_regs, or 0 for anything else. */
static cg_u32 *reg32_slot(cg_regs *r, ZydisRegister zr)
{
    switch (zr) {
    case ZYDIS_REGISTER_EAX: return &r->eax;
    case ZYDIS_REGISTER_ECX: return &r->ecx;
    case ZYDIS_REGISTER_EDX: return &r->edx;
    case ZYDIS_REGISTER_EBX: return &r->ebx;
    case ZYDIS_REGISTER_ESP: return &r->esp;
    case ZYDIS_REGISTER_EBP: return &r->ebp;
    case ZYDIS_REGISTER_ESI: return &r->esi;
    case ZYDIS_REGISTER_EDI: return &r->edi;
    default:                 return 0;
    }
}

static int is_stack_reg(ZydisRegister zr)
{
    return zr == ZYDIS_REGISTER_ESP || zr == ZYDIS_REGISTER_SP || zr == ZYDIS_REGISTER_SPL;
}

/* Clears a general-purpose destination register (8/16/32-bit forms). Returns 1 on success. */
static int clear_register(cg_regs *r, ZydisRegister zr)
{
    cg_u32 *slot;
    if (is_stack_reg(zr))
        return 0;
    if ((slot = reg32_slot(r, zr)) != 0) { *slot = 0; return 1; }
    switch (zr) {
    case ZYDIS_REGISTER_AX: r->eax &= 0xFFFF0000u; return 1;
    case ZYDIS_REGISTER_CX: r->ecx &= 0xFFFF0000u; return 1;
    case ZYDIS_REGISTER_DX: r->edx &= 0xFFFF0000u; return 1;
    case ZYDIS_REGISTER_BX: r->ebx &= 0xFFFF0000u; return 1;
    case ZYDIS_REGISTER_BP: r->ebp &= 0xFFFF0000u; return 1;
    case ZYDIS_REGISTER_SI: r->esi &= 0xFFFF0000u; return 1;
    case ZYDIS_REGISTER_DI: r->edi &= 0xFFFF0000u; return 1;
    case ZYDIS_REGISTER_AL: r->eax &= 0xFFFFFF00u; return 1;
    case ZYDIS_REGISTER_CL: r->ecx &= 0xFFFFFF00u; return 1;
    case ZYDIS_REGISTER_DL: r->edx &= 0xFFFFFF00u; return 1;
    case ZYDIS_REGISTER_BL: r->ebx &= 0xFFFFFF00u; return 1;
    case ZYDIS_REGISTER_AH: r->eax &= 0xFFFF00FFu; return 1;
    case ZYDIS_REGISTER_CH: r->ecx &= 0xFFFF00FFu; return 1;
    case ZYDIS_REGISTER_DH: r->edx &= 0xFFFF00FFu; return 1;
    case ZYDIS_REGISTER_BH: r->ebx &= 0xFFFF00FFu; return 1;
    default: return 0;
    }
}

/*
 * Effective address of a memory operand. esp_bias is added when ESP is used as base/index
 * (needed when the CALL already pushed its return address). Returns 0 if not computable.
 */
static int effective_address(const ZydisDecodedOperand *op, const cg_regs *regs,
                             cg_u32 esp_bias, cg_u32 *ea)
{
    cg_regs tmp = *regs;
    cg_u32 a, *p;
    if (op->type != ZYDIS_OPERAND_TYPE_MEMORY || op->mem.type != ZYDIS_MEMOP_TYPE_MEM)
        return 0;
    switch (op->mem.segment) {          /* flat segments only; FS/GS cannot be evaluated here */
    case ZYDIS_REGISTER_NONE: case ZYDIS_REGISTER_DS:
    case ZYDIS_REGISTER_SS:   case ZYDIS_REGISTER_ES: case ZYDIS_REGISTER_CS:
        break;
    default:
        return 0;
    }
    tmp.esp += esp_bias;
    a = op->mem.disp.has_displacement ? (cg_u32)op->mem.disp.value : 0;
    if (op->mem.base != ZYDIS_REGISTER_NONE) {
        if ((p = reg32_slot(&tmp, op->mem.base)) == 0) return 0;
        a += *p;
    }
    if (op->mem.index != ZYDIS_REGISTER_NONE) {
        if ((p = reg32_slot(&tmp, op->mem.index)) == 0) return 0;
        a += *p * (cg_u32)(op->mem.scale ? op->mem.scale : 1);
    }
    *ea = a;
    return 1;
}

static int read_code(const cg_env *env, cg_u32 addr, void *dst, cg_u32 len)
{
    if (env->text_image && addr >= env->text_begin && addr + len <= env->text_end && addr + len > addr) {
        const cg_u8 *src = env->text_image + (addr - env->text_begin);
        cg_u8 *d = (cg_u8 *)dst;
        cg_u32 i;
        for (i = 0; i < len; i++) d[i] = src[i];
        return 1;
    }
    return env->read(env->read_user, addr, dst, len);
}

static int decode(const cg_env *env, cg_u32 addr, cg_u32 maxlen,
                  ZydisDecodedInstruction *insn, ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT])
{
    cg_u8 buf[ZYDIS_MAX_INSTRUCTION_LENGTH];
    if (maxlen > sizeof(buf)) maxlen = sizeof(buf);
    if (env->text_image && addr >= env->text_begin && addr < env->text_end && addr + maxlen > env->text_end)
        maxlen = env->text_end - addr;
    if (maxlen == 0 || !read_code(env, addr, buf, maxlen))
        return 0;
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&g_decoder, buf, maxlen, insn, ops));
}

static cg_verdict finish(cg_result *out, cg_verdict v, const char *reason)
{
    out->verdict = v;
    out->reason = reason;
    return v;
}

/* --- A: fault inside game code --------------------------------------------------------- */
static cg_verdict decide_in_text(const cg_env *env, cg_u32 access, cg_u32 fault_addr,
                                 cg_regs *regs, cg_result *out)
{
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    cg_u32 maxlen = env->text_end - regs->eip, ea, i;

    if (access == CG_ACCESS_EXECUTE)
        return finish(out, CG_GIVE_UP, "execute fault inside game code");
    if (!decode(env, regs->eip, maxlen, &insn, ops))
        return finish(out, CG_GIVE_UP, "faulting instruction cannot be decoded");
    out->insn_len = insn.length;

    if (insn.meta.category == ZYDIS_CATEGORY_CALL) {
        if (access != CG_ACCESS_READ || insn.operand_count_visible < 1
            || ops[0].type != ZYDIS_OPERAND_TYPE_MEMORY)
            return finish(out, CG_GIVE_UP, "call faulted while pushing its return address");
        if (!effective_address(&ops[0], regs, 0, &ea) || fault_addr - ea >= 4u)
            return finish(out, CG_GIVE_UP, "call operand does not match the fault address");
        if (!env->recover_bad_calls)
            return finish(out, CG_GIVE_UP, "bad-call recovery disabled");
        out->call_site = regs->eip;
        if (env->correct_stack)
            out->arg_known = cg_call_arg_bytes(env, out->call_site, &out->arg_bytes);
        regs->eip += insn.length;
        regs->esp += out->arg_bytes;   /* the callee would have removed its arguments */
        regs->eax = 0;
        out->kind = CG_KIND_SKIP_CALL;
        return finish(out, CG_RECOVERED, "call through an unreadable pointer skipped, EAX=0");
    }

    switch (insn.meta.category) {
    case ZYDIS_CATEGORY_COND_BR: case ZYDIS_CATEGORY_UNCOND_BR: case ZYDIS_CATEGORY_RET:
    case ZYDIS_CATEGORY_PUSH:    case ZYDIS_CATEGORY_POP:       case ZYDIS_CATEGORY_INTERRUPT:
    case ZYDIS_CATEGORY_SYSTEM:  case ZYDIS_CATEGORY_SYSCALL:   case ZYDIS_CATEGORY_SYSRET:
        return finish(out, CG_GIVE_UP, "control-flow/stack instruction cannot be skipped");
    default:
        break;
    }
    for (i = 0; i < insn.operand_count; i++)
        if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER && is_stack_reg(ops[i].reg.value)
            && (ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
            return finish(out, CG_GIVE_UP, "instruction changes ESP, cannot be skipped");

    if (!env->recover_faults)
        return finish(out, CG_GIVE_UP, "fault recovery disabled");

    if (env->zero_loaded_reg
        && (insn.mnemonic == ZYDIS_MNEMONIC_MOV || insn.mnemonic == ZYDIS_MNEMONIC_MOVZX
            || insn.mnemonic == ZYDIS_MNEMONIC_MOVSX)
        && insn.operand_count_visible == 2
        && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER
        && ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY)
        out->zeroed = clear_register(regs, ops[0].reg.value);

    regs->eip += insn.length;
    out->kind = CG_KIND_SKIP_INSTRUCTION;
    return finish(out, CG_RECOVERED, out->zeroed ? "load from bad pointer skipped, register = 0"
                                                 : "instruction with bad memory operand skipped");
}

/* --- B: execution landed outside game code ------------------------------------------- */
static cg_verdict decide_bad_call(const cg_env *env, cg_u32 access, cg_u32 fault_addr,
                                  cg_regs *regs, cg_result *out)
{
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    cg_u32 ret, n, target, ea, *slot;
    int found = 0;

    /* Must be the very first instruction fetch at EIP, so [ESP] is still the return address. */
    if ((access != CG_ACCESS_READ && access != CG_ACCESS_EXECUTE) || fault_addr != regs->eip)
        return finish(out, CG_FOREIGN, "not an instruction-fetch fault");
    if (!env->read(env->read_user, regs->esp, &ret, 4) || !in_text(env, ret))
        return finish(out, CG_FOREIGN, "no game return address on the stack");

    for (n = 2; n <= 7 && !found; n++) {
        if (ret - n < env->text_begin)
            break;
        if (!decode(env, ret - n, n, &insn, ops) || insn.length != n
            || insn.mnemonic != ZYDIS_MNEMONIC_CALL || insn.operand_count_visible < 1)
            continue;
        switch (ops[0].type) {
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:              /* call rel32 */
            target = ret + (cg_u32)ops[0].imm.value.s;
            found = (target == regs->eip);
            break;
        case ZYDIS_OPERAND_TYPE_REGISTER:               /* call reg   */
            slot = reg32_slot(regs, ops[0].reg.value);
            found = slot && *slot == regs->eip;
            break;
        case ZYDIS_OPERAND_TYPE_MEMORY:                 /* call [mem] */
            found = effective_address(&ops[0], regs, 4, &ea)
                    && env->read(env->read_user, ea, &target, 4) && target == regs->eip;
            break;
        default:
            break;
        }
        if (found) {
            out->call_site = ret - n;
            out->insn_len = (cg_u8)n;
        }
    }
    if (!found)
        return finish(out, CG_FOREIGN, "return address is not preceded by a matching call");
    if (!env->recover_bad_calls)
        return finish(out, CG_GIVE_UP, "bad-call recovery disabled");

    if (env->correct_stack)
        out->arg_known = cg_call_arg_bytes(env, out->call_site, &out->arg_bytes);
    regs->eip = ret;
    regs->esp += 4 + out->arg_bytes;
    regs->eax = 0;
    out->kind = CG_KIND_RETURN_BAD_CALL;
    return finish(out, CG_RECOVERED, "call into non-executable memory returned, EAX=0");
}

int cg_call_before(const cg_env *env, cg_u32 ret, cg_u32 *call_site)
{
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    cg_u32 n;
    if (!g_ready || !in_text(env, ret))
        return 0;
    for (n = 2; n <= 7; n++) {
        if (ret - n < env->text_begin)
            break;
        if (decode(env, ret - n, n, &insn, ops) && insn.length == n
            && insn.mnemonic == ZYDIS_MNEMONIC_CALL) {
            *call_site = ret - n;
            return 1;
        }
    }
    return 0;
}


/* ------------------------------------------------------------------------------------------ */
/* Argument size of a call, from the caller's code (see cg_call_arg_bytes in core.h).          */
/* ------------------------------------------------------------------------------------------ */

enum { K_OTHER, K_PUSH, K_POP, K_CALL, K_BOUNDARY, K_SEH, K_SUBESP };

typedef struct ainsn {
    cg_u32 addr, target, sub_imm;
    cg_u8  len, kind, direct, refs_esp, mov_from_esp, mov_ebp_esp, push_eax, add_esp;
    ZydisRegister push_reg, wreg;
} ainsn;

#define WIN_MAX   56     /* instructions looked at before a call */
#define RING      96     /* instructions kept while decoding a function */

static int is_saved(ZydisRegister r)
{
    return r == ZYDIS_REGISTER_EBX || r == ZYDIS_REGISTER_ESI || r == ZYDIS_REGISTER_EDI ||
           r == ZYDIS_REGISTER_EBP;
}

static int summarize(const cg_env *env, cg_u32 addr, ainsn *a)
{
    ZydisDecodedInstruction in;
    ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];
    cg_u32 i;
    int vis;
    if (!decode(env, addr, ZYDIS_MAX_INSTRUCTION_LENGTH, &in, op))
        return 0;
    vis = in.operand_count_visible;
    a->addr = addr; a->len = in.length; a->kind = K_OTHER; a->target = 0; a->sub_imm = 0;
    a->direct = a->refs_esp = a->mov_from_esp = a->mov_ebp_esp = a->push_eax = a->add_esp = 0;
    a->push_reg = a->wreg = ZYDIS_REGISTER_NONE;

    for (i = 0; i < (cg_u32)vis; i++)
        if (op[i].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (op[i].mem.segment == ZYDIS_REGISTER_FS) a->kind = K_SEH;
            if (op[i].mem.base == ZYDIS_REGISTER_ESP) a->refs_esp = 1;
        }
    if (a->kind == K_SEH) return 1;

    switch (in.mnemonic) {
    case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_RET: case ZYDIS_MNEMONIC_JMP:
    case ZYDIS_MNEMONIC_PUSHFD: case ZYDIS_MNEMONIC_PUSHAD: case ZYDIS_MNEMONIC_POPFD:
    case ZYDIS_MNEMONIC_POPAD: case ZYDIS_MNEMONIC_LEAVE: case ZYDIS_MNEMONIC_ENTER:
        a->kind = K_BOUNDARY; return 1;
    case ZYDIS_MNEMONIC_PUSH:
        a->kind = K_PUSH;
        if (vis >= 1 && op[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            a->push_reg = op[0].reg.value;
            a->push_eax = a->push_reg == ZYDIS_REGISTER_EAX;
        }
        return 1;
    case ZYDIS_MNEMONIC_POP:
        a->kind = K_POP;
        if (vis >= 1 && op[0].type == ZYDIS_OPERAND_TYPE_REGISTER) a->wreg = op[0].reg.value;
        return 1;
    case ZYDIS_MNEMONIC_CALL:
        a->kind = K_CALL;
        if (vis >= 1 && op[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            a->direct = 1;
            a->target = addr + in.length + (cg_u32)op[0].imm.value.s;
        }
        return 1;
    default:
        break;
    }
    if (in.meta.category == ZYDIS_CATEGORY_COND_BR) { a->kind = K_BOUNDARY; return 1; }
    if (vis >= 1 && op[0].type == ZYDIS_OPERAND_TYPE_REGISTER && op[0].reg.value == ZYDIS_REGISTER_ESP) {
        if (in.mnemonic == ZYDIS_MNEMONIC_SUB && vis == 2 && op[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            a->kind = K_SUBESP;
            a->sub_imm = (cg_u32)op[1].imm.value.u;
        } else if (in.mnemonic == ZYDIS_MNEMONIC_ADD && vis == 2 && op[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            a->kind = K_BOUNDARY;
            a->add_esp = 1;
        } else {
            a->kind = K_BOUNDARY;      /* mov/lea/and esp, ... */
        }
        return 1;
    }
    if ((in.mnemonic == ZYDIS_MNEMONIC_MOV || in.mnemonic == ZYDIS_MNEMONIC_LEA ||
         in.mnemonic == ZYDIS_MNEMONIC_MOVZX || in.mnemonic == ZYDIS_MNEMONIC_MOVSX) &&
        vis >= 1 && op[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        a->wreg = op[0].reg.value;
        if (in.mnemonic == ZYDIS_MNEMONIC_MOV && vis == 2 && op[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            op[1].reg.value == ZYDIS_REGISTER_ESP) {
            a->mov_from_esp = 1;
            a->mov_ebp_esp = op[0].reg.value == ZYDIS_REGISTER_EBP;
        }
    }
    return 1;
}

/* "ret N" of a function, found by decoding it from its first instruction. */
static int callee_pop(const cg_env *env, cg_u32 target, cg_u32 *bytes)
{
    ZydisDecodedInstruction in;
    ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];
    cg_u32 a = target, n;
    if (a < env->text_begin || a >= env->text_end) return 0;
    for (n = 0; n < 8000; n++) {
        if (!decode(env, a, ZYDIS_MAX_INSTRUCTION_LENGTH, &in, op)) return 0;
        if (in.mnemonic == ZYDIS_MNEMONIC_INT3) return 0;
        if (in.mnemonic == ZYDIS_MNEMONIC_RET) {
            *bytes = (in.operand_count_visible >= 1 && op[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
                     ? (cg_u32)op[0].imm.value.u : 0;
            return 1;
        }
        a += in.length;
    }
    return 0;
}

typedef struct afunc {
    ainsn ins[RING + 1];     /* last instructions before the call, then the call itself */
    int   count;             /* instructions before the call */
    int   from_start;        /* ins[0] is the first instruction of the function */
} afunc;

enum { W_LIMIT, W_FUNC, W_PROLOGUE, W_OTHER, W_CALL };

static int estimate_at(const cg_env *env, const afunc *f, int idx, int depth, cg_u32 *out);

/* Indices (oldest first) of the instructions that set up the arguments of the call f->ins[idx]. */
static int arg_window(const cg_env *env, const afunc *f, int idx, int depth, int *w, int *kind)
{
    int j = idx - 1, n = 0, k, i;
    *kind = W_LIMIT;
    while (j >= 0 && idx - j < WIN_MAX) {
        const ainsn *e = &f->ins[j];
        if (e->kind == K_BOUNDARY || e->kind == K_SEH) {
            *kind = e->kind == K_SEH ? W_PROLOGUE : W_OTHER;
            break;
        }
        if (e->kind == K_CALL) {
            /* f(a, g(b)): the result of an inner call is pushed as an argument of this call */
            if (depth < 2 && f->ins[j + 1].push_eax) {
                cg_u32 p = 0, skipped = 0;
                int ok = e->direct ? callee_pop(env, e->target, &p) : estimate_at(env, f, j, depth + 1, &p);
                if (ok) {
                    k = j - 1;
                    while (k >= 0 && skipped < p && idx - k < WIN_MAX) {
                        const ainsn *t = &f->ins[k];
                        if (t->kind == K_BOUNDARY || t->kind == K_SEH || t->kind == K_CALL) break;
                        if (t->kind == K_PUSH) skipped += 4;
                        k--;
                    }
                    if (skipped == p) { j = k; continue; }
                }
            }
            *kind = W_CALL;
            break;
        }
        w[n++] = j--;
    }
    if (j < 0 && f->from_start) *kind = W_FUNC;
    for (i = 0; i < n / 2; i++) { k = w[i]; w[i] = w[n - 1 - i]; w[n - 1 - i] = k; }
    return n;
}

static int estimate_at(const cg_env *env, const afunc *f, int idx, int depth, cg_u32 *out)
{
    int w[WIN_MAX], n, kind, k, m, lead;
    long bytes = 0;
    n = arg_window(env, f, idx, depth, w, &kind);
    lead = kind == W_FUNC || kind == W_PROLOGUE;
    for (k = 0; k < n; k++) {
        const ainsn *e = &f->ins[w[k]];
        if (lead && e->kind == K_PUSH && is_saved(e->push_reg))
            continue;                                   /* prologue: callee-saved registers */
        lead = 0;
        if (e->kind == K_PUSH && is_saved(e->push_reg)) {
            int overwritten = 0;
            for (m = k + 1; m < n; m++) if (f->ins[w[m]].wreg == e->push_reg) { overwritten = 1; break; }
            if (overwritten) {                          /* a register save, not an argument */
                while (k + 1 < n && f->ins[w[k + 1]].kind == K_PUSH && is_saved(f->ins[w[k + 1]].push_reg)) k++;
                bytes = 0;
                continue;
            }
        }
        if (e->kind == K_PUSH) bytes += 4;
        else if (e->kind == K_POP) bytes -= 4;
        else if (e->kind == K_SUBESP) {
            int arg_space = 0;
            for (m = k + 1; m < n && m <= k + 2; m++)
                if (f->ins[w[m]].refs_esp || f->ins[w[m]].mov_from_esp) arg_space = 1;
            if (arg_space) bytes += e->sub_imm;
            else { bytes = 0; lead = 1; }               /* locals of the prologue */
        } else if (e->mov_ebp_esp) { bytes = 0; lead = 1; }
    }
    *out = bytes > 0 ? (cg_u32)bytes : 0;
    return 1;
}

/* Decodes the function containing call_site from its start; keeps the last RING instructions. */
static int load_function(const cg_env *env, cg_u32 call_site, afunc *f)
{
    static ainsn ring[RING];
    cg_u32 start, a, total, i;
    cg_u8 b;
    int tries = 0;
    for (start = call_site & ~15u; start > env->text_begin && call_site - start < 0x10000 && tries < 256;
         start -= 16) {
        if (!read_code(env, start - 1, &b, 1) || b != 0xCC) continue;
        tries++;
        total = 0;
        for (a = start; a < call_site; a += ring[(total - 1) % RING].len) {
            if (!summarize(env, a, &ring[total % RING])) break;
            total++;
        }
        if (a != call_site) continue;                   /* did not land on the call: try further back */
        f->count = total < RING ? (int)total : RING;
        f->from_start = total <= RING;
        for (i = 0; i < (cg_u32)f->count; i++)
            f->ins[i] = ring[(total - f->count + i) % RING];
        return summarize(env, call_site, &f->ins[f->count]);
    }
    return 0;
}

int cg_call_arg_bytes(const cg_env *env, cg_u32 call_site, cg_u32 *bytes)
{
    static afunc f;
    ainsn call, next;
    *bytes = 0;
    if (!g_ready || !summarize(env, call_site, &call) || call.kind != K_CALL)
        return 0;
    if (summarize(env, call_site + call.len, &next) && next.add_esp)
        return 1;                                       /* __cdecl: the caller cleans up */
    if (!load_function(env, call_site, &f))
        return 0;
    if (!estimate_at(env, &f, f.count, 0, bytes))
        return 0;
    if (*bytes > 128) { *bytes = 0; return 0; }         /* implausible: do not guess */
    return 1;
}

cg_verdict cg_decide(const cg_env *env, cg_u32 access, cg_u32 fault_addr,
                     cg_regs *regs, cg_result *out)
{
    out->verdict = CG_FOREIGN;
    out->kind = CG_KIND_NONE;
    out->insn_len = 0;
    out->call_site = 0;
    out->arg_bytes = 0;
    out->arg_known = 0;
    out->zeroed = 0;
    out->reason = "";
    if (!g_ready)
        return finish(out, CG_FOREIGN, "decoder not initialised");
    if (in_text(env, regs->eip))
        return decide_in_text(env, access, fault_addr, regs, out);
    return decide_bad_call(env, access, fault_addr, regs, out);
}
