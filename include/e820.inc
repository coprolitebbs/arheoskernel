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

;e820.inc — получение карты памяти через BIOS INT 0x15
;Использовать только в реальном режиме (16-bit)
;Вход: ES:DI = буфер для записей (каждая запись 24 байта)
;Выход: CF=0 успех, CX = количество записей, буфер заполнен
;CF=1 ошибка (E820 не поддерживается или нет записей)

e820_detect:
    pusha
    push es
    push di
    xor ebx, ebx
    xor cx, cx          ;счётчик
    mov edx, 0x534D4150 ;"SMAP"

.next_entry:
    mov eax, 0xE820
    mov ecx, 20         ;размер структуры (базовый E820 — 20 байт)
    push di             ;сохраняем DI (он будет изменён)
    int 0x15
    pop di
    jc .done            ;ошибка или конец
    cmp eax, 0x534D4150 ;проверка сигнатуры
    jne .done
    inc cx
    add di, 24          ;переходим к следующей записи (резервируем 24 байта)
    test ebx, ebx
    jnz .next_entry

.done:
    pop di
    pop es
    popa
    test cx, cx
    jz .fail
    clc
    ret
.fail:
    stc

    ret
