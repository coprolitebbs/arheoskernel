;рабочая версия загрузчика Stage 2

BITS 16
ORG 0x7E00

global _stage2_start
_start:
    jmp _stage2_entry
    nop
    nop

_stage2_entry:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFF0
    sti

    push es

    mov [drive], dl
    call ext2_disk_init

    call ext2_init
    jc .fatal_halt

    call ext2_read_group_table
    jc .fatal_halt

    call a20_init
    call a20_enable

    ;Грузим Ядро сразу в 0x100000 (unreal-режим, ES = phys>>4)
    mov si, kernel_filename
    call ext2_find_file
    jc .fatal_halt
    mov [kernel_inode], ax

    call ext2_inode_to_group
    call ext2_get_group_inode_table

    mov word [cs:file_load_segment], KERNEL_PHYS_SEG
                                    ; unreal-сегмент: 0x10000*16 = 0x100000;
                                    ; ES выставляет ext2_load_file (.read)
    mov ax, [kernel_inode]
    call ext2_load_file
    jc .fatal_halt

    mov eax, [file_size]
    mov [kernel_size_pm], eax

    ;Грузим Framebuffer Driver (unreal-режим: ES = phys>>4, прямая запись по физ. адресам)
    mov si, fb_driver_filename
    call ext2_find_file
    jc .fatal_halt
    mov [fb_inode], ax

    call ext2_inode_to_group
    call ext2_get_group_inode_table

    mov ax, FB_SEG                ; 0x3000 -> линейный адрес 0x30000
    mov es, ax
    mov [cs:file_load_segment], ax
    mov ax, [fb_inode]
    call ext2_load_file
    jc .fatal_halt

    mov eax, [file_size]
    mov [fb_driver_size], eax

    ;Грузим Драйвер FAT12 (unreal-режим)
    mov si, fat12_driver_filename
    call ext2_find_file
    jc .fatal_halt
    mov [fat12_inode], ax

    call ext2_inode_to_group
    call ext2_get_group_inode_table

    mov ax, F12_SEG               ; 0x4000 -> линейный адрес 0x40000
    mov es, ax
    mov [cs:file_load_segment], ax
    mov ax, [fat12_inode]
    call ext2_load_file
    jc .fatal_halt

    mov eax, [file_size]
    mov [fat12_driver_size], eax

    ;Грузим Драйвер EXT2 (unreal-режим)
    mov si, ext2_driver_filename
    call ext2_find_file
    jc .fatal_halt
    mov [ext2_inode], ax

    call ext2_inode_to_group
    call ext2_get_group_inode_table

    mov ax, EXT2DRV_SEG           ; 0x5000 -> линейный адрес 0x50000
    mov es, ax
    mov [cs:file_load_segment], ax
    mov ax, [ext2_inode]
    call ext2_load_file
    jc .fatal_halt

    mov eax, [file_size]
    mov [ext2_driver_size], eax

    ; Восстанавливаем обычный real-mode ES (stage2 лежит на 0x7E00)
    mov ax, 0x7E0
    mov es, ax

    pop es
    cli

    ;Инициализация VBE и A20
    call vbe_init
    call a20_init

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

    ;Формирование структур для ядра
    call build_fs_info
    call build_modules_info
    call build_boot_info

    call a20_enable

    ;Переход в защищённый режим
    lgdt [gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

.fatal_halt:
    mov al, '!'
    out 0xE9, al
    jmp halt

BITS 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    cli

    ;Ядро уже загружено в 0x100000 напрямую (unreal-режим, stage2),
    ;релокация не нужна.

    mov ebx, BOOTINFO_ADDR

    jmp 0x08:0x100000

halt32:
    cli
.loop32:
    hlt
    jmp .loop32

copy_error:
    jmp halt32

BITS 16
halt:
    cli
.loop:
    hlt
    jmp .loop

;Переменные
KERNEL_PHYS_SEG equ 0x10000     ; unreal-сегмент для ядра: 0x10000*16 = 0x100000
FB_SEG          equ 0x3000      ; линейный адрес драйвера FB = 0x30000 (совпадает с bootinfo)
F12_SEG         equ 0x4000      ; линейный адрес драйвера FAT12 = 0x40000
EXT2DRV_SEG     equ 0x5000      ; линейный адрес драйвера EXT2 = 0x50000

kernel_size_pm       dd 0
kernel_inode         dw 0
fb_inode             dw 0
fat12_inode          dw 0
ext2_inode           dw 0
drive                db 0
cluster              dw 0

loader_name          db "ext.ld",0
kernel_filename      db "kernel",0
fb_driver_filename   db "fb_dr.drv",0
fat12_driver_filename db "f12_dr.drv",0
ext2_driver_filename  db "ext2_dr.drv",0


%include "include/bootinfo.inc"
%include "include/ext2disk.inc"
%include "include/debug.inc"
%include "include/ext2debug.inc"
%include "include/ext2_stage2.inc"
%include "include/ext2_group.inc"
%include "include/ext2_inode.inc"
%include "include/ext2_dir.inc"
%include "include/ext2_load.inc"
%include "include/vbe.inc"
%include "include/a20.inc"
%include "include/gdt.inc"
%include "include/graphics.inc"
%include "include/font.inc"
%include "include/e820.inc"
%include "include/ext2_bootinfo.inc"
%include "include/bootinfo_builder.inc"

align 4
ext2_block_size         dw 1024
ext2_inode_size         dw 128
ext2_inodes_per_group   dw 8192
ext2_blocks_per_group   dw 8192
ext2_first_data_block   dw 1
inode_offset            dw 0

%if ($-$$) > 32256
    %error "Ошибка! OSDev: Код hddld.asm превысил лимит 63 секторов!"
%endif

times 32256-($-$$) db 0
