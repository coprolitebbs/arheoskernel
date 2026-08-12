BITS 16
ORG 0x7C00

jmp short start
nop

; ---- BPB для 720K ----
OEMLabel          db "MYOS    "
BytesPerSector    dw 512
SectorsPerCluster db 2          ; <-- 2 для 720K
ReservedSectors   dw 1
NumberOfFATs      db 2
RootEntries       dw 224
TotalSectors      dw 1440
MediaDescriptor   db 0xF9
SectorsPerFAT     dw 3          ; <-- 3 для 720K
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

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [drive], dl

    call disk_init

; ---- Чтение FAT (LBA=1, 3 сектора) ----
    mov ax, 0x9000
    mov es, ax
    xor bx, bx
    mov ax, 1
    mov cx, 3
    call disk_read_sectors
    jc disk_error
    mov al, 'F'
    call print_char

; ---- Чтение корневого каталога (LBA=7, 14 секторов) ----
    mov ax, 0x8000
    mov es, ax
    xor bx, bx
    mov ax, 7
    mov cx, 14
    call disk_read_sectors
    jc disk_error
    mov al, 'R'
    call print_char

; ---- Поиск F12.LD ----
    mov ax, 0x8000
    mov es, ax
    xor bx, bx
    mov cx, 224
search:
    push cx
    push bx
    mov al, [es:bx]
    cmp al, 0
    je not_found
    cmp al, 0xE5
    je skip_entry
    mov si, filename
    mov di, bx
    mov cx, 11
    repe cmpsb
    pop bx
    pop cx
    je found
skip_entry:
    pop bx
    pop cx
    add bx, 32
    loop search
    jmp not_found

not_found:
    mov al, 'N'
    call print_char
    jmp $

found:
    mov ax, [es:bx + 26]
    mov [cluster], ax
    mov al, 'X'
    call print_char

; ---- Преобразование кластера в LBA ----
; data_start = 21, SectorsPerCluster = 2
    mov ax, [cluster]
    sub ax, 2
    mov bx, 2
    mul bx
    add ax, 21
    mov [sector], ax

; ---- Загрузка F12.LD (16 секторов = 8192 байт) ----
    xor ax, ax
    mov es, ax
    mov bx, 0x7E00
    mov ax, [sector]
    mov cx, 16
    call disk_read_sectors
    jc disk_error
    mov al, 'L'
    call print_char
    mov al, 'S'
    call print_char
    jmp 0x0000:0x7E00

disk_error:
    mov al, 'E'
    call print_char
    jmp $

print_char:
    mov ah, 0x0E
    int 0x10
    ret

; ---- Данные ----
drive   db 0
filename db "F12     LD "
cluster dw 0
sector  dw 0
disk_lba dw 0
disk_spt dw 9
disk_heads dw 2

; ---- Включаем disk.inc (он должен быть адаптирован для 720K) ----
%include "include/disk.inc"



times 510-($-$$) db 0
dw 0xAA55
