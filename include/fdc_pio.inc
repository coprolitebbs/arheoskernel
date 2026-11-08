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

; ============================================================
; fdc_pio.inc — поллинговый драйвер НГМД для stage2 (флоппи).
;
; ЗАЧЕМ: на TA-NC529 (Geode GX1 300МГц + CS5530) BIOS внутри
; int 13h ждёт прерывание FDC (IRQ6) без таймаута и зависает
; навсегда (раньше это выглядело как "!80" = AH=0x80 — таймаут
; BIOS, а на других фазах зависание без возврата). Поэтому
; stage2 больше НЕ использует int 13h: чтение сектора делает
; этот драйвер — опрос MSR (порт 0x3F4), без DMA и без
; зависимости от IRQ6 (на время операции IRQ6 маскируется в
; PIC, чтобы BIOS-обработчик int 0Eh не съел результаты).
;
; Все ожидания ограничены программными таймаутами — драйвер
; не может зависнуть ни при каком состоянии контроллера.
; При ошибке печатается диагностика:
;   'f' + код фазы + MSR (hex) — на каждой повторной попытке;
;   'F' + фаза + ST0 ST1 ST2 (hex) + 'M' + MSR (hex) — когда попытки
;   исчерпаны. Финальный дамп печатается ДО fdc_reset: сброс затирает
;   фазу и регистры состояния. MSR — последний прочитанный Main
;   Status Register на момент таймаута: 0x10..0x30 = командная фаза,
;   0x50 = ждёт чтения байта, 0x70 = есть байт результата,
;   0x80 = простой (RQM=1, DIO=0), 0x90 = готов приём команды,
;   0xD0 = исполняет команду (данные с носителя не идут).
;   'r0'/'r2' из bpb_init — проба скорости 500/250 кбит/с.
;   'B' — FDC на машине нет (шина 0x3F4 читается 0xFF, напр. загрузка
;   с USB-FDD) либо PIO так и не смог: чтение переключено на BIOS
;   int 13h и останется там до конца загрузки. ПЕРЕД первой BIOS-
;   попыткой драйвер выдаёт SPECIFY с ND=0: наш fdc_reset ставит
;   ND=1 (non-DMA), а BIOS читает через DMA — с ND=1 на CS5530
;   передача в память идёт битой (см. «ИСТОРИЯ БАГА RECAL» ниже).
; Коды фаз: 1=готовность после reset, 2=SPECIFY, 3=SENSE INT,
; 4=RECALIBRATE, 5=SEEK, 6=команда READ, 7=фаза данных,
; 8=фаза результатов, 9=проверка.
; Дополнительные маркеры:
;   'T'+hex — READ завершился ДО 512 байт (цифра = сколько байт данных
;   реально принято); дальше печатается НАСТОЯЩИЙ результат F<фаза>...
;   'd' — байт данных не пришёл за таймаут, но результат появился
;   сразу после него (гонка завершения); ST0/ST1/ST2 вычитаны и
;   попадут в финальный дамп.
;   'N'+hex+дамп — fat12_find_file не нашёл файл: hex = офсет
;   остановки скана (32*индекс записи; 0xE00 = все 224 просмотрены),
;   дамп — первые 11 байт корневого каталога.
;   'Z' — корневой каталог перечитан с диска (повторная попытка).
;   'a'/'b'/'c'/'p' — ПОШАГОВЫЙ след щупа скоростей (только при
;   fdc_probe=1): a = fdc_reset прошёл, b = RECALIBRATE прошёл,
;   c = SEEK прошёл, p = команда READ выдана, ждём данные. Если на
;   реальном железе лог снова обрывается МОЛЧА, обрыв между этими
;   маркерами сразу указывает шаг.
;   ВНИМАНИЕ: запись в DSR (0x3F4 на запись) УБРАНА. Она появилась
;   одной из последних («дубль CCR»), и ровно сборка с ней на Geode
;   (TA-NC529/CS5530) впервые умерла МОЛЧА сразу после 'r0' — до
;   первого дампа. Все предыдущие сборки (без записи в 0x3F4)
;   печатали 'f7'/'F7' по каждой пробе, т.е. CPU жил и порты читались.
;   Запись в порт не может «отказать» тихо: если шина/SMI-ловушка на
;   запись 0x3F4 не завершает цикл, CPU стоит ВНУТРИ out навсегда.
;   Поэтому скорость задаём ТОЛЬКО через CCR (0x3F7), как во всех
;   сборках, печатавших дампы.
;
; ============================================================
; ИСТОРИЯ БАГА RECAL (Geode GX1 + CS5530, лог 'f4 81'/'F4 800000 M81')
; ============================================================
;   На реальной машине ЛЮБОЙ PIO-доступ падал на RECALIBRATE:
;   по каждой пробе скорости 'f4 81' (фаза 4, MSR=0x81) и финал
;   'F4 800000 M81' (ST0=0x80 INVALID — «висячих» результатов нет,
;   ST1/ST2=00 — ни одного READ ещё не было, MSR=0x81 — чип спокоен,
;   байт ждёт). Причина: старый fdc_wait_seek_done считал ОДИН
;   неудачный опрос SENSE INTERRUPT фатальным (jc .wsd_fail на
;   первом же CF). Пока recal физически двигает голову (сотни мс —
;   stage1/BIOS оставляют её на цилиндрах 1-13+), чип на CS5530 НЕ
;   отвечает на SENSE INTERRUPT вообще (ни валидным ST0, ни
;   INVALID): fdc_in упирается в таймаут, sense_int возвращает CF,
;   и recal «проваливался», не успев начаться. QEMU/86Box завершают
;   recal мгновенно, поэтому там ошибки не было.
;   Дальше было хуже: после трёх «проваленных» проб включался
;   фолбэк на BIOS ('B'), НО к этому моменту наш fdc_reset успел
;   запрограммировать SPECIFY с ND=1 (non-DMA). BIOS читает дискету
;   ЧЕРЕЗ DMA; с ND=1 на CS5530 передача в память идёт битой —
;   в буферах FAT/корня оставались «первый байт сектора верный,
;   дальше мусор» (дампы F9 80 57 C0 / 46 20 45 20 45 ... вместо
;   F9 FF FF 03 / F12    LD ), файлы находились через раз, а
;   перечитывание корня ('Z') падало с ошибкой ('e' + "Disk error").
;   При этом сам stage1 через ЧИСТЫЙ BIOS (маркеры SIFRXLS/Sl)
;   читал тот же диск идеально — дискеты были в порядке.
;   ИСПРАВЛЕНИЕ:
;     1) fdc_wait_seek_done опрашивает SENSE INTERRUPT в цикле до
;        дедлайна по тикам BIOS (~2.5 c): CF и INVALID = «ещё
;        работает», не фатально; пауза между опросами fdc_settle.
;     2) .rl_bios_fb перед первой BIOS-попыткой возвращает
;        контроллер в DMA-режим (SPECIFY c ND=0).
; ============================================================

