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

BITS 16
INODE_SEG equ 0x1400

ext2_read_inode:
    pusha
    push ds
    push es

    mov cx, ax
    dec cx

    push cx
    mov ax, cx
    xor dx, dx
    mov bx, 8192
    div bx
    mov word [cs:inode_group_tmp], ax
    pop cx

    mov ax, word [cs:inode_group_tmp]
    xor dx, dx
    mov bx, 32
    mul bx
    mov si, ax

    mov ax, GDT_SEG
    mov ds, ax
    mov bx, si
    add bx, 8
    mov ax, [bx]
    mov dx, [bx+2]
    mov word [cs:bg_inode_table_tmp], ax

    mov ax, cx
    xor dx, dx
    mov bx, 8192
    div bx
    mov ax, dx

    xor dx, dx
    mov bx, 128
    mul bx

    mov cx, 1024
    div cx

    mov [cs:inode_offset], dx

    mov bp, sp
    mov [bp+6], dx

    mov cx, ax
    mov ax, word [cs:bg_inode_table_tmp]
    add ax, cx
    adc dx, 0

    mov bx, 2
    xor dx, dx
    mul bx

    mov si, ax
    mov di, dx

    mov ax, INODE_SEG
    mov es, ax
    xor bx, bx

    mov cx, 2

    mov ax, si
    mov dx, di

    call ext2_read_sectors
    jc .error

    clc
    jmp .exit

.error:
    mov al, '&'
    call debug_char
    stc

.exit:
    pop es
    pop ds
    popa
    ret

;Variables

inode_group_tmp: dw 0
bg_inode_table_tmp: dw 0
