.code

AsmGetCs PROC
    xor rax, rax
    mov ax, cs
    ret
AsmGetCs ENDP

AsmGetDs PROC
    xor rax, rax
    mov ax, ds
    ret
AsmGetDs ENDP

AsmGetEs PROC
    xor rax, rax
    mov ax, es
    ret
AsmGetEs ENDP

AsmGetSs PROC
    xor rax, rax
    mov ax, ss
    ret
AsmGetSs ENDP

AsmGetFs PROC
    xor rax, rax
    mov ax, fs
    ret
AsmGetFs ENDP

AsmGetGs PROC
    xor rax, rax
    mov ax, gs
    ret
AsmGetGs ENDP

AsmGetTr PROC
    xor rax, rax
    str ax
    ret
AsmGetTr ENDP

AsmGetLdtr PROC
    xor rax, rax
    sldt ax
    ret
AsmGetLdtr ENDP

AsmGetGdtr PROC
    sgdt [rcx]
    ret
AsmGetGdtr ENDP

AsmGetIdtr PROC
    sidt [rcx]
    ret
AsmGetIdtr ENDP

AsmInveptAllContexts PROC
    ; invept requires the type in a register (2 = all-context) 
    ; and the descriptor pointer in memory
    mov rax, 2
    invept rax, oword ptr [rcx]
    ret
AsmInveptAllContexts ENDP

END
