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

;vbe_rm_blob.asm — машинный блоб тримплина protected mode -> real mode -> PM.
;Назначение: позволять ядру и драйверам (vesa_fb) вызывать функции BIOS
;int 10h (VBE 4F00/4F01/4F02) после включения страничной трансляции.
;
;Как это работает:
;  1. vbe_rm.c копирует блоб на физический адрес VBE_RM_BLOB_ADDR (0x4000)
;     на старте ядра (пейджинг ещё выключен, linear == physical).
;  2. C-код входит в блоб через lcall 0x08:0x4000 (CS flat, EIP = 0x4000).
;  3. Блоб сохраняет контекст (включая маски PIC — BIOS не должен ловить
;     IRQ0/IRQ1 по старым векторам IVT), выключает PG и PE, работает
;     в реальном режиме, выполняет int 10h и возвращает всё как было.
;
;Ключевой трюк с CS: 16-битный дескриптор кода лежит в GDT на индексе 128
;(селектор 0x0400) и имеет базу 0x4000. В реальном режиме трансляция CS
;считается как видимое_значение<<4 = 0x4000 — то же самое, что и в PM.
;Поэтому в момент mov cr0 (PE=0) выборка команд НЕ разрывается, и не нужен
;разрыв стека между PM и RM участками.
;
;Карта блоба (смещения от начала — фиксированные, между секциями times):
;  0x000..0x07F  32-битный вход: сохранение контекста, маскировка IRQ
;  0x080..0x0BF  16-битный PM: выключение PG и PE
;  0x0C0..0x17F  реальный режим: параметры из 0x4200, int 10h, результат в 0x4220
;  0x180..0x1C4  возврат в 32-битный PM и retf (конец кода = 0x41c5)
;Служебные ячейки (просто RAM поверх, копироваться не обязаны) лежат ЗА
;кодом возврата: 0x41D0 ESP вызывающего, 0x41D4 SS, 0x41D8/0x41D9 маски
;PIC, 0x41DC флаг IF. Прежнее размещение 0x41C0..0x41CC перекрывало хвост
;pm32_ret: запись esp/ss при входе затирала test/jz/sti/retf, и на выходе
;процессор исполнял затертые байты вместо retf (вечное скольжение по RAM)
;Параметры: vbe_rm_regs_t по 0x4200 (вход), результат по 0x4220 (выход).
;Стек реального режима: 0x7000 вниз (зона 0x6400..0x7BFF свободна — stage2
;лежит с 0x7E00, таблица e820 заканчивается до 0x6400).

BITS 32

global vbe_rm_blob_start
global vbe_rm_blob_end

section .rodata

vbe_rm_blob_start:

;-------------- 32-битный вход (EIP = 0x4000) --------------
blob_entry:
    pushfd
    pop eax
    and eax, 0x200
    mov [0x41DC], eax       ;запомнить IF вызывающего (до cli!)
    cli
    mov [0x41D4], ss
    push ds
    push es
    push fs
    push gs
    pusha
    mov eax, cr0
    push eax
    mov eax, cr3
    push eax
    ;esp сохраняем ПОСЛЕ всех пушей: он обязан указывать на дно кадра
    ;контекста (cr3), а не на адрес возврата lcall. при сохранении до пушей
    ;два первых pop на выходе съедают кадр возврата lcall, cr3/cr0 получают
    ;мусор (cr0 = CS возврата -> PE=0), а retf прыгает по чужому стеку
    mov [0x41D0], esp
    ;BIOS не должен получить прерывание, пока PIC переназначен на 0x20,
    ;а мы сидим в реальном режиме с родными векторами IVT
    in al, 0x21
    mov [0x41D8], al
    in al, 0xA1
    mov [0x41D9], al
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al
    ;в 16-битный PM (CS=0x0400: база 0x4000, byte-granular, limit 64K)
    jmp 0x0400:0x0080

times (0x80 - ($-vbe_rm_blob_start)) db 0

;-------------- 16-битный PM --------------
BITS 16
pm16:
    mov eax, cr0
    and eax, 0x7FFFFFFF      ;PG off (всё трогаемое identity-mapped)
    mov cr0, eax
    xor eax, eax
    mov cr3, eax             ;сброс TLB
    mov eax, cr0
    and eax, 0xFFFFFFFE      ;PE off -> реальный режим; CS видимое 0x0400
                             ;даёт базу 0x4000 — код течёт без разрыва
    mov cr0, eax
    jmp 0x0400:0x00C0

times (0xC0 - ($-vbe_rm_blob_start)) db 0

;-------------- реальный режим (CS=0x400: база 0x4000) --------------
real_part:
    xor ax, ax
    mov ds, ax               ;данные адресуем абсолютно через DS=0
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7000           ;свой стек в свободной младшей памяти

    ;параметры вызова из блока 0x4200
    mov ax, [0x4200]         ;AX
    mov bx, [0x4202]         ;BX
    mov cx, [0x4204]         ;CX
    mov dx, [0x4206]         ;DX
    mov di, [0x4208]         ;DI
    mov si, [0x420A]         ;SI
    mov es, [0x420C]         ;ES:DI -> буфер структуры VBE (0x430:0)
    int 0x10

    ;результаты в блок 0x4220
    mov [0x4220], ax
    mov [0x4222], bx
    mov [0x4224], cx
    mov [0x4226], dx
    mov [0x4228], di
    mov [0x422A], si
    mov [0x422C], es

    ;обратно в PM: база CS-кэша сохраняется, выборка не рвётся
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:0x4180          ;в 32-битный flat; смещение = абсолютный линейный

times (0x180 - ($-vbe_rm_blob_start)) db 0

;-------------- возврат в 32-битный PM --------------
BITS 32
pm32_ret:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ax, [0x41D4]
    mov ss, ax
    mov esp, [0x41D0]        ;PM-стек на дно кадра контекста ДО обращений к нему
    pop eax
    mov cr3, eax             ;сначала CR3, потом CR0 (PG) — иначе fault
    pop eax
    mov cr0, eax
    mov al, [0x41D8]
    out 0x21, al
    mov al, [0x41D9]
    out 0xA1, al
    popa
    pop gs
    pop fs
    pop es
    pop ds
    mov eax, [0x41DC]
    test eax, 0x200
    jz .if_off
    sti
.if_off:
    retf

vbe_rm_blob_end:
