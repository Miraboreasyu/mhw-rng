; ============================================================================
;  MHW Universal RNG Microkernel v3.0 — x64 Entry Register-Capture Thunks
;  Preserves all 8 non-volatile GPRs + entry RSP/RIP into MhwRngCallerRegs
;  before forwarding to C++ `HookImpl_*` detours without touching arg regs.
; ============================================================================

EXTERN HookImpl_MtRandomCtorSeed:PROC
EXTERN HookImpl_MtRandomCtorDefault:PROC
EXTERN HookImpl_MtRandomSrand:PROC
EXTERN HookImpl_MtRandomNrand:PROC
EXTERN HookImpl_MtRandomSphere:PROC
EXTERN HookImpl_StdRandomDevice:PROC
EXTERN HookImpl_Mt19937Range:PROC
EXTERN HookImpl_Mt19937HeapInit:PROC
EXTERN HookImpl_Mt19937HeapDraw:PROC
EXTERN HookImpl_CrtRand:PROC

.CODE

; Stack frame layout after `sub rsp, 88h`:
;   [rsp+00h..rsp+1Fh] : 32B shadow space for C++ callee
;   [rsp+20h..rsp+77h] : MhwRngCallerRegs (88 bytes = 11 QWORDs):
;     +00h (rsp+20h) rbx
;     +08h (rsp+28h) rbp
;     +10h (rsp+30h) rsi
;     +18h (rsp+38h) rdi
;     +20h (rsp+40h) r12
;     +28h (rsp+48h) r13
;     +30h (rsp+50h) r14
;     +38h (rsp+58h) r15
;     +40h (rsp+60h) callerRsp (`lea rax, [rsp+88h]`)
;     +48h (rsp+68h) callerRip (`[callerRsp]`)
;     +50h (rsp+70h) reserved (0)
;   [rsp+78h..rsp+87h] : 16B alignment pad (entry RSP % 16 == 8 -> sub 88h => 16B aligned)

RNG_DISPATCH MACRO TargetFn, RegDest
    sub rsp, 88h
    .allocstack 88h
    .endprolog
    mov qword ptr [rsp+20h], rbx
    mov qword ptr [rsp+28h], rbp
    mov qword ptr [rsp+30h], rsi
    mov qword ptr [rsp+38h], rdi
    mov qword ptr [rsp+40h], r12
    mov qword ptr [rsp+48h], r13
    mov qword ptr [rsp+50h], r14
    mov qword ptr [rsp+58h], r15
    lea rax, [rsp+88h]
    mov qword ptr [rsp+60h], rax
    mov rax, qword ptr [rax]
    mov qword ptr [rsp+68h], rax
    xor eax, eax
    mov qword ptr [rsp+70h], rax
    lea RegDest, [rsp+20h]
    call TargetFn
    add rsp, 88h
    ret
ENDM

; 2-arg: (rcx = self, edx = seed, r8 = regs)
MhwRngAsm_MtRandomCtorSeed PROC FRAME
    RNG_DISPATCH HookImpl_MtRandomCtorSeed, r8
MhwRngAsm_MtRandomCtorSeed ENDP

; 1-arg: (rcx = self, rdx = regs)
MhwRngAsm_MtRandomCtorDefault PROC FRAME
    RNG_DISPATCH HookImpl_MtRandomCtorDefault, rdx
MhwRngAsm_MtRandomCtorDefault ENDP

; 2-arg: (rcx = self, edx = seed, r8 = regs)
MhwRngAsm_MtRandomSrand PROC FRAME
    RNG_DISPATCH HookImpl_MtRandomSrand, r8
MhwRngAsm_MtRandomSrand ENDP

; 1-arg: (rcx = self, rdx = regs)
MhwRngAsm_MtRandomNrand PROC FRAME
    RNG_DISPATCH HookImpl_MtRandomNrand, rdx
MhwRngAsm_MtRandomNrand ENDP

; 2-arg: (rcx = self, rdx = outVec3, r8 = regs)
MhwRngAsm_MtRandomSphere PROC FRAME
    RNG_DISPATCH HookImpl_MtRandomSphere, r8
MhwRngAsm_MtRandomSphere ENDP

; 0-arg: (rcx = regs)
MhwRngAsm_StdRandomDevice PROC FRAME
    RNG_DISPATCH HookImpl_StdRandomDevice, rcx
MhwRngAsm_StdRandomDevice ENDP

; 3-arg float: (rcx = ctx, xmm1 = minVal, xmm2 = maxVal, r9 = regs)
MhwRngAsm_Mt19937Range PROC FRAME
    RNG_DISPATCH HookImpl_Mt19937Range, r9
MhwRngAsm_Mt19937Range ENDP

; 2-arg: (rcx = state, edx = seed, r8 = regs)
MhwRngAsm_Mt19937HeapInit PROC FRAME
    RNG_DISPATCH HookImpl_Mt19937HeapInit, r8
MhwRngAsm_Mt19937HeapInit ENDP

; 1-arg: (rcx = state, rdx = regs)
MhwRngAsm_Mt19937HeapDraw PROC FRAME
    RNG_DISPATCH HookImpl_Mt19937HeapDraw, rdx
MhwRngAsm_Mt19937HeapDraw ENDP

; 0-arg: (rcx = regs)
MhwRngAsm_CrtRand PROC FRAME
    RNG_DISPATCH HookImpl_CrtRand, rcx
MhwRngAsm_CrtRand ENDP

END
