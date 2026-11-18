# ChaseGuard - code caves appended to the end of .text of the base build
# License: GPL-3.0 (see ../LICENSE.txt)
    .intel_syntax noprefix
    .text
    .globl cP, cA, cB, cC, cD, cE, nulstr, cend

# ---- P: early filter - only handle an AV if EIP or [ESP] is inside speed.exe ----
cP:
    mov   eax, [ecx+0xc]            # ExceptionAddress
    sub   eax, 0x401000
    cmp   eax, 0xBFF000
    jb    1f
    mov   eax, [edi+4]              # CONTEXT*
    mov   eax, [eax+0xc4]           # Esp
    mov   eax, [eax]                # [Esp] (return address)
    sub   eax, 0x401000
    cmp   eax, 0xBFF000
    jae   RET0                      # foreign exception -> EXCEPTION_CONTINUE_SEARCH
1:
    mov   eax, [edi+4]              # (overwritten original instruction 1)
    push  dword ptr [ecx+0xc]       # (overwritten original instruction 2)
    jmp   P_AFTER

# ---- A: "the instruction itself faults" path - only if EIP is inside speed.exe ----
cA:
    mov   eax, [edi]
    mov   ecx, [eax+0xc]
    sub   ecx, 0x401000
    cmp   ecx, 0xBFF000
    jae   RET0
    mov   ecx, 1
    jmp   A_AFTER

# ---- B: "call through a bad pointer" path - only if the return address is inside speed.exe ----
cB:
    mov   ecx, esi
    sub   ecx, 0x401000
    cmp   ecx, 0xBFF000
    jae   RET0
    push  0x1c
    lea   eax, [esp+0x14]
    jmp   B_AFTER

# ---- C: do not decode at EIP if the page is not executable (avoids a nested fault inside Zydis) ----
cC:
    xor   edx, edx
    test  byte ptr [esp+0x24], 0xF0
    jz    B_START
    mov   eax, [edi]
    push  1
    mov   ecx, [eax+0xc]
    jmp   C_AFTER

# ---- D: after a failed instruction check / VirtualProtect: show the box and EXIT without installing the handler ----
cD:
    push  0
    call  dword ptr ds:IAT_MSGBOX
    pop   edi
    pop   esi
    pop   ebx
    jmp   RET_TRUE

# ---- E: if the log file cannot be opened, log to NUL but still apply the patches ----
cE:
    push  offset MODE_A
    push  offset nulstr
    call  dword ptr ds:IAT_FOPEN
    add   esp, 8
    mov   dword ptr ds:G_FILE, eax
    test  eax, eax
    jz    RET_TRUE
    jmp   DLL_CONT
nulstr:
    .asciz "NUL"
cend:
