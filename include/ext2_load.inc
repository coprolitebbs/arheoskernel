 ; Copyright (C) 2026 ARHeOS by Dmitry H
 ;
 ; This program is free software: you can redistribute it and/or modify
 ; it under the terms of the GNU General Public License as published by
 ; the Free Software Foundation, either version 3 of the License, or
 ; (at your option) any later version.
 ;
 ; This program is distributed in the hope that it will be useful,
 ; but WITHOUT ANY WARRANTY; without even the implied warranty of
 ; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 ; GNU General Public License for more details.
 ;
 ; You should have received a copy of the GNU General Public License
 ; along with this program. If not, see <https://www.gnu.org/licenses/>.

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

    xor dx, dx
    shl ax, 1
    rcl dx, 1

    mov bx, [cs:file_load_offset]
    push ax
    mov ax, [cs:file_load_segment]
    mov es, ax
    pop ax

    mov cx, bx
    shr cx, 4
    push ax
    mov ax, es
    add ax, cx
    mov es, ax
    pop ax

    and bx, 0x0F
    mov cx, 2
    call ext2_read_sectors
    jc .error

    add word [cs:file_load_offset], 1024
    adc word [cs:file_load_offset+2], 0
    inc word [cs:file_block_index]
    jmp .next

.done:
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
    mov ax, INDIRECT_SEG
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



ext2_load_kernel_high:
    pushad
    push ds
    push es
    push fs
    push gs

    mov word [cs:file_block_index], 0
    mov byte [cs:file_indirect_loaded], 0
    mov dword [cs:kernel_load_offset], 0

    mov edi, 0x100000

    mov ax, INODE_SEG
    mov ds, ax
    mov si, [cs:inode_offset]
    mov [cs:file_inode_offset], si

.block_loop:
    mov eax, [cs:kernel_load_offset]
    cmp eax, [cs:kernel_size_pm]
    jae .done

    mov ax, INODE_SEG
    mov ds, ax

    call ext2_get_file_block
    test ax, ax
    jz .done

    xor dx, dx
    shl ax, 1
    rcl dx, 1

    mov bx, SUPER_SEG
    mov es, bx
    xor bx, bx
    mov cx, 2
    call ext2_read_sectors
    jc .error

    mov eax, [cs:kernel_size_pm]
    sub eax, [cs:kernel_load_offset]
    mov dx, 1024
    cmp eax, 1024
    jae .full_block
    mov dx, ax
.full_block:

    mov ax, SUPER_SEG
    mov es, ax
    xor si, si
    mov cx, dx

    cli
    mov eax, [cs:gdtr+2]
    cmp eax, 0x7E00
    jae .unreal_ok
    add eax, 0x7E00
    mov [cs:gdtr+2], eax
.unreal_ok:
    lgdt [cs:gdtr]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    mov ax, 0x10
    mov gs, ax

    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax

    jmp .unreal_rm_clear
.unreal_rm_clear:
    sti

.copy_byte:
    mov al, [es:si]
    mov [gs:edi], al
    inc si
    inc edi
    dec cx
    jnz .copy_byte

    movzx eax, dx
    add [cs:kernel_load_offset], eax

    inc word [cs:file_block_index]
    jmp .block_loop

.done:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    clc
    ret

.error:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    stc
    ret



;Variables

file_inode          dw 0
file_load_offset    dd 0
file_inode_offset   dw 0
file_block_index    dw 0
file_load_segment   dw 0
file_indirect_loaded db 0
file_indirect_block  dd 0

global file_size
file_size:          dd 0
kernel_load_offset dd 0
