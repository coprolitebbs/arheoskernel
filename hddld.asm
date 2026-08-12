BITS 16
ORG 0x7E00


; ============================================================
; Stage2 EXT2 loader test
;
; Сейчас:
;
; - BIOS LBA
; - EXT2 superblock
; - GDT
; - inode table
; - inode read
;
; ============================================================

    cli

; ------------------------------------------------------------
; setup segments
; ------------------------------------------------------------
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov sp,0x9C00
    sti

	push es
; ------------------------------------------------------------
; save BIOS drive
; ------------------------------------------------------------
	mov [drive],dl
; ------------------------------------------------------------
; init disk
; ------------------------------------------------------------
    call ext2_disk_init
; ------------------------------------------------------------
; read EXT2 superblock
; ------------------------------------------------------------
    call ext2_init
    jc halt

    call ext2_read_group_table
    jc halt
    ; ищем файл kernel
	mov si,kernel_filename
    call ext2_find_file
    jc halt

    mov [kernel_inode],ax

    ; вычислили группу inode
    ;mov ax,[kernel_inode]
    call ext2_inode_to_group
    call ext2_get_group_inode_table

    mov ax,[kernel_inode]

    ; грузим файл

	mov ax,KERNEL_SEG
    mov [cs:file_load_segment],ax
	mov ax,[kernel_inode]
	call ext2_load_file
	jc halt
    mov [kernel_inode],ax
	mov eax,[file_size]
	mov [kernel_size_pm],eax

    ;call dump_kernel

    ; грузим framebuffer driver
    mov si,fb_driver_filename
    call ext2_find_file
    jc halt
    ; AX = inode драйвера
    mov [fb_inode],ax
    mov ax,[fb_inode]
    ; выбрать inode table группы
    call ext2_inode_to_group
    call ext2_get_group_inode_table
    ; сегмент загрузки драйвера
    mov eax,0x3000
    mov [cs:file_load_segment],ax
    ; загрузить
    mov ax,[fb_inode]
    call ext2_load_file
    jc halt
    ;mov [fb_inode],ax
    mov eax,[file_size]
    mov [fb_driver_size],eax


    ; грузим драйвер fat12
    mov si,fat12_driver_filename
    call ext2_find_file
    jc halt
    ; AX = inode драйвера
    mov [fat12_inode],ax
    mov ax,[fat12_inode]
    ; выбрать inode table группы
    call ext2_inode_to_group
    call ext2_get_group_inode_table
    ; сегмент загрузки драйвера
    mov eax,0x3400
    mov [cs:file_load_segment],ax
    ; загрузить
    mov ax,[fat12_inode]
    call ext2_load_file
    jc halt
    mov eax,[file_size]
    mov [fat12_driver_size],eax


    ; грузим драйвер ext2
    mov si,ext2_driver_filename
    call ext2_find_file
    jc halt
    ; AX = inode драйвера
    mov [ext2_inode],ax
    mov ax,[ext2_inode]
    ; выбрать inode table группы
    call ext2_inode_to_group
    call ext2_get_group_inode_table
    ; сегмент загрузки драйвера
    mov eax,0x4000
    mov [cs:file_load_segment],ax
    ; загрузить
    mov ax,[ext2_inode]
    call ext2_load_file
    jc halt
    mov eax,[file_size]
    mov [ext2_driver_size],eax

	;call dump_fb

	pop es

	cli

	;jmp halt
	; ---- Инициализация VBE ----
    call vbe_init

    ; ---- Инициализация A20 ----
    call a20_init

	; ---- Получение карты памяти через E820 ----
    push es
    push di
    xor ax, ax
    mov es, ax
    mov di, 0x6002       ; буфер для записей (начинается с 0x6002)
    call e820_detect
    mov word [0x6000], cx ; сохраняем количество записей по адресу 0x6000
    ; если ошибка, количество записей будет 0
    pop di
    pop es

	; ---- Формирование структур ----
	call build_fs_info
	call build_modules_info
	call build_boot_info

    ; ---- Переход в защищённый режим ----
    lgdt [gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry
BITS 32
pm_entry:
    ; ---- Отладка через видеопамять ----
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    cli

	; ---- Копирование ядра в 100000 ----
	push esi
    push edi
    push ecx
	cld
	mov esi,0x20000
	mov edi,0x100000
	mov ecx,[kernel_size_pm]
	rep movsb
	pop ecx
    pop edi
    pop esi

	cli

	mov ebx,BOOTINFO_ADDR

    jmp 0x08:0x100000

    jmp halt32

halt32:
    cli

.loop32:
    hlt
    jmp .loop32

copy_error:
    mov si, msg_copy_error
    call print_debug
    jmp halt32

BITS 16

halt:
    cli

.loop:
    hlt
    jmp .loop

; ============================================================
; Variables
; ============================================================


BITS 16

kernel_size_pm dd 0

kernel_inode dw 0
fb_inode dw 0
fat12_inode dw 0
ext2_inode dw 0

drive      db 0
cluster     dw 0

; ============================================================
; Messages
; ============================================================

msg_after_super: db "AFTER SUPER",13,10,0
;msg_test: db "msg_test (old MSG_INODE_TABLE): ",0
;msg_inode_ok db "INODE 97 OK",13,10,0
;msg_inode2_ok db "INODE 2 OK",13,10,0
;msg_inode1_ok db "INODE 1 OK",13,10,0
;msg_ds_test    db "DS=",0
;msg_before_kernel_load db "BEFORE KERNEL LOAD TABLE: ",0

msg_before_find db "BEFORE FIND",13,10,0
;msg_after_find  db "AFTER FIND",13,10,0
msg_find_error  db "FIND ERROR",13,10,0
msg_kernel_inode_ok db "KERNEL INODE READ OK",13,10,0
msg_after_find db "AFTER FIND AX=",0

msg_copy_error       db "KERNEL COPY ERR ",0
msg_inode_found db "FOUND INODE: ",0


loader_name          db "ext.ld",0
kernel_filename      db "kernel",0
fb_driver_filename   db "fb_dr.drv",0
;fs_driver_filename   db "fs_dr.drv",0
fat12_driver_filename  db "f12_dr.drv",0
ext2_driver_filename  db "ext2_dr.drv",0

; ============================================================
; Includes
; ============================================================
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
%include "include/gdt.inc"
%include "include/graphics.inc"
%include "include/font.inc"
%include "include/e820.inc"
%include "include/ext2_bootinfo.inc"
%include "include/bootinfo_builder.inc"


BITS 16
; ============================================================
; Stage2 padding
; ============================================================

times 8192-($-$$) db 0
