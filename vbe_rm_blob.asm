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
;  0x0C0..0x17F  реальный режим: сетка IVT, параметры из 0x4300,
;                int 10h, результат в 0x4320
;  0x180..0x23F  возврат в 32-битный PM и retf
;  0x240..0x26F  RM-ловушка исключений IVT (beacon 'E' + iret)
;  0x270..0x28F  RM-ловушка IRQ IVT (EOI + beacon 'i' + iret)
;Служебные ячейки вынесены ЗА пределы блоба (0x4340..0x434f), чтобы запись
;esp/ss при входе физически не могла затереть код возврата:
;  0x4340 ESP вызывающего, 0x4344 SS, 0x4348/0x4349 маски PIC, 0x434C флаг IF
;Параметры: vbe_rm_regs_t по 0x4300 (вход), результат по 0x4320 (выход).
;Стек реального режима: 0x7000 вниз (зона 0x6400..0x7BFF свободна — stage2
;лежит с 0x7E00, таблица e820 заканчивается до 0x6400).
;
;Маяки отладки: на каждой стадии блоб выдаёт один символ в COM1 (0x3f8).
;Последний видимый символ в журнале — точка смерти:
;  '1' вход в блоб (32-битный PM, пейджинг включен)
;  '2' IRQ замаскированы, уход в 16-битный PM
;  '3' PG выключен (16-битный PM)
;  '4' PE выключен (реальный режим)
;  '5' сегменты/стек RM готовы
;  '6' сетка IVT установлена, параметры ещё не загружены, int 10h впереди
;  '7' BIOS вернулся, результаты сложены в 0x4320
;  '8' возврат в 32-битный PM
;  '9' CR0/CR3 восстановлены (PG снова включен)
;  'a' маски PIC восстановлены
;  'b' всё готово, retf
;  'E' в RM сработала ловушка исключения IVT (fault внутри BIOS)
;  'i' в RM сработала ловушка IRQ (прерывание просочилось сквозь маски)
;Маяк — один байт; comch ждёт готовности THR (LSR.5), символы не теряются.

BITS 32

;ловушки IVT: смещения внутри блоба (линейные адреса 0x4000+off) и
;сегмент реального режима для IVT-записей. видимое CS = 0x0400 дает
;базу 0x4000, так что 0x0400:TRAP_*_OFF указывает ровно на стабы
%define TRAP_EXC_OFF 0x0240
%define TRAP_IRQ_OFF 0x0270
%define TRAP_SEG     0x0400


%macro comch 1
    mov dx, 0x3FD
%%com_wait:
    in al, dx
    test al, 0x20
    jz %%com_wait
    mov dx, 0x3F8
    mov al, %1
    out dx, al
%endmacro

global vbe_rm_blob_start
global vbe_rm_blob_end

section .rodata

vbe_rm_blob_start:

;-------------- 32-битный вход (EIP = 0x4000) --------------
blob_entry:
    comch '1'
    pushfd
    pop eax
    and eax, 0x200
    mov [0x434C], eax       ;запомнить IF вызывающего (до cli!)
    cli
    mov [0x4344], ss
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
    mov [0x4340], esp
    ;BIOS не должен получить прерывание, пока PIC переназначен на 0x20,
    ;а мы сидим в реальном режиме с родными векторами IVT
    in al, 0x21
    mov [0x4348], al
    in al, 0xA1
    mov [0x4349], al
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al
    comch '2'
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
    comch '3'
    mov eax, cr0
    and eax, 0xFFFFFFFE      ;PE off -> реальный режим; CS видимое 0x0400
                             ;даёт базу 0x4000 — код течёт без разрыва
    mov cr0, eax
    comch '4'
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
    comch '5'

    ;защитная сетка IVT перед int 10h: BIOS внутри int 10h сам включает
    ;IF (seaBIOS entry_10 делает sti), а векторы переназначенных IRQ
    ;0x20-0x2F BIOS никогда не заполнял — там лежит мусор младшей памяти
    ;(остатки данных ядра/загрузчика). просочившийся IRQ исполняет эти
    ;данные как код: мусорные байты в COM1, затем jmp $ — ровно то, что
    ;видно в журнале после маяка '6'. развешиваем ловушки: исключения
    ;0x00-0x0F -> beacon 'E' + iret, IRQ 0x20-0x2F -> EOI + beacon 'i'
    ;+ iret. вектор 0x10 не трогаем — это сам int 10h
    mov di, 0x00
    mov cx, 0x10
