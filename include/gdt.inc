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

;GDT

BITS 16

align 8

gdt_start:
    ;Null descriptor
    dq 0x0000000000000000

    ;Code descriptor (Selector 0x08)
    ;Base=0, Limit=4GB, Present=1, DPL=0, Type=Code (Exec+Read), 32-bit
    dq 0x00CF9A000000FFFF

    ;Data descriptor (Selector 0x10)
    ;Base=0, Limit=4GB, Present=1, DPL=0, Type=Data (Read/Write), Expand-up
    dq 0x00CF92000000FFFF

gdt_end:

gdtr:
    dw gdt_end - gdt_start - 1  ;Limit
    dd gdt_start                ;Base
