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

static int decode(const cg_env *env, cg_u32 addr, cg_u32 maxlen,
                  ZydisDecodedInstruction *insn, ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT])
{
    cg_u8 buf[ZYDIS_MAX_INSTRUCTION_LENGTH];
    if (maxlen > sizeof(buf)) maxlen = sizeof(buf);
    if (maxlen == 0 || !env->read(env->read_user, addr, buf, maxlen))
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
        regs->eip += insn.length;
        regs->eax = 0;
        out->kind = CG_KIND_SKIP_CALL;
        out->call_site = regs->eip - insn.length;
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

    regs->eip = ret;
    regs->esp += 4;
    regs->eax = 0;
    out->kind = CG_KIND_RETURN_BAD_CALL;
    return finish(out, CG_RECOVERED, "call into non-executable memory returned, EAX=0");
}

cg_verdict cg_decide(const cg_env *env, cg_u32 access, cg_u32 fault_addr,
                     cg_regs *regs, cg_result *out)
{
    out->verdict = CG_FOREIGN;
    out->kind = CG_KIND_NONE;
    out->insn_len = 0;
    out->call_site = 0;
    out->zeroed = 0;
    out->reason = "";
    if (!g_ready)
        return finish(out, CG_FOREIGN, "decoder not initialised");
    if (in_text(env, regs->eip))
        return decide_in_text(env, access, fault_addr, regs, out);
    return decide_bad_call(env, access, fault_addr, regs, out);
}
