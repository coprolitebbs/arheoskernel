
;Загрузка файла с Indirect Blocks

BITS 16

KERNEL_SEG   equ 0x2000
INDIRECT_SEG equ 0x1800

ext2_load_file:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov word [cs:file_block_index], 0
    mov byte [cs:file_indirect_loaded], 0

    mov [cs:file_inode], ax
    mov word [cs:file_load_offset], 0
    mov word [cs:file_load_offset+2], 0
    mov ax, [cs:file_load_segment]
    mov [cs:file_base_seg], ax

    mov ax, [cs:file_inode]
    call ext2_read_inode
    jc .error

    mov ax, INODE_SEG
    mov ds, ax
    mov si, [cs:inode_offset]
    mov [cs:file_inode_offset], si

    mov ax, [si+4]
    mov [cs:file_size], ax
    mov ax, [si+6]
    mov [cs:file_size+2], ax
    mov byte [cs:file_indirect_loaded], 0

.next:
    mov ax, [cs:file_load_offset]
    mov dx, [cs:file_load_offset+2]
    cmp dx, [cs:file_size+2]
    jb .read
    cmp ax, [cs:file_size]
    jae .done

.read:
    call ext2_get_file_block
    test ax, ax
    jz .done

    ;блок на диске -> сектор (LBA AX = block*2)
    xor dx, dx
    shl ax, 1
    rcl dx, 1

    ;назначение: file_base_seg + (смещение внутри файла >> 4).
    ;Файл грузится с начала блока (смещение кратно 1024), поэтому старшие
    ;биты смещения всегда нулевые и adc не нужен.
    mov bx, [cs:file_load_offset]
    shr bx, 1
    shr bx, 1
    shr bx, 1
    shr bx, 1                       ;BX = смещение внутри файла >> 4
    add bx, [cs:file_base_seg]      ;BX = unreal-сегмент назначения
    mov es, bx                      ;16-битная загрузка сохраняет high word ES
    xor bx, bx                      ;адресация выровнена по 16 байт

    mov cx, 2
    call ext2_read_sectors
    jc .error

    add word [cs:file_load_offset], 1024
    adc word [cs:file_load_offset+2], 0
    inc word [cs:file_block_index]
    jmp .next

.done:
    ;восстановить unreal-состояние ES вызывающего: после чтения PM-прокси
    ;high word ES обнулён; low word тот же, поэтому повторная загрузка того
    ;же селектора восстанавливает полный 32-битный дескриптор.
    mov ax, [cs:file_load_segment]
    mov es, ax
    clc
    jmp .exit

.error:
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

ext2_get_file_block:
    push bx
    push dx
    push si
    push ds
    push es

    mov si, [cs:file_inode_offset]
    mov ax, [cs:file_block_index]

    cmp ax, 12
    jae .single
    mov bx, ax
    shl bx, 1
    shl bx, 1
    add bx, si
    add bx, 40
    mov ax, [bx]
    jmp .exit

.single:
    cmp byte [cs:file_indirect_loaded], 0
    jne .ready

    mov bx, si
    add bx, 88
    mov ax, [bx]
    test ax, ax
    jz .zero
    mov [cs:file_indirect_block], ax

    xor dx, dx
    shl ax, 1
    rcl dx, 1
    push ax
    mov ax, INDIRECT_SEG            ; 0x1800 -> линейный 0x18000 (< 1MB)
    mov es, ax
    pop ax
    xor bx, bx
    mov cx, 2
    call ext2_read_sectors
    jc .zero
    mov byte [cs:file_indirect_loaded], 1

.ready:
    mov ax, [cs:file_block_index]
    sub ax, 12
    shl ax, 1
    shl ax, 1
    mov bx, ax

    mov ax, INDIRECT_SEG
    mov ds, ax
    mov ax, [bx]
    jmp .exit

.zero:
    xor ax, ax

.exit:
    pop es
    pop ds
    pop si
    pop dx
    pop bx
    ret

;Variables

file_inode          dw 0
file_load_offset    dd 0
file_inode_offset   dw 0
file_block_index    dw 0
file_load_segment   dw 0       ;unreal-сегмент базы файла (phys >> 4)
file_base_seg       dw 0       ;то же, что file_load_segment (для аддинга)
file_indirect_loaded db 0
file_indirect_block  dd 0

global file_size
file_size:          dd 0
