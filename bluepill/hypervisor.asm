.code

EXTERN SetupVmcsThunk:PROC
EXTERN CppVmExitDispatcher:PROC

EXTERN g_ShutdownThisCpu:BYTE

EXTERN g_ShutdownResumeRip:QWORD
EXTERN g_ShutdownGuestRsp:QWORD
EXTERN g_ShutdownGuestRflags:QWORD

AsmVirtualize PROC
    push r15
    push r14
    push r13
    push r12
    push rdi
    push rsi
    push rbp
    push rbx
    
    ; rcx already contains the 'this' pointer passed from Initialize()
    mov rdx, rsp     
    lea r8, GuestResume 

    sub rsp, 28h
    call SetupVmcsThunk 
    add rsp, 28h
    
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r12
    pop r13
    pop r14
    pop r15

    test rax, rax
    jz Abort

    vmlaunch
    
Abort:
    ; if we abort or if vmlaunch fails, we fall through and return FALSE.
    xor rax, rax
    ret
    
GuestResume:
    ; if we reached here vmlaunch has succeeded, so we return TRUE.
    ; we need to pop everything again since we rewinded our stack pointer to the value before the original pops.

    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r12
    pop r13
    pop r14
    pop r15
    mov rax, 1 
    ret
AsmVirtualize ENDP

AsmVmExitHandler PROC
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rbx
    push rdx
    push rcx
    push rax
    
    ; rsp now points directly to the saved rax (top of the saved GPRs).
    ; Pass this context pointer to CppVmExitDispatcher via rcx.
    mov rcx, rsp

    sub rsp, 512
    vmovups [rsp + 1E0h], ymm15
    vmovups [rsp + 1C0h], ymm14
    vmovups [rsp + 1A0h], ymm13
    vmovups [rsp + 180h], ymm12
    vmovups [rsp + 160h], ymm11
    vmovups [rsp + 140h], ymm10
    vmovups [rsp + 120h], ymm9
    vmovups [rsp + 100h], ymm8
    vmovups [rsp + 0E0h], ymm7
    vmovups [rsp + 0C0h], ymm6
    vmovups [rsp + 0A0h], ymm5
    vmovups [rsp + 080h], ymm4
    vmovups [rsp + 060h], ymm3
    vmovups [rsp + 040h], ymm2
    vmovups [rsp + 020h], ymm1
    vmovups [rsp + 000h], ymm0
    
    sub rsp, 28h
    call CppVmExitDispatcher
    add rsp, 28h
    
    vmovups ymm0,  [rsp + 000h]
    vmovups ymm1,  [rsp + 020h]
    vmovups ymm2,  [rsp + 040h]
    vmovups ymm3,  [rsp + 060h]
    vmovups ymm4,  [rsp + 080h]
    vmovups ymm5,  [rsp + 0A0h]
    vmovups ymm6,  [rsp + 0C0h]
    vmovups ymm7,  [rsp + 0E0h]
    vmovups ymm8,  [rsp + 100h]
    vmovups ymm9,  [rsp + 120h]
    vmovups ymm10, [rsp + 140h]
    vmovups ymm11, [rsp + 160h]
    vmovups ymm12, [rsp + 180h]
    vmovups ymm13, [rsp + 1A0h]
    vmovups ymm14, [rsp + 1C0h]
    vmovups ymm15, [rsp + 1E0h]
    add rsp, 512

    pop rax
    pop rcx
    pop rdx
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    cmp byte ptr [g_ShutdownThisCpu], 0
    jne ShutdownPath

    vmresume
    jmp $   ; infinite loop for debugging purposes

ShutdownPath:
    ; clearing the flag for next time so we don't shut down vcpus by accident
    mov byte ptr [g_ShutdownThisCpu], 0

    ; saving the resume state
    mov r10, qword ptr [g_ShutdownResumeRip]
    mov r11, qword ptr [g_ShutdownGuestRsp]
    mov r12, qword ptr [g_ShutdownGuestRflags]

    vmxoff
    jc VmxoffFailed   ; CF=1 means vmxoff failed
    jz VmxoffFailed   ; ZF=1 & CF=0 means the operation is unsupported

    ; switching to guest stack, restoring rflags and jumping back to the instruction after vmcall
    mov rsp, r11
    push r12
    popfq
    jmp r10

VmxoffFailed:
    jmp $ ; infinite loop for debugging purposes
AsmVmExitHandler ENDP

END
