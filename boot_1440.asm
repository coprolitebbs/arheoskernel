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
ORG 0x7C00

jmp short start
nop

;BPB для 1.44MB
OEMLabel          db "MYOS    "
BytesPerSector    dw 512
SectorsPerCluster db 1
ReservedSectors   dw 1
NumberOfFATs      db 2
RootEntries       dw 224
TotalSectors      dw 2880
MediaDescriptor   db 0xF0
SectorsPerFAT     dw 9
SectorsPerTrack   dw 18
Heads             dw 2
HiddenSectors     dd 0
TotalSectorsBig   dd 0

DriveNumber       db 0
Unused            db 0
ExtendedBootSig   db 0x29
VolumeSerial      dd 0x12345678
VolumeLabel       db "MYOS      "
FileSystem        db "FAT12   "

%macro DEBUG 1
    ;int 10h AH=0E портит AX и BX; BX на всех точках вызова не живой —
    ;сохраняем только AX (экономия байт в boot-секторе)
    push ax
    mov al, %1
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop ax
%endmacro

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    mov [drive], dl
    DEBUG 'S'

    ;Форсированный motor-on + spin-up. DOR пишем КОНСТАНТОЙ: на части
    ;чипсетов (в т.ч. CS5530 на Geode GX1) порт 0x3F2 не читается, и
    ;read-modify-write "in/or/and/out" записывает мусор (все моторы + drive 3)
    mov dx, 0x3F2           ; DOR
    mov al, 0x1C            ; drive 0, RESET# снят, DMA+IRQ разрешены, мотор0
    out dx, al

    ;Ждём раскрутку мотора по тикам BIOS (~55 мс на тик, ~550 мс на 10 тиков).
    ;Не зависит от частоты CPU, в отличие от пустых циклов
    mov ax, 10
    call wait_ticks

    ;Сброс контроллера через BIOS (провал не фатален: чтения в disk.inc
    ;сами делают сброс между попытками)
    mov ah, 0x00
    mov dl, [cs:drive]
    int 0x13

    ;DEBUG 'I'


    ;Вычисляем root_start, root_size, data_start
    xor ax, ax
    mov al, [NumberOfFATs]
    mul word [SectorsPerFAT]
    add ax, [ReservedSectors]
    mov [root_start], ax

    mov ax, [RootEntries]
    shl ax, 5
    add ax, 511
    shr ax, 9
    mov [root_size], ax

    mov ax, [root_start]
    add ax, [root_size]
    mov [data_start], ax

    ;DEBUG 'B'

    ; ---- Инициализация диска ----
    ;mov ah, 0x00
    ;mov dl, [drive]
    ;int 0x13
    ;jc disk_error
    ;DEBUG 'C'

    ; ---- Чтение FAT ----
    mov ax, [ReservedSectors]
    mov cx, [SectorsPerFAT]
    mov bx, 0x9000
    mov es, bx
    xor bx, bx
    call disk_read_sectors
    jc disk_error
    ;DEBUG 'F'

    ; ---- Чтение корневого каталога ----
    mov ax, [root_start]
    mov cx, [root_size]
    mov bx, 0x8000
    mov es, bx
    xor bx, bx
    call disk_read_sectors
    jc disk_error
    ;DEBUG 'R'

    ; ---- Поиск F12.LD ----
    mov ax, 0x8000
    mov es, ax
    xor bx, bx
    mov cx, [RootEntries]

.search_loop:
    test cx, cx
    jz .nf

    mov al, [es:bx]
    test al, al
    jz .nf
    cmp al, 0xE5
    jz .next

    push cx
    push si
    push di
    mov si, filename
    mov di, bx
    mov cx, 11
    cld
    repe cmpsb
    pop di
    pop si
    pop cx
    jz found

.next:
    add bx, 32
    dec cx
    jmp .search_loop

.nf:
    DEBUG 'N'
    jmp $

found:
    mov ax, [es:bx + 26]
    mov [cluster], ax
    ;DEBUG 'X'

    ; ---- Кластер -> LBA: LBA = (cluster - 2) * spc + data_start ----
    mov ax, [cluster]
    sub ax, 2
    mov bx, 1               ; 1.44M: sectors_per_cluster = 1
    mul bx
    add ax, [data_start]
    mov [sector], ax

    ; ---- Загрузка F12.LD (16 секторов = 8192 байт) ----
    xor ax, ax
    mov es, ax
    mov bx, 0x7E00
    mov ax, [sector]
    mov cx, 16
    call disk_read_sectors
    jc disk_error
    DEBUG 'l'
    ;DEBUG 'S'

    mov dl, [drive]
    jmp 0x0000:0x7E00

disk_error:
    DEBUG 'e'
    jmp $

; ---- Данные ----
drive   db 0
filename db "F12     LD "
cluster dw 0
sector  dw 0
root_start dw 0
root_size  dw 0
data_start dw 0

disk_spt   dw 18
disk_heads dw 2

%include "include/disk.inc"

times 510-($-$$) db 0
dw 0xAA55
