BITS 16
ORG 0x7C00

jmp short start
nop

; ---- BPB для 1.44MB ----
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

; ---------- Макрос для отладки ----------
%macro DEBUG 1
    push ax
    push dx
    mov dx, 0xE9
    mov al, %1
    out dx, al
    pop dx
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

	;DEBUG 'A'

    mov [drive], dl

    ; ---- Устанавливаем параметры для disk.inc ----
    mov word [disk_spt], 18
    mov word [disk_heads], 2

    ; ---- Вычисляем root_start, root_size, data_start ----
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

    DEBUG 'B'

    ; ---- Инициализация диска ----
    mov ah, 0x00
    int 0x13
    jc disk_error

    DEBUG 'C'

    ; ---- Чтение FAT (LBA=1, 9 секторов) ----
    mov ax, [ReservedSectors]       ; LBA=1
    mov cx, [SectorsPerFAT]         ; 9
    mov bx, 0x9000
    mov es, bx
    xor bx, bx
    call disk_read_sectors
    jc disk_error
    DEBUG 'F'

    ; ---- Чтение корневого каталога (LBA=19, 14 секторов) ----
    mov ax, [root_start]            ; 19
    mov cx, [root_size]             ; 14
    mov bx, 0x8000
    mov es, bx
    xor bx, bx
    call disk_read_sectors
    jc disk_error
    DEBUG 'R'

    ; ---- Поиск F12.LD ----
    mov ax, 0x8000
    mov es, ax
    xor bx, bx
    mov cx, [RootEntries]
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
    DEBUG 'N'
    jmp $

found:
    mov ax, [es:bx + 26]
    mov [cluster], ax
    DEBUG 'X'

    ; ---- Преобразование кластера в LBA ----
    ; data_start = root_start + root_size = 19 + 14 = 33
    ; SectorsPerCluster = 1
    mov ax, [cluster]
    sub ax, 2
    mov bx, 1
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
    DEBUG 'L'
    DEBUG 'S'
    jmp 0x0000:0x7E00

disk_error:
    DEBUG 'E'
    jmp $



; ---- Данные ----
drive   db 0
filename db "F12     LD "
cluster dw 0
sector  dw 0
root_start dw 0
root_size  dw 0
data_start dw 0

; ---- ЯВНЫЕ ПАРАМЕТРЫ ДЛЯ disk.inc (переопределяют значения в disk.inc) ----
disk_lba   dw 0
disk_spt   dw 18      ; <-- 18 для 1.44
disk_heads dw 2       ; <-- 2 для 1.44



; ---- Включаем disk.inc (функции чтения) ----
%include "include/disk.inc"

times 510-($-$$) db 0
dw 0xAA55
