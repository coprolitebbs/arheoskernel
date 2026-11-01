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

; BIOS disk access layer

BITS 16

; Инициализация диска
disk_init:
    push ax
    push dx
    mov ah, 0x00
    int 0x13
    pop dx
    pop ax
    ret

;Чтение одного сектора по LBA
;AX = LBA, ES:BX = destination
disk_read_sector:
    push ax
    push bx
    push cx
    push dx
    mov [cs:disk_lba], ax
    ; LBA -> CHS
    xor dx, dx
    div word [cs:disk_spt]      ;AX = LBA / SPT, DX = LBA % SPT
    inc dx                   ;сектор (1-based)
    mov cl, dl               ;сохраняем сектор
    xor dx, dx
    div word [cs:disk_heads]    ;AX = (LBA/SPT)/HEADS = цилиндр, DX = (LBA/SPT)%HEADS = головка
    mov dh, dl               ;головка
    mov ch, al               ;младшие 8 бит цилиндра
    mov al, ah               ;старшие 2 бита цилиндра
    and al, 3
    shl al, 6
    or cl, al                ;объединяем с сектором
    mov dl, [cs:drive]
    mov ah, 0x02
    mov al, 1
    int 0x13
    pop dx
    pop cx
    pop bx
    pop ax

    ret

;Чтение нескольких секторов
;AX = первый LBA, CX = количество, ES:BX = destination
disk_read_sectors:
    pusha

.read_loop:
    push ax
    push cx
    call disk_read_sector
    pop cx
    pop ax
    jc .error
    add bx, 512
    inc ax
    loop .read_loop
    popa
    clc

    ret

.error:
    popa
    stc

    ret

