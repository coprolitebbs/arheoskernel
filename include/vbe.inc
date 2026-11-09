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

;vbe.inc — инициализация VESA VBE в stage2:
;  1) 4F00 — информация о контроллере;
;  2) vbe_enum_modes — перечисление ВСЕХ LFB-режимов в таблицу 0x5400
;     (VBE_MODES_ADDR из bootinfo.inc; ядро читает её через bootinfo.h);
;  3) vbe_pick_boot_mode — выбор режима загрузки (1024x768, приоритет bpp);
;  4) установка режима и заполнение глобальных переменных для bootinfo.
;Если список режимов контроллера не дал результата — включается старый
;грубый перебор 0x0100..0x01FF с допущением банковского режима (S3 Trio64
;в 86box и подобные BIOS без нормального списка). Найденный банковским
;перебором LFB-режим дописывается в таблицу отдельной записью.

BITS 16

;Глобальные переменные (совместимость со старым кодом и bootinfo_builder)
lfb_addr:       dd 0
pitch:          dd 0
bytes_pp:       dd 0
bpp:            db 0
                db 0, 0, 0      ;Выравнивание
screen_width:   dd 0
screen_height:  dd 0

;Флаги графики: бит 0 = LFB активен (бит 1 PMI больше не используется —
;банковский режим из PM всё равно не работает без тримплина)
graphics_flags: dd 0

vbe_ctrl_info:  times 512 db 0
mode_info:      times 256 db 0
msg_err:        db "VBE Error!", 0

best_mode:      dw 0
best_bpp:       db 0
best_index:     dw 0            ;индекс выбранного режима в таблице, 0xFFFF = нет
mode_count:     dw 0            ;сколько записей собрал vbe_enum_modes
enum_cnt:       dw 0            ;рабочий счётчик записей перечисления

;Порядок приоритета bpp при выборе режима загрузки: 24 > 16 > 32 > 8
bpp_prio:       db 24, 16, 32, 8

; VBE Init
vbe_init:
    pusha
    push es
    push ds

    mov dword [graphics_flags], 0
    mov dword [lfb_addr], 0
    mov dword [pitch], 0
    mov dword [bytes_pp], 0
    mov dword [screen_width], 0
    mov dword [screen_height], 0
    mov byte [bpp], 0
    mov word [best_mode], 0
    mov byte [best_bpp], 0
    mov word [best_index], 0xFFFF
    mov word [mode_count], 0
    mov word [enum_cnt], 0

    ;Отладка: старт
    mov al, 'v'
    call debug_char

    ;Получение информации о контроллере
    mov ax, cs
    mov es, ax
    mov di, vbe_ctrl_info
    ;Сигнатура VBE2 обязана лежать в буфере ДО вызова: без неё BIOS имеет
    ;право ответить структурой VBE 1.x без полного списка режимов
    mov word [es:di], 0x4256             ;"VB"
    mov word [es:di+2], 0x3245           ;"E2"
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    mov al, 'O'                 ;OK Controller
    call debug_char

    ;Отсутствие списка режимов (битый BIOS) — сразу в fallback-перебор,
    ;иначе vbe_enum_modes пойдёт lodsw-ом с указателем 0:0 по IVT
    mov eax, [cs:vbe_ctrl_info+0x0E]
    test eax, eax
    jz .fallback_scan

    ;Полное перечисление LFB-режимов в таблицу 0x5400
    call vbe_enum_modes

    ;Выбор режима загрузки из таблицы: 1024x768 с наивысшим приоритетом bpp
    call vbe_pick_boot_mode
    cmp word [best_mode], 0
    je .fallback_scan

    ;Режим из таблицы — PhysBasePtr проверен при перечислении,
    ;поэтому сначала ставим с LFB-флагом
    mov bx, [best_mode]
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    je .table_lfb_ok

    ;Подстраховка для VBE 1.x-подобных BIOS: LFB-установка не прошла —
    ;пробуем банковский режим (vesa-драйвер в ядре тогда не включится,
    ;сработает штатный fbdrv поверх boot-режима)
    mov bx, [best_mode]
    and bx, 0x3FFF
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail
    mov al, 'N'                 ;No LFB
    call debug_char
    mov dword [graphics_flags], 0
    mov word [best_index], 0xFFFF
    jmp .fill_vars

.table_lfb_ok:
    mov dword [graphics_flags], 1
    jmp .fill_vars

.fallback_scan:
    ;Кривые BIOS без списка режимов: старый грубый перебор 0x0100..0x01FF
    mov bx, 0x0100
.fb_scan:
    cmp bx, 0x0200
    jge .fb_check

    ;Запрос информации о режиме BX
    mov ax, cs
    mov es, ax
    mov di, mode_info
    mov ax, 0x4F01
    mov cx, bx
    int 0x10
    cmp ax, 0x004F
    jne .fb_next

    ;Проверка атрибутов — логика старого кода сохранена
    mov ax, [cs:mode_info + 0]
    test ax, 0x0002
    jz .fb_next

    ;Чтение параметров
    mov ax, [cs:mode_info + 18]         ;X
    mov dx, [cs:mode_info + 20]         ;Y
    movzx cx, byte [cs:mode_info + 25]  ;BPP

    ;Жесткая проверка 1024x768
    cmp ax, 1024
    jne .fb_next
    cmp dx, 768
    jne .fb_next
    cmp cx, 8
    jl .fb_next

    ;Логика выбора приоритета цвета (24 > 16 > 32 > 8)
    cmp cx, 24
    je .fb_take_24
    cmp cx, 16
    je .fb_take_16
    cmp cx, 32
    je .fb_take_32
    cmp cx, 8
    je .fb_take_8
    jmp .fb_next

.fb_take_24:
    mov word [best_mode], bx
    mov byte [best_bpp], 24
    jmp .fb_set

.fb_take_16:
    cmp byte [best_bpp], 24
    je .fb_next
    mov word [best_mode], bx
    mov byte [best_bpp], 16
    jmp .fb_next

.fb_take_32:
    cmp byte [best_bpp], 24
    je .fb_next
    cmp byte [best_bpp], 16
    je .fb_next
    mov word [best_mode], bx
    mov byte [best_bpp], 32
    jmp .fb_next

.fb_take_8:
    cmp byte [best_bpp], 24
    je .fb_next
    cmp byte [best_bpp], 16
    je .fb_next
    cmp byte [best_bpp], 32
    je .fb_next
    mov word [best_mode], bx
    mov byte [best_bpp], 8
    jmp .fb_next

.fb_next:
    inc bx
    jmp .fb_scan

.fb_check:
    cmp word [best_mode], 0
    je .vbe_fail

.fb_set:
    ;С флагом LFB (0x4000)
    mov bx, [best_mode]
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    je .fb_lfb_ok

    ;Без флага LFB — банковский режим
    mov bx, [best_mode]
    and bx, 0x3FFF
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    mov al, 'N'                 ;No LFB
    call debug_char
    mov dword [graphics_flags], 0
    jmp .fill_vars

.fb_lfb_ok:
    mov dword [graphics_flags], 1

.fill_vars:
    ;Запрос mode_info реально выставленного режима.
    ;4F01 принимает ЧИСТЫЙ номер режима: флаги LFB/очистки экрана
    ;(биты 14-15) некоторые BIOS считают за неизвестный режим
    mov ax, cs
    mov es, ax
    mov di, mode_info
    mov ax, 0x4F01
    mov cx, bx
    and cx, 0x3FFF
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail_info

    ;Заполнение переменных
    movzx eax, word [cs:mode_info + 18]
    mov [cs:screen_width], eax

    movzx eax, word [cs:mode_info + 20]
    mov [cs:screen_height], eax

    mov al, [cs:mode_info + 0x19]
    mov [bpp], al

    movzx ecx, al
    add ecx, 7
    shr ecx, 3
    mov eax, ecx
    mov [bytes_pp], eax

    movzx eax, word [cs:mode_info + 0x10]
    mov [pitch], eax

    mov eax, [cs:mode_info + 0x28]
    mov [lfb_addr], eax

    ;Если работаем LFB-режимом — фиксируем текущий режим в таблице.
    ;Для банковского режима таблицу не трогаем: vesa-драйвер в ядре сам
    ;откажется регистрироваться (нет LFB — нет и нашего пути)
    cmp dword [graphics_flags], 1
    jne .finish
    mov eax, [cs:mode_info + 0x28]
    test eax, eax
    jz .finish

    mov cx, [VBE_MODES_ADDR+VBE_TBL_COUNT]
    test cx, cx
    jnz .set_current

    ;Таблица пуста (fallback нашёл режим без списка) — добавляем запись
    cmp cx, VBE_MODES_MAX
    jae .finish
    xor ax, ax
    mov es, ax
    mov di, cx
    shl di, 4
    add di, VBE_MODES_ADDR+VBE_TBL_RECORDS
    mov ax, [best_mode]
    or ax, 0x4000
    mov [es:di+VBE_REC_MODE], ax
    mov ax, [cs:screen_width]
    mov [es:di+VBE_REC_XRES], ax
    mov ax, [cs:screen_height]
    mov [es:di+VBE_REC_YRES], ax
    mov al, [bpp]
    mov [es:di+VBE_REC_BPP], al
    mov byte [es:di+VBE_REC_FLAGS], VBE_REC_FLAG_LFB
    mov ax, [pitch]
    mov [es:di+VBE_REC_PITCH], ax
    mov word [es:di+VBE_REC_RESERVED], 0
    mov eax, [lfb_addr]
    mov [es:di+VBE_REC_PHYS], eax
    inc cx
    mov [VBE_MODES_ADDR+VBE_TBL_COUNT], cx
    mov [mode_count], cx
    ;добавленная запись и есть текущий режим
    mov ax, cx
    dec ax
    mov [best_index], ax

