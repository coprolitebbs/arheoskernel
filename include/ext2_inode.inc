; ============================================================
; ext2_inode.inc
;
; Read EXT2 inode
;
; IN:
;
; AX = inode number
;
; OUT:
;
; ES:BX = inode data
;
; ============================================================

BITS 16
INODE_SEG equ 0x1400

; ============================================================
; ext2_read_inode
;
; AX = inode number
;
; Reads inode block into INODE_SEG:0000
;
; ============================================================

ext2_read_inode:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds
    push es
;сохранить inode number
    mov [cs:inode_number],ax
	xor ax,ax
    mov ds,ax
    mov ax,[cs:inode_number]
    dec ax
    ; --------------------------------
    ; byte offset
    ; --------------------------------
	xor dx,dx
    mov bx,[cs:ext2_inode_size]
	mul bx
    ; --------------------------------
    ; block inside inode table
    ; --------------------------------
	xor dx,dx
    mov bx,[cs:ext2_block_size]
	div bx
; сохранить offset внутри блока
	mov [cs:inode_offset],dx
; AX = block inside inode table
; добавить inode table block
	xor dx,dx
	;mov ax,[cs:ext2_inode_table]
	;call debug_hex_word
	mov ax,[cs:ext2_inode_table+2]
	;call debug_hex_word
	;call newline

	add ax,[cs:ext2_inode_table]
	adc dx,[cs:ext2_inode_table+2]

	;push ax
	;push dx
	;mov si,msg_inode_table_used
	;call print_debug
	;mov ax,[cs:ext2_inode_table]
	;call debug_hex_word
	;mov ax,[cs:ext2_inode_table+2]
	;call debug_hex_word
	;call newline
	;pop dx
	;pop ax
; тольтко блок 1024
    cmp word [cs:ext2_block_size],1024
    jne .bad_block
; block -> LBA
    shl ax,1
    rcl dx,1
    mov [cs:tmp_lba_low],ax
    mov [cs:tmp_lba_high],dx
; read inode block
    mov ax,INODE_SEG
    mov es,ax
    xor bx,bx
    mov cx,2
    mov ax,[cs:tmp_lba_low]
    mov dx,[cs:tmp_lba_high]
    call ext2_read_sectors
    jc .error
; parse inode
    clc
    jmp .exit

.bad_block:
    mov si,msg_bad_block
    call print_debug
    stc
    jmp .exit

.error:
    push ds
    xor ax,ax
    mov ds,ax
    mov si,msg_inode_error
    call print_debug
    pop ds
    stc

.exit:
    pop es
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax

    ret

; ============================================================
; TEST READ EXT2 BLOCK
;
; AX = ext2 block number
;
; reads block into INODE_SEG
;
; ============================================================

ext2_read_block_test:
    push ax
    push bx
    push cx
    push dx
    cmp word [cs:ext2_block_size],1024
    jne .error
    shl ax,1
    xor dx,dx
    push ax
    push dx
    mov ax,INODE_SEG
    mov es,ax
    xor bx,bx
    mov cx,2
    pop dx
    pop ax
    call ext2_read_sectors
    jc .error
    clc
    pop dx
    pop cx
    pop bx
    pop ax

    ret

.error:
    stc
    pop dx
    pop cx
    pop bx
    pop ax

    ret

; ============================================================
; Variables
; ============================================================

ext2_inode_table: dd 0
inode_number: dw 0
inode_offset: dw 0

; ============================================================
; Messages
; ============================================================
msg_inode_error db "INODE ERROR",13,10,0
;msg_inode_lba db "INODE LBA: ",0
msg_bad_block: db "msg_bad_block: ", 0
;msg_inode_table_used db "INODE TABLE USED: ",0