BITS 16

FDC_DOR     equ 0x3F2           ; Digital Output Register
FDC_MSR     equ 0x3F4           ; Main Status Register (чтение)
                                ; (запись в 0x3F4 = DSR — НЕ используем:
                                ;  на TA-NC529/CS5530 сборка с этой записью
                                ;  умирала молча сразу после 'r0', см.
                                ;  шапку файла)
FDC_FIFO    equ 0x3F5           ; FIFO / данные
FDC_CCR     equ 0x3F7           ; скорость передачи (запись)

; ============================================================
; FDC_IOWAIT — задержка восстановления шины I/O (~1-2 мкс).
; ПОЧЕМУ НУЖНА: на реальном железе (Geode GX1 300МГц + CS5530)
; чтение MSR СРАЗУ после записи в FIFO возвращает СТАРОЕ
; значение RQM (чип ещё не успел сбросить бит) — драйвер считал
; порт готовым и писал следующий байт, который FDC молча
; терял. RECALIBRATE (2 байта) после этого вечно ждал второй:
; в логе 'f4 81' / 'F4 800000 M81' (MSR застрял на 0x81 —
; командная фаза, ждёт байт). На QEMU/86Box циклы I/O
; синхронны, поэтому там ошибки не было.
; Запись в порт 0x80 (POST) безвредна и занимает полный цикл
; ISA-шины на классических чипсетах.
; ============================================================
%macro FDC_IOWAIT 0
    out 0x80, al
    out 0x80, al
%endmacro

; ============================================================
; FDC_ACKRQM — ПОДТВЕРЖДЕНИЕ ПРИЁМА/ВЫДАЧИ БАЙТА FIFO.
; После каждого обмена с FIFO контроллер должен СНЯТЬ бит RQM
; (латентность сотни нс — единицы мкс). Фиксированная пауза
; FDC_IOWAIT (2 out 0x80) оказалась на грани: на реальном Geode
; (GX1 + CS5530) следующий опрос MSR всё ещё видел УСТАРЕВШИЙ
; RQM=1, и байты команд терялись/дублировались: RECALIBRATE
; падал с 'f4'/'F4 800000 M81' (PH_RECAL, ST0=0x80 INVALID,
; MSR=0x81 — контроллер в покое). Детерминированное решение —
; после обмена ждать ФАКТИЧЕСКОГО сброса RQM (с ограничением
; числа итераций на случай сверхбыстрых/нестандартных чипов,
; у которых RQM не видно в нуле):
;   while (--bound) { if (!(MSR & RQM)) break; IOWAIT; }
; На QEMU/86Box RQM снимается сразу — цикл выходит за 1-2
; итерации, поведение не меняется.
; ВАЖНО: макрос портит AL и сохраняет CX/DX сам.
; ============================================================
%macro FDC_ACKRQM 0
    push cx
    push dx
    mov  dx, FDC_MSR
    mov  cx, 2000
%%.ack:
    in   al, dx
    test al, 0x80               ; RQM ещё поднят?
    loopnz %%.ack               ; крутим, пока RQM=1 и не исчерпан лимит
    FDC_IOWAIT
    pop  dx
    pop  cx
%endmacro

MSR_RQM     equ 0x80            ; готов к обмену байтом
MSR_DIO     equ 0x40            ; 1 = FDC -> CPU

; Таймауты: единица = 65535 чтений MSR. Чтение ISA-порта
; ~0.15-1 мкс при 100-300 МГц => единица ~ 10-60 мс.
TMO_CMD     equ 60              ; завершение seek/recal/read ~1-4 c
TMO_READY   equ 60              ; готовность после reset
TMO_BYTEC   equ 16              ; байт командной/результирующей фазы
TMO_BYTED   equ 8               ; байт фазы данных

; Коды фаз для диагностики
PH_READY    equ 1
PH_SPECIFY  equ 2
PH_SENSED   equ 3
PH_RECAL    equ 4
PH_SEEK     equ 5
PH_READCMD  equ 6
PH_DATA     equ 7
PH_RESULT   equ 8
PH_VERIFY   equ 9

; --- переменные драйвера ---
fdc_unit        db 0            ; номер устройства (drive & 3)
fdc_motor_up    db 0            ; мотор раскручен
fdc_calibrated  db 0            ; позиция головки известна
fdc_cyl_cur     db 0xFF         ; текущий цилиндр (0xFF = неизвестен)
fdc_ph          db 0            ; текущая фаза (диагностика)
fdc_st0         db 0
fdc_st1         db 0
fdc_st2         db 0
fdc_msr_last    db 0            ; MSR на момент последнего таймаута
fdc_rc          db 0            ; результат: C
fdc_rr          db 0            ; результат: R
fdc_C           db 0            ; целевые C/H/R
fdc_H           db 0
fdc_R           db 0
fdc_stmp        db 0            ; целевой цилиндр для seek
fdc_pic         db 0            ; сохранённая маска PIC
fdc_attempts    db 0
fdc_max_attempts db 3           ; попыток на чтение; bpb_init на время
                                ; проб скорости временно ставит 1
fdc_inited      db 0            ; контроллер уже переведён в наш режим
fdc_ccr         db 0x00         ; код скорости: 0=500 кбит/с, 2=250 кбит/с
fdc_ccr_force   db 1            ; 1 = программируем CCR при каждом reset
fdc_probed      db 0            ; щуп шины 0x3F4 уже делали
fdc_use_bios    db 0            ; 1 = FDC нет/умер, читаем через BIOS
fdc_probe       db 0            ; 1 = щуп скоростей: BIOS-фолбэк запрещён
fdc_probe_ok    db 0            ; 1 = PIO доказал скорость в щупе (bpb_init)
fdc_dst_seg     dw 0            ; приёмник сектора
fdc_dst_off     dw 0

; ------------------------------------------------------------
; fdc_drv_init: вызвать один раз при старте stage2
; ------------------------------------------------------------
fdc_drv_init:
        pusha
        mov al, [cs:drive]
        and al, 0x03
        mov [cs:fdc_unit], al
        popa
        ret

; ------------------------------------------------------------
; fdc_read_lba: AX = LBA, ES:BX = приёмник. Выход: CF.
; Все регистры сохраняются. Заменяет BIOS int 13h AH=02.
; ------------------------------------------------------------
fdc_read_lba:
        pusha
        push ds
        push es

        ; LBA -> CHS (геометрия из BPB, заполненного bpb_init)
        xor dx, dx
        div word [cs:spt]       ; AX = трек, DX = сектор-1
        inc dx
        mov [cs:fdc_R], dl
        xor dx, dx
        div word [cs:heads]     ; AX = цилиндр, DX = головка
        mov [cs:fdc_H], dl
        mov [cs:fdc_C], al

        mov [cs:fdc_dst_seg], es
        mov [cs:fdc_dst_off], bx

        ; режим BIOS уже включён (FDC нет на машине)?
        cmp byte [cs:fdc_use_bios], 0
        jne .bios_path

        ; Одноразовый щуп шины 0x3F4: два чтения подряд возвращают
        ; 0xFF — легаси-FDC не декодируется (CF-18/USB-FDD и т.п.).
        ; Весь диск тогда читаем через BIOS int 13h, как в старых
        ; сборках; на машинах с живым FDC щуп прозрачен.
        cmp byte [cs:fdc_probed], 0
        jne .probe_done
        mov byte [cs:fdc_probed], 1
        mov dx, FDC_MSR
        in  al, dx
        mov bl, al
        in  al, dx
        cmp al, bl
        jne .probe_done         ; значение дрожит — FDC живой
        cmp al, 0xFF
        jne .probe_done
        mov byte [cs:fdc_use_bios], 1
        pusha
        mov al, 'B'
        call debug_char
        popa
.bios_path:
        call fdc_bios_read
        pop es
        pop ds
        popa
        ret
.probe_done:

        ; IRQ6 маскируем на время операции: прерывание нам не
        ; нужно, а BIOS-обработчик int 0Eh не должен трогать
        ; результаты команды
        in  al, 0x21
        mov [cs:fdc_pic], al
        or  al, 0x40
        out 0x21, al

        mov al, [cs:fdc_max_attempts]
        mov [cs:fdc_attempts], al
.rl_try:
        ; не даём BIOS выключить мотор по своему таймеру
        push ds
        mov ax, 0x0040
        mov ds, ax
        mov byte [0x0040], 0xFF     ; счётчик motor-off = макс
        pop ds

        ; первый доступ: BIOS оставил контроллер в СВОЁМ режиме
        ; (SPECIFY c ND=0 — DMA). Наше чтение без DMA-обслуживания
        ; в этом режиме отклоняется (ST0=0x20, без фазы данных).
        ; Полный fdc_reset ставит НАШ SPECIFY (ND=1), скорость из
        ; fdc_ccr и чистое состояние — после него чтения идут без отказов.
        cmp byte [cs:fdc_inited], 0
        jne .rl_inited
        call fdc_reset
        mov byte [cs:fdc_inited], 1
        jc .rl_retry
        mov bl, 'a'                 ; след щупа: reset прошёл
        call fdc_trace
.rl_inited:
        ; мотор. fdc_reset уже включил мотор и отработал раскрутку;
        ; состояние мотора проверяем ТОЛЬКО по своему флагу.
        cmp byte [cs:fdc_motor_up], 0
        jne .rl_motor_ok
        call fdc_dor_motor
        mov dx, FDC_DOR
        out dx, al
        call fdc_spinup
        mov byte [cs:fdc_motor_up], 1
        ; Порт 0x3F2 на части чипсетов (CS5530) при ЧТЕНИИ возвращает
        ; мусор, поэтому состояние мотора проверяем только по своему
        ; флагу: fdc_reset после сброса всегда раскручивает мотор, а
        ; BIOS не успеет его погасить — счётчик motor-off обновлён выше.
.rl_motor_ok:
        ; калибровка (если позиция головки неизвестна)
        cmp byte [cs:fdc_calibrated], 0
        jne .rl_cal_ok
        call fdc_recal
        jc .rl_retry
        mov bl, 'b'                 ; след щупа: recal прошёл
        call fdc_trace
.rl_cal_ok:
        ; seek, если цилиндр не тот
        mov al, [cs:fdc_C]
        cmp al, [cs:fdc_cyl_cur]
        je  .rl_seek_ok
        mov bl, al
        call fdc_seek
        jc .rl_retry
        mov bl, 'c'                 ; след щупа: seek прошёл
        call fdc_trace
.rl_seek_ok:
        ; само чтение
        call fdc_read_op
        jnc .rl_ok
.rl_retry:
        ; диагностика повтора: 'f' + код фазы + последний MSR (hex)
        pusha
        mov al, 'f'
        call debug_char
        mov al, [cs:fdc_ph]
        add al, '0'
        call debug_char
        mov al, [cs:fdc_msr_last]
        call screen_hex_byte
        mov al, ' '
        call debug_char
        popa
        dec byte [cs:fdc_attempts]
        jz  .rl_final
        call fdc_reset
        jmp .rl_try
.rl_ok:
        mov al, [cs:fdc_pic]
        out 0x21, al
        pop es
        pop ds
        popa
        clc
        ret
.rl_final:
        ; финальная диагностика: 'F' + фаза + ST0 ST1 ST2 + 'M' + MSR.
        ; Печатаем ДО fdc_reset: сброс затирает фазу и регистры
        ; состояния (раньше вместо реальной ошибки печатался мусор
        ; вида "F3 800000" от пустой очереди SENSE INTERRUPT).
        pusha
        push ds
        push es
        mov al, 'F'
        call debug_char
        mov al, [cs:fdc_ph]
        add al, '0'
        call debug_char
        mov al, ' '
        call debug_char
        mov al, [cs:fdc_st0]
        call screen_hex_byte
        mov al, [cs:fdc_st1]
        call screen_hex_byte
        mov al, [cs:fdc_st2]
        call screen_hex_byte
        mov al, ' '
        call debug_char
        mov al, 'M'
        call debug_char
        mov al, [cs:fdc_msr_last]
        call screen_hex_byte
        mov al, ' '
        call debug_char
        pop es
        pop ds
        popa
        call fdc_reset              ; снять хвосты, вернуть в покой
        mov al, [cs:fdc_pic]
        out 0x21, al

        ; Во время щупа скоростей (fdc_probe=1, см. bpb_init) BIOS
        ; НЕ подключаем: одна удавшаяся BIOS-попытка на НЕВЕРНОЙ
        ; скорости навсегда выключала PIO (fdc_use_bios=1), и весь
        ; дальнейший диск шёл через int 13h — а на TA-NC529/CS5530
        ; BIOS-чтения с seek зависают внутри в ожидании IRQ6. Из-за
        ; этого Geode грузился до '01234' (BPB/каталог/драйверы —
        ; цилиндры 0-3), а на чтении ядра (цилиндры 2+) умирал.
        cmp byte [cs:fdc_probe], 0
        je  .rl_bios_fb
        stc                         ; щуп: просто ошибка, вызывающий
        pop es                      ; попробует следующую скорость
        pop ds
        popa
        ret
