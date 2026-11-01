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
    xor cx, cx              ; счётчик
    mov edx, 0x534D4150

.next_entry:
    push cx                 ; ← сохраняем счётчик ДО mov ecx
    mov eax, 0xE820
    mov ecx, 20
    push di
    int 0x15
    pop di
    pop cx                  ; ← восстанавливаем счётчик ПОСЛЕ
    jc .done
    cmp eax, 0x534D4150
    jne .done

    ; Защита от «финальной» пустой записи (base=0, length=0)
    cmp dword [es:di+8], 0  ; length low
    jne .store
    cmp dword [es:di+4], 0  ; base high
    je .done                ; и base=0, и length=0 → конец списка

.store:
    inc cx
    add di, 24
    test ebx, ebx
    jnz .next_entry

.done:
    ; Сохраняем СЧЁТЧИК в память до popa
    mov [cs:e820_count], cx
    pop di
    pop es
    popa
    mov cx, [cs:e820_count] ; ← возвращаем счётчик в CX для вызывающего
    test cx, cx
    jz .fail
    clc
    ret
.fail:
    xor cx, cx
    stc
    ret

e820_count: dw 0
