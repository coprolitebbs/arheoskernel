BITS 16
ORG 0x7E00
; =====================================================================
;  F12.LD - stage1.5, универсальная версия для любой FAT12 дискеты/диска
;  Все параметры файловой системы (bytes/sector, sectors/cluster,
;  reserved sectors, число FAT, размер FAT, размер корня, CHS-геометрия)
;  читаются из BPB загрузочного сектора (LBA0) во время выполнения,
;  вместо жёстко заданных %define-констант для 720K.
; =====================================================================

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x9C00
    sti
    mov [drive], dl
    ; ---- Инициализация диска ----
    mov ah, 0x00
    int 0x13
    jc error

    pusha
	; ---- Чтение и разбор BPB ----
    call bpb_init
    jc error
	popa

	pusha
	; ---- Отладка: вывод значений ----
    mov dx, 0xE9
    mov si, msg_bpb
    call print_debug
    mov ax, [bps]
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov al, [spc]
	mov ah, 0
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov ax, [rsvd]
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov al, [nfats]
	mov ah, 0
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov ax, [root_ent]
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov ax, [fatsz]
    call debug_hex_word
    mov si, msg_delimiter
    call print_debug
    mov ax, [spt]
    call debug_hex_word
	mov si, msg_delimiter
    call print_debug
    mov ax, [heads]
    call debug_hex_word
    mov al, 13
    out dx, al
    mov al, 10
    out dx, al
	popa

    ; ---- Чтение FAT ----
    push es
    mov ax, [rsvd]
    mov cx, [fatsz]
    mov bx, 0x9000
    mov es, bx
    xor bx, bx
    call disk_read_sectors
    pop es
    jc error

    ; ---- Чтение корневого каталога ----
    push es
    mov ax, [root_start]
    mov cx, [root_size]
    mov bx, 0x8000
    mov es, bx
    xor bx, bx
    call disk_read_sectors
    pop es
    jc error

    ;call dump_of_catalogue

; ---- Поиск KERNEL ----
    ;push si
    ;push di
    push es

    mov bx, 0x8000
    mov es, bx
    xor bx, bx
    mov cx, [root_ent]

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

    ; ---- Читаем кластер и размер ----
    mov ax, [es:bx + 26]
    mov [cluster], ax
    mov ax, [es:bx + 28]
    mov [file_size], ax
    call cluster_size_debug

    ; ---- LBA = DATA_START + (cluster-2)*SECTORS_PER_CLUSTER ----
    mov ax, [cluster]
    sub ax, 2
    mov bx, [spc]
    mul bx
    add ax, [data_start]
    mov [sector], ax

    push es

    ; ---- Загрузка KERNEL в 0x20000 ----
    mov si,kernel_name
    mov ax,0x2000
    mov es,ax
    xor bx,bx
    call fat12_load_file
    jc error

    ;call dump_kernel_16bytes

    ; ---- Загрузка framebuffer драйвера ----
    mov si,fb_driver_name
    mov ax,0x3000
    mov es,ax
    xor bx,bx
    call fat12_load_file
    jc error

    mov eax,[current_file_size]
    mov [fb_driver_size],eax

    ; ---- Загрузка драйвера fat12 ----
    mov si,fat12_driver_name
    mov ax,0x3400
    mov es,ax
    xor bx,bx
    call fat12_load_file
    jc error

    mov eax,[current_file_size]
    mov [fat12_driver_size],eax

    ; ---- Загрузка драйвера ext2 ----
    mov si,ext2_driver_name
    mov ax,0x4000
    mov es,ax
    xor bx,bx
    call fat12_load_file
    jc error

    mov eax,[current_file_size]
    mov [ext2_driver_size],eax

    ;call dump_fat12
    ;jmp halt

    pop es

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

    ; ---- дамп modules_info ----
	;mov esi,0x5200
    ;mov ecx,64

;.dump_pm:
    ;mov al,[esi]
    ;call debug_hex_byte
    ;inc esi
    ;loop .dump_pm

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
    mov esi, 0x20000
    mov edi, 0x100000
    mov ecx, [file_size]
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
    hlt
    jmp halt32


BITS 16


