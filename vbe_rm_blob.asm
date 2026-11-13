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

;vbe_rm_blob.asm — машинный блоб трамплина protected mode -> real mode -> PM.
;Назначение: позволять ядру и драйверам (vesa_fb) вызывать функции BIOS int 10h
;(VBE 4F00/4F01/4F02) после включения страничной трансляции.
;
;Как это работает:
;  1. vbe_rm.c копирует блоб на физический адрес VBE_RM_BLOB_ADDR (0x4000) на старте ядра
;     (пейджинг ещё выключен, linear == physical).
;  2. C-код входит в блоб через lcall 0x08:0x4000 (CS flat, EIP = 0x4000), предварительно
;     сохраняя текущий IDTR ядра в ячейку 0x4360 и кладя RM-указатель IDT {limit 0x3FF, base 0}
;     в ячейку 0x4368.
;  3. Блоб сохраняет контекст, выключает PG и PE и в реальном режиме переключает IDTR НА IVT
;  4. PIC переконфигурируется на легаси-базу 0x08/0x70, как его оставил BIOS на POST
;    (тиковые ожидания внутри BIOS рассчитаны на родную раскладку векторов),
;     разрешены только IRQ0 и IRQ1 — тик таймера и клавиатура: BIOS int 09h в RM-окне опорожняет
;     OBF 8042
;  5. int 10h выполняется с разрешенными прерываниями, затем PIC и IDTR возвращаются
;     на ядерные значения и блоб уходит обратно в PM.
;
;Ключевой трюк с CS: 16-битный дескриптор кода лежит в GDT на индексе 128 (селектор 0x0400) и имеет базу 0x4000.
;В реальном режиме трансляция CS считается как видимое_значение<<4 = 0x4000 — то же самое, что и в PM.
;Поэтому в момент mov cr0 (PE=0) выборка команд не разрывается, и не нужен разрыв стека между PM и RM участками.
;
;Карта блоба (смещения от начала — фиксированные, между секциями times):
;  0x000..0x07F  32-битный вход: сохранение контекста, маскировка IRQ
;  0x080..0x0BF  16-битный PM: выключение PG и PE
;  0x0C0..0x1FF  реальный режим: IDTR->IVT, PIC->легаси, параметры из
;                0x4300, int 10h, результат в 0x4320, PIC->0x20/0x28,
;                IDTR->PM IDT ядра, экранные маяки стадий (teletype)
;  0x200..0x263  возврат в 32-битный PM и retf
;Служебные ячейки вынесены за пределы блоба (0x4340..0x436D), чтобы записи esp/ss при входе физически не могли затереть код возврата:
;  0x4340 ESP вызывающего, 0x4344 SS, 0x4348/0x4349 маски PIC, 0x434C флаг IF
;  0x4360 сохраненный IDTR ядра (6 байт), 0x4368 RM-указатель IDT (6 байт)
;  0x4370 сохраненный GDTR ядра (6 байт sgdt, 0x4376..0x4377 паддинг)
;Параметры: vbe_rm_regs_t по 0x4300 (вход), результат по 0x4320 (выход).
;Стек реального режима: 0x7000 вниз (зона 0x6400..0x7BFF свободна — stage2
;лежит с 0x7E00, таблица e820 заканчивается до 0x6400).
;
;Маяки отладки: на каждой стадии блоб выдаёт один символ в COM1 (0x3f8).
;  '1' вход в блоб (32-битный PM, пейджинг включен)
;  '2' IRQ замаскированы, уход в 16-битный PM
;  '3' PG выключен (16-битный PM)
;  '4' PE выключен (реальный режим)
;  '5' сегменты/стек RM готовы
;  '6' IDTR->IVT и PIC->легаси-база сделаны, int 10h впереди
;  '7' BIOS вернулся, результаты сложены в 0x4320
;  '8' возврат в 32-битный PM
;  '9' CR0/CR3 восстановлены (PG снова включен)
;  'a' маски PIC восстановлены
;  'b' всё готово, retf
;Маяк — один байт; comch ждёт готовности THR (LSR.5), символы не теряются.
;
;Экранные маяки стадий: каждый вызов блоба печатает на экран символы
;'0'..'3' через BIOS teletype (scrmk выше) — отладка на машинах без COM:
;  '0' параметры на месте, int 10h впереди
;  '1' BIOS вернулся из int 10h
;  '2' VBE успех (AX=0x004F)
;  '3' VBE отказ (AX != 0x004F)
;все маяки и сам вызов идут с IF=1 (sti перед маяком '0', cli только после
;'2'/'3'): teletype в графическом режиме на реальных машинах зовет int 15h
;AH=86h (панельные задержки), и с закрытыми прерываниями то ожидание висит;
;позиция символа — текущий курсор BIOS, смена режима может его сдвинуть;
;нет ни одного символа — висим до входа в RM (вход/выход блоба, PG/PE)

