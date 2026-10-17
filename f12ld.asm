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

; f12ld.asm — Загрузчик Stage 2 ядра операционной системы для файловой системы FAT12
BITS 16
ORG 0x7E00

start:
    ;аппаратная фиксация стека реального режима
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x9C00
    sti

    mov [cs:drive], dl

    ;Инициализация A20
    call a20_init
    call a20_enable

    ;Инициализация контроллера диска
    call disk_reset
    jc error

    ;Чтение и разбор BPB загрузочного сектора
    pusha
    call bpb_init
    jc error
    popa

    ;Чтение таблицы FAT (в буфер 0x9000:0x0000)
    push es
    mov ax, [cs:rsvd]
    mov cx, [cs:fatsz]
    mov bx, 0x9000
    mov es, bx
    xor bx, bx
    call disk_read_sectors
    pop es
    jc error

    call disk_reset

    ;Чтение корневого каталога (в буфер 0x8000:0x0000)
    push es
    mov ax, [cs:root_start]
    mov cx, [cs:root_size]
    mov bx, 0x8000
    mov es, bx
    xor bx, bx
    call disk_read_sectors
    pop es
    jc error

    call disk_reset

    ;Загрузка драйверов
    push es

    ;Загрузка Framebuffer драйвера
    mov si, fb_driver_name
    mov ax, 0x3000
    mov es, ax
    xor bx, bx
    call fat12_load_file
    jc error
    mov eax, [cs:current_file_size]
    mov [cs:fb_driver_size], eax

    call disk_reset

    ;Загрузка драйвера FAT12
    mov si, fat12_driver_name
    mov ax, 0x4000
    mov es, ax
    xor bx, bx
    call fat12_load_file
    jc error
    mov eax, [cs:current_file_size]
    mov [cs:fat12_driver_size], eax

    call disk_reset

    ;Загрузка драйвера EXT2
    mov si, ext2_driver_name
    mov ax, 0x5000
    mov es, ax
    xor bx, bx
    call fat12_load_file
    jc error
    mov eax, [cs:current_file_size]
    mov [cs:ext2_driver_size], eax

    pop es
    call disk_reset

    ;Получение карты памяти через E820
    push es
    push di
    xor ax, ax
    mov es, ax
    mov di, 0x6002
    call e820_detect
    mov word [0x6000], cx
    pop di
    pop es

    ;Поиск ядра в корневом каталоге
    push es
    mov bx, 0x8000
    mov es, bx
    xor bx, bx
    mov cx, [cs:root_ent]

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
    mov si, kernel_name
    mov di, bx
    mov cx, 11
    cld
    repe cmpsb
    pop di
    pop si
    pop cx
    jz .found

.next:
    add bx, 32
    dec cx
    jmp .search_loop

.nf:
    jmp not_found

.found:
    mov ax, [es:bx + 26]
    mov [cs:cluster], ax
    mov eax, [es:bx + 28]
    mov [cs:kernel_size_pm], eax
    pop es

    ;Загрузка ядра прямо в 0x100000
    call fat12_load_kernel_high
    jc error

    call vbe_init

    ;Восстанавливаем дефолтные сегменты для сборщиков метаданных
    xor ax, ax
    mov ds, ax
    mov es, ax

    ;Формирование структур для ядра (пишут по адресу 0x9000)
    call build_fs_info
    call build_modules_info
    call build_boot_info
;call dump_kernel



    ;Переход в protected mode
    cli

    ;Корректируем линейный базовый адрес GDT в структуре gdtr
    mov eax, [cs:gdtr+2]
    cmp eax, 0x7E00
    jae .final_gdt_ok
    add eax, 0x7E00
    mov [cs:gdtr+2], eax
