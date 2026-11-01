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

BITS 32

global driver_entry
extern driver_main

section .text


driver_entry:
    jmp start


align 16


driver_header:
    dd 'DRV1'
    dd 1
    dd driver_header_end-driver_header
    dd start-driver_entry
    dd 0
    dd 0
    db "Framebuffer driver",0

driver_header_end:

start:
    ; EBX = boot_info
    push ebx
    call driver_main
    add esp,4

    ret
