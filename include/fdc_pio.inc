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
; fdc_pio.inc — дисковый слой stage2 (флоппи).
;
; Стратегия "Лестница + верификация"
;
; Зачем сделано. История отладки на TA-NC529 (Geode GX1 300МГц + CS5530) показала три факта:
;   1) PIO-путь не мог завершить RECALIBRATE: по каждой пробе 'f4 81' (MSR=0x81 — RQM + drive0 seek-busy)
;      и финал 'F4 800000 M81' (ST0=0x80 INVALID — seek-end так и не пришёл за 2.5 с опроса
;      SENSE INTERRUPT).
;   2) После PIO-неудач BIOS-фолбэк возвращал CF=0 с порченным буфером:
;      "первый байт сектора верный, дальше мусор" при
;      чистом CRC (дампы F9 80 57 C0 / 46 20 45 20 45... вместо
;      F9 FF FF 03 / "F12    LD "), причём повторное чтение давало
;      другой мусор (46 00 31 00 31...) — порча недетерминированная.
;
; Корень зла — SPECIFY с ND=1 (non-DMA), который fdc_reset программировал до recalibrate:
;   - RECALIBRATE/SEEK на CS5530 с ND=1 не завершаются вовсе
;     (MSR висит 0x81, SENSE INTERRUPT вечно INVALID);
;   - SPECIFY ND=0 из фолбэка писался при MSR=0x81: fdc_out
;     проверял только RQM|DIO, а занятый seek-ом контроллер
;     байты молча выбрасывал — ND=1 выживал, и все последующие BIOS DMA-чтения шли битыми (см. п.3).
;
; Новая схема (ред. 10 — см. п.9: PIO-первичный):
;   1) Ярус 1 — PIO (fdc_reset: SPECIFY ND=0, CCR, recal/seek, READ
;      с ND=1 только на фазу данных). каждая фаза ограничена
;      таймаутами — PIO висеть не может в принципе. FDC не трогаем,
;      только когда он не декодируется (pio_dead -> сразу BIOS).
;   1а) Фолбэк — BIOS int 13h (доказанный путь stage1 на C0..C2,
;      "лестница" поднимается к нему при сбоях PIO).
;   2) Ярус 2 — PIO: fdc_reset программирует SPECIFY с ND=0
;      (recalibrate/seek работают как у BIOS), а ND=1 включается
;      только в fdc_read_op, непосредственно перед READ DATA
;      (ND нужен исключительно фазе данных).
;      ВАЖНО: бит 0 третьего байта SPECIFY — это ND; ND=0 = 0x04, ND=1 = 0x05/0x15.
;   3) fdc_out оставлен с классической проверкой RQM|DIO: биты ACT
;      у 82077+ легитимно горят в режиме поллинга и после RESET, так
;      что "строгий покой" по MSR отличить от застрявшего seek-а
;      невозможно в принципе. Вместо этого гарантии даёт порядок:
;      перед каждым SPECIFY-в-неуверенном-состоянии стоит импульс
;      RESET в DOR (fdc_dor_pulse / fdc_reset), прерывающий любую
;      зависшую команду; ND=1 включается только в fdc_read_op.
;   4) fdc_recover_bios перед любым возвратом на BIOS после PIO:
;      импульс RESET в DOR -> ожидание покоя MSR=0x80 по тикам ->
;      осушение SENSE -> SPECIFY ND=0 при гарантированном покое ->
;      AH=00 BIOS -> кэш цилиндра BIOS 0x40:0x94/95 = 0.
;   5) Лестница из 8 шагов на каждый сектор (fdc_read_lba):
;        0,1 — чистая попытка текущим ярусом;
;        2   — восстановление текущего яруса;
;        3   — цикл мотора (выкл/пауза/вкл) + восстановление;
;        4   — Смена яруса (BIOS<->PIO) + восстановление;
;        5   — чистая попытка новым ярусом;
;        6   — цикл мотора + восстановление;
;        7   — смена яруса + восстановление.
;      Сдача — только после всех 8. Успех на шаге N>0 переводит
;      следующие секторы на шаг N-1 (мягкий спуск к быстрому пути).
;   6) Верификация данных (BIOS может вернуть CF=0 с мусором):
;      - bpb_init (f12ld.asm) проверяет 0xAA55 и полевые
;        ограничения BPB — мусорный сектор 0 = несостоявшееся
;        чтение (лестница перезапустится);
;      - fdc_verify_cluster перечитывает первый кластер каждого
;        файла (драйверы, ядро) в scratch 0000:0x2000 и сравнивает
;        байт-в-байт: два чтения дали разное = порча (маркер 'V'),
;        кластер перечитывается, при повторной порче — ошибка.
;   7) Пока скорость не подтверждена (BPB ещё не разобран),
;      PIO-ярус ротирует скорости 500->250->300 кбит/с по попыткам
;      (маркеры 'r0'/'r2'/'r1' — код CCR 0/2/1).
;
;   8) На реальном железе (загрузка TA-NC529): SPECIFY
;      в fdc_reset/fdc_recover_bios писался байтом 0x05, а у SPECIFY
;      БИТ 0 = ND. 0x05 = ND=1 — "исправление ND=0" из п.2 шапки не
;      применялось вовсе: recalibrate с ND=1 на CS5530 не завершался
;      (лог железа: r0/r2/r1 + 'f481' + 'F4 800000 M81' лестницами),
;      загрузка шла только после случайного BIOS AH=00, а выживший
;      ND=1 портил данные последующих BIOS-чтений (дампы
;      F9 80 57 C0 / 46 20 45 20 45... при чистом CRC). Теперь
;      SPECIFY ND=0 = байт 0x04 (бит 0 сброшен, HLT тот же).
;      Симптомы там же: 'f481' = фаза 4 (RECALIBRATE), MSR=0x81,
;      SENSE INTERRUPT вечно INVALID (ST0=0x80).
;
;   9) Первичный ярус — PIO (fdc_tier=1), BIOS — фолбэк. Изменено
;      после прогона 1.44M на TA-NC529 (Geode GX1): BIOS-primary
;      молча умирал на первом чтении дальнего цилиндра. Экран:
;      «Sl>W X F0 FF FF 03 Y 46 31 32 20 20 20 20 20 4C 44 20»
;      (FAT и корень — верные, чтения C0..C2 чистые) и тишина:
;      int 13h завис внутри на кластере FB_DR.DRV (KERNEL занял
;      кластеры 65..285, драйвер ушёл на C8H1) — документированный
;      режим этой платы (ждёт IRQ6 без таймаута), через ~14 с BIOS
;      погасил мотор по счётчику 0x40:0x40 и всё. "Доказательство"
;      stage1 покрывало только C0..C2 (63 сектора F12.LD) — на
;      дальние цилиндры не переносится. Дополнительно:
;      fdc_pio_read держит счётчик мотора 0x40:0x40=0xFF (иначе
;      int 08h гасит мотор посреди многосеректорных загрузок PIO),
;      fdc_bios_read при поднятой лестнице печатает 'B'+цилиндр,
;      settle увеличен до ~15-20 мс даже на 300МГц.
;
; Диагностика (COM/экран):
;   '>' — начало bpb_init; 'a' — fdc_reset прошёл; 'b'/'c' —
;   recalibrate/seek прошли (только когда лестница поднималась);
;   'f'+фаза+MSR — PIO-попытка не удалась; 'b'+AH — BIOS-попытка
;   не удалась; 'B'+hex(C) — начало BIOS-попытки при поднятой
;   лестнице (атрибуция висяков внутри int 13h); 'L'+шаг — выполнено
;   действие лестницы (2..7); 'r'+код скорости — ротация скоростей
;   PIO; 'F'+фаза+ST0 ST1 ST2+'M'+MSR — финальный дамп после лестницы;
;   'T'+hex — READ завершился до 512 байт (сколько принято);
;   'd' — гонка завершения (байт не пришёл, результат появился);
;   'V' — верификация кластера поймала порчу (два чтения разны);
;   'N'+hex+дамп — fat12_find_file не нашёл файл;
;   'Z' — корневой каталог перечитан; 'W'/'X'/'Y'/'0'-'4' — вехи
;   f12ld.asm; 'e'+«Disk error» — чтение не удалось вовсе.
;
; Коды фаз: 1=готовность после reset, 2=SPECIFY, 3=SENSE INT,
; 4=RECALIBRATE, 5=SEEK, 6=команда READ, 7=фаза данных,
; 8=фаза результатов, 9=проверка.
;
; ВНИМАНИЕ: запись в DSR (0x3F4 на запись) по-прежнему запрещена:
; на TA-NC529/CS5530 сборка с ней умирала молча сразу после 'r0'.
; Скорость задаём только через CCR (0x3F7).
;
; ============================================================
; ИСТОРИЯ: старые версии этого файла см. в git. Ключевые моменты,
; которые здесь сохранены:
;   - fdc_wait_seek_done опрашивает SENSE INTERRUPT в цикле до
;     дедлайна по тикам BIOS (~2.5 c): CF/INVALID = «ещё работает»;
;   - FDC_ACKRQM ждёт ФАКТИЧЕСКОГО снятия RQM после каждого байта;
;   - FDC_IOWAIT (2 x out 0x80) после каждого обмена с портами;
;   - чтение DOR (0x3F2) на CS5530 возвращает мусор — состояние
;     мотора отслеживаем собственным флагом;
;   - запись в 0x3F4 (DSR) запрещена (тихая смерть CPU в out).
; ============================================================

BITS 16

FDC_DOR     equ 0x3F2           ; Digital Output Register
FDC_MSR     equ 0x3F4           ; Main Status Register (чтение)
                                ; (запись в 0x3F4 = DSR — ЗАПРЕЩЕНА,
                                ;  см. шапку)
FDC_FIFO    equ 0x3F5           ; FIFO / данные
FDC_CCR     equ 0x3F7           ; скорость передачи (запись)

; ============================================================
; FDC_IOWAIT — задержка восстановления шины I/O (~1-2 мкс).
; На реальном железе (Geode GX1 + CS5530) чтение MSR СРАЗУ после
; записи в FIFO возвращает СТАРОЕ значение RQM. Запись в порт
; 0x80 (POST) безвредна и занимает полный цикл ISA-шины.
; ============================================================
%macro FDC_IOWAIT 0
    out 0x80, al
    out 0x80, al
%endmacro

; ============================================================
; FDC_ACKRQM — ПОДТВЕРЖДЕНИЕ ПРИЁМА/ВЫДАЧИ БАЙТА FIFO.
; После каждого обмена с FIFO контроллер должен СНЯТЬ бит RQM
; (латентность сотни нс — единицы мкс). Ждём фактического сброса
; RQM (с ограничением числа итераций). Макрос портит AL,
; сохраняет CX/DX сам.
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
fdc_probed      db 0            ; щуп шины 0x3F4 уже делали
fdc_pio_dead    db 0            ; 1 = FDC не декодируется (шина 0xFF):
                                ;     PIO-ярус запрещён, читаем BIOS
fdc_tier        db 1            ; ТЕКУЩИЙ ЯРУС: 1 = PIO (ПЕРВИЧНЫЙ!);
                                ;             0 = BIOS (фолбэк лестницы).
                                ; ПОЧЕМУ PIO ПЕРВЫЙ (п.9 шапки): на
                                ; TA-NC529 BIOS AH=02 зависает навсегда
                                ; (ждёт IRQ6 без таймаута) на первом же
                                ; чтении ДАЛЬНЕГО цилиндра; PIO целиком
                                ; ограничен таймаутами и висеть не может