.final_gdt_ok:

    lgdt [cs:gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

bpb_init:
    push es
    push bx
    push ax
    push cx
    push dx
    push si

    xor ax, ax
    mov es, ax
    mov bx, 0x0600
    xor ax, ax          ;LBA загрузочного сектора = 0
    mov cx, 1
    call disk_read_sectors
    jc .fail

    mov si, 0x0600

    mov ax, [cs:si+11]      ;BPB_BytsPerSec
    mov [cs:bps], ax

    xor ax, ax
    mov al, [cs:si+13]      ;BPB_SecPerClus
    mov [cs:spc], ax

    mov ax, [cs:si+14]      ;BPB_RsvdSecCnt
    mov [cs:rsvd], ax

    xor ax, ax
    mov al, [cs:si+16]      ;BPB_NumFATs
    mov [cs:nfats], ax

    mov ax, [cs:si+17]      ;BPB_RootEntCnt
    mov [cs:root_ent], ax

    mov ax, [cs:si+22]      ;BPB_FATSz16
    mov [cs:fatsz], ax

    mov ax, [cs:si+24]      ;BPB_SecPerTrk
    mov [cs:spt], ax

    mov ax, [cs:si+26]      ;BPB_NumHeads
    mov [cs:heads], ax

    ;root_start = rsvd + nfats*fatsz
    mov ax, [cs:nfats]
    mul word [cs:fatsz]
    add ax, [cs:rsvd]
    mov [cs:root_start], ax

    ;root_size = ceil( root_ent*32 / bps )
    mov ax, [cs:root_ent]
    mov bx, 32
    mul bx               ;dx:ax = root_ent*32
    mov cx, [cs:bps]
    add ax, cx
    adc dx, 0
    sub ax, 1
    sbb dx, 0
    div cx               ;ax = ceil(root_ent*32 / bps)
    mov [cs:root_size], ax

    ;data_start = root_start + root_size
    mov ax, [cs:root_start]
    add ax, [cs:root_size]
    mov [cs:data_start], ax

    clc
.fail:
    pop si
    pop dx
    pop cx
    pop ax
    pop bx
    pop es
    ret

not_found:
    pop es
    mov si, msg_not_found
    call print_string
    jmp halt

error:
    mov si, msg_error
    call print_string
    jmp halt

.fatal_halt:
    mov al, '!'
    call debug_char
    jmp halt

BITS 32
pm_entry:
    ;Инициализация 32-битных сегментов данных flat-модели
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000            ;Стек ядра

    cli

    ;Аргумент для ядра в EBX
    mov ebx, BOOTINFO_ADDR

    ;Передаем управление ядру операционной системы
    jmp 0x08:0x100000

halt32:
    cli
    hlt
    jmp halt32

BITS 16
halt:
    cli
.loop:
    hlt
    jmp .loop

global bps, spc, rsvd, nfats, root_ent, fatsz, spt, heads
global root_start, root_size, data_start, drive, cluster

;Переменные загрузчика
kernel_size_pm       dd 0       ;32-битный размер файла ядра
kernel_inode         dw 0
fb_inode             dw 0
fat12_inode          dw 0
ext2_inode           dw 0
drive                db 0
cluster              dw 0
sector               dw 0
file_size            dw 0       ;Сохранено для совместимости

;Имена файлов
kernel_name           db "KERNEL     "
loader_name           db "F12.LD",0
fb_driver_name        db "FB_DR   DRV"
fb_driver_filename    db "FB_DR.DRV",0
fat12_driver_name     db "F12_DR  DRV"
fat12_driver_filename db "F12_DR.DRV",0
ext2_driver_name      db "EXT2_DR DRV"
ext2_driver_filename  db "EXT2_DR.DRV",0

;Геометрия диска BPB
bps         dw 512
spc         dw 2
rsvd        dw 1
nfats       dw 2
root_ent    dw 224
fatsz       dw 3
spt         dw 9
heads       dw 2

root_start  dw 0
root_size   dw 0
data_start  dw 0

;Сообщения отладки
msg_not_found   db "KERNEL not found", 0
msg_error       db "Disk error", 0
msg_found_name  db "Found: ", 0
msg_kcp         db "Kernel copied", 0
msg_bpb         db "BPB: ", 0
msg_delimiter   db " : ", 0

%include "include/bootinfo_builder.inc"
%include "include/debug.inc"
%include "include/fat12_stage2.inc"
%include "include/vbe.inc"
%include "include/a20.inc"
%include "include/gdt.inc"
%include "include/graphics.inc"
%include "include/font.inc"
%include "include/e820.inc"
%include "include/fat12_bootinfo.inc"
%include "include/bootinfo.inc"

;Выравнивание и заполнение до фиксированного размера Stage 2 (8192 байт)
times 8192-($-$$) db 0