.rl_bios_fb:
        ; Последний шанс остаться загрузиться: одна BIOS-попытка.
        ; Сброс через BIOS обязателен — PIO правил DOR за спиной BIOS.
        ; На платах, где BIOS внутри int 13h реально ждёт IRQ6, мы сюда
        ; не попадаем: там FDC живой и PIO работает. Если BIOS помог —
        ; дальше читаем только через BIOS (флаг fdc_use_bios).
        mov byte [cs:fdc_use_bios], 1
        mov ah, 0x00
        mov dl, [cs:drive]
        int 0x13
        ; === SPECIFY ND=0: вернуть контроллер в DMA-режим ===
        ; Наш fdc_reset (выше, из .rl_final) запрограммировал SPECIFY
        ; с ND=1 (non-DMA), а BIOS читает дискету ЧЕРЕЗ DMA. AH=00 на
        ; многих BIOS делает только recalibrate БЕЗ сброса FDC-ядра и
        ; БЕЗ перепрограммирования SPECIFY — ND=1 переживает его, и на
        ; CS5530 DMA-передача в память идёт битой (в буферах «первый
        ; байт сектора верный, дальше мусор» при чистом CRC: дампы
        ; F9 80 57 C0 / 46 20 45 ... вместо F9 FF FF 03 / "F12 LD ").
        ; Явный SPECIFY с ND=0 возвращает штатный DRQ/DMA-handshake.
        ; Если байты не приняты (FDC реально мёртв, напр. USB-FDD) —
        ; просто идём на BIOS как раньше.
        pusha
        mov bl, 0x03            ; SPECIFY
        call fdc_out
        jc  .rbf_skip
        mov bl, 0xCF            ; SRT=6мс, HUT=240мс (как в fdc_reset)
        call fdc_out
        jc  .rbf_skip
        mov bl, 0x14            ; HLT=20мс, ND=0 (DMA разрешён)
        call fdc_out
.rbf_skip:
        popa
        pusha
        mov al, 'B'
        call debug_char
        popa
        call fdc_bios_read          ; CF от int 13h
        pop es
        pop ds
        popa
        ret

; ------------------------------------------------------------
; fdc_trace: печать маркера шага щупа (BL = символ). Печатает ТОЛЬКО
; во время проба скоростей (fdc_probe=1), чтобы молчаливый обрыв
; лога на реальном железе указывал конкретный шаг (см. шапку файла).
; Регистры сохраняет.
; ------------------------------------------------------------
fdc_trace:
        cmp byte [cs:fdc_probe], 0
        je  .tr_ret
        push ax
        mov al, bl
        call debug_char
        pop ax
.tr_ret:
        ret

; ------------------------------------------------------------
; fdc_bios_read: чтение сектора fdc_C/H/R через BIOS int 13h в
; fdc_dst_seg:off. Путь для машин без легаси-FDC (USB-FDD) и
; аварийный резерв, когда PIO не смог. CF от int 13h.
; ------------------------------------------------------------
fdc_bios_read:
        push ax
        push bx
        push cx
        push dx
        push es
        push di
        push si
        push ds
        ; Не даём BIOS погасить мотор по своему таймеру: после
        ; PIO-неудачи все чтения идут через int 13h, серия может
        ; длиться дольше стандартных 2 с мотора. Без этого BIOS
        ; в середине загрузки гасит мотор и ближайшее чтение
        ; возвращается ошибку (на Geode падало чтение корня).
        push ds
        mov ax, 0x0040
        mov ds, ax
        mov byte [0x0040], 0xFF     ; счётчик motor-off = макс
        pop ds
        mov cl, [cs:fdc_R]          ; сектор (1-based), биты 0-5 CL
        mov al, [cs:fdc_C]
        mov ch, al                  ; младшие 8 бит цилиндра
        ; Старшие биты цилиндра (CL бит 6-7): fdc_C — БАЙТ, у НГМД
        ; цилиндр <= 79, поэтому биты 8-9 всегда нулевые и в CL ничего
        ; до-упаковывать НЕ нужно. Раньше здесь стояло
        ; "shr al,2/and al,3/shl al,6/or cl,al" — это тащило в CL
        ; биты 2-3 цилиндра (скопировано из disk.inc, где AL берётся
        ; из AH после 16-битного div). Для C>=4 CL получал мусор,
        ; BIOS возвращал AH=01 (invalid parameter) — из-за этого
        ; BIOS-fallback падал ровно на первой загрузке драйвера
        ; ('e' после BWXY на 86box/Geode), а FAT/корень (C0..C1)
        ; читались нормально.
        mov ax, 0x0201              ; AH=02 read, AL=1 сектор

        ; ГРАНИЦА 64К ДЛЯ DMA: BIOS читает дискету через контроллер
        ; DMA, который не умеет пересекать 64К-границу. SeaBIOS (и
        ; реальные BIOS) в этом случае возвращают AH=09. Приёмник
        ; двигается по ES:BX вместе с кластерами, поэтому рано или
        ; поздно ES:BX+511 пересекает границу. Если так — читаем в
        ; bounce-буфер 0000:7C00 (труп stage1, больше никем не
        ; используется) и копируем на место.
        ; ВНИМАНИЕ: проверка портит DX, поэтому DH/DL (головка/диск)
        ; выставляются в каждой ветке ПОСЛЕ неё, заново.
        mov di, es
        mov dx, di
        shl dx, 4                   ; DX = (ES*16) & 0xFFFF (= addr mod 64К)
        add dx, bx                  ; + смещение приёмника
        add dx, 0x1FF               ; + последний байт сектора
        jc  .br_bounce              ; перенос через 0x10000 = пересечение
        mov es, [cs:fdc_dst_seg]
        mov bx, [cs:fdc_dst_off]
        mov dh, [cs:fdc_H]
        mov dl, [cs:drive]
        int 0x13
        jmp .br_ret