fdc_step        db 0            ; шаг лестницы 0..7 (живёт между
                                ;     секторами, спускается на 1 при
                                ;     успехе)
fdc_escalated   db 0            ; 1 = лестница поднималась: включить
                                ;     пошаговую трассировку 'b'/'c'
fdc_bios_ah     db 0            ; AH последней BIOS-попытки (диагн.)
fdc_inited      db 0            ; контроллер уже переведён в наш режим
fdc_ccr         db 0x00         ; код скорости: 0=500 кбит/с, 2=250,
                                ;              1=300 (привод 1.2М)
fdc_ccr_force   db 1            ; 1 = программируем CCR при каждом reset
fdc_ccr_known   db 0            ; 1 = скорость подтверждена (BPB
                                ;     разобран); пока 0 — PIO ротирует
fdc_rate_idx    db 0            ; индекс ротации скоростей PIO
fdc_pio_proven  db 0            ; 1 = PIO-попытка УДАЛАСЬ при ротации:
                                ;     её скорость (fdc_ccr) доказана
                                ;     железом и не переопределяется BPB
                                ;     (привод 1.2М + дискета 720К:
                                ;     факт 300 кбит/с, BPB говорит 250)
fdc_dst_seg     dw 0            ; приёмник сектора
fdc_dst_off     dw 0
fdc_vlba        dw 0            ; LBA первого сектора кластера (вериф.)
fdc_vcur        dw 0            ; текущий LBA при верификации
fdc_vseg        dw 0            ; сегмент приёмника при верификации

; скорости ротации PIO (CCR-коды): 500 -> 250 -> 300 кбит/с
fdc_rates       db 0x00, 0x02, 0x01

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

; ============================================================
; fdc_read_lba: ГЛАВНЫЙ ВХОД. AX = LBA, ES:BX = приёмник.
; CF=0 при успехе. Все регистры сохраняются.
; Заменяет BIOS int 13h AH=02. Держит лестницу из 8 шагов:
; см. шапку. Состояние лестницы (fdc_step, fdc_tier) живёт между
; вызовами: успех на шаге N спускает на N-1, полная неудача
; возвращает лестницу на шаг 0 и BIOS-ярус для следующего сектора.
; ============================================================
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

        ; Одноразовый щуп шины 0x3F4: два чтения подряд возвращают
        ; 0xFF — легаси-FDC не декодируется (CF-18/USB-FDD и т.п.).
        ; Это лишь ЗАПРЕЩАЕТ PIO-ярус: BIOS-ярус работает всегда.
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
        mov byte [cs:fdc_pio_dead], 1
.probe_done:

        ; ---------------- ЛЕСТНИЦА (8 шагов) ----------------
.step:
        mov al, [cs:fdc_step]
        ; 0,1,5 — чистая попытка; 2,4,7 — восстановление/смена;
        ; 3,6 — цикл мотора + восстановление
        test al, al
        jz  .try
        cmp al, 1
        je  .try
        cmp al, 5
        je  .try
        cmp al, 2
        je  .recov
        cmp al, 6
        je  .motor_recov
        cmp al, 3
        je  .motor_recov
        ; шаги 4 и 7: смена яруса
        mov byte [cs:fdc_escalated], 1
        cmp byte [cs:fdc_pio_dead], 0
        jne .sw_stay_bios       ; PIO запрещён — BIOS и только BIOS
        xor byte [cs:fdc_tier], 1
        pusha
        mov al, 'L'
        call debug_char
        mov al, [cs:fdc_step]
        add al, '0'
        call debug_char
        mov al, ' '
        call debug_char
        popa
        call fdc_recover_cur    ; подготовка НОВОГО яруса
        jmp .try
.sw_stay_bios:
        mov byte [cs:fdc_tier], 0
        pusha
        mov al, 'L'
        call debug_char
        mov al, [cs:fdc_step]
        add al, '0'
        call debug_char
        mov al, ' '
        call debug_char
        popa
        call fdc_recover_cur
        jmp .try
.motor_recov:
        mov byte [cs:fdc_escalated], 1
        pusha
        mov al, 'L'
        call debug_char
        mov al, [cs:fdc_step]
        add al, '0'
        call debug_char
        mov al, ' '
        call debug_char
        popa
        call fdc_motor_cycle
        call fdc_recover_cur
        jmp .try
.recov:
        mov byte [cs:fdc_escalated], 1
        pusha
        mov al, 'L'
        call debug_char
        mov al, [cs:fdc_step]
        add al, '0'
        call debug_char
        mov al, ' '
        call debug_char
        popa
        call fdc_recover_cur
        ; jmp .try
.try:
        cmp byte [cs:fdc_tier], 0
        jne .try_pio
        call fdc_bios_read      ; ЯРУС BIOS
        jmp .tried
.try_pio:
        ; FDC не декодируется (шина 0xFF, USB-FDD/CF-18)? PIO-ярус
        ; мгновенно мёртв — не жжём 4 шага лестницы на таймаутах,
        ; читаем BIOS сразу
        cmp byte [cs:fdc_pio_dead], 0
        je  .tp_real
        mov byte [cs:fdc_tier], 0
        call fdc_bios_read
        jmp .tried
.tp_real:
        call fdc_pio_read       ; ЯРУС PIO
.tried:
        jnc .ok
        ; --- диагностика неудавшейся попытки ---
        pusha
        cmp byte [cs:fdc_tier], 0
        jne .dbg_pio
        mov al, 'b'             ; BIOS: 'b' + код AH от int 13h
        call debug_char
        mov al, [cs:fdc_bios_ah]
        call screen_hex_byte
        mov al, ' '
        call debug_char
        jmp .dbg_done
