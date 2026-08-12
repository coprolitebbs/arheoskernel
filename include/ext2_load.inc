BITS 16

KERNEL_SEG equ 0x2000
INDIRECT_SEG equ 0x1800


; ============================================================
; ext2_load_file
;
; Загружает file по inode из file_inode
;
; ============================================================

ext2_load_file:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov word [cs:file_block_index],0
    mov byte [cs:file_indirect_loaded],0
; сохранить inode файла
    mov [cs:file_inode],ax
    mov word [cs:file_load_offset],0
    mov word [cs:file_load_offset+2],0
    mov ax,[cs:file_inode]
    call ext2_read_inode
    jc .error
    mov ax,INODE_SEG
    mov ds,ax
    mov si,[cs:inode_offset]
    mov [cs:file_load_offset],si
; size
    mov ax,[si+4]
    mov [cs:file_size],ax
    mov ax,[si+6]
    mov [cs:file_size+2],ax
    mov byte [cs:file_indirect_loaded],0

.next:
    mov ax,[cs:file_load_offset]
    mov dx,[cs:file_load_offset+2]
    cmp dx,[cs:file_size+2]
    jb .read
    cmp ax,[cs:file_size]
    jae .done

.read:
    call ext2_get_file_block
    test ax,ax
    jz .done
; AX = ext2 block
    xor dx,dx
    shl ax,1
    rcl dx,1
; destination
    mov bx,[cs:file_load_offset]
	push ax
    mov ax,[cs:file_load_segment]
	mov es,ax
	pop ax
    mov cx,bx
    shr cx,4
	push ax
	mov ax,es
	add ax,cx
	mov es,ax
	pop ax
    ;add es,cx
    and bx,0x0F
    mov cx,2
    call ext2_read_sectors
    jc .error
    add word [cs:file_load_offset],1024
    adc word [cs:file_load_offset+2],0
    ;add word [cs:file_block_index],4
    inc word [cs:file_block_index]
    jmp .next

.done:
    mov si,msg_file_loaded
    call print_debug
    clc
    jmp .exit

.error:
    mov si,msg_file_error
    call print_debug
    stc

.exit:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax

    ret


; ============================================================
; ext2_get_file_block
;
; IN:
;   file_block_index
;
; OUT:
;   AX = номер блока
;        0 = конец
;
; ============================================================

ext2_get_file_block:
    push bx
    push dx
    push si
    push ds
    push es

    mov si,[cs:file_inode_offset]
    mov ax,[cs:file_block_index]
; direct blocks 0..11
    cmp ax,12
    jae .single
    mov bx,ax
    shl bx,1
    shl bx,1          ; index * 4
    add bx,si
    add bx,40
    mov ax,[bx]
    jmp .exit

; single indirect

.single:
    cmp byte [cs:file_indirect_loaded],0
    jne .ready
; inode i_block[12]
    mov bx,si
    add bx,88
    mov ax,[bx]
    test ax,ax
    jz .zero
    mov [cs:file_indirect_block],ax
; block -> LBA
    xor dx,dx
    shl ax,1
    rcl dx,1
	push ax
    mov ax,INDIRECT_SEG
	mov es,ax
	pop ax
    xor bx,bx
    mov cx,2
    call ext2_read_sectors
    jc .zero
    mov byte [cs:file_indirect_loaded],1

.ready:
    mov ax,[cs:file_block_index]
    sub ax,12
; индекс * 4
    shl ax,1
    shl ax,1
    mov bx,ax
    mov ax,INDIRECT_SEG
    mov ds,ax
    mov ax,[bx]
    jmp .exit

.zero:
    xor ax,ax

.exit:
    pop es
    pop ds
    pop si
    pop dx
    pop bx

    ret

; ============================================================
; variables
; ============================================================
file_inode       dw 0
file_load_offset dd 0
file_size        dd 0
file_inode_offset dw 0
file_block_index  dw 0
file_load_segment dw 0

; ------------------------------------------------------------
; direct/indirect
; ------------------------------------------------------------

file_indirect_loaded db 0
file_indirect_block  dd 0

; ============================================================
; messages
; ============================================================
;msg_file_block db "FILE BLOCK: ",0
msg_file_loaded db "FILE LOADED",13,10,0
msg_file_error db "FILE LOAD ERROR",13,10,0
;msg_file_lba db "FILE LBA: ",0
msg_file_size_error db "FILE SIZE ERROR",13,10,0
;msg_file_size db "FILE SIZE: ",13,10,0
;msg_space db " ",0
;msg_load_done db "FILE loaded: ",0

