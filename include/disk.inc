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

;Ожидание AX тиков BIOS (ячейка 0x0040:0x006C, ~18.2 Гц, ~55 мс на тик).
;Время не зависит от частоты процессора, в отличие от пустых циклов.
;Требует включённых прерываний (sti), все регистры сохраняет
wait_ticks:
    push ax
    push bx
    push cx
    push ds
    mov cx, ax          ;сколько тиков ждать
    mov bx, 0x0040
    mov ds, bx
    mov bx, [0x006C]    ;стартовый тик
.wt_loop:
    mov ax, [0x006C]
    sub ax, bx          ;Word-арифметика: перенос через 0xFFFF не страшен
    cmp ax, cx
    jb .wt_loop
    pop ds
    pop cx
    pop bx
    pop ax
    ret

;Чтение одного сектора по LBA
;AX = LBA, ES:BX = destination
;На реальном FDC (в отличие от USB-FDD и эмуляторов) BIOS иногда даёт
;разовый AH=0x80 "привод не отвечает": не готов мотор, потерялось IRQ,
;залип контроллер. Поэтому до 3 попыток с BIOS-сбросом между ними
disk_read_sector:
    push ax
    push bx
    push cx
    push dx
    ; LBA -> CHS
    xor dx, dx
    div word [cs:disk_spt]      ;AX = LBA / SPT, DX = LBA % SPT
    inc dx                   ;сектор (1-based)
    mov cl, dl               ;сохраняем сектор
    xor dx, dx
    div word [cs:disk_heads]    ;AX = цилиндр, DX = головка
    mov dh, dl               ;головка
    mov ch, al               ;младшие 8 бит цилиндра
    mov al, ah               ;старшие 2 бита цилиндра
    and al, 3
    shl al, 6
    or cl, al                ;объединяем с сектором
    mov si, 3               ;счётчик попыток (SI внутри процедуры свободен)
.try:
    mov dl, [cs:drive]
    mov ax, 0x0201          ;AH=02 read, AL=1 сектор
    push cx
    push dx
    int 0x13
    pop dx                  ;DH = головка (DL переустановим ниже)
    pop cx
    jnc .ok
    dec si
    jz .err
    xor ax, ax              ;AH=00 — BIOS-сброс диска и повтор
    mov dl, [cs:drive]
    int 0x13
    jmp .try
.err:
    stc
.ok:                        ;успех: CF=0 уже выставлен самим int 13h
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