.br_bounce:
        ; чтение в bounce 0000:7C00 (труп stage1), затем копия на место
        xor ax, ax
        mov es, ax
        mov bx, 0x7C00
        mov ax, 0x0201
        mov dh, [cs:fdc_H]
        mov dl, [cs:drive]
        int 0x13
        jc .br_ret
        push ds
        xor ax, ax
        mov ds, ax
        mov si, 0x7C00
        mov es, [cs:fdc_dst_seg]
        mov di, [cs:fdc_dst_off]
        mov cx, 256
        cld
        rep movsw
        pop ds
        clc
.br_ret:
        pop ds
        pop si
        pop di
        pop es
        pop dx
        pop cx
        pop bx
        pop ax
        ret

; ------------------------------------------------------------
; fdc_reset: полный сброс контроллера + SPECIFY + drain SENSE
; INTERRUPT + скорость данных из fdc_ccr. CF=0 при успехе.
; ------------------------------------------------------------
fdc_reset:
        push ax
        push bx
        push cx
        push dx
        mov byte [cs:fdc_calibrated], 0
        mov byte [cs:fdc_cyl_cur], 0xFF
        mov byte [cs:fdc_motor_up], 0
        mov byte [cs:fdc_inited], 1

        ; RESET=0, мотор выключен
        mov al, [cs:fdc_unit]
        or  al, 0x08
        mov dx, FDC_DOR
        out dx, al
        FDC_IOWAIT
        call fdc_delay_2ms

        ; RESET=1 + мотор юнита
        call fdc_dor_motor
        mov dx, FDC_DOR
        out dx, al
        FDC_IOWAIT
        call fdc_spinup
        mov byte [cs:fdc_motor_up], 1

        ; готовность: RQM=1, DIO=0, CB=0
        mov byte [cs:fdc_ph], PH_READY
        mov al, 0x80
        mov ah, 0xD0
        mov cx, TMO_READY
        call fdc_wait_msr
        jc .rs_fail

        ; SPECIFY: SRT=6мс, HUT=240мс, HLT=20мс, ND=1 (без DMA)
        mov byte [cs:fdc_ph], PH_SPECIFY
        mov bl, 0x03
        call fdc_out
        jc .rs_fail
        mov bl, 0xCF
        call fdc_out
        jc .rs_fail
        mov bl, 0x15
        call fdc_out
        jc .rs_fail

        ; скорость данных. После сброса чип по умолчанию даёт 250
        ; кбит/с, поэтому скорость НЕОБХОДИМО программировать явно:
        ; чтение DD-медиа (720K/360K) на 500 кбит/с даёт таймаут фазы
        ; данных (FDC не находит адресные метки), HD-медиа (1.2M/1.44M)
        ; наоборот требует 500. Значение берём из fdc_ccr — его
        ; выставляет bpb_init по BPB (spt<=9 -> 250, иначе 500), а до
        ; разбора BPB перебирает 500/250/300 пробами загрузочного сектора.
        ;
        ; ПИШЕМ ТОЛЬКО В CCR (0x3F7). Попытка дублировать запись в DSR
        ; (0x3F4 на запись) в сборке 9eb47f5 привела на TA-NC529/CS5530
        ; к БЕСШУМНОЙ смерти сразу после 'r0' (см. шапку): это ЕДИНСТВЕННОЕ
        ; новое обращение к портам относительно сборок, печатавших дампы;
        ; незавершённый I/O-цикл записи на real-чипсете останавливает CPU
        ; внутри out. CCR-запись в тех сборках отрабатывала всегда.
        cmp byte [cs:fdc_ccr_force], 0
        je  .rs_noccr
        mov al, [cs:fdc_ccr]
        and al, 0x03
        mov dx, FDC_CCR
        out dx, al
        FDC_IOWAIT
.rs_noccr:

        ; осушить очередь SENSE INTERRUPT (после reset)
        mov byte [cs:fdc_ph], PH_SENSED
        mov cx, 4
.rs_drain:
        push cx
        call fdc_sense_int
        pop cx
        jc .rs_fail
        mov al, [cs:fdc_st0]
        mov ah, al
        and ah, 0xC0
        cmp ah, 0x80            ; INVALID = очередь пуста
        je  .rs_done
        loop .rs_drain
.rs_done:
        clc
        jmp .rs_ret
.rs_fail:
        stc
.rs_ret:
        pop dx
        pop cx
        pop bx
        pop ax
        ret

; ------------------------------------------------------------
; fdc_wait_seek_done: дождаться завершения RECALIBRATE/SEEK.
;
; ПОЧЕМУ ТАК: SEEK/RECALIBRATE — команды БЕЗ фазы результата;
; сразу после приёма байт интерфейс возвращается в покой, и
; MSR = 0x8n (RQM=1 + бит busy юнита n, бит 0-3). Старый код
; ждал (MSR & 0xD0)==0x80 — это условие истинно УЖЕ ВО ВРЕМЯ
; движения головы, после чего ОДИН SENSE INTERRUPT возвращал
; INVALID (ST0=0x80, без PCN): прерывание seek-end ещё не
; наступило. На реальном CS5530 recal РЕАЛЬНО двигает голову
; (stage1/BIOS оставляют её на цилиндрах 1-13+, шаг ~6мс —
; до сотен мс).
;
; РЕШЕНИЕ (стандартное для поллингового драйвера): опрашивать
; SENSE INTERRUPT в цикле до ДЕДЛАЙНА ПО ТИКАМ BIOS:
;   - sense_int вернул CF (ответа нет вообще — чип на CS5530
;     молчит, пока recal/seek физически выполняются) -> НЕ
;     фатально: шаг ~6мс, recal до сотен мс, просто ждём дальше;
;   - ST0 INVALID (ничего не ждёт) -> recal/seek ещё не завершился,
;     опрос снова (команда безопасна в любой момент и состояние
;     контроллера не меняет);
;   - валидный ST0 -> успех (fdc_st0/BH=PCN, как у sense_int).
; Пауза между опросами — fdc_settle. Дедлайн — 45 тиков BIOS
; (~2.5 с) по 0x40:0x6C: не зависит от частоты CPU и гарантирует
; завершение даже при мёртвом/отсутствующем накопителе (раньше
; таймауты были в «пустых циклах», и на 300МГц Geode сжимались
; втрое против 100МГц, а на супер-быстрых чипах сжигали бюджет
; за микросекунды).
; Выход: CF=0 — валидный ST0 (BL=ST0, BH=PCN, как у sense_int);
; CF=1 — таймаут (голова не пришла за ~2.5 c).
; ------------------------------------------------------------
fdc_wait_seek_done:
        ; ВНИМАНИЕ: BX НЕ сохраняем — как у fdc_sense_int, результат
        ; возвращается в BL=ST0 и BH=PCN, вызывающий (fdc_recal /
        ; fdc_seek) читает BH сразу после возврата. Внутренние
        ; push/pop bx ниже — локальные, вокруг чтения тиков.
        push ax
        push cx
        push dx
        push ds
        mov ax, 0x0040
        mov ds, ax
        mov cx, [0x006C]        ; стартовый тик BIOS (sense_int/settle
        xor ax, ax              ; CX сохраняют)
        mov ds, ax
