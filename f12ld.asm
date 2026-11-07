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
    ;mov sp, 0x9C00
    ;mov sp, 0x7C00
    mov sp, 0xFFF0
    sti

    mov [cs:drive], dl

    ;Инициализация поллингового FDC-драйвера (include/fdc_pio.inc).
    ;BIOS int 13h на TA-NC529/CS5530 зависает в ожидании IRQ6,
    ;поэтому все чтения stage2 идёт мимо BIOS.
    call fdc_drv_init

    ;pusha
    ;mov al, 'T'
    ;call debug_char
    ;popa



    ;Инициализация контроллера диска
    ;call disk_reset
    ;jc error

    ; ---- V: reset прошёл ----
    ;pusha
    ;mov al, 'V'
    ;call debug_char
    ;popa


    ; --- ЯВНО включить мотор, СОХРАНИВ RESET=1 ---
    ;push dx
    ;push ax
    ;mov dx, 0x3F2
    ;in al, dx
    ;or  al, 0x1C           ; bit 2 (RESET)=1, bit 3 (DMA_IRQ)=1,
                           ; bit 4 (MOTOR)=1, bits 0-1 (drive 0)=0
    ;out dx, al
    ;pop ax
    ;pop dx

    ; --- пауза 2000 мс: на Geode GX1 100 МГц это ~2 сек реального времени ---
    ;push bx
    ;push cx
    ;mov bx, 2000
;.fdd_warmup_outer:
    ;xor cx, cx
;.fdd_warmup_inner:
    ;loop .fdd_warmup_inner
    ;dec bx
    ;jnz .fdd_warmup_outer
    ;pop cx
    ;pop bx


    ;Чтение и разбор BPB загрузочного сектора
    pusha
    call bpb_init
    jc error
    popa


    ; ---- W: BPB прочитан ----
    pusha
    mov al, 'W'
    call debug_char
    popa

    ;call vbe_init
    ;Инициализация A20
    call a20_init
    call a20_enable

    ;call disk_reset
    ;jc error

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

    ;pusha
    ;mov al, 'x'
    ;call debug_char
    ;popa


    ;call disk_reset

    pusha
    mov al, 'X'
    call debug_char
    popa

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

    pusha
    mov al, 'Y'
    call debug_char
    popa

    ;call disk_reset

    ;Загрузка драйверов
    push es

    ; --- 0: Framebuffer Driver ---
    mov si, fb_driver_name
    mov ax, [cs:module_next_segment]
    mov [cs:fb_driver_seg], ax
    mov es, ax
    xor bx, bx
    call fat12_load_file
    jc error
    mov eax, [cs:current_file_size]
    mov [cs:fb_driver_size], eax
    call advance_module_segment
    ;call disk_reset

    pusha
    mov al, '0'
    call debug_char
    popa


    ; --- 1: Block Device Driver ---
    mov si, block_driver_name
    mov ax, [cs:module_next_segment]
    mov [cs:block_driver_seg], ax
    mov es, ax
    xor bx, bx
    call fat12_load_file
    jc error
    mov eax, [cs:current_file_size]
    mov [cs:block_driver_size], eax
    call advance_module_segment
   ;call disk_reset

    pusha
    mov al, '1'
    call debug_char
    popa

    ; --- 2: USB Driver ---
    mov si, usb_driver_name
    mov ax, [cs:module_next_segment]
    mov [cs:usb_driver_seg], ax
    mov es, ax
    xor bx, bx
    call fat12_load_file
    jc error
    mov eax, [cs:current_file_size]
    mov [cs:usb_driver_size], eax
    call advance_module_segment
    ;call disk_reset

    pusha
    mov al, '2'
    call debug_char
    popa

    ; --- 3: FAT12 Driver ---
    mov si, fat12_driver_name
    mov ax, [cs:module_next_segment]
    mov [cs:fat12_driver_seg], ax
    mov es, ax
    xor bx, bx
    call fat12_load_file
    jc error
    mov eax, [cs:current_file_size]
    mov [cs:fat12_driver_size], eax
    call advance_module_segment
    ;call disk_reset

    pusha
    mov al, '3'
    call debug_char
    popa

    ; --- 4: EXT2 Driver ---
    mov si, ext2_driver_name
    mov ax, [cs:module_next_segment]
    mov [cs:ext2_driver_seg], ax
    mov es, ax
    xor bx, bx
    call fat12_load_file
    jc error
    mov eax, [cs:current_file_size]
    mov [cs:ext2_driver_size], eax
    call advance_module_segment

    pusha
    mov al, '4'
    call debug_char
    popa

    pop es
    ;call disk_reset

    ;Получение карты памяти через E820
    push es
    push di
    xor ax, ax
    mov es, ax
    mov ds, ax
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

    pusha
    mov al, '$'
    call debug_char
    popa