; =====================================================================
; bpb_init - читает загрузочный сектор (LBA0) во временный буфер
;            0x0000:0x0600 и заполняет переменные геометрии FAT12
;            из его BPB. На выходе CF=1 при ошибке чтения диска.
;
; ВАЖНО: чтение LBA0 безопасно ещё до разбора BPB, т.к. для LBA=0
; CHS-преобразование (через disk_read_sectors) всегда даёт
; цилиндр=0, головку=0, сектор=1 независимо от текущих значений
; spt/heads (используются стартовые заглушки 9/2 ниже).
; =====================================================================
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
    xor ax, ax          ; LBA загрузочного сектора = 0
    mov cx, 1
    call disk_read_sectors
    jc .fail

    mov si, 0x0600

    mov ax, [si+11]      ; BPB_BytsPerSec
    mov [bps], ax

    xor ax, ax
    mov al, [si+13]      ; BPB_SecPerClus
    mov [spc], ax

    mov ax, [si+14]      ; BPB_RsvdSecCnt
    mov [rsvd], ax

    xor ax, ax
    mov al, [si+16]      ; BPB_NumFATs
    mov [nfats], ax

    mov ax, [si+17]      ; BPB_RootEntCnt
    mov [root_ent], ax

    mov ax, [si+22]      ; BPB_FATSz16
    mov [fatsz], ax

    mov ax, [si+24]      ; BPB_SecPerTrk
    mov [spt], ax

    mov ax, [si+26]      ; BPB_NumHeads
    mov [heads], ax

    ; root_start = rsvd + nfats*fatsz
    mov ax, [nfats]
    mul word [fatsz]
    add ax, [rsvd]
    mov [root_start], ax

    ; root_size = ceil( root_ent*32 / bps )
    mov ax, [root_ent]
    mov bx, 32
    mul bx               ; dx:ax = root_ent*32
    mov cx, [bps]
    add ax, cx
    adc dx, 0
    sub ax, 1
    sbb dx, 0
    div cx               ; ax = ceil(root_ent*32 / bps)
    mov [root_size], ax

    ; data_start = root_start + root_size
    mov ax, [root_start]
    add ax, [root_size]
    mov [data_start], ax

    clc
.fail:
    pop si
    pop dx
    pop cx
    pop ax
    pop bx
    pop es
    ret





; ---------- Обработчики ошибок ----------
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

; ---------- Инклюды ----------
%include "include/bootinfo_builder.inc"
%include "include/debug.inc"
%include "include/fat12_stage2.inc"
%include "include/vbe.inc"
%include "include/gdt.inc"
%include "include/graphics.inc"
%include "include/font.inc"
%include "include/e820.inc"
%include "include/fat12_bootinfo.inc"
%include "include/bootinfo.inc"
;%include "include/modules_builder.inc"

BITS 16
; ---------- Данные ----------
; File names to load
kernel_name        db "KERNEL     "

drive       db 0
;filename    db "KERNEL     "
cluster     dw 0
sector      dw 0
file_size   dw 0


; ---- Параметры, полученные из BPB (заполняются в bpb_init) ----
; Начальные значения - заглушки для 720K, используются ТОЛЬКО
; для самого первого чтения LBA0 в bpb_init (см. комментарий там).
bps         dw 512      ; байт на сектор           (BPB_BytsPerSec)
spc         dw 2        ; секторов на кластер       (BPB_SecPerClus)
rsvd        dw 1        ; зарезервированных секторов(BPB_RsvdSecCnt)
nfats       dw 2        ; число FAT                 (BPB_NumFATs)
root_ent    dw 224      ; записей в корневом каталоге(BPB_RootEntCnt)
fatsz       dw 3        ; секторов на одну FAT       (BPB_FATSz16)
spt         dw 9        ; секторов на дорожку (CHS)  (BPB_SecPerTrk)
heads       dw 2        ; число головок (CHS)        (BPB_NumHeads)

; ---- Производные величины (вычисляются в bpb_init) ----
root_start  dw 0        ; первый сектор корневого каталога
root_size   dw 0        ; размер корневого каталога в секторах
data_start  dw 0        ; первый сектор области данных (кластер 2)

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

; ---------- Заполнение до 8192 ----------
times 8192-($-$$) db 0
