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
    ;mov al, 'S'
    ;call debug_char

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
    call debug_char
    jmp .lba_ok

.no_lba:
    mov byte [cs:lba_supported], 0
    mov al, 'C'
    call debug_char

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
    call debug_char

    mov ax, 0x0000
    mov es, ax
    mov bx, 0x7E00      ;ES:BX = 0x0000:0x7E00 (Буфер назначения Stage2)

    mov eax, 1          ;Стартовый LBA сектор Stage2 (сектор 1 жесткого диска)
    mov cx, 63          ;Количество секторов (8 КБ кода Stage2)

    call read_sectors
    jnc .read_success

    ;Ошибка чтения - маркер [F]
    mov al, 'F'
    call debug_char
    jmp .boot_error

.read_success:
    ; Маркер [J] - успешно, передаем управление Stage2 на 0x7E00
    ;mov al, 'J'
    call debug_char

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
    jc .default
    mov [cs:heads], dh
    inc byte [cs:heads]
    mov [cs:sectors], cl
    and byte [cs:sectors], 0x3F
    cmp byte [cs:heads], 0
    je .default
    cmp byte [cs:sectors], 0
    je .default
    ;heads_tmp/sectors_tmp теперь не нужны для CHS, но оставим для совместимости
    mov al, [cs:heads]
    mov [cs:heads_tmp], al
    mov al, [cs:sectors]
    mov [cs:sectors_tmp], al
    clc
    ret
.default:
    mov byte [cs:heads], 16
    mov byte [cs:sectors], 63
    mov word [cs:heads_tmp], 16
    mov word [cs:sectors_tmp], 63
    clc
    ret
;Чтение секторов (LBA или CHS)
;Чтение секторов. Вход: AX = LBA, CX = количество, ES:BX = буфер.
;Возврат: CF=0 успех / CF=1 провал.
read_sectors:
    push ax
    push bx
    push cx
    push dx
    push es

    ;Пытаемся LBA, если BIOS рапортовал поддержку
    cmp byte [cs:lba_supported], 1
    jne .try_chs

    call .lba_read
    jnc .done

    ;BIOS соврал. Сбрасываем флаг навсегда и переключаемся на CHS.
    mov byte [cs:lba_supported], 0
    call get_geometry

.try_chs:
    call .chs_read
    jnc .done
    stc
    jmp .done

.done:
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; --- LBA read (INT 13h AH=42h) ---
.lba_read:
    push ds
    push si
    push eax

    ;DAP на стеке: [len+resv][count][off][seg][lba_lo][lba_hi]
    push dword 0
    push eax
    push es
    push bx
    push cx
    push word 0x0010

    mov ax, ss
    mov ds, ax
    mov si, sp

    mov dl, 0x80
    mov ah, 0x42
    int 0x13

    add sp, 16
    pop eax
    pop si
    pop ds
    ret

; --- CHS read (INT 13h AH=02h), по одному сектору ---
.chs_read:
    push ds
    push si
    push di
    push bp

    mov bp, sp
    mov si, ax                    ;SI = LBA
    mov di, cx                    ;DI = сколько секторов

.next_chs:
    ;LBA -> C/H/S
    mov ax, si
    xor dx, dx
    movzx cx, byte [cs:sectors]   ;SPT (динамическая геометрия)
    test cx, cx
    jz .chs_fail
    div cx
    mov cl, dl
    inc cl                        ;CL = sector (1-based)

    xor dx, dx
    movzx cx, byte [cs:heads]     ;heads
    test cx, cx
    jz .chs_fail
    div cx                        ;AX = cylinder, DX = head

    mov dh, dl                    ;DH = head
    mov ch, al                    ;CH = cyl low
    ; cyl high (2 бита) в CL[6:7] для полноты (для LBA < 2^14 не нужно, но пусть будет)
    movzx bx, byte [cs:cylinder_high]
    and bl, 0x03
    shl bl, 6
    or  cl, bl

    mov dl, 0x80
    mov ax, 0x0201                ;AH=02 (read), AL=1
    int 0x13
    jnc .chs_step_ok

    ;Один retry с reset
    push ax
    push bx
    push cx
    push dx
    push es
    xor ax, ax
    mov dl, 0x80
    int 0x13
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    mov ax, 0x0201
    int 0x13
    jc .chs_fail

.chs_step_ok:
    add bx, 0x0200
    jnc .no_seg_inc
    mov ax, es
    add ax, 0x1000
    mov es, ax
.no_seg_inc:
    inc si
    dec di
    jnz .next_chs

    pop bp
    pop di
    pop si
    pop ds
    clc
    ret

.chs_fail:
    pop bp
    pop di
    pop si
    pop ds
    stc
    ret

;Один символ: AL = символ
debug_char:
    push ax
    push bx
    mov ah, 0x0E
    mov bx, 0x0007          ;page 0, light gray on black
    int 0x10
    pop bx
    pop ax
    ret


;Секция данных загрузчика STAGE1
lba_supported     db 0
heads_tmp         dw 16
sectors_tmp       dw 63
heads             db 0
sectors           db 0
heads_temp        dw 0
sectors_temp      dw 0
cylinder_high     db 0

;Заполнение сектора MBR до таблицы разделов
times 446-($-$$) db 0
;times 64 db 0        ;Пространство таблицы разделов HDD
;dw 0xAA55            ;Сигнатура MBR
; Partition entry 1: active, тип 0x00 (не используется)
db 0x80, 0x00, 0x00, 0x00
db 0x00, 0x00, 0x00, 0x00
dd 0
dd 0
times 48 db 0
dw 0xAA55