;call dump_kernel
;jmp halt

    call vbe_init

    pusha
    mov al, '%'
    call debug_char
    popa

    ;Восстанавливаем дефолтные сегменты для сборщиков метаданных
    xor ax, ax
    mov ds, ax
    mov es, ax

    ;Формирование структур для ядра (пишут по адресу 0x9000)
    call build_fs_info
    call build_modules_info
    call build_boot_info

    push es
pusha
mov al, 'C'
call debug_char
popa
    pop es
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

    pusha
    mov al, '>'
    call debug_char
    popa

    ; === Boot sector: свой FDC-драйвер вместо BIOS int 13h ===
    ; BIOS TA-NC529/CS5530 внутри int 13h ждёт IRQ6 без таймаута
    ; и зависает навсегда. disk_read_sector теперь поллинговый.
    xor ax, ax
    mov es, ax
    mov bx, 0x0600
    xor ax, ax              ; LBA 0
    call disk_read_sector
    jc .fail

    mov si, 0x0600
    mov ax, [cs:si+11]
    mov [cs:bps], ax
    xor ax, ax
    mov al, [cs:si+13]
    mov [cs:spc], ax
    mov ax, [cs:si+14]
    mov [cs:rsvd], ax
    xor ax, ax
    mov al, [cs:si+16]
    mov [cs:nfats], ax
    mov ax, [cs:si+17]
    mov [cs:root_ent], ax
    mov ax, [cs:si+22]
    mov [cs:fatsz], ax
    mov ax, [cs:si+24]
    mov [cs:spt], ax
    mov ax, [cs:si+26]
    mov [cs:heads], ax
    ; root_start, root_size, data_start посчитать ниже (как было)
    mov ax, [cs:nfats]
    mul word [cs:fatsz]
    add ax, [cs:rsvd]
    mov [cs:root_start], ax
    mov ax, [cs:root_ent]
    mov bx, 32
    mul bx
    mov cx, [cs:bps]
    add ax, cx
    adc dx, 0
    sub ax, 1
    sbb dx, 0
    div cx
    mov [cs:root_size], ax
    mov ax, [cs:root_start]
    add ax, [cs:root_size]
    mov [cs:data_start], ax
    clc
    jmp .done
.fail:
    stc
.done:
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
    mov al, 'e'
    call debug_char
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

;jmp halt32
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

; Продвигает module_next_segment на ceil(current_file_size/16) + 16 параграфов
; (16 параграфов = 256 байт padding). Проверяет, что не въехали в область FSINFO.
advance_module_segment:
    push eax
    mov eax, [cs:current_file_size]
    add eax, 15
    shr eax, 4                  ; paragraphs = ceil(file_size / 16)
    add eax, 16                 ; padding 256 байт
    add [cs:module_next_segment], eax
    ;add word [cs:module_next_segment], 0x0FFF
    ;and word [cs:module_next_segment], 0xF000

    ; Потолок = начало EBDA/VGA (0xA0000). Оставляем 16 KB запаса
    ; (0x9C00 сегм. = 0x9C000 phys) на случай нестандартно большой EBDA
    cmp word [cs:module_next_segment], 0x9C00
    jb .ok
    mov al, 'S'
    call debug_char
    jmp halt
.ok:
    pop eax
    ret


global bps, spc, rsvd, nfats, root_ent, fatsz, spt, heads
global root_start, root_size, data_start, drive, cluster

;Переменные загрузчика
sector               dw 0
file_size            dw 0       ;Сохранено для совместимости

kernel_size_pm       dd 0
kernel_inode         dw 0
fb_inode             dw 0
block_inode          dw 0
usb_inode          dw 0
fat12_inode          dw 0
ext2_inode           dw 0
drive                db 0
cluster              dw 0

;Динамическое размещение модулей (единица = параграф, 16 байт)
module_next_segment  dd 0x3000       ; стартовый сегмент fb_driver (phys 0x30000)
fb_driver_seg        dw 0
block_driver_seg     dw 0
usb_driver_seg       dw 0
fat12_driver_seg     dw 0
ext2_driver_seg      dw 0

;Имена файлов
kernel_name            db "KERNEL     "
loader_name            db "F12.LD",0
fb_driver_name         db "FB_DR   DRV"
fb_driver_filename     db "FB_DR.DRV",0
fat12_driver_name      db "F12_DR  DRV"
fat12_driver_filename  db "F12_DR.DRV",0
ext2_driver_name       db "EXT2_DR DRV"
ext2_driver_filename   db "EXT2_DR.DRV",0
block_driver_name      db "BLOCK   DRV"
block_driver_filename  db "BLOCK.DRV",0
usb_driver_name        db "USB     DRV"
usb_driver_filename    db "USB.DRV",0

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
%include "include/fdc_pio.inc"
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
%if ($-$$) > 32256
    %error "Ошибка! OSDev: Код hddld.asm превысил лимит 63 секторов!"
%endif

times 32256-($-$$) db 0
