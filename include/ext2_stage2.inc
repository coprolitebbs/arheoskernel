; ============================================================
; ext2_stage2.inc
;
; EXT2 superblock reader
;
; ============================================================

BITS 16
SUPER_SEG equ 0x1000

; ============================================================
; EXT2 INIT
;
; Читает superblock
;
; EXT2 superblock:
; offset 1024 bytes
; LBA 2
;
; ============================================================

ext2_init:
    mov ax,SUPER_SEG
    mov es,ax
    xor bx,bx
    mov ax,2
    xor dx,dx
    mov cx,2
    call ext2_read_sectors
    jc .error
    call ext2_parse_super
    ;call dump_ext2_info
    clc

    ret

.error:
    mov si,msg_ext2_error
    call print_debug
    stc

    ret

; ============================================================
; Parse superblock
;
; DS = SUPER_SEG during reading
;
; ============================================================

ext2_parse_super:
    push ax
    push bx
    push ds
    mov ax,SUPER_SEG
    mov ds,ax
; first data block offset 20
    mov ax,[20]
    mov [cs:ext2_first_data_block],ax

; block size
; offset 24
; 1024 << value

    mov ax,[24]
    cmp ax,0
    je .block_1024
    cmp ax,1
    je .block_2048
    cmp ax,2
    je .block_4096

.block_1024:
    mov word [cs:ext2_block_size],1024
    jmp .groups

.block_2048:
    mov word [cs:ext2_block_size],2048
    jmp .groups

.block_4096:
    mov word [cs:ext2_block_size],4096

.groups:

; blocks per group
; offset 32
    mov ax,[32]
    mov [cs:ext2_blocks_per_group],ax
; inodes per group
; offset 40
    mov ax,[40]
    mov [cs:ext2_inodes_per_group],ax
; inode size
; offset 88
    mov ax,[88]
    mov [cs:ext2_inode_size],ax
; magic check
; offset 56
; EF53
    mov ax,[56]
    cmp ax,0EF53h
    jne .bad_magic
    pop ds
    pop bx
    pop ax

    ret

.bad_magic:
    mov si,msg_bad_magic
    call print_debug
    pop ds
    pop bx
    pop ax
    stc

    ret


; ============================================================
; Variables
; ============================================================

ext2_block_size dw 1024
ext2_inode_size dw 128
ext2_inodes_per_group dw 16
ext2_blocks_per_group dw 8192
ext2_first_data_block dw 1

; ============================================================
; Messages
; ============================================================

msg_ext2_error db "EXT2 ERROR",13,10,0
msg_bad_magic db "BAD EXT2 MAGIC",13,10,0