.wsd_poll:
        call fdc_sense_int
        jnc .wsd_got
        jmp .wsd_again          ; ответа ещё нет — recal/seek продолжается
.wsd_got:
        mov al, [cs:fdc_st0]
        mov ah, al
        and ah, 0xC0
        cmp ah, 0x80            ; INVALID = recal/seek ещё идёт
        je  .wsd_again
        clc                     ; валидный ST0: BL=ST0, BH=PCN
        jmp .wsd_ret
.wsd_again:
        call fdc_settle         ; пауза ~10-30 мс между опросами
        ; дедлайн по тикам: (текущий - стартовый) >= 45 -> таймаут
        push ax
        push bx
        mov ax, 0x0040
        mov ds, ax
        mov ax, [0x006C]
        xor bx, bx
        mov ds, bx
        sub ax, cx              ; прошло тиков (word-wrap безопасен)
        cmp ax, 45              ; ~2.5 c
        pop bx
        pop ax
        jb  .wsd_poll
        stc
.wsd_ret:
        pop ds
        pop dx
        pop cx
        pop ax
        ret

; ------------------------------------------------------------
; fdc_recal: RECALIBRATE (головка на цилиндр 0). CF=0 при успехе.
; ------------------------------------------------------------
fdc_recal:
        push bx
        mov byte [cs:fdc_ph], PH_RECAL
        mov bl, 0x07
        call fdc_out
        jc .rc_fail
        mov bl, [cs:fdc_unit]
        call fdc_out
        jc .rc_fail
        ; ждать ЗАВЕРШЕНИЯ recal (опрос SENSE INTERRUPT до валидного
        ; ST0 — см. fdc_wait_seek_done; старое (MSR&0xD0)==0x80
        ; выполнялось мгновенно и ловило recal посередине движения)
        call fdc_wait_seek_done
        jc .rc_fail
        mov al, [cs:fdc_st0]
        test al, 0xC0
        jnz .rc_fail            ; IC != 00
        test al, 0x10
        jnz .rc_fail            ; equipment check
        test al, 0x20
        jz  .rc_fail            ; нет признака seek-end
        test bh, bh
        jnz .rc_fail            ; PCN != 0
        mov byte [cs:fdc_cyl_cur], 0
        mov byte [cs:fdc_calibrated], 1
        clc
        jmp .rc_ret
.rc_fail:
        stc
.rc_ret:
        pop bx
        ret

; ------------------------------------------------------------
; fdc_seek: BL = целевой цилиндр. CF=0 при успехе.
; ------------------------------------------------------------
fdc_seek:
        push bx
        mov byte [cs:fdc_ph], PH_SEEK
        mov al, bl
        mov [cs:fdc_stmp], al
        mov bl, 0x0F
        call fdc_out
        jc .sk_fail
        mov bl, [cs:fdc_H]
        shl bl, 2
        or  bl, [cs:fdc_unit]
        call fdc_out
        jc .sk_fail
        mov bl, [cs:fdc_stmp]
        call fdc_out
        jc .sk_fail
        ; ждать ЗАВЕРШЕНИЯ seek (опрос SENSE INTERRUPT до валидного
        ; ST0 — та же проблема, что у recal: см. fdc_wait_seek_done)
        call fdc_wait_seek_done
        jc .sk_fail
        mov al, [cs:fdc_st0]
        test al, 0xC0
        jnz .sk_fail
        test al, 0x10
        jnz .sk_fail
        test al, 0x20
        jz  .sk_fail
        mov al, [cs:fdc_stmp]
        cmp bh, al
        jne .sk_fail            ; PCN != цель
        call fdc_settle
        mov al, [cs:fdc_stmp]
        mov [cs:fdc_cyl_cur], al
        clc
        jmp .sk_ret
.sk_fail:
        stc
.sk_ret:
        pop bx
        ret