.set_current:
    ;Индекс текущего режима пишем только если он реально из таблицы:
    ;табличный путь заполнил best_index, fallback в пустую таблицу — тоже,
    ;а банковский/LFB-вне-таблицы случай оставляет 0xFFFF
    mov ax, [best_index]
    cmp ax, 0xFFFF
    je .finish
    mov [VBE_MODES_ADDR+VBE_TBL_CURRENT], ax

.finish:
    pusha
    ;Отладка - Успешно
    mov al, 'S'
    call debug_char
    mov al, 'O'
    call debug_char
    mov al, 'K'
    call debug_char
    popa

    pop ds
    pop es
    popa
    ret

.vbe_fail_info:
    mov al, 'I'
    call debug_char
    jmp .vbe_fail

.vbe_fail:
    mov al, '!'
    call debug_char
    mov si, msg_err
    mov ah, 0x0E
    mov bx, 0x0007

.err_loop:
    lodsb
    test al, al
    jz .hang
    int 0x10
    jmp .err_loop
.hang:
    cli
    hlt
    jmp .hang

;---------------------------------------------------------------
;vbe_enum_modes — перечисление всех LFB-режимов контроллера.
;Идёт по списку режимов из VbeInfoBlock (VideoModePtr), каждый проверяет
;через 4F01: поддерживается + графический + цветной + bpp 8/16/24/32 +
;PhysBasePtr != 0. Годные записи пишет в таблицу 0x5400 с флагом LFB,
;номер режима сохраняется уже с битом 0x4000.
;Счётчик ведётся в переменной enum_cnt (cs-относительной): регистр CX
;в цикле занят под номер режима для 4F01, и push/pop вокруг int 0x10
;молча откатывали бы inc обратно — таблица всегда оставалась бы пустой.
;DS внутри равен сегменту списка режимов (у реальных BIOS это ROM 0xC000),
;поэтому перед записями шапки/счётчика DS принудительно возвращается в 0:
;иначе слово счётчика уходит в тень VGA BIOS и портит чужой код.
;Все регистры и сегменты сохраняет.
;---------------------------------------------------------------
vbe_enum_modes:
    pusha
    push es
    push ds

    ;Шапка таблицы (DS ещё равен 0 — как на входе)
    mov word [VBE_MODES_ADDR+VBE_TBL_MAGIC], (VBE_TABLE_MAGIC & 0xFFFF)
    mov word [VBE_MODES_ADDR+VBE_TBL_MAGIC+2], (VBE_TABLE_MAGIC >> 16)
    mov ax, [cs:vbe_ctrl_info+4]        ;VbeVersion (BCD)
    mov [VBE_MODES_ADDR+VBE_TBL_VERSION], ax
    mov word [VBE_MODES_ADDR+VBE_TBL_COUNT], 0
    mov word [VBE_MODES_ADDR+VBE_TBL_CURRENT], 0xFFFF
    mov word [VBE_MODES_ADDR+VBE_TBL_RESERVED], 0
    mov word [cs:enum_cnt], 0

    ;DS:SI -> список режимов контроллера (VideoModePtr, offset:segment)
    mov si, [cs:vbe_ctrl_info+0x0E]     ;offset
    mov ax, [cs:vbe_ctrl_info+0x10]     ;segment
    mov ds, ax
    cld

.next_mode:
    mov cx, [cs:enum_cnt]
    cmp cx, VBE_MODES_MAX
    jae .enum_done
    lodsw
    cmp ax, 0xFFFF                      ;терминатор списка
    je .enum_done
    mov dx, ax                          ;DX = номер режима

    ;4F01 по режиму: буфер в CS:mode_info
    push si
    push ds
    mov ax, cs
    mov es, ax
    mov di, mode_info
    mov cx, dx
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .enum_skip

    ;Фильтры: режим поддерживается + графический + цветной
    mov ax, [cs:mode_info+0]
    test ax, 0x0001                     ;бит 0: supported in hardware
    jz .enum_skip
    test ax, 0x0010                     ;бит 4: graphics mode
    jz .enum_skip
    test ax, 0x0008                     ;бит 3: color mode
    jz .enum_skip

    ;Глубина цвета
    mov al, [cs:mode_info+0x19]
    cmp al, 8
    je .enum_bpp_ok
    cmp al, 16
    je .enum_bpp_ok
    cmp al, 24
    je .enum_bpp_ok
    cmp al, 32
    je .enum_bpp_ok
    jmp .enum_skip

.enum_bpp_ok:
    ;Только LFB: физический адрес фреймбуфера ненулевой
    mov eax, [cs:mode_info+0x28]
    test eax, eax
    jz .enum_skip

    ;Запись в таблицу (ES=0, DI = 0x540C + номер*16)
    xor ax, ax
    mov es, ax
    mov di, [cs:enum_cnt]
    shl di, 4
    add di, VBE_MODES_ADDR+VBE_TBL_RECORDS
    mov ax, dx
    or ax, 0x4000
    mov [es:di+VBE_REC_MODE], ax
    mov ax, [cs:mode_info+0x12]
    mov [es:di+VBE_REC_XRES], ax
    mov ax, [cs:mode_info+0x14]
    mov [es:di+VBE_REC_YRES], ax
    mov al, [cs:mode_info+0x19]
    mov [es:di+VBE_REC_BPP], al
    mov byte [es:di+VBE_REC_FLAGS], VBE_REC_FLAG_LFB
    mov ax, [cs:mode_info+0x10]
    mov [es:di+VBE_REC_PITCH], ax
    mov word [es:di+VBE_REC_RESERVED], 0
    mov eax, [cs:mode_info+0x28]
    mov [es:di+VBE_REC_PHYS], eax
    inc word [cs:enum_cnt]

.enum_skip:
    pop ds
    pop si
    jmp .next_mode

.enum_done:
    ;Записи шапки идут через DS=0: на момент выхода DS равен сегменту
    ;списка режимов (на реальных BIOS — ROM 0xC000), и запись слова
    ;счётчика попала бы в тень VGA BIOS вместо таблицы 0x5400
    xor ax, ax
    mov ds, ax
    mov cx, [cs:enum_cnt]
    mov [cs:mode_count], cx
    mov [VBE_MODES_ADDR+VBE_TBL_COUNT], cx

    pop ds
    pop es
    popa
    ret

;---------------------------------------------------------------
;vbe_pick_boot_mode — выбор режима загрузки из таблицы.
;Ищет 1024x768 в порядке приоритета bpp_prio (24 > 16 > 32 > 8).
;Находит — заполняет best_mode/best_bpp/best_index и возвращается.
;Не находит — best_mode остаётся 0 (включится fallback-перебор).
;Требует DS=0. Все регистры сохраняет.
;---------------------------------------------------------------
vbe_pick_boot_mode:
    pusha
    mov cx, [VBE_MODES_ADDR+VBE_TBL_COUNT]
    test cx, cx
    jz .pick_none

    mov si, 0                           ;индекс приоритета
.prio_loop:
    cmp si, 4
    jae .pick_none
    xor di, di                          ;индекс записи
.rec_loop:
    cmp di, cx
    jae .pick_next_prio
    mov ax, di
    shl ax, 4
    add ax, VBE_MODES_ADDR+VBE_TBL_RECORDS
    mov bx, ax                          ;BX = смещение записи
    cmp word [bx+VBE_REC_XRES], 1024
    jne .pick_next_rec
    cmp word [bx+VBE_REC_YRES], 768
    jne .pick_next_rec
    ;приоритетный bpp: 16-битная адресация не допускает base+index,
    ;поэтому адрес bpp_prio+si берём через [bx] и возвращаем BX на запись
    mov ax, bpp_prio
    add ax, si
    mov bx, ax
    mov dl, [bx]                        ;DL = приоритетный bpp
    mov ax, di
    shl ax, 4
    add ax, VBE_MODES_ADDR+VBE_TBL_RECORDS
    mov bx, ax                          ;BX = смещение записи
    cmp dl, [bx+VBE_REC_BPP]
    jne .pick_next_rec

    ;Найден режим с максимальным доступным приоритетом bpp
    mov ax, [bx+VBE_REC_MODE]
    mov [best_mode], ax
    mov al, [bx+VBE_REC_BPP]
    mov [best_bpp], al
    mov [best_index], di
    popa
    ret

.pick_next_rec:
    inc di
    jmp .rec_loop

.pick_next_prio:
    inc si
    jmp .prio_loop

.pick_none:
    popa
    ret
