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

;Включение линии A20 и проверка, что она реально включена
;
;ПОРЯДОК (a20_enable):
;  1. wrap-тест: сравниваем [0xFFFF:0x0010] с [0x0000:0x0000].
;     A20 on  -> это физически разные адреса (0x100000 vs 0x0).
;     A20 off -> второй алиасится на первый: записанный паттерн виден в 0x0000.
;         Паттерн = ~исходное значение, поэтому ложное совпадение исключено.
;  2. если A20 off — слепая запись 0x02 в порт 0x92: часть чипсетов декодирует порт 0x92
;         только на запись и отвечает 0xFF на чтение
;  3. снова тест; затем int 15h AX=2401; снова тест.
;  4. не помогло — явное сообщение 'A20!' и halt.
;
;Классический путь через KBC (порт 0x64) не используем: на Geode GX1
;контроллера клавиатуры в классическом виде нет, цикл ожидания
;готовности входного буфера зависает навсегда

BITS 16

; ------------------------------------------------------------
; a20_test: CF=0 — A20 включена (0x100000 НЕ алиасится на 0x0).
; Портит AL/BX. Восстанавливает байт IVT[0], если трогали.
; ------------------------------------------------------------
a20_test:
        push ds
        push fs
        push bx
        xor ax, ax
        mov ds, ax
        mov ax, 0xFFFF
        mov fs, ax
        mov bl, [ds:0x0000]     ;исходный байт IVT[0]
        mov bh, bl
        not bh                  ;паттерн ~BL
        mov [fs:0x0010], bh     ;A20 off -> ляжет в 0x0000
        mov al, [ds:0x0000]
        cmp al, bh
        je .a20_off             ;записанный паттерн виден в нуле -> off
        clc
        jmp .a20_ret
.a20_off:
        stc
.a20_ret:
        mov [ds:0x0000], bl     ;вернуть байт IVT[0] (безвредно при on)
        pop bx
        pop fs
        pop ds
        ret

; ------------------------------------------------------------
; a20_init: быстрый путь через порт 0x92 (Fast A20 Gate).
; На чипсетах, отвечающих на 0x92 нулём бита 1 при выключенной
; A20, реально включает. При чтении 0xFF ничего не делает —
; дальше разбирается a20_enable через wrap-тест.
; ------------------------------------------------------------
a20_init:
    in  al, 0x92
    test al, 0x02          ;A20 уже включён?
    jnz .on
    or  al, 0x02
    and al, 0xFE           ;не трогаем бит 0 (Fast Reset)
    out 0x92, al
.on:
    ret

; ------------------------------------------------------------
; a20_enable: включить и ДОКАЗАТЬ, что A20 включена.
; CF=0 при успехе; иначе — сообщение и halt (грузиться нельзя).
; ------------------------------------------------------------
a20_enable:
    call a20_test
    jnc .a20_ok
    ; слепая запись в 0x92 (без чтения: декод может быть write-only)
    mov al, 0x02
    out 0x92, al
    out 0x80, al
    out 0x80, al
    call a20_test
    jnc .a20_ok
    ; попытка через BIOS
    mov ax, 0x2401
    int 0x15
    call a20_test
    jnc .a20_ok
    ; A20 мертва: копировать ядро выше 1 МБ нельзя — явный диагноз
    pusha
    mov al, 'A'
    call debug_char
    mov al, '2'
    call debug_char
    mov al, '0'
    call debug_char
    mov al, '!'
    call debug_char
    popa
    jmp halt
.a20_ok:
    ret
