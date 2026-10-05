option casemap:none
EXTERN Hook_Direct3DCreate9:PROC
EXTERN Hook_Direct3DCreate9On12:PROC
EXTERN ResolveD3D9Export:PROC

.code

; -----------------------------------------------------------------------------
; FORWARD
;
; Save the volatile integer argument registers and XMM0-XMM3.
;
; Windows x64 requires:
;   - 32 bytes shadow space for a called function
;   - 16-byte stack alignment before CALL
;
; On entry RSP is 8 mod 16 because the caller's return address is present.
; Subtracting 0A8h leaves RSP aligned for our resolver call.
;
; Resolver return address is saved at [rsp+20h].
; After restoring the original arguments, we JMP directly to the real D3D9
; export. The original caller's return address remains untouched.
; -----------------------------------------------------------------------------

FORWARD MACRO ordinal:req

Export_&ordinal PROC

    sub rsp, 0A8h

    ; Preserve integer/pointer arguments.
    mov [rsp+28h], rcx
    mov [rsp+30h], rdx
    mov [rsp+38h], r8
    mov [rsp+40h], r9

    ; Preserve floating/vector argument registers as well.
    movdqu XMMWORD PTR [rsp+50h], xmm0
    movdqu XMMWORD PTR [rsp+60h], xmm1
    movdqu XMMWORD PTR [rsp+70h], xmm2
    movdqu XMMWORD PTR [rsp+80h], xmm3

    ; Ask C++ for the real function address.
    mov ecx, ordinal
    call ResolveD3D9Export

    ; Keep target while restoring original argument registers.
    mov [rsp+48h], rax

    mov rcx, [rsp+28h]
    mov rdx, [rsp+30h]
    mov r8,  [rsp+38h]
    mov r9,  [rsp+40h]

    movdqu xmm0, XMMWORD PTR [rsp+50h]
    movdqu xmm1, XMMWORD PTR [rsp+60h]
    movdqu xmm2, XMMWORD PTR [rsp+70h]
    movdqu xmm3, XMMWORD PTR [rsp+80h]

    mov rax, [rsp+48h]

    add rsp, 0A8h

    ; Tail-call the real D3D9 function.
    jmp rax

Export_&ordinal ENDP

ENDM


FORWARD 16
FORWARD 17
FORWARD 18
FORWARD 19

Export_20 PROC
    jmp Hook_Direct3DCreate9On12
Export_20 ENDP

FORWARD 21
FORWARD 22
FORWARD 23
FORWARD 24
FORWARD 25
FORWARD 26
FORWARD 27
FORWARD 28
FORWARD 29
FORWARD 30
FORWARD 31
FORWARD 32
FORWARD 33
FORWARD 34
FORWARD 35
FORWARD 36
Export_37 PROC
    jmp Hook_Direct3DCreate9
Export_37 ENDP
FORWARD 38

END