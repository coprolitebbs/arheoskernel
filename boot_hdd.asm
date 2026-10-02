BITS 16
ORG 0x7C00

%define QEMU_DEBUG_PORT 0xE9

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00      ;Инициализируем стек Ring 0 на безопасный адрес
    sti

    ;Маркер [S] - Старт MBR загрузчика
    mov al, 'S'
    out QEMU_DEBUG_PORT, al

    ;Проверка поддержки LBA Расширений BIOS
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, 0x80        ;Жесткий диск 0 (C:)
    int 0x13
    jc .no_lba
    cmp bx, 0xAA55
    jne .no_lba

    ;Поддерживается LBA - маркер [L]
    mov byte [cs:lba_supported], 1
    mov al, 'L'
    out QEMU_DEBUG_PORT, al
    jmp .lba_ok

.no_lba:
    mov byte [cs:lba_supported], 0
    mov al, 'C'
    out QEMU_DEBUG_PORT, al

.lba_ok:
    ;Если LBA не поддерживается, получаем CHS геометрию
    cmp byte [cs:lba_supported], 1
    je .skip_geometry

    call get_geometry
    jc .geometry_error
    jmp .geometry_ok
.geometry_error:
    mov word [cs:heads_tmp], 16
    mov word [cs:sectors_tmp], 63
.geometry_ok:

.skip_geometry:
    ;Маркер [R] - Готовы читать Stage2 с диска LBA 1
    mov al, 'R'
    out QEMU_DEBUG_PORT, al

    mov ax, 0x0000
    mov es, ax
    mov bx, 0x7E00      ;ES:BX = 0x0000:0x7E00 (Буфер назначения Stage2)

    mov eax, 1          ;Стартовый LBA сектор Stage2 (сектор 1 жесткого диска)
    mov cx, 16          ;Количество секторов (8 КБ кода Stage2)

    call read_sectors
    jnc .read_success

    ;Ошибка чтения - маркер [F]
    mov al, 'F'
    out QEMU_DEBUG_PORT, al
    jmp .boot_error

.read_success:
    ; Маркер [J] - успешно, передаем управление Stage2 на 0x7E00
    ;mov al, 'J'
    ;out QEMU_DEBUG_PORT, al

    mov dl, 0x80        ;Номер диска передаем в Stage2 через DL
    jmp 0x0000:0x7E00

.boot_error:
    cli
.halt_loop:
    hlt
    jmp .halt_loop

;Подпрограмма получения геометрии CHS
get_geometry:
    mov ah, 0x08
    mov dl, 0x80
    int 0x13
    jc .error
    mov [cs:heads], dh
    inc byte [cs:heads]
    mov [cs:sectors], cl
    and byte [cs:sectors], 0x3F
    mov al, [cs:heads]
    mov [cs:heads_tmp], al
    mov al, [cs:sectors]
    mov [cs:sectors_tmp], al
    clc
    ret
.error:
    stc
    ret

;Чтение секторов (LBA или CHS)
read_sectors:
    cmp byte [cs:lba_supported], 1
    je .lba

    ;CHS fallback с динамической геометрией
    pusha
    push es
    push bx
    mov si, ax          ;LBA
    mov di, cx          ;количество
    mov ax, [cs:heads_tmp]
    mov [cs:heads_temp], ax
    mov ax, [cs:sectors_tmp]
    mov [cs:sectors_temp], ax
.next_chs:
    mov ax, si
    xor dx, dx
    div word [cs:sectors_temp]   ;ax = LBA / SPT, dx = LBA % SPT
    mov cl, dl
    inc cl
    xor dx, dx
    div word [cs:heads_temp]     ;ax = cylinder, dx = head
    mov dh, dl
    mov ch, al
    mov dl, 0x80
    mov ah, 0x02
    mov al, 1
    int 0x13
    jc .error_chs
    add bx, 512
    inc si
    dec di
    jnz .next_chs
    clc
    pop bx
    pop es
    popa
    ret
.error_chs:
    stc
    pop bx
    pop es
    popa
    ret

.lba:
    pusha
    push ds

    ;Заталкиваем 16 байт структуры DAP на стек в обратном порядке:
    push dword 0        ;[Байты 12..15] Старшие 32 бита LBA (всегда 0)
    push eax            ;[Байты 8..11]  Младшие 32 бита LBA адреса (число 1)
    push es             ;[Байты 6..7]   Сегмент буфера назначения (0x0000)
    push bx             ;[Байты 4..5]   Смещение буфера назначения (0x7E00)
    push cx             ;[Байты 2..3]   Количество секторов для чтения (16)
    push word 0x0010    ;[Байты 0..1]   Размер пакета (16 байт) и резервный 0

    ;Передаем указатель на вершину стека в регистр SI
    mov ax, ss
    mov ds, ax
    mov si, sp          ;DS:SI теперь идеально указывает на DAP на стеке!

    mov dl, 0x80        ;Жесткий диск 0
    mov ah, 0x42        ;Функция расширенного чтения LBA
    int 0x13

    ;Освобождаем 16 байт структуры DAP со стека после вызова
    add sp, 16

    pop ds
    popa
    ret

;Секция данных загрузчика STAGE1
lba_supported     db 0
heads_tmp         dw 16
sectors_tmp       dw 63
heads             db 0
sectors           db 0
heads_temp        dw 0
sectors_temp      dw 0

;Заполнение сектора MBR до таблицы разделов
times 446-($-$$) db 0
times 64 db 0        ;Пространство таблицы разделов HDD
dw 0xAA55            ;Сигнатура MBR