BITS 32

;comch ждет готовности THR не дольше 65536 опросов LSR: на машине без
;рабочего UART на 0x3F8 (заглушен в BIOS-setup) LSR читается нулями,
;и вечный цикл вешал бы сам трамплин еще до вызова BIOS
%macro comch 1
    mov dx, 0x3FD
    xor ecx, ecx
%%com_wait:
    in al, dx
    test al, 0x20
    jnz %%com_send
    loop %%com_wait
%%com_send:
    mov dx, 0x3F8
    mov al, %1
    out dx, al
%endmacro

;Экранный маяк стадии:
;BIOS teletype AH=0Eh, символ — параметр макроса.
;символ кладется в AL ПОСЛЕ pusha: в момент входа AL/AH — часть AX вызова VBE (4F02/4F03/4F01 — AL это номер функции),
;а на стадии 1 AX — уже готовый результат, его нельзя портить до записи в блок 0x4320.
;Работает в любом режиме экрана (текст — символ в буфер, графика — глиф шрифта VBIOS).
;только штатный вызов BIOS: прямой рисунок в LFB из реального режима невозможен —
;сегменты в RM имеют лимит 64 КБ, запись по адресу LFB (0xF0000000+) дает #GP и хаос через IVT
%macro scrmk 1
    pusha
    mov al, %1
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    popa
%endmacro

global vbe_rm_blob_start
global vbe_rm_blob_end

section .rodata

vbe_rm_blob_start:

;-------------- 32-битный вход (EIP = 0x4000) --------------
blob_entry:
    ;comch '1'
    pushfd
    pop eax
    and eax, 0x200
    mov [0x434C], eax       ;запомнить IF вызывающего (до cli)
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
    ;esp сохраняем после всех пушей: он обязан указывать на дно кадра
    ;контекста (cr3), а не на адрес возврата lcall. при сохранении до пушей
    ;два первых pop на выходе съедают кадр возврата lcall, cr3/cr0 получают
    ;мусор (cr0 = CS возврата -> PE=0), а retf прыгает по чужому стеку
    mov [0x4340], esp
    ;GDTR ядра обязан пережить RM-окно: BIOS внутри int 10h (teletype в
    ;графическом режиме / programming DISPI в VBE mode set) ходит через
    ;собственный PM-трамплин и после него GDTR указывает на BIOS-овский GDT.
    ;без этого sgdt возврат в PM по ljmp $0x08 ищет селектор 0x08
    ;в чужой GDT и получает #GP(0x8) -> тройной фолт -> "hard reboot"
    ;(поймано в QEMU: v=0d e=0008 IP=0400:0x41f2 — ровно на ljmp)
    sgdt [0x4370]
    ;BIOS не должен получить прерывание, пока PIC переназначен на 0x20,
    ;а мы сидим в реальном режиме с родными векторами IVT
    in al, 0x21
    mov [0x4348], al
    in al, 0xA1
    mov [0x4349], al
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al
    ;comch '2'
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
    ;comch '3'
    mov eax, cr0
    and eax, 0xFFFFFFFE      ;PE off -> реальный режим; CS видимое 0x0400
                             ;даёт базу 0x4000 — код течёт без разрыва
    mov cr0, eax
    ;comch '4'
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
    ;строго 32-битная запись стека (66 BC):
    ;после PM в ESP сидят старшие 16 бит адреса PM-стека ядра.
    ;mov sp,imm16 обновил бы только младшую половину — ESP остался бы вида 0x00177000,
    ;и все 32-битные push/pop внутри BIOS (SeaVGABIOS int10 построен на
    ;66-префиксных операциях) уходили бы в память ядра выше 1 МБ:
    ;стек расщепляется, plain-путь entry_10 получает мусорный bregs
    ;(AH=0 -> 40x25 текстовый режим, "маленький экранчик с артефактами"
    ;результат вызова наружу не доезжает — эхо res==req и GR_ERR_IO), а extra-stack путь берет
    ;код возврата из чужой памяти и iret прыгает в мусор (observed cs=bbd1 ip=0000 в debugcon)
    ;до "In resume / hard reboot".
    mov esp, 0x7000          ;свой стек в свободной младшей памяти
    ;comch '5'

    ;IDTR обязан указывать на IVT (0:0x3FF) ДО первого int: в реальном
    ;режиме на 286+ векторы диспетчируются через IDT-механизм, и IDTR
    ;после загрузки PM IDT ядра указывал на шлюзы ядра. без этого lidt
    ;int 10h уходит в 32-битный стаб isr16 с селектором 0x08: в RM
    ;селектор 0x08 дает базу 0x80, стаб исполняется как 16-битный мусор
    ;и CPU бесконечно ловит #UD — снаружи висение без маяков '7'/'E'
    lidt [0x4368]

    ;PIC переконфигурируется на легаси-базу 0x08/0x70, как его оставил BIOS на POST:
    ;тиковые ожидания внутри BIOS-кода рассчитаны на родную раскладку векторов (IRQ0 -> int 08h).
    ;IRQ0 и IRQ1 разрешены, остальные под маской — дискета ядру не нужна, пока идет смена
    ;режима, а IRQ1 нужен: BIOS int 09h опорожняет OBF 8042, скан-код, пришедший в окно смены режима, иначе блокирует контроллер
    mov al, 0x11
    out 0x20, al
    out 0xA0, al
    mov al, 0x08
    out 0x21, al
    mov al, 0x70
    out 0xA1, al
    mov al, 0x04
    out 0x21, al
    mov al, 0x02
    out 0xA1, al
    mov al, 0x01
    out 0x21, al
    out 0xA1, al
    mov al, 0xF8             ;маска: IRQ0 (тик), IRQ1 (клавиатура — BIOS
                             ;int 09h опорожняет OBF 8042) и IRQ2 (каскад
                             ;к ведомому), остальные под маской
    out 0x21, al
    mov al, 0xFE             ;IRQ8 (RTC) открыт: реальные VBIOS внутри смены
                             ;режима зовут int 15h AH=86h (микросекундные
                             ;задержки панели ноутбука) — тот ждет периодики
                             ;RTC через ведомый PIC, и без каскада+IRQ8
                             ;ожидание не кончается никогда: BIOS виснет
                             ;прямо в int 10h (QEMU/86Box это не ловят)
    out 0xA1, al

    ;comch '6'
    ;параметры вызова из блока 0x4300
    mov ax, [0x4300]         ;AX
    mov bx, [0x4302]         ;BX
    mov cx, [0x4304]         ;CX
    mov dx, [0x4306]         ;DX
    mov di, [0x4308]         ;DI
    mov si, [0x430A]         ;SI
    mov es, [0x430C]         ;ES:DI -> буфер структуры VBE
    ;SeaVGABIOS (QEMU/86Box/Bochs): vbe_104f02 -> vga_set_mode после каждого
    ;успешного VBE-сета гасит бит BF_EXTRA_STACK (0x40) в BDA 0x4B9 — эвристика
    ;"modern OS" (vgabios.c: MASK_BDA_EXT(flags, BF_EXTRA_STACK, MF_LEGACY?..)).
    ;Бит сброшен уже загрузочным 4F02 stage2, поэтому все последующие int10
    ;уходят из entry_10_extrastack в plain-путь entry_10:
    ;обработчик исполняется на стеке вызывающего, и результаты вызова наружу не
    ;доезжают — регистры возвращаются как были (эхо res==req), драйвер
    ;видит "4F02 refused" и отдаёт GR_ERR_IO. Перед каждым int10 бит
    ;восстанавливается через OR: младший нибл 0x4B9 — BF_PM_MASK (маски
    ;PIC), его трогать нельзя.
    ;Безопасность на чужих VBIOS: 0x4B9 — нестандартная ячейка, реальный
    ;VBIOS (S3 Trio64 в 86box и т.п.) может хранить в ней своё. Бит
    ;ставится ТОЛЬКО если вектор int10 указывает на entry_10_extrastack
    ;SeaVGABIOS — проверяется сигнатура пролога (cli;cld;push ds;push eax;
    ;mov ax,0x40;mov ds,ax;test byte [0xB9],0x40).
    ;Вектор указывает на entry_10_extrastack только когда BIOS реально выделил extra stack
    ;(vgainit.c: allocate_extra_stack), так что путь по биту всегда
    ;работоспособен.
    ;Несовпадение сигнатуры — ничего не трогаем.
    push es
    push si
    push ax                  ;AX уже содержит номер функции VBE
    mov ax, [0x42]           ;сегмент вектора int 10h
    mov es, ax
    mov si, [0x40]           ;смещение вектора int 10h
    cmp word [es:si+0x00], 0xFCFA    ;cli; cld
    jne .bf_done
    cmp word [es:si+0x02], 0x661E    ;push ds; [66 50] push eax
    jne .bf_done
    cmp word [es:si+0x06], 0x0040    ;mov ax,0x40 (b8 40 00)
    jne .bf_done
    cmp word [es:si+0x08], 0xD88E    ;mov ds,ax (8e d8)
    jne .bf_done
    cmp word [es:si+0x0A], 0x06F6    ;test byte [0xB9],.. (f6 06 b9 00 40)
    jne .bf_done
    cmp word [es:si+0x0C], 0x00B9
    jne .bf_done
    cmp byte [es:si+0x0E], 0x40
    jne .bf_done
    mov al, [0x4B9]
    or al, 0x40
    mov [0x4B9], al