; ------------------------------------------------------------
; fdc_read_op: READ DATA сектора fdc_C/H/R в приёмник.
; CF=0 при успехе.
; ------------------------------------------------------------
fdc_read_op:
        mov byte [cs:fdc_ph], PH_READCMD
        mov bl, 0x46            ; READ DATA: MFM=1, MT=0, SK=0
        call fdc_out
        jc .ro_fail
        mov bl, [cs:fdc_H]
        shl bl, 2
        or  bl, [cs:fdc_unit]
        call fdc_out
        jc .ro_fail
        mov bl, [cs:fdc_C]
        call fdc_out
        jc .ro_fail
        mov bl, [cs:fdc_H]
        call fdc_out
        jc .ro_fail
        mov bl, [cs:fdc_R]
        call fdc_out
        jc .ro_fail
        mov bl, 0x02            ; N=2: сектор 512 байт (9-байтовая
        call fdc_out            ; команда READ DATA по 82077!)
        jc .ro_fail
        mov bl, [cs:fdc_R]      ; EOT = R: ровно один сектор
        call fdc_out
        jc .ro_fail
        mov bl, 0x1B            ; GPL
        call fdc_out
        jc .ro_fail
        mov bl, 0xFF            ; DTL
        call fdc_out
        jc .ro_fail

        ; след щупа: команда READ выдана полностью, ждём данные ('p')
        mov bl, 'p'
        call fdc_trace

        ; фаза данных: 512 байт прямо в приёмник (без DMA).
        ; ПЕРВЫЙ байт ждём с ДЛИННЫМ таймаутом (TMO_CMD ~ 2 оборота
        ; дискеты): FDC должен найти заголовок сектора. Если первого
        ; байта нет — чтение заведомо не состоится (неверная скорость
        ; данных, нет медиы) и НЕЛЬЗЯ ждать оставшиеся 511 байтов по
        ; TMO_BYTED: на Geode это давало ~512*80мс = 40+ c молчаливого
        ; «виса» на КАЖДУЮ неверную скорость пробы. После первого
        ; байта данные идут с интервалом байтовой скорости (32 мкс
        ; при 250 кбит/с) — короткий TMO_BYTED корректен.
        ;
        ; РАЗЛИЧЕНИЕ ФАЗ ПО MSR (критично для диагностики): в фазе
        ; ИСПОЛНЕНИЯ (ND=1) готовый байт данных выглядит как
        ; RQM|DIO|NDM|CB|юнит = 1111 0001 ((MSR&0x30)=0x30); в фазе
        ; РЕЗУЛЬТАТОВ — RQM|DIO|CB|юнит, NDM=0 ((MSR&0x30)=0x10).
        ; Раньше оба случая елись как «данные»: при ошибке чтения
        ; (нет адресной метки на неверной скорости, CRC...) FDC
        ; завершал команду с ошибкой, драйвер съедал 7 байтов
        ; результата (ST0 ST1 ST2 C H R N) как данные, падал на
        ; 8-м байте, и диагностика показывала мусор
        ; ('F7 200000 M80': ST0=0x20 — огрызок от seek, MSR=0x80 —
        ; результат уже опустошён). Теперь завершение команды ловим
        ; по NDM=0 и уходим читать НАСТОЯЩИЙ ST0/ST1/ST2.
        mov byte [cs:fdc_ph], PH_DATA
        mov es, [cs:fdc_dst_seg]
        mov di, [cs:fdc_dst_off]
        mov cx, 512
.ro_data:
        push cx
        mov al, 0xC0
        mov ah, 0xC0
        cmp cx, 512            ; первый байт данных?
        jne .ro_tmo_short
        mov cx, TMO_CMD        ; да: ждём появления данных долго
        jmp .ro_wait
.ro_tmo_short:
        mov cx, TMO_BYTED
.ro_wait:
        call fdc_wait_msr
        pop cx
        jc  .ro_nodata
        mov al, bl
        and al, 0x30
        cmp al, 0x30           ; NDM=1 -> байт данных исполнения
        jne .ro_early          ; NDM=0 -> команда завершилась досрочно
        mov dx, FDC_FIFO
        in  al, dx
        mov [es:di], al
        FDC_ACKRQM             ; байт выдан: ждём фактического снятия RQM
        inc di                 ; (stale-MSR гонка: повтор/потеря байта)
        loop .ro_data
        jmp .ro_results        ; все 512 байт приняты — фаза результатов
.ro_early:
        ; READ завершился до 512 байт: печатаем, сколько байт данных
        ; реально принято ('T'+hex), и проваливаемся в чтение
        ; результатов — ST0/ST1/ST2 будут НАСТОЯЩИМИ (ND/MA/CRC/EN...).
        push ax
        push cx
        mov al, 'T'
        call debug_char
        mov ax, 512
        sub ax, cx
        call screen_hex_word
        mov al, ' '
        call debug_char
        pop cx
        pop ax
.ro_results:

        ; фаза результатов: ровно 7 байт ST0 ST1 ST2 C H R N
        mov byte [cs:fdc_ph], PH_RESULT
        call fdc_in
        jc .ro_fail
        mov [cs:fdc_st0], bl
        call fdc_in
        jc .ro_fail
        mov [cs:fdc_st1], bl
        call fdc_in
        jc .ro_fail
        mov [cs:fdc_st2], bl
        call fdc_in
        jc .ro_fail
        mov [cs:fdc_rc], bl     ; C
        call fdc_in
        jc .ro_fail             ; H — не нужен
        call fdc_in
        jc .ro_fail
        mov [cs:fdc_rr], bl     ; R
        call fdc_in
        jc .ro_fail             ; N — не нужен

        ; проверка результатов
        mov byte [cs:fdc_ph], PH_VERIFY
        mov al, [cs:fdc_st0]
        test al, 0xC0
        jnz .ro_fail            ; IC != 00 (нормальное завершение)
        cmp byte [cs:fdc_st1], 0
        jne .ro_fail
        cmp byte [cs:fdc_st2], 0
        jne .ro_fail
        mov al, [cs:fdc_rc]
        cmp al, [cs:fdc_C]
        jne .ro_fail            ; головка не там
        mov al, [cs:fdc_rr]
        cmp al, [cs:fdc_R]
        jne .ro_fail
        mov al, [cs:fdc_C]
        mov [cs:fdc_cyl_cur], al
        clc
        ret
.ro_nodata:
        ; Байт не пришёл за таймаут (данные И результат). ГОНКА:
        ; команда могла завершиться в момент между опросами MSR —
        ; даём фазе результатов КОРОТКИЙ шанс появиться и вычитываем
        ; ST0/ST1/ST2 для диагностики ('d'). Если появился байт
        ; ДАННЫХ (NDM=1) — уже поздно что-то делать, фиксируем провал.
        push cx
        mov al, 0xC0
        mov ah, 0xC0
        mov cx, TMO_BYTEC
        call fdc_wait_msr
        pop cx
        jc  .ro_drain           ; результата нет: осушить хвосты и выйти
        mov al, bl
        and al, 0x30
        cmp al, 0x30
        je  .ro_drain           ; данные пошли слишком поздно
        push ax
        mov al, 'd'
        call debug_char
        mov al, ' '
        call debug_char
        pop ax
        call fdc_in
        jc  .ro_drain
        mov al, bl
        mov [cs:fdc_st0], al
        call fdc_in
        jc  .ro_drain
        mov al, bl
        mov [cs:fdc_st1], al
        call fdc_in
        jc  .ro_drain
        mov al, bl
        mov [cs:fdc_st2], al
.ro_drain:
        ; ОСУШЕНИЕ ХВОСТА ФАЗЫ РЕЗУЛЬТАТОВ (C H R N).
        ; БЕЗ ЭТОГО: после вычитки ST0/ST1/ST2 (3 из 7 байтов) в FIFO
        ; остаётся 4 байта, контроллер висит в фазе результатов, и
        ; ПЕРВЫЙ fdc_out следующей попытки упирается в полный
        ; TMO_BYTEC (~1-2 с на реальном железе), а MSR-last в дампе
        ; показывает 0xD0-подобный мусор вместо реальной причины.
        ; Осушаем до 4 байтов с КОРОТКИМ таймаутом (1 единица).
        push cx
        push ax
        mov cx, 4
