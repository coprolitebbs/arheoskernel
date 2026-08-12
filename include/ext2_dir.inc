; ============================================================
; ext2_dir.inc
;
; EXT2 find file
;
; ============================================================

BITS 16
DIR_SEG equ 0x1600

; ============================================================
; ext2_find_file
;
; IN:
;
; DS:SI = ASCIIZ filename
;
; OUT:
;
; AX = inode
;
; CF=0 found
; CF=1 not found
;
; ============================================================

ext2_find_file:
    push bx
    push cx
    push dx
    push di
    push si
    push ds
    push es

    mov ax,2
    call ext2_inode_to_group
    call ext2_get_group_inode_table

    ; сохраняем имя поиска
    mov [cs:search_name_off],si
    mov [cs:search_name_seg],ds

    ; вычисляем длину искомого имени
    push si
    xor cx,cx

.calc_name_len:
    cmp byte [si],0
    je .name_len_done
    inc cx
    inc si
    jmp .calc_name_len

.name_len_done:
    mov [cs:search_name_len],cx
    pop si

    ; читаем inode 2 (root directory)
    mov ax,2
    call ext2_read_inode
    jc .inode_error

    ; inode buffer
    mov ax,INODE_SEG
    mov ds,ax
    mov si,[cs:inode_offset]
    call ext2_read_root_inode_blocks
    jc .dir_error

    ; ------------------------------------------------
    ; DS = DIR_SEG
    ; ES = сегмент имени
    ; ------------------------------------------------

    mov ax,DIR_SEG
    mov ds,ax
    mov ax,[cs:search_name_seg]
    mov es,ax
    mov di,[cs:search_name_off]
    xor dx,dx
    mov [cs:dir_current_block],dx

.next_dir_block:
    mov dx,[cs:dir_current_block]
    cmp dx,[cs:dir_block_count]
    jae .not_found
    mov ax,dx
    call mul1024
    mov si,ax       ; offset блока внутри DIR_SEG

.next_entry:
    ; inode
    mov ax,[si]
    mov dx,[si+2]
    or ax,dx
    jnz .have_inode

    ; удалённая запись — пропускаем её
    mov bx,[si+4]
    test bx,bx
    jz .next_block

    add si,bx
    jmp .next_entry

.have_inode:
    ; rec_len
    mov bx,[si+4]
    test bx,bx
    jz .not_found
    mov [cs:dir_entry_rec_len],bx
    ; name length в каталоге
    xor cx,cx
    mov cl,[si+6]

    ; длины разные - пропускаем запись
    cmp cx,[cs:search_name_len]
    jne .skip_entry

    ; длины разные - пропускаем запись
    cmp cx,[cs:search_name_len]
    jne .skip_entry

    ; сохраняем начало entry
    push si

    ; имя начинается +8
    add si,8
    mov cx,[cs:search_name_len]

.compare:
    cmp cx,0
    je .found
    mov al,[si]
    cmp al,[es:di]
    jne .different
    inc si
    inc di
    dec cx
    jmp .compare

.found:
    pop si
    mov ax,[si]
    clc
    jmp .exit

.different:
    pop si
    mov bx,[cs:dir_entry_rec_len]
    add si,bx
    mov ax,[cs:search_name_seg]
    mov es,ax
    mov di,[cs:search_name_off]
    jmp .next_entry

.skip_entry:
    mov bx,[cs:dir_entry_rec_len]
    add si,bx
    mov ax,[cs:search_name_seg]
    mov es,ax
    mov di,[cs:search_name_off]
    jmp .next_entry

.not_found:
    stc
    jmp .exit

.next_block:
    inc word [cs:dir_current_block]
    mov ax,[cs:search_name_seg]
    mov es,ax
    mov di,[cs:search_name_off]
    jmp .next_dir_block

.error:
    push ds
    xor ax,ax
    mov ds,ax
    mov si,msg_find_inode_read_error
    call print_debug
    pop ds
    stc
    jmp .exit

.inode_error:
    mov si,msg_inode_read_failed
    call print_debug
    stc
    jmp .exit

.dir_error:
    mov si,msg_dir_read_failed
    call print_debug
    stc
    jmp .exit

.exit:
    pop es
    pop ds
    pop si
    pop di
    pop dx
    pop cx
    pop bx

    ret


; ============================================================
; ext2_read_root_inode_blocks
;
; Читает все direct blocks inode root directory
;
; inode_offset должен указывать на inode root
;
; OUT:
;
; DIR_SEG содержит первый блок каталога
;
; ============================================================

ext2_read_root_inode_blocks:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    mov ax,INODE_SEG
    mov ds,ax
    mov si,[cs:inode_offset]
    ; i_block[0]
    add si,40
    xor di,di              ; индекс direct block

.next_block:
    cmp di,12
    jae .done
    ; получить inode->block[di]
    mov bx,di
    shl bx,1
    shl bx,1               ; di*4
    mov ax,[si+bx]
    test ax,ax
    jz .done
    ; сохранить block number
    push ax
    ; вычислить buffer offset
    ; BX = di * 1024
    mov ax,di
    call mul1024
    mov bx,ax
    ; ES:BX = DIR_SEG:offset
    mov ax,DIR_SEG
    mov es,ax
    ; восстановить block number
    pop ax
    ; block -> LBA
    ; block size 1024
    cmp word [cs:ext2_block_size],1024
    jne .error
    shl ax,1
    xor dx,dx
    mov cx,2
    call ext2_read_sectors
    jc .error
    inc di
    jmp .next_block

.done:
    mov [cs:dir_block_count],di
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax

    clc

    ret

.error:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax

    stc

    ret


; AX = block index
; OUT AX = block index * 1024

mul1024:
    push cx
    mov cl,10
    shl ax,cl
    pop cx

    ret


; ============================================================
; variables
; ============================================================
;search_name dw 0
search_name_off dw 0
search_name_seg dw 0
;dir_block_index dw 0
;dir_buffer_seg dw DIR_SEG
dir_block_count dw 0
dir_current_block dw 0
search_name_len dw 0
dir_entry_rec_len dw 0

; ============================================================
; messages
; ============================================================
;msg_kernel_found db "KERNEL INODE: ",0
;msg_dir_lba db "DIR LBA: ",0
msg_kernel_not_found db "KERNEL NOT FOUND",13,10,0
msg_dir_error db "DIR ERROR",13,10,0
;msg_root_block db "ROOT BLOCK: ",0

msg_find_inode_read_error db "FIND INTERNAL ERROR",13,10,0
;msg_inode_ok_find db "ROOT INODE OK",13,10,0
msg_inode_read_failed db "ROOT INODE READ FAILED",13,10,0
;msg_dir_read_ok db "DIR READ OK",13,10,0
msg_dir_read_failed db "DIR READ FAILED",13,10,0
;msg_root_block_find db "ROOT DIR BLOCK: ",0
msg_dir_entry db "ENTRY: ",0
