BITS 32

SCREEN_WIDTH equ 1024
SCREEN_HEIGHT equ 768
BPP equ 3

; ============================================================
; Вывод строки
; ebx = X
; ecx = Y
; esi = строка
; ============================================================

draw_string:
    pusha

.next:
    mov al,[esi]
    test al,al
    jz .done
    movzx eax,al
    push esi
    push ebx
    call draw_char
    pop ebx
    pop esi
    add ebx,8
    inc esi
    jmp .next

.done:
    popa

    ret


; ============================================================
; Рисование символа 8x8
;
; eax = код символа
; ebx = X
; ecx = Y
; ============================================================

draw_char:

; адрес glyph
    mov esi,font_data
    shl eax,3
    add esi,eax
; адрес framebuffer
    mov edi,[lfb_addr]
; Y * pitch
    mov eax,ecx
    mul dword [pitch]
    add edi,eax
; X * 3
    mov eax,ebx
	mov edx,3
	push eax
	mov eax,[bytes_pp]
	cmp eax,3
	je .ton2
	mov edx,4

.ton2:
	pop eax
    mul edx
    add edi,eax
    mov ebp,8

.row:
    mov al,[esi]
    push edi
    mov edx,8

.bit:
    test al,80h
    jz .skip
	push eax

	;jmp .bgr32_3

	mov eax,[bytes_pp]
	cmp eax,3
	je .bgr24_3
	cmp eax,4
	je .bgr32_3

.bgr24_3:
    mov byte [edi],0FFh
    mov byte [edi+1],0FFh
    mov byte [edi+2],0FFh
    jmp .bgr_next_3

.bgr32_3:
    mov dword [edi],0xFFFFFFFF

.bgr_next_3:
	pop eax

.skip:
    add edi,0x03
	push eax
	mov eax,[bytes_pp]
	cmp eax,3
	je .ton
	add edi,0x01

.ton:
	pop eax
	shl al,1
    dec edx
    jnz .bit
    pop edi
    add edi,[pitch]
    inc esi
    dec ebp
    jnz .row

    ret


; -------------------------------------------------
; Fill screen blue
; -------------------------------------------------

fill_blue:
	pusha
	mov edi,[lfb_addr]
    mov ebx,[pitch]
    mov edx,SCREEN_HEIGHT

.y_fill:
    push edx
    mov eax,SCREEN_WIDTH
    mov esi,edi

.x_fill:
	push eax
	mov eax,[bytes_pp]
	cmp eax,3
	je .bgr24

	cmp eax,4
	je .bgr32

.bgr24:
    mov byte [edi],0xFF
    mov byte [edi+1],0
    mov byte [edi+2],0
    add edi,3
    jmp .bgr_next

.bgr32:
    mov dword [edi],0x000000FF
    add edi,4

.bgr_next:
	pop eax
    dec eax
    jnz .x_fill
    pop edx
    add esi,ebx
    mov edi,esi
    dec edx
    jnz .y_fill
	popa

	ret


; -------------------------------------------------
; Inner black rectangle
; -------------------------------------------------

inner_black:
	push edi
	push esi
	mov ebx,[pitch]
    mov edi,[lfb_addr]
    mov eax,10
    mul ebx
    add edi,eax
    add edi,30
    mov edx,748

.y_inner:
    push edx
    mov eax,1004
    mov esi,edi

.x_inner:
	push eax
	mov eax,[bytes_pp]
	cmp eax,3
	je .bgr24_2
	cmp eax,4
	je .bgr32_2

.bgr24_2:
    mov byte [edi],0
    mov byte [edi+1],0
    mov byte [edi+2],0
    add edi,3
    jmp .bgr_next_2

.bgr32_2:
    mov dword [edi],0
    add edi,4

.bgr_next_2:
	pop eax
    dec eax
    jnz .x_inner
    pop edx
    add esi,ebx
    mov edi,esi
    dec edx
    jnz .y_inner
	pop esi
	pop edi

	ret