.dbg_pio:
        mov al, 'f'             ; PIO: 'f' + фаза + последний MSR
        call debug_char
        mov al, [cs:fdc_ph]
        add al, '0'
        call debug_char
        mov al, [cs:fdc_msr_last]
        call screen_hex_byte
        mov al, ' '
        call debug_char
.dbg_done:
        popa
        inc byte [cs:fdc_step]
        cmp byte [cs:fdc_step], 8
        jb  .step
        ; --- лестница исчерпана: финальный дамп состояния FDC ---
        ; (печатаем ДО каких-либо сбросов: они затирают фазу)
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
        call fdc_reset          ; снять хвосты, вернуть в покой
        ; лестница для СЛЕДУЮЩЕГО сектора начнётся заново с PIO-яруса
        ; (первичный; pio_dead гасится в .try_pio на лету)
        mov byte [cs:fdc_step], 0
        mov byte [cs:fdc_tier], 1
        stc
        jmp .ret
.ok:
        ; МЯГКИЙ СПУСК ПО ЛЕСТНИЦЕ (CF выставляем В КОНЦЕ — cmp ниже
        ; портит флаги!). Спуск НЕ должен проеезжать через «дорогие»
        ; шаги (2 = восстановление, 3/6 = цикл мотора с паузой ~1-2.5 с,
        ; 4/7 = смена яруса) — иначе после одной эскалации КАЖДЫЙ
        ; сектор возил бы мотор туда-сюда (на ядре в сотни секторов
        ; это часы). Правила:
        ;   0,1 -> 0   (быстрый путь);
        ;   2   -> 1   (восстановление помогло — следующая попытка чистая);
        ;   3..7 -> 5  (работает НЕ-bios/восстановленный путь — остаются
        ;               ЧИСТЫЕ попытки текущего яруса; при очередном сбое
        ;               лестница снова пойдёт вверх с 6-го шага).
        mov al, [cs:fdc_step]
        cmp al, 2
        jb  .ok_ret              ; 0,1 — остаёмся на быстром пути
        je  .ok_step2
        mov byte [cs:fdc_step], 5
        jmp .ok_ret
.ok_step2:
        mov byte [cs:fdc_step], 1
.ok_ret:
        clc                      ; успех: CF=0
.ret:
        pop es
        pop ds
        popa
        ret

; ------------------------------------------------------------
; fdc_recover_cur: восстановление ТЕКУЩЕГО яруса.
; BIOS  -> fdc_recover_bios (полная реанимация DMA-режима);
; PIO   -> fdc_reset (сброс + SPECIFY ND=0 + CCR + мотор).
; ------------------------------------------------------------
fdc_recover_cur:
        cmp byte [cs:fdc_tier], 0
        jne .rc_pio
        call fdc_recover_bios
        ret
.rc_pio:
        call fdc_reset
        ret

; ------------------------------------------------------------
; fdc_pio_read: ОДНА PIO-попытка чтения сектора fdc_C/H/R в
; fdc_dst_seg:off. CF=0 при успехе. Маскирует IRQ6 только на время
; операции (BIOS-ярус НЕ маскируется никогда: int 13h сам ждёт
; IRQ6). Пока скорость не подтверждена (fdc_ccr_known=0), ротирует
; 500/250/300 кбит/с по попыткам с форс-ресетом.
; ------------------------------------------------------------
fdc_pio_read:
        pusha
        push ds
        push es

        ; ротация скоростей, пока не подтверждена (щуп из bpb_init
        ; удалён: он отравлял контроллер до первого BIOS-чтения)
        cmp byte [cs:fdc_ccr_known], 0
        jne .rate_ok
        mov al, [cs:fdc_rate_idx]
        inc byte [cs:fdc_rate_idx]
        and al, 3
        cmp al, 3
        jb  .ri_ok
        xor al, al
.ri_ok:
        mov bx, ax
        mov al, [cs:fdc_rates+bx]
        mov [cs:fdc_ccr], al
        mov byte [cs:fdc_inited], 0     ; форсировать fdc_reset
        pusha
        mov al, 'r'
        call debug_char
        mov al, [cs:fdc_ccr]
        add al, '0'             ; '0'/'2'/'1' — как в старых маркерах
        call debug_char
        popa
.rate_ok:
        ; IRQ6 маскируем на время операции: прерывание нам не нужно,
        ; а BIOS-обработчик int 0Eh не должен трогать результаты
        in  al, 0x21
        mov [cs:fdc_pic], al
        or  al, 0x40
        out 0x21, al

        ; контроллер уже в нашем режиме?
        cmp byte [cs:fdc_inited], 0
        jne .inited
        call fdc_reset
        mov byte [cs:fdc_inited], 1
        jc  .fail
        pusha
        mov al, 'a'             ; reset прошёл (всегда — редко бывает)
        call debug_char
        popa
        jmp .motor
.inited:
.motor:
        ; мотор: состояние — только по своему флагу (чтение DOR на
        ; CS5530 возвращает мусор)
        cmp byte [cs:fdc_motor_up], 0
        jne .motor_ok
        call fdc_dor_motor
        mov dx, FDC_DOR
        out dx, al
        call fdc_spinup
        mov byte [cs:fdc_motor_up], 1
.motor_ok:
        ; BIOS-обработчик int 08h гасит мотор, когда истекает счётчик
        ; 0x40:0x40 (до ~14 с от 0xFF): его запись в DOR УБИВАЕТ нашу
        ; команду посреди фазы данных. Пока PIO-ярус жив, счётчик
        ; держим максимальным — так же делает fdc_bios_read
        push ds
        push ax
        mov ax, 0x0040
        mov ds, ax
        mov byte [0x0040], 0xFF
        pop ax
        pop ds
        ; калибровка (если позиция головки неизвестна)
        cmp byte [cs:fdc_calibrated], 0
        jne .cal_ok
        call fdc_recal
        jc  .fail
        mov bl, 'b'
        call fdc_trace
.cal_ok:
        ; seek, если цилиндр не тот
        mov al, [cs:fdc_C]
        cmp al, [cs:fdc_cyl_cur]
        je  .seek_ok
        mov bl, al
        call fdc_seek
        jc  .fail
        mov bl, 'c'
        call fdc_trace
.seek_ok:
        call fdc_read_op
        jc  .fail
        mov byte [cs:fdc_pio_proven], 1 ; скорость PIO доказана фактом
        jmp .done
.fail:
        stc
.done:
        pushf
        mov al, [cs:fdc_pic]
        out 0x21, al            ; вернуть маску PIC
        popf
        pop es
        pop ds
        popa
        ret

; ------------------------------------------------------------
; fdc_trace: печать маркера шага (BL = символ) — ТОЛЬКО когда
; лестница поднималась (fdc_escalated=1): в нормальной загрузке
; трасса молчит, при проблемах — виден каждый шаг.
; ------------------------------------------------------------
fdc_trace:
        cmp byte [cs:fdc_escalated], 0
        je  .tr_ret
        push ax
        mov al, bl
        call debug_char
        pop ax
.tr_ret:
        ret

; ------------------------------------------------------------
; fdc_dor_pulse: импульс RESET через DOR — прерывает ЛЮБУЮ
; зависшую команду (stuck seek/recal/read). Оставляет контроллер
; с RESET=1, мотором юнита и IRQ/DMA-разрешением; флаг мотора
; сброшен (раскрутку делает вызывающий ярус: PIO — fdc_spinup,
; BIOS — сам внутри int 13h).
; ------------------------------------------------------------
fdc_dor_pulse:
        push ax
        push dx
        mov al, [cs:fdc_unit]
        or  al, 0x08            ; RESET=0 — контроллер в сбросе
        mov dx, FDC_DOR
        out dx, al
        FDC_IOWAIT
        call fdc_delay_2ms
        call fdc_dor_motor      ; RESET=1 + мотор юнита
        mov dx, FDC_DOR
        out dx, al
        FDC_IOWAIT
        mov byte [cs:fdc_motor_up], 0
        pop dx
        pop ax
        ret

; ------------------------------------------------------------
; fdc_wait_idle: лучший из возможных (best-effort) ожидатель покоя —
; (MSR & 0xF0) == 0x80: RQM=1, DIO=0, NDM=0, CB=0; биты ACT игнорируются,
; потому что после RESET включается режим ПОЛЛИНГА, и на реальных чипах
; MSR легитимно висит на 0x81 (ACT юнита) без всякой застрявшей команды.
; Гарантию отсутствия команд даёт НЕ эта проверка, а предшествующий
; импульс RESET в DOR (fdc_dor_pulse) — он прерывает всё. Дедлайн —
; 36 тиков BIOS (~2 c). CF=0 — покой; CF=1 — таймаут.
; ------------------------------------------------------------
fdc_wait_idle:
        push ax
        push cx
        push dx
        push si
        push ds
        mov ax, 0x0040
        mov ds, ax
        mov cx, [0x006C]        ; стартовый тик BIOS
        xor ax, ax
        mov ds, ax
        mov dx, FDC_MSR
.wi_poll:
        in  al, dx
        mov ah, al
        and ah, 0xF0            ; RQM|DIO|NDM|CB (ACT игнорируем)
        cmp ah, 0x80
        je  .wi_ok
        push ax
        mov ax, 0x0040
        mov ds, ax
        mov si, [0x006C]
        xor ax, ax
        mov ds, ax
        sub si, cx              ; прошло тиков (word-wrap безопасен)
        pop ax
        cmp si, 36              ; ~2 c
        jb  .wi_poll
        mov [cs:fdc_msr_last], al
        stc
        jmp .wi_ret
.wi_ok:
        clc
.wi_ret:
        pop ds
        pop si
        pop dx
        pop cx
        pop ax
        ret

; ------------------------------------------------------------
; fdc_recover_bios: ПОЛНАЯ реанимация контроллера перед BIOS-ярусом
; после любых PIO-экспериментов (или зависших команд).
;   1) импульс RESET в DOR — прервать stuck-команду;
;   2) дождаться покоя MSR==0x80 (до ~2 c; при таймауте — второй
;      импульс RESET и снова ожидание);
;   3) осушить очередь SENSE INTERRUPT (до 4 байтов);
;   4) SPECIFY с ND=0 — байты ГАРАНТИРОВАННО приняты, чип в покое;
;   5) скорость (CCR) — только если BPB уже подтверждён;
;   6) счётчик мотора BIOS 0x40:0x40 = 0xFF (не гасить мотор);
;   7) int 13h AH=00 — сброс+recalibrate ШТАТНЫМИ средствами BIOS;
;   8) кэш цилиндра BIOS 0x40:0x94/0x95 = 0 (наши RESET/recal сбили
;      представление BIOS о позиции головки — без этого BIOS пропускал
;      seek и читал «не там»; в связке с fix в fdc_bios_read старых
;      версий это выглядело как AH=01 на цилиндрах >= 4).
; CF не возвращает: восстановление — лучшее из возможного; вердикт
; даст сама BIOS-попытка.
; ------------------------------------------------------------
fdc_recover_bios:
        push ax
        push cx
        push dx
        push ds
        call fdc_dor_pulse
        call fdc_wait_idle
        jnc .idle
        call fdc_dor_pulse      ; второй заход
        call fdc_wait_idle
.idle:
        ; осушить SENSE INTERRUPT (после RESET там до 4 результатов)
        mov cx, 4
.drain:
        push cx
        call fdc_sense_int
        pop cx
        loop .drain
        ; SPECIFY: SRT=6мс, HUT=240мс, HLT=8-12мс, ND=0 (DMA-режим!).
        ; ТРЕТИЙ БАЙТ: биты 7-1 = HLT, БИТ 0 = ND. 0x05 = ND=1 (!) —
        ; именно он оставлял контроллер в non-DMA перед BIOS-чтениями
        ; (порча «первый байт верный, дальше мусор»). Правильный ND=0 —
        ; это 0x04: тот же HLT, бит 0 СБРОШЕН.
        mov bl, 0x03
        call fdc_out
        mov bl, 0xCF
        call fdc_out
        mov bl, 0x04            ; HLT=2, ND=0 (бит 0 = 0!)
        call fdc_out
        ; скорость — только если известна (иначе оставляем BIOS-у)
        cmp byte [cs:fdc_ccr_known], 0
        je  .noccr
        mov al, [cs:fdc_ccr]
        and al, 0x03
        mov dx, FDC_CCR
        out dx, al
        FDC_IOWAIT
.noccr:
        ; BIOS-сброс с recalibrate + синхронизация состояния BIOS
        push ds
        mov ax, 0x0040
        mov ds, ax
        mov byte [0x0040], 0xFF ; счётчик мотора = макс
        mov byte [0x0094], 0    ; кэш цилиндра drive 0
        mov byte [0x0095], 0    ; кэш цилиндра drive 1
        pop ds
        mov ah, 0x00
        mov dl, [cs:drive]
        int 0x13
        mov [cs:fdc_bios_ah], ah
        pop ds
        pop dx
        pop cx
        pop ax
        ret

; ------------------------------------------------------------
; fdc_motor_cycle: выключить мотор, пауза ~1-2.5 c (та же
; калибровка, что spinup), флаг мотора сброшен: следующий PIO-цикл
; сделает раскрутку заново, BIOS сам заведёт мотор при AH=02.
; Лечит «залипший» шпиндель/интерфейс привода.
; ------------------------------------------------------------
fdc_motor_cycle:
        push ax
        push dx
        mov al, [cs:fdc_unit]
        or  al, 0x0C            ; RESET=1, IRQ/DMA=1, МОТОР ВЫКЛЮЧЕН
        mov dx, FDC_DOR
        out dx, al
        FDC_IOWAIT
        call fdc_spinup
        mov byte [cs:fdc_motor_up], 0
        pop dx
        pop ax
        ret

; ------------------------------------------------------------
; fdc_bios_read: чтение сектора fdc_C/H/R через BIOS int 13h в
; fdc_dst_seg:off. ЯРУС ПО УМОЛЧАНИЮ: тот же путь, что у stage1
; (на TA-NC529 читает диск идеально) и у CP/M-дискет. Код ошибки
; BIOS сохраняется в fdc_bios_ah для диагностики ('b'+AH).
; CF от int 13h.
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
        ; Не даём BIOS погасить мотор по своему таймеру: серия
        ; чтений может длиться дольше стандартных 2 с мотора.
        push ds
        mov ax, 0x0040
        mov ds, ax
        mov byte [0x0040], 0xFF     ; счётчик motor-off = макс
        pop ds
        ; ДИАГНОСТИКА ВИСЯКОВ: при поднятой лестнице печатаем 'B'+цилиндр
        ; перед КАЖДОЙ BIOS-попыткой: если машина умрёт молча, по экрану
        ; видно, на каком цилиндре сидит int 13h (документированный
        ; режим TA-NC529: ждёт IRQ6 без таймаута)
        cmp byte [cs:fdc_escalated], 0
        je  .br_nodiag
        pusha
        mov al, 'B'
        call debug_char
        mov al, [cs:fdc_C]
        call screen_hex_byte
        mov al, ' '
        call debug_char
        popa
.br_nodiag:
        mov cl, [cs:fdc_R]          ; сектор (1-based), биты 0-5 CL
        mov al, [cs:fdc_C]
        mov ch, al                  ; младшие 8 бит цилиндра
        ; Старшие биты цилиндра (CL биты 6-7): у НГМД цилиндр <= 79,
        ; биты 8-9 всегда нулевые, до-упаковывать в CL нечего.
        mov ax, 0x0201              ; AH=02 read, AL=1 сектор

        ; ГРАНИЦА 64К ДЛЯ DMA: BIOS читает через контроллер DMA,
        ; который не умеет пересекать 64К-границу. Если ES:BX+511
        ; пересекает границу — читаем в bounce-буфер 0000:7C00
        ; (труп stage1) и копируем на место.
        mov di, es
        mov dx, di
        shl dx, 4                   ; DX = (ES*16) & 0xFFFF
        add dx, bx                  ; + смещение приёмника
        add dx, 0x1FF               ; + последний байт сектора
        jc  .br_bounce              ; перенос = пересечение
        mov es, [cs:fdc_dst_seg]
        mov bx, [cs:fdc_dst_off]
        mov dh, [cs:fdc_H]
        mov dl, [cs:drive]
        int 0x13
        mov [cs:fdc_bios_ah], ah    ; код ошибки BIOS (0 при успехе)
        jmp .br_ret
.br_bounce:
        ; чтение в bounce 0000:7C00, затем копия на место
        xor ax, ax
        mov es, ax
        mov bx, 0x7C00
        mov ax, 0x0201
        mov dh, [cs:fdc_H]
        mov dl, [cs:drive]
        int 0x13
        mov [cs:fdc_bios_ah], ah
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
; fdc_reset: полный сброс контроллера + SPECIFY с ND=0 + drain
; SENSE INTERRUPT + скорость данных из fdc_ccr. CF=0 при успехе.
;
; ND=0, А НЕ ND=1: non-DMA нужен ТОЛЬКО фазе данных READ (его
; включает fdc_read_op непосредственно перед командой). На
; CS5530 SPECIFY с ND=1 до recalibrate ломал ВСЁ: recalibrate не
; завершался ('f4 81' по каждой пробе), а SPECIFY ND=0 перед
; BIOS-фолбэком выбрасывался занятым чипом — BIOS-чтения шли
; битыми («первый байт сектора верный, дальше мусор»).
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

        ; SPECIFY: SRT=6мс, HUT=240мс, HLT=8-12мс, ND=0 (DMA-режим).
        ; ГЛАВНЫЙ ФИКС ЗАГРУЗКИ НА РЕАЛЬНОМ ЖЕЛЕЗЕ: третий байт SPECIFY —
        ; биты 7-1 HLT, БИТ 0 = ND. Здесь годами стоял 0x05 = ND=1 при
        ; комментарии «ND=0» — то есть «исправление ND=0 перед
        ; recalibrate» из шапки НИКОГДА не применялось. На CS5530
        ; RECALIBRATE/SEEK с ND=1 не завершаются вовсе ('f481': фаза 4,
        ; MSR=0x81, SENSE INTERRUPT вечно INVALID) — лестница выживала
        ; только после случайного BIOS AH=00, а застрявший ND=1 портил
        ; и последующие BIOS DMA-чтения. Правильно: 0x04 = тот же HLT,
        ; ND=0.
        mov byte [cs:fdc_ph], PH_SPECIFY
        mov bl, 0x03
        call fdc_out
        jc .rs_fail
        mov bl, 0xCF
        call fdc_out
        jc .rs_fail
        mov bl, 0x04            ; HLT=2 (биты 7-1), ND=0 (бит 0 = 0!)
        call fdc_out
        jc .rs_fail

        ; скорость данных. После сброса чип по умолчанию даёт 250
        ; кбит/с, поэтому скорость программируем явно: чтение
        ; DD-медиа (720K/360K) на 500 кбит/с даёт таймаут фазы
        ; данных (FDC не находит адресные метки), HD-медиа
        ; (1.2M/1.44M) наоборот требует 500. Значение — из fdc_ccr:
        ; bpb_init ставит по BPB (spt<=9 -> 250, иначе 500); до
        ; разбора BPB PIO-ярус ротирует 500/250/300 сам.
        ; ПИШЕМ ТОЛЬКО В CCR (0x3F7): запись в DSR (0x3F4 на запись)
        ; на TA-NC529/CS5530 убивала CPU молча (см. шапку).
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
; SEEK/RECALIBRATE — команды БЕЗ фазы результата; после приёма
; байт MSR = 0x8n (RQM + бит busy юнита), и он МОЛЧИТ, пока голова
; физически движется (на CS5530 — до сотен мс). Решение: опрашивать
; SENSE INTERRUPT в цикле до ДЕДЛАЙНА ПО ТИКАМ BIOS (~2.5 c):
;   - CF (ответа нет: fdc_out отверг запись при MSR=0x8n — чип
;     занят) -> не фатально, ждём дальше;
;   - ST0 INVALID (очередь пуста) -> recal/seek ещё не завершился;
;   - валидный ST0 -> успех (fdc_st0/BH=PCN).
; Выход: CF=0 — валидный ST0 (BL=ST0, BH=PCN); CF=1 — таймаут.
; ------------------------------------------------------------
fdc_wait_seek_done:
        ; ВНИМАНИЕ: BX НЕ сохраняем — результат в BL=ST0 и BH=PCN.
        push ax
        push cx
        push dx
        push ds
        mov ax, 0x0040
        mov ds, ax
        mov cx, [0x006C]        ; стартовый тик BIOS
        xor ax, ax
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
        sub ax, cx
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
; ПЕРЕД командой READ программ содержит SPECIFY с ND=1: non-DMA
; нужен ТОЛЬКО фазе данных (байт идёт через порт FIFO под
; управлением CPU). В fdc_reset ND=0 — recalibrate/seek должны
; вести себя как у BIOS (см. шапку: ND=1 до recal на CS5530
; вешал recal навсегда).
; ------------------------------------------------------------
fdc_read_op:
        mov byte [cs:fdc_ph], PH_SPECIFY
        mov bl, 0x03
        call fdc_out
        jc .ro_fail
        mov bl, 0xCF
        call fdc_out
        jc .ro_fail
        mov bl, 0x15            ; HLT=20мс, ND=1 (non-DMA: фаза данных)
        call fdc_out
        jc .ro_fail

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

        ; фаза данных: 512 байт прямо в приёмник (без DMA).
        ; ПЕРВЫЙ байт ждём с ДЛИННЫМ таймаутом (TMO_CMD ~ 2 оборота
        ; дискеты): FDC должен найти заголовок сектора. Остальные —
        ; с коротким TMO_BYTED (интервал байтовой скорости).
        ;
        ; РАЗЛИЧЕНИЕ ФАЗ ПО MSR: в фазе ИСПОЛНЕНИЯ (ND=1) готовый
        ; байт данных выглядит как RQM|DIO|NDM|CB|юнит
        ; ((MSR&0x30)=0x30); в фазе РЕЗУЛЬТАТОВ NDM=0
        ; ((MSR&0x30)=0x10) — команда завершилась досрочно, читаем
        ; НАСТОЯЩИЙ ST0/ST1/ST2.
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
        ; ОСУШЕНИЕ ХВОСТА ФАЗЫ РЕЗУЛЬТАТОВ (C H R N): без него
        ; контроллер висит в фазе результатов и первый fdc_out
        ; следующей попытки упирается в таймаут.
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
; CF=1: таймаут (последний MSR — в fdc_msr_last).
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
; fdc_out: отправить байт BL в FIFO (ждёт RQM=1, DIO=0).
;
; ПОЧЕМУ ПРОВЕРКА ТОЛЬКО RQM|DIO ((MSR&0xC0)==0x80), А НЕ СТРОГО
; «ПОЛНЫЙ ПОКОЙ»: у 82077+ после RESET включается режим поллинга, и
; MSR легитимно держит бит ACT юнита (0x81) БЕЗ всякой застрявшей
; команды — строгая проверка ломала бы SPECIFY на реальном железе.
; От застрявших команд защищает ПОРЯДОК вызовов: перед SPECIFY при
; восстановлении всегда стоит fdc_dor_pulse (RESET прерывает всё),
; а в fdc_reset SPECIFY идёт сразу после нашего же DOR-сброса.
; CF=0 при успехе, CF=1 при таймауте.
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
        FDC_ACKRQM              ; байт принят: ждём снятия RQM
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
; Часть контроллеров/эмуляторов (86box) на SENSE INTERRUPT без
; ожидающегося прерывания отдают ТОЛЬКО ST0=0x8x (INVALID) и НЕ
; выдают второй байт (PCN) — поэтому PCN ждём коротко и считаем 0,
; если он не пришёл: INVALID-ответ без PCN — легитимный.
; Занятый seek-ом контроллер (MSR=0x8n) отвергнет команду на
; fdc_out — это НЕ фатально: fdc_wait_seek_done трактует CF как
; «команда ещё исполняется».
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
        ; только ST0). Ждём его КОРОТКО и, если пришёл, вычитываем:
        ; оставленный байт застревает в фазе результата (MSR=0xD0).
        mov al, 0xC0
        mov ah, 0xC0
        mov cx, 1               ; короткий таймаут (~1 единица)
        call fdc_wait_msr
        mov bh, 0               ; PCN по умолчанию: второго байта нет
        jc  .sn_got
        mov dx, FDC_FIFO
        in  al, dx
        FDC_IOWAIT              ; PCN вычитан, дать чипу уйти в покой
        mov bh, al              ; PCN
