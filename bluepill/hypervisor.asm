INCLUDE vmcsFields.inc

; ShutdownPath scratch layout. each descriptor slot is padded to 16 bytes so 
; the whole block is a multiple of 16 and stack alignment is preserved.
SHUTDOWN_SCRATCH_SIZE   EQU 48

IDTR_SCRATCH_OFF        EQU 0
GDTR_SCRATCH_OFF        EQU 16
VMCS_ADDRESS_OFF        EQU 32

; layout inside a 10 byte pseudo descriptor, as it's consumed by LGDT / LIDT
DESCRIPTOR_LIMIT_OFF    EQU 0
DESCRIPTOR_BASE_OFF     EQU 2

.code

EXTERN HandleVmresumeFailure:PROC
EXTERN HandleVmxoffFailure:PROC
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
    ; we pass this context pointer to CppVmExitDispatcher via rcx.
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
    call CppVmExitDispatcher    ; returns in al if shutdown is requested or not
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

    ; ymm restores don't touch GPRs, so al still has the dispatcher's return value
    test al, al
    jnz ShutdownPath

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

    ; if we reached this line, vmresume failed
    sub rsp, 20h
    call HandleVmresumeFailure
    int 3   ; HandleVmresumeFailure is noreturn, but if it still returns we trap for debugging purposes

ShutdownPath:
    ; these are the guest's original GPRs
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

    ; one scratch block for everything that has to survive past VMXOFF
    sub rsp, SHUTDOWN_SCRATCH_SIZE

    ; rdx holds the VMCS physical address passed to the hypercall (0 if the
    ; VMCS was somehow missing)
    mov qword ptr [rsp + VMCS_ADDRESS_OFF], rdx

    ; reading resume info from this core's VMCS into volatile registers for later use
    mov rcx, GUEST_RIP
    vmread r10, rcx

    mov rcx, GUEST_RSP
    vmread r11, rcx

    mov rcx, GUEST_RFLAGS
    vmread rax, rcx

    ; guest GDTR pseudo descriptor
    mov rcx, GUEST_GDTR_BASE
    vmread rdx, rcx
    mov qword ptr [rsp + GDTR_SCRATCH_OFF + DESCRIPTOR_BASE_OFF], rdx

    mov rcx, GUEST_GDTR_LIMIT
    vmread rdx, rcx
    mov word ptr [rsp + GDTR_SCRATCH_OFF + DESCRIPTOR_LIMIT_OFF], dx

    ; guest IDTR pseudo descriptor. the base already matches what windows had
    ; (it's what we put in HOST_IDTR_BASE), but the limit was forced to 0FFFFh
    ; by the VM exits and no HOST_IDTR_LIMIT field exists that could have held
    ; the real one, so it has to come back out of the VMCS.
    mov rcx, GUEST_IDTR_BASE
    vmread rdx, rcx
    mov qword ptr [rsp + IDTR_SCRATCH_OFF + DESCRIPTOR_BASE_OFF], rdx

    mov rcx, GUEST_IDTR_LIMIT
    vmread rdx, rcx
    mov word ptr [rsp + IDTR_SCRATCH_OFF + DESCRIPTOR_LIMIT_OFF], dx

    ; every vmread is done so the VMCS can be cleared now
    cmp qword ptr [rsp + VMCS_ADDRESS_OFF], 0
    jz SkipVmclear
    vmclear qword ptr [rsp + VMCS_ADDRESS_OFF]

SkipVmclear:
    vmxoff
    jc VmxoffFailed   ; CF=1 means vmxoff failed
    jz VmxoffFailed   ; ZF=1 & CF=0 means the operation is unsupported

    ; restoring windows' descriptor tables before the guest resumes
    lgdt fword ptr [rsp + GDTR_SCRATCH_OFF]
    lidt fword ptr [rsp + IDTR_SCRATCH_OFF]

    ; switching to the guest's stack, restoring rflags, and jumping back to the
    ; instruction after vmcall (which is the ret inside AsmVmcall)
    mov rsp, r11
    push rax
    popfq
    jmp r10

VmxoffFailed:
    sub rsp, 20h
    call HandleVmxoffFailure
    int 3

AsmVmExitHandler ENDP

END
