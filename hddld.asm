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

;рабочая версия загрузчика Stage 2 для hdd

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

    mov [drive], dl

    ;Инициализация VBE и A20
    call vbe_init
    call a20_init
    call a20_enable

    push es

    call ext2_disk_init

    call ext2_init
    jc .fatal_halt

    call ext2_read_group_table
    jc .fatal_halt

    ;Грузим Framebuffer Driver
    mov si, fb_driver_filename
    call ext2_find_file
    jc .fatal_halt
    mov [fb_inode], ax

    call ext2_inode_to_group
    call ext2_get_group_inode_table

    mov ax, 0x3000
    mov [cs:file_load_segment], ax
    mov ax, [fb_inode]
    call ext2_load_file
    jc .fatal_halt

    mov eax, [file_size]
    mov [fb_driver_size], eax

    ;Грузим Драйвер FAT12
    mov si, fat12_driver_filename
    call ext2_find_file
    jc .fatal_halt
    mov [fat12_inode], ax

    call ext2_inode_to_group
    call ext2_get_group_inode_table

    mov ax, 0x4000
    mov [cs:file_load_segment], ax
    mov ax, [fat12_inode]
    call ext2_load_file
    jc .fatal_halt

    mov eax, [file_size]
    mov [fat12_driver_size], eax

    ;Грузим Драйвер EXT2
    mov si, ext2_driver_filename
    call ext2_find_file
    jc .fatal_halt
    mov [ext2_inode], ax

    call ext2_inode_to_group
    call ext2_get_group_inode_table

    mov ax, 0x5000
    mov [cs:file_load_segment], ax
    mov ax, [ext2_inode]
    call ext2_load_file
    jc .fatal_halt

    mov eax, [file_size]
    mov [ext2_driver_size], eax

    pop es

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

    ;Грузим Ядро
    mov si, kernel_filename
    call ext2_find_file
    jc .fatal_halt
    mov [kernel_inode], ax

    call ext2_inode_to_group
    call ext2_get_group_inode_table

    mov ax, [kernel_inode]
    call ext2_read_inode
    jc .fatal_halt

    mov ax, INODE_SEG
    mov ds, ax
    mov si, [cs:inode_offset]
    mov eax, [si+4]               ;Читаем 32-битный размер ядра
    mov [cs:kernel_size_pm], eax

    ;Вызываем изолированную функцию загрузки ядра
    call ext2_load_kernel_high
    jc .fatal_halt

    ;Восстанавливаем дефолтные сегменты для сборщиков метаданных
    xor ax, ax
    mov ds, ax
    mov es, ax


    ;call dump_kernel
    ;jmp halt


    ;Формирование структур для ядра
    call build_fs_info
    call build_modules_info
    call build_boot_info

    cli

    mov eax, [cs:gdtr+2]
    cmp eax, 0x7E00
    jae .final_gdt_ok
    add eax, 0x7E00
    mov [cs:gdtr+2], eax
.final_gdt_ok:

    ;Переход в защищённый режим
    lgdt [cs:gdtr]
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
