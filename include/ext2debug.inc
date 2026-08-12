BITS 16



; ============================================================
; dump_inode
;
; DS:SI -> inode
;
; EXT2 inode:
; +0  mode
; +2  uid
; +4  size low
; +40 block[0]
;
; ============================================================
dump_inode:
    push ax
    push si
    mov ax,[si+0]
    call debug_hex_word
    mov ax,[si+2]
    call debug_hex_word
    mov ax,[si+4]
    call debug_hex_word
    mov ax,[si+40]
    call debug_hex_word
    call newline
    pop si
    pop ax

    ret


; ============================================================
; dump_inode_bytes
;
; DS:SI -> inode
;
; dump first 64 bytes
;
; ============================================================
dump_inode_bytes:
    push ax
    push bx
    push cx
    push si
    xor bx,bx
    mov cx,32

.loop:
    mov ax,[si+bx]
    push bx
    push cx
    push si
    call debug_hex_word
    pop si
    pop cx
    pop bx
    add bx,2
    loop .loop
    call newline
    pop si
    pop cx
    pop bx
    pop ax

    ret

; ============================================================
; dump_kernel
;
; Dump первых 32 байт ядра
;
; ============================================================

dump_kernel:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds
    mov ax,KERNEL_SEG
    mov ds,ax
    xor si,si
    mov cx,32

.next:
    lodsb
    ; ---------- старшая тетрада ----------
    mov ah,al
    shr ah,4
    mov al,ah
    call .print_nibble
    ; ---------- младшая ----------
    mov al,[si-1]
    and al,0Fh
    call .print_nibble
    mov al,' '
    mov ah,0Eh
    int 10h
    ; каждые 16 байт новая строка
    test si,15
    jnz .cont
    mov al,13
    mov ah,0Eh
    int 10h
    mov al,10
    mov ah,0Eh
    int 10h

.cont:
    loop .next
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax

    ret

; ============================================================
; AL = nibble (0..15)
; ============================================================
.print_nibble:
    cmp al,10
    jb .digit
    add al,'A'-10
    jmp .out

.digit:
    add al,'0'

.out:
    mov ah,0Eh
    int 10h

    ret


; ============================================================
; Messages
; ============================================================