.bf_done:
    pop ax                   ;номер функции VBE восстановлен
    pop si
    pop es                   ;ES:DI на буфер VBE восстановлен
    ;BIOS и teletype-маяки работают с разрешенными прерываниями: тики BDA
    ;(0x40:6C) обязаны идти, иначе тиковые ожидания внутри BIOS длятся вечно.
    ;sti обязан стоять до маяка '0': teletype в графическом режиме на реальных
    ;машинах (панель ноутбука) вставляет задержки через int 15h AH=86h, и
    ;с IF=0 периодика RTC не доходит, ожидание не кончается — маяк '0'
    ;печатался и все висло, хотя до основного вызова дело не дошло
    sti
    ;маяк стадии 0 — int 10h впереди; символ '0' на экране
    scrmk '0'
    int 0x10
    ;маяк стадии 1 — BIOS вернулся из int 10h; AX — готовый результат,
    ;pusha/popa внутри маяка защищают его до записи в блок 0x4320
    scrmk '1'

    ;результаты в блок 0x4320
    mov [0x4320], ax
    mov [0x4322], bx
    mov [0x4324], cx
    mov [0x4326], dx
    mov [0x4328], di
    mov [0x432A], si
    mov [0x432C], es
    comch '7'
    ;маяк стадии 2 = VBE успех (AX=0x004F), стадии 3 = отказ BIOS:
    ;по последнему символу на экране видно, дошел ли вызов и чем кончился
    mov ax, [0x4320]
    cmp ax, 0x004F
    mov al, '2'
    je .beacon_res
    mov al, '3'
.beacon_res:
    scrmk al                 ;символ уже в AL ('2'/'3'), регистры мертвы
    ;cli только здесь, после всех обращений к BIOS: маяки '0'..'3' и запись
    ;результатов идут с IF=1, как и сам вызов int 10h; ICW-последовательность
    ;PIC ниже наоборот обязана идти с закрытыми прерываниями
    cli

    ;PIC обратно на базу ядра 0x20/0x28, все IRQ под маской; точную
    ;маску ядра вернет pm32_ret из ячеек 0x4348/0x4349
    mov al, 0x11
    out 0x20, al
    out 0xA0, al
    mov al, 0x20
    out 0x21, al
    mov al, 0x28
    out 0xA1, al
    mov al, 0x04
    out 0x21, al
    mov al, 0x02
    out 0xA1, al
    mov al, 0x01
    out 0x21, al
    out 0xA1, al
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al

    ;IDTR обратно на PM IDT ядра — ДО обратного включения PE, чтобы
    ;любое прерывание/исключение в PM диспетчировалось штатно
    lidt [0x4360]

    ;GDTR обратно на GDT ядра — ДО ljmp $0x08: BIOS внутри RM-окна
    ;переключал GDTR на свой, и возврат в PM по селектору 0x08 обязан
    ;идти по родной таблице (см. sgdt на входе блоба)
    lgdt [0x4370]

    ;обратно в PM: база CS-кэша сохраняется, выборка не рвётся
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:0x4200          ;в 32-битный flat; смещение = абсолютный линейный

;экранные маяки стадий печатает scrmk (макрос выше):
;символы '0'..'3' через BIOS teletype видны в текстовом и графическом режиме.
;Прямой рисунок в LFB из блоба невозможен в принципе: в реальном режиме сегменты
;имеют лимит 64 КБ, запись по адресу LFB дает #GP — именно это ломало
;смену режима: маяк стадии 0 выполнялся перед каждым int 10h и первой же записью в LFB уходил в #GP/IVT

times (0x200 - ($-vbe_rm_blob_start)) db 0

;-------------- возврат в 32-битный PM (физический 0x4200) --------------
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
    mov esp, [0x4340]        ;PM-стек на дно кадра контекста до обращений к нему
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

vbe_rm_blob_end:

