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

;Включение линии A20 через Fast A20 Gate (порт 0x92).
;Работает на всех системах с 1993 года, включая Geode GX1 (CS5530).
;Классический путь через KBC (порт 0x64) НЕ используем: на Geode GX1
;контроллера клавиатуры в классическом виде нет, цикл ожидания
;готовности входного буфера зависает навсегда.

BITS 16

a20_init:
    in  al, 0x92
    test al, 0x02          ;A20 уже включён?
    jnz .on
    or  al, 0x02
    and al, 0xFE           ;не трогаем бит 0 (Fast Reset)
    out 0x92, al
.on:
    ret

a20_enable:
    jmp a20_init