.ro_drain_loop:
        push cx
        mov al, 0xC0
        mov ah, 0xC0
        mov cx, 1
        call fdc_wait_msr
        pop cx
        jc  .ro_drain_done      ; байтов больше нет — хватит
        mov dx, FDC_FIFO
        in  al, dx
        FDC_IOWAIT
        loop .ro_drain_loop
.ro_drain_done:
        pop ax
        pop cx
.ro_fail:
        stc
        ret

; ------------------------------------------------------------
; fdc_wait_msr: ждать, пока (MSR & AH) == AL.
; CX = таймаут в единицах. CF=0: успех (raw MSR в BL),
; CF=1: таймаут.
; ------------------------------------------------------------
fdc_wait_msr:
        push di
        push si
        mov dx, FDC_MSR
        mov si, cx              ; ВАЖНО: таймаут сначала (целый CX),
        mov ch, al              ; и только потом ожидаемое значение в
                                ; CH — иначе SI получит (expect<<8)|tmo
.wm_outer:
        mov di, 0xFFFF
.wm_inner:
        in  al, dx
        mov bl, al
        and al, ah
        cmp al, ch
        je  .wm_ok
        dec di
        jnz .wm_inner
        dec si
        jnz .wm_outer
        mov [cs:fdc_msr_last], bl   ; запомнить MSR для диагностики
        stc
        jmp .wm_ret
.wm_ok:
        clc
.wm_ret:
        pop si
        pop di
        ret

; ------------------------------------------------------------
; fdc_out: отправить байт BL в FIFO (ждёт RQM=1, DIO=0)
; ------------------------------------------------------------
fdc_out:
        push bx
        mov bh, bl
        mov al, 0x80
        mov ah, 0xC0
        mov cx, TMO_BYTEC
        call fdc_wait_msr
        jc .ou_ret
        mov al, bh
        mov dx, FDC_FIFO
        out dx, al
        FDC_ACKRQM             ; байт принят: ждём фактического снятия RQM
.ou_ret:
        pop bx
        ret

; ------------------------------------------------------------
; fdc_in: принять байт из FIFO в BL (ждёт RQM=1, DIO=1)
; ------------------------------------------------------------
fdc_in:
        push cx
        mov al, 0xC0
        mov ah, 0xC0
        mov cx, TMO_BYTEC
        call fdc_wait_msr
        jc .in_ret
        mov dx, FDC_FIFO
        in  al, dx
        mov bl, al
        FDC_ACKRQM             ; байт прочитан: ждём снятия RQM (гонка MSR)
.in_ret:
        pop cx
        ret

; ------------------------------------------------------------
; fdc_sense_int: SENSE INTERRUPT STATUS.
; Выход: BL = ST0, BH = PCN. CF=1 при таймауте.
;
; ВАЖНО: часть контроллеров/эмуляторов (86box в частности) на SENSE
; INTERRUPT без ожидающегося прерывания отдают ТОЛЬКО ST0=0x8x
; (INVALID) и НЕ выдают второй байт (PCN) — контроллер возвращается
; в покой (MSR=0x80). Раньше это ловилось как таймаут и заваливало
; весь fdc_reset (в логе: 'f3'/'F3 800000 M80' при живом контроллере).
; Поэтому второй байт ждём с коротким таймаутом и считаем PCN=0,
; если он не пришёл: INVALID-ответ без PCN — легитимный.
; ------------------------------------------------------------
fdc_sense_int:
        push cx
        mov bl, 0x08
        call fdc_out
        jc .sn_ret
        call fdc_in
        jc .sn_ret
        mov al, bl
        mov [cs:fdc_st0], al
        ; PCN: второй байт есть не всегда (86box на INVALID отдаёт
        ; только ST0). Ждём его КОРОтко и, если пришёл, читаем из FIFO
        ; ВАЖНО: сам байт обязательно вычитать — fdc_wait_msr только
        ; опрашивает MSR, и если байт оставить, контроллер застрянет
        ; в фазе результата (MSR=0xD0) и следующий же fdc_out
        ; затянется по таймауту.
        mov al, 0xC0
        mov ah, 0xC0
        mov cx, 1               ; короткий таймаут (~1 единица)
        call fdc_wait_msr
        mov bh, 0               ; PCN по умолчанию: второго байта нет
        jc  .sn_got
        mov dx, FDC_FIFO
        in  al, dx
        FDC_IOWAIT             ; PCN: байт вычитан, дать чипу уйти в покой
        mov bh, al              ; PCN
.sn_got:
        mov bl, [cs:fdc_st0]    ; BL = ST0 (контракт функции)
        clc
.sn_ret:
        pop cx
        ret

; ------------------------------------------------------------
; fdc_dor_motor: AL = значение DOR (unit, RESET=1, IRQ/DMA=1,
; мотор юнита).
; ------------------------------------------------------------
fdc_dor_motor:
        push cx
        mov al, [cs:fdc_unit]
        or  al, 0x0C
        mov cl, [cs:fdc_unit]
        mov ch, 0x10
        shl ch, cl
        or  al, ch
        pop cx
        ret

; ------------------------------------------------------------
; Программные задержки (без опоры на прерывания/таймер BIOS).
; ------------------------------------------------------------
fdc_delay_2ms:                  ; ~2-3 мс @100МГц
        push ax
        push dx
        mov dx, 2
.d2a:   mov ax, 0xFFFF
.d2b:   dec ax
        jnz .d2b
        dec dx
        jnz .d2a
        pop dx
        pop ax
        ret

fdc_spinup:                     ; ~2.5 c @100МГц / ~0.9 c @300МГц
        push ax
        push dx
        mov dx, 2000
.spa:   mov ax, 0xFFFF
.spb:   dec ax
        jnz .spb
        dec dx
        jnz .spa
        pop dx
        pop ax
        ret

fdc_settle:                     ; ~25-30 мс @100МГц: установление
        push ax                 ; головки после seek
        push dx
        mov dx, 20
.sta:   mov ax, 0xFFFF
.stb:   dec ax
        jnz .stb
        dec dx
        jnz .sta
        pop dx
        pop ax
        ret
