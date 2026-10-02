BITS 16
ORG 0x7E00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x9C00
    sti

    mov ax, cs
    mov ds, ax

    mov [cs:drive], dl
    ;Инициализация диска
    mov ah, 0x00
    int 0x13
    jc error

    pusha
	;Чтение и разбор BPB

	call disk_reset

    call bpb_init
    jc error
	popa

	pusha
	;Отладка: вывод значений
    mov dx, 0xE9
    mov si, msg_bpb
    call print_debug
    mov ax, [cs:bps]
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov al, [cs:spc]
	mov ah, 0
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov ax, [cs:rsvd]
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov al, [cs:nfats]
	mov ah, 0
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov ax, [cs:root_ent]
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov ax, [cs:fatsz]
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov ax, [cs:spt]
    call debug_hex_word
	mov si, msg_delimiter
    call print_debug
    mov ax, [cs:heads]
    call debug_hex_word
    mov al, 13
    out dx, al
    mov al, 10
    out dx, al
	popa

    ;Чтение FAT
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
    ;Чтение корневого каталога
    push es
    mov ax, [cs:root_start]
    mov cx, [cs:root_size]
    mov bx, 0x8000
    mov es, bx
    xor bx, bx
    call disk_read_sectors
    pop es
    jc error

    ;call dump_of_catalogue
    call disk_reset
    ;Поиск KERNEL
    ;push si
    ;push di

    push es

    mov bx, 0x8000
    mov es, bx
    xor bx, bx
    mov cx, [cs:root_ent]

search:
    push cx
    push bx
    mov al, [es:bx]
    cmp al, 0
    je not_found
    cmp al, 0xE5
    je skip_entry
    mov si, kernel_name
    mov di, bx
    mov cx, 11
    cld
    repe cmpsb
    pop bx
    pop cx
    je found
skip_entry:
    pop bx
    pop cx
maybe_jump:
    add bx, 32
    loop search
    pop es
    jmp not_found
found:

    pop es

    ;Читаем кластер и размер
    mov ax, [es:bx + 26]
    mov [cs:cluster], ax
    mov ax, [es:bx + 28]
    mov [cs:file_size], ax
    call cluster_size_debug

    ;LBA = DATA_START + (cluster-2)*SECTORS_PER_CLUSTER
    mov ax, [cs:cluster]
    sub ax, 2
    mov bx, [cs:spc]
    mul bx
    add ax, [cs:data_start]
    mov [cs:sector], ax

    push es

    push cs
    pop ds

    ;Загрузка KERNEL в 0x20000
    mov si,kernel_name
    mov ax,0x2000
    mov es,ax
    xor bx,bx
    call fat12_load_file
    jc error

    ;call dump_kernel_16bytes

    call disk_reset
    ;Загрузка framebuffer драйвера
    mov si,fb_driver_name
    mov ax,0x3000
    mov es,ax
    xor bx,bx
    call fat12_load_file
    jc error

    mov eax,[cs:current_file_size]
    mov [cs:fb_driver_size],eax

    call disk_reset

    push cs
    pop ds
    ;Загрузка драйвера fat12
    mov si,fat12_driver_name
    mov ax,0x4000
    mov es,ax
    xor bx,bx
    call fat12_load_file
    jc error

    mov eax,[cs:current_file_size]
    mov [cs:fat12_driver_size],eax

    call disk_reset

    push cs
    pop ds
    ;Загрузка драйвера ext2
    mov si,ext2_driver_name
    mov ax,0x5000
    mov es,ax
    xor bx,bx
    call fat12_load_file
    jc error

    mov eax,[cs:current_file_size]
    mov [cs:ext2_driver_size],eax

    ;call dump_fat12
    ;jmp halt

    pop es

    ;Инициализация VBE
    call vbe_init

    ;cli

	;Получение карты памяти через E820
    push es
    push di
    xor ax, ax
    mov es, ax
    mov di, 0x6002       ;буфер для записей (начинается с 0x6002)
    call e820_detect
    mov word [0x6000], cx ;сохраняем количество записей по адресу 0x6000
    ;если ошибка, количество записей будет 0
    pop di
    pop es

	;Формирование структур
	call build_fs_info
	call build_modules_info
	call build_boot_info

    ;дамп modules_info
	;mov esi,0x5200
    ;mov ecx,64

;.dump_pm:
    ;mov al,[esi]
    ;call debug_hex_byte
    ;inc esi
    ;loop .dump_pm

    ;xor eax,eax
    ;mov eax, [cs:file_size]
    ;mov edi, eax
    ;mov ebx, eax
    xor ebx,ebx
    movzx ebx, word [cs:file_size]

    ;cli
    ;xor ax, ax
    ;mov ds, ax
    ;mov es, ax
    ;mov fs, ax
    ;mov gs, ax

    cli
    xor ax, ax
    mov ss, ax
    mov sp, 0x9C00      ;Тот же адрес, что и в начале start:
    sti

    call a20_enable

    ;Переход в защищённый режим
    lgdt [cs:gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry
BITS 32
pm_entry:
    ;Отладка через видеопамять
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ;cli



    ;Копирование ядра в 100000
    push esi
    push edi
    push ecx

    ;cld


    mov esi, 0x20000
    mov edi, 0x100000
    mov ecx, ebx
    cld
    ;mov ecx, [file_size]
    rep movsb
    pop ecx
    pop edi
    pop esi

    cli

	mov ebx,BOOTINFO_ADDR

;halt_33:
	;jmp halt_33

    jmp 0x08:0x100000

    jmp halt32


halt32:
    cli
    hlt
    jmp halt32


BITS 16


;bpb_init - читает загрузочный сектор (LBA0) во временный буфер
;0x0000:0x0600 и заполняет переменные геометрии FAT12
;из его BPB. На выходе CF=1 при ошибке чтения диска
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

;Обработчики ошибок
not_found:
    pop es
    pop si
    mov si, msg_not_found
    call print_string
    jmp halt
error:
    mov si, msg_error
    call print_string
    jmp halt
halt:
    ;cli
    hlt
    jmp halt

BITS 16

;Инклюды
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

BITS 16
;Данные
;File names to load
kernel_name        db "KERNEL     "

drive       db 0
;filename    db "KERNEL     "
cluster     dw 0
sector      dw 0
file_size   dw 0


;Параметры, полученные из BPB (заполняются в bpb_init)
;Начальные значения - заглушки для 720K, используются только
;для самого первого чтения LBA0 в bpb_init (см. комментарий там).
bps         dw 512      ;байт на сектор           (BPB_BytsPerSec)
spc         dw 2        ;секторов на кластер       (BPB_SecPerClus)
rsvd        dw 1        ;зарезервированных секторов(BPB_RsvdSecCnt)
nfats       dw 2        ;число FAT                 (BPB_NumFATs)
root_ent    dw 224      ;записей в корневом каталоге(BPB_RootEntCnt)
fatsz       dw 3        ;секторов на одну FAT       (BPB_FATSz16)
spt         dw 9        ;секторов на дорожку (CHS)  (BPB_SecPerTrk)
heads       dw 2        ;число головок (CHS)        (BPB_NumHeads)

; ---- Производные величины (вычисляются в bpb_init)
root_start  dw 0        ;первый сектор корневого каталога
root_size   dw 0        ;размер корневого каталога в секторах
data_start  dw 0        ;первый сектор области данных (кластер 2)

msg_not_found   db "KERNEL not found", 0
msg_error       db "Disk error", 0

msg_found_name  db "Found: ", 0
msg_kcp db "Kernel copied", 0
msg_bpb         db "BPB: ", 0
msg_delimiter   db " : ", 0
loader_name db "F12.LD",0

fb_driver_name         db "FB_DR   DRV"
fb_driver_filename     db "FB_DR.DRV",0
fat12_driver_name      db "F12_DR  DRV"
fat12_driver_filename  db "F12_DR.DRV",0
ext2_driver_name       db "EXT2_DR DRV"
ext2_driver_filename   db "EXT2_DR.DRV",0

;Заполнение до 8192
times 8192-($-$$) db 0
