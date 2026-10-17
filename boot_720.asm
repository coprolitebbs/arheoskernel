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

; ---- BPB для 720K ----
OEMLabel          db "MYOS    "
BytesPerSector    dw 512
SectorsPerCluster db 2
ReservedSectors   dw 1
NumberOfFATs      db 2
RootEntries       dw 224
TotalSectors      dw 1440
MediaDescriptor   db 0xF9
SectorsPerFAT     dw 3
SectorsPerTrack   dw 9
Heads             dw 2
HiddenSectors     dd 0
TotalSectorsBig   dd 0

DriveNumber       db 0
Unused            db 0
ExtendedBootSig   db 0x29
VolumeSerial      dd 0x12345678
VolumeLabel       db "MYOS      "
FileSystem        db "FAT12   "

; Макрос отладки — BIOS INT 10h, работает на реальном железе
%macro DEBUG 1
    push ax
    push bx
    push dx
    mov al, %1
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop dx
    pop bx
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

    ; --- Форсированный motor-on + spin-up ---
    mov dx, 0x3F2           ; DOR
    in  al, dx
    or  al, 0x1C            ; motor0 on, DMA+IRQ, no reset
    and al, 0xFC            ; drive 0, no reset
    out dx, al

    ;Грубая задержка ~100-250 мс (для старта мотора)
    ;Работает при 100-500 МГц, реальное время зависит от CPU.
    mov bx, 200
.delay_outer:
    xor dx, dx
.delay_inner:
    dec dx
    jnz .delay_inner
    dec bx
    jnz .delay_outer
    ; 200 × 65536 ≈ 13 000 000 итераций dec+jnz
    ; ≈ 260 мс @ 100 МГц, ≈ 130 мс @ 200 МГц, ≈ 78 мс @ 333 МГц

    ;Теперь reset через BIOS
    mov ah, 0x00
    mov dl, [cs:drive]
    int 0x13
    jc disk_error

    ; --- Ещё немного подождать после reset ---
    mov cx, 0
.reset_delay:
    loop .reset_delay

    DEBUG 'I'

    ; ---- Инициализация диска ----
    ;mov ah, 0x00
    ;mov dl, [drive]
    ;int 0x13
    ;jc disk_error
    ;DEBUG 'I'

    ; ---- Чтение FAT (LBA=1, 3 сектора) в 0x9000:0 ----
    mov ax, 0x9000
    mov es, ax
    xor bx, bx
    mov ax, 1
    mov cx, 3
    call disk_read_sectors
    jc disk_error
    DEBUG 'F'

    ; ---- Чтение корневого каталога (LBA=7, 14 секторов) в 0x8000:0 ----
    mov ax, 0x8000
    mov es, ax
    xor bx, bx
    mov ax, 7
    mov cx, 14
    call disk_read_sectors
    jc disk_error
    DEBUG 'R'

    ; ---- Поиск F12.LD ----
    mov ax, 0x8000
    mov es, ax
    xor bx, bx
    mov cx, 224

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
    DEBUG 'X'

    ; ---- Кластер -> LBA: LBA = (cluster - 2) * spc + 21 ----
    mov ax, [cluster]
    sub ax, 2
    mov bx, 2               ; 720K: sectors_per_cluster = 2
    mul bx
    add ax, 21              ; data_start = 1 + 2*3 + 14
    mov [sector], ax

    ; ---- Загрузка F12.LD (16 секторов = 8192 байт) в 0x7E00 ----
    xor ax, ax
    mov es, ax
    mov bx, 0x7E00
    mov ax, [sector]
    mov cx, 16
    call disk_read_sectors
    jc disk_error
    DEBUG 'L'
    DEBUG 'S'

    mov dl, [drive]
    jmp 0x0000:0x7E00

disk_error:
    DEBUG 'E'
    jmp $

; ---- Данные ----
drive   db 0
filename db "F12     LD "
cluster dw 0
sector  dw 0
disk_lba   dw 0
disk_spt   dw 9         ; 720K
disk_heads dw 2

%include "include/disk.inc"

times 510-($-$$) db 0
dw 0xAA55
