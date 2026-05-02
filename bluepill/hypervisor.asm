.code

EXTERN SetupVmcsThunk:PROC
EXTERN CppVmExitDispatcher:PROC

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

    test rax, rax
    jz Abort

    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r12
    pop r13
    pop r14
    pop r15

    vmlaunch

    ; if vmlaunch fails, we fall through and return FALSE.
    xor rax, rax
    ret

Abort:
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r12
    pop r13
    pop r14
    pop r15

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

    mov rcx, rsp 

    sub rsp, 256
    movups [rsp + 0F0h], xmm15
    movups [rsp + 0E0h], xmm14
    movups [rsp + 0D0h], xmm13
    movups [rsp + 0C0h], xmm12
    movups [rsp + 0B0h], xmm11
    movups [rsp + 0A0h], xmm10
    movups [rsp + 090h], xmm9
    movups [rsp + 080h], xmm8
    movups [rsp + 070h], xmm7
    movups [rsp + 060h], xmm6
    movups [rsp + 050h], xmm5
    movups [rsp + 040h], xmm4
    movups [rsp + 030h], xmm3
    movups [rsp + 020h], xmm2
    movups [rsp + 010h], xmm1
    movups [rsp + 000h], xmm0

    sub rsp, 28h
    call CppVmExitDispatcher
    add rsp, 28h

    movups xmm0, [rsp + 000h]
    movups xmm1, [rsp + 010h]
    movups xmm2, [rsp + 020h]
    movups xmm3, [rsp + 030h]
    movups xmm4, [rsp + 040h]
    movups xmm5, [rsp + 050h]
    movups xmm6, [rsp + 060h]
    movups xmm7, [rsp + 070h]
    movups xmm8, [rsp + 080h]
    movups xmm9, [rsp + 090h]
    movups xmm10, [rsp + 0A0h]
    movups xmm11, [rsp + 0B0h]
    movups xmm12, [rsp + 0C0h]
    movups xmm13, [rsp + 0D0h]
    movups xmm14, [rsp + 0E0h]
    movups xmm15, [rsp + 0F0h]
    add rsp, 256

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

    vmresume
    
    jmp $ ; infinite loop for debugging purposes - as we are not supposed to reach this code

AsmVmExitHandler ENDP

END