.sn_got:
        mov bl, [cs:fdc_st0]    ; BL = ST0 (контракт функции)
        clc
.sn_ret:
        pop cx
        ret

; ------------------------------------------------------------
; fdc_verify_cluster: НЕЗАВИСИМАЯ ПРОВЕРКА ПРОЧИТАННОГО КЛАСТЕРА.
; BIOS/PIO может вернуть CF=0 с ПОРЧЕННЫМ буфером (недетерминированная
; порча DMA на CS5530: «первый байт верный, дальше мусор», при
; чистом CRC). Единственная защита, которой нельзя обмануть
; статусом CF=0, — перечитать данные и СРАВНИТЬ.
;
; IN:  ES = сегмент приёмника (кластер начинается с offset 0);
;      fdc_vlba = LBA первого сектора кластера;
;      [cs:spc] = секторов в кластере (1-2).
; Использует scratch 0000:0x2000 (свободная память ниже BOOTINFO
; 0x5000; НЕ 0:7C00 — при spc=2 кластер размером 1024 байта заехал
; бы в тело stage2 на 0x7E00!).
; OUT: CF=0 — совпало (или верификационное чтение само не удалось:
;      вердикта нет, мешать загрузке нечем — CF даст обычная лестница);
;      CF=1 — ГАРАНТИРОВАННАЯ ПОРЧА: два чтения одного сектора дали
;      разные байты (маркер 'V' печатает вызывающий).
; ------------------------------------------------------------
fdc_verify_cluster:
        push ax
        push bx
        push cx
        push dx
        push si
        push di
        push es
        push ds
        mov ax, es
        mov [cs:fdc_vseg], ax
        mov ax, [cs:fdc_vlba]
        mov [cs:fdc_vcur], ax
        mov cx, [cs:spc]
        mov dx, 0x2000          ; offset scratch в сегменте 0
.v_loop:
        push cx
        push dx
        xor ax, ax
        mov es, ax
        mov bx, dx
        mov ax, [cs:fdc_vcur]
        call disk_read_sector   ; AX/ES сохраняет, CF — результат
        pop dx
        pop cx
        jc  .v_pass             ; верификационное чтение сорвалось:
                                ; вердикта нет — не блокируем загрузку
        add dx, 512
        inc word [cs:fdc_vcur]
        loop .v_loop
        ; сравнить: DS:SI = 0000:0x2000, ES:DI = fdc_vseg:0
        xor ax, ax
        mov ds, ax
        mov es, [cs:fdc_vseg]
        mov si, 0x2000
        xor di, di
        mov cx, [cs:spc]
        shl cx, 9               ; spc * 512 байт
        cld
        rep cmpsb
        je  .v_pass
        stc                     ; ДВА РАЗНЫХ ЧТЕНИЯ = ПОРЧА
        jmp .v_ret
.v_pass:
        clc
.v_ret:
        pop ds
        pop es
        pop di
        pop si
        pop dx
        pop cx
        pop bx
        pop ax
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

fdc_settle:                     ; ~50-60 мс @100МГц / ~18-20 мс @300МГц:
        push ax                 ; установление головки после seek
        push dx                 ; (спека HD-привода 15 мс; старые 20
        mov dx, 40              ; циклов давали на Geode GX1 ~8-10 мс)
.sta:   mov ax, 0xFFFF
.stb:   dec ax
        jnz .stb
        dec dx
        jnz .sta
        pop dx
        pop ax
        ret
