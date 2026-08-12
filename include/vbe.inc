BITS 16

; =================================================
; VBE error
; =================================================

vbe_fail:
    mov si,msg_err
    call print_16
    jmp $


; =================================================
; print_16
; =================================================

print_16:
    pusha
    mov ah,0x0E

.loop:
    lodsb
    test al,al
    jz .done
    int 10h
    jmp .loop

.done:
    popa

    ret


; =================================================
; Data
; =================================================

msg_err: db "VBE error!",13,10,0

mode_info: times 256 db 0
lfb_addr: dd 0
pitch: dd 0
bytes_pp: dd 0
bpp: db 0


; =================================================
; VBE Init
; =================================================

vbe_init:
	push es
	push di
	push eax

	mov ax,0x4F01
    mov cx,0x4118
    mov di,mode_info

    int 0x10

    cmp ax,0x004F
    jne vbe_fail

; Читаем bits_per_pixel и вычисляем bytes_per_pixel
    movzx eax, byte [mode_info + 0x19]
    mov [bpp], al
    xor edx, edx
    movzx ecx, al
    add ecx, 7
    shr ecx, 3
	movzx eax,cl
	mov [bytes_pp],eax

; Сохраняем LFB и pitch
    mov eax, [mode_info + 0x28]
    mov [lfb_addr], eax
    movzx eax,word [mode_info+0x10]
	mov [pitch],eax

	mov ax,0x4F02
    mov bx,0x4118
    or bx,0x4000

    int 0x10

    cmp ax,0x004F
    jne vbe_fail

	pop eax
	pop di
	pop es

	ret

; =================================================
; A20 Init
; =================================================

a20_init:
	in al,0x92
    or al,2
    out 0x92,al

	ret


BITS 16