.rm_ivt_exc:
    mov word [di], TRAP_EXC_OFF      ;offset ловушки
    mov word [di+2], TRAP_SEG        ;segment 0x0400 (база 0x4000)
    add di, 4
    loop .rm_ivt_exc
    mov di, 0x80                     ;вектор 0x20*4 = 0x80
    mov cx, 0x10
.rm_ivt_irq:
    mov word [di], TRAP_IRQ_OFF
    mov word [di+2], TRAP_SEG
    add di, 4
    loop .rm_ivt_irq

    comch '6'
    ;параметры вызова из блока 0x4300
    mov ax, [0x4300]         ;AX
    mov bx, [0x4302]         ;BX
    mov cx, [0x4304]         ;CX
    mov dx, [0x4306]         ;DX
    mov di, [0x4308]         ;DI
    mov si, [0x430A]         ;SI
    mov es, [0x430C]         ;ES:DI -> буфер структуры VBE
    int 0x10

    ;результаты в блок 0x4320
    mov [0x4320], ax
    mov [0x4322], bx
    mov [0x4324], cx
    mov [0x4326], dx
    mov [0x4328], di
    mov [0x432A], si
    mov [0x432C], es
    comch '7'

    ;обратно в PM: база CS-кэша сохраняется, выборка не рвётся
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:0x4180          ;в 32-битный flat; смещение = абсолютный линейный

times (0x180 - ($-vbe_rm_blob_start)) db 0

;-------------- возврат в 32-битный PM --------------
BITS 32

pm32_ret:
    comch '8'
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ax, [0x4344]
    mov ss, ax
    mov esp, [0x4340]        ;PM-стек на дно кадра контекста ДО обращений к нему
    pop eax
    mov cr3, eax             ;сначала CR3, потом CR0 (PG) — иначе fault
    pop eax
    mov cr0, eax
    comch '9'
    mov al, [0x4348]
    out 0x21, al
    mov al, [0x4349]
    out 0xA1, al
    comch 'a'
    popa
    pop gs
    pop fs
    pop es
    pop ds
    mov eax, [0x434C]
    test eax, 0x200
    jz .if_off
    sti
.if_off:
    comch 'b'
    retf

;-------------- RM-ловушки IVT (защитная сетка) --------------
;вызываются только из реального режима, когда просочился IRQ или BIOS
;поймал исключение. CS при входе = 0x0400 (база 0x4000), стек любой —
;push/pop работают с текущим SS. EOI обоим PIC unconditional: для
;исключений это безопасно, для spurious IRQ7/15 — обязательная часть
;обработки. все потревоженные регистры сохраняются: BIOS не должен
;заметить ловушку

times (TRAP_EXC_OFF - ($-vbe_rm_blob_start)) db 0
BITS 16
rm_trap_exc:
    push ax
    push dx
    mov al, 0x20
    out 0xA0, al             ;EOI и здесь: векторы 0x08-0x0F — это еще и
    out 0x20, al             ;legacy IRQ0-7, если базу PIC кто-то вернул
    comch 'E'                ;маяк: исключение поймано ловушкой
    pop dx
    pop ax
    iret

times (TRAP_IRQ_OFF - ($-vbe_rm_blob_start)) db 0
rm_trap_irq:
    push ax
    push dx
    mov al, 0x20
    out 0xA0, al             ;EOI slave сначала (если IRQ пришёл от него)
    out 0x20, al             ;EOI master
    comch 'i'                ;маяк: IRQ просочился сквозь маски
    pop dx
    pop ax
    iret


vbe_rm_blob_end:
