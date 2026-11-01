; Copyright (C) 2026 ARHeOS by Dmitry H
; Licensed under GNU GPL v3

BITS 16
ORG 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00      ; Инициализируем стек строго под MBR
    sti

    mov [cs:boot_drive], dl ; Сохраняем номер диска от BIOS (0x80)

    ; Проверка поддержки LBA Расширений BIOS
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [cs:boot_drive]
    int 0x13
    jc .no_lba
    cmp bx, 0xAA55
    jne .no_lba
    mov byte [cs:lba_supported], 1
    jmp .skip_geometry

.no_lba:
    mov byte [cs:lba_supported], 0

    ; Получаем CHS геометрию, если LBA недоступен
    mov ah, 0x08
    mov dl, [cs:boot_drive]
    int 0x13
    jc .geometry_error

    mov [cs:heads], dh
    inc byte [cs:heads]
    and cl, 0x3F
    mov [cs:sectors], cl
    jmp .skip_geometry

.geometry_error:
    mov byte [cs:heads], 16
    mov byte [cs:sectors], 63

.skip_geometry:
    mov bx, 0x7E00      ; Буфер назначения Stage2 в памяти (0x0000:0x7E00)
    mov ax, 1           ; AX = Стартовый логический сектор Stage2 (LBA 1)
    mov cx, 63          ; Количество секторов для чтения (32256 байт)

.read_loop:
    push cx             ; Сохраняем счетчик циклов
    push ax             ; Сохраняем текущий логический сектор

    call read_sectors   ; Читаем строго 1 сектор
    jc .read_failed

    pop ax              ; Восстанавливаем LBA
    inc ax              ; Переходим к следующему сектору
    add bx, 0x0200      ; Сдвигаем буфер на 512 байт вперед

    pop cx              ; Восстанавливаем счетчик циклов
    loop .read_loop     ; Цикл по всем 63 секторам

    ; =================================================================
    ; УСПЕХ: Передаем управление Stage 2
    ; =================================================================
    mov dl, [cs:boot_drive]
    jmp 0x0000:0x7E00       ; Прыгаем на выполнение Stage 2

.read_failed:
    pop ax
    pop cx
    mov al, 'F'         ; Выведем 'F', если все попытки чтения провалились
    call debug_char

.boot_error:
    cli
.halt_loop:
    hlt
    jmp .halt_loop

; Функция чтения одного сектора (Отремонтированная и изолированная)
read_sectors:
    push ax
    push bx
    push cx
    push dx

    cmp byte [cs:lba_supported], 1
    jne .try_chs_mode

    ; --- Безопасный LBA Режим (3 попытки чтения со сбросом) ---
    mov [cs:dap_lba_low], ax
    mov [cs:dap_buffer_off], bx
    mov byte [cs:retry_count], 3

.lba_retry_loop:
    mov ah, 0x42
    mov dl, [cs:boot_drive]
    mov si, dap_packet
    int 0x13
    jnc .done               ; Если прочиталось — уходим на выход

    ; Сброс дискового контроллера при ошибке LBA
    xor ax, ax
    mov dl, [cs:boot_drive]
    int 0x13

    dec byte [cs:retry_count]
    jnz .lba_retry_loop

    ; Если LBA окончательно отказал, аварийно пробуем CHS
    mov byte [cs:lba_supported], 0

.try_chs_mode:
    ; --- Исправленный CHS Режим под спецификацию IBM PC ---
    mov si, ax              ; SI = Текущий LBA сектор
    xor dx, dx
    movzx cx, byte [cs:sectors]
    div cx
    inc dl
    mov [cs:chs_s], dl      ; Сохраняем точный 1-based номер сектора

    xor dx, dx
    movzx cx, byte [cs:heads]
    div cx                  ; AX = Cylinder, DX = Head

    mov dh, dl              ; DH = Head
    mov ch, al              ; CH = Cylinder low
    mov cl, [cs:chs_s]      ; CL = Восстанавливаем чистый сектор
    shl ah, 6
    or cl, ah               ; Подмешиваем старшие биты цилиндра в CL

    mov byte [cs:retry_count], 3

.chs_retry_loop:
    mov dl, [cs:boot_drive]
    mov ax, 0x0201          ; AH=02 (Read), AL=1 сектор
    int 0x13
    jnc .done

    ; Безопасный сброс контроллера (БЕЗ затирания регистров CX/DX)
    push ax
    push dx
    xor ax, ax
    mov dl, [cs:boot_drive]
    int 0x13
    pop dx
    pop ax

    dec byte [cs:retry_count]
    jnz .chs_retry_loop
    stc                     ; Если и CHS не помог — взводим флаг ошибки Carry Flag

.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Вспомогательная функция печати символа
debug_char:
    push ax
    push bx
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop bx
    pop ax
    ret

; Раздел данных Stage 1
lba_supported     db 0
boot_drive        db 0x80
heads             db 0
sectors           db 0
retry_count       db 0
chs_s             db 0

; Статическая структура DAP, жестко выровненная
align 4
dap_packet:
dap_size          db 0x10
dap_reserved      db 0x00
dap_sectors       dw 0x0001
dap_buffer_off    dw 0x7E00
dap_buffer_seg    dw 0x0000
dap_lba_low       dw 0x0001
dap_lba_mid       dw 0x0000
dap_lba_high      dd 0x00000000

; =====================================================================
; ТОЧНАЯ РАЗМЕТКА СЕКТОРОВ И ТАБЛИЦЫ РАЗДЕЛОВ MBR (РОВНО 512 БАЙТ)
; =====================================================================
times 446-($-$$) db 0

; Легальная таблица разделов (Partition Entry 1) - старт с LBA 1
db 0x80, 0x00, 0x01, 0x00, 0x83, 0x0F, 0x3F, 0x82
dd 0x00000001, 0x0001F000

times 48 db 0       ; Нули для оставшихся 3 разделов
dw 0xAA55           ; Сигнатура MBR
