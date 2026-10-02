;Низкоуровневые функции Stage 2

BITS 16

lba_supported db 0
heads_tmp     dw 16
sectors_tmp   dw 63
heads         db 0
sectors       db 0
read_lba_low  dw 0
read_lba_high dw 0
read_count    dw 0
read_buf_off  dw 0
read_buf_seg  dw 0
tmp_lba_low  dw 0
tmp_lba_high dw 0

EXT2_START_LBA equ 64


; Disk Address Packet (наследие CHS/LBA-пути INT 13h; PIO-код не использует)

align 4
dap:
    db 16
    db 0
    dw 0
    dw 0
    dw 0
    dd 0
    dd 0
times 32 db 0


;ext2_disk_init & ext2_get_geometry

ext2_disk_init:
    mov ah,00h
    mov dl,[drive]
    int 13h
    mov ah,41h
    mov bx,55AAh
    mov dl,[drive]
    int 13h
    jc .chs
    cmp bx,0AA55h
    jne .chs
    mov byte [lba_supported],1
    ret
.chs:
    mov byte [lba_supported],0
    call ext2_get_geometry
    ret

ext2_get_geometry:
    mov ah,08h
    mov dl,[drive]
    int 13h
    jc .default
    mov [heads],dh
    inc byte [heads]
    mov al,cl
    and al,3Fh
    mov [sectors],al
    xor ax,ax
    mov al,[heads]
    mov [heads_tmp],ax
    xor ax,ax
    mov al,[sectors]
    mov [sectors_tmp],ax
    ret
.default:
    mov word [heads_tmp],16
    mov word [sectors_tmp],63
    ret


; ============================================================================
; ext2_read_sectors - чтение секторов с IDE-диска прямым PIO (без INT 13h)
;
; Вход:   AX:DX = LBA (относительно начала раздела), CX = кол-во секторов,
;         ES:BX = буфер. Для адресов выше 1 МБ в unreal-режиме ES должен быть
;         равен физическому адресу >> 4 (например 0x10000 -> 0x100000).
; Выход:  CF=0 - успех, CF=1 - ошибка. Все регистры сохранены.
;
; Реализация: кратковременный protected mode с flat-дескрипторами GDT
; (base=0, limit=4GB) + rep insw из data-порта IDE. Это и есть рабочий
; механизм доступа к памяти выше 1 МБ на этом этапе (эквивалент unreal для
; записи): сегментная пара ES:BX переводится в линейный адрес вручную как
; (ES << 4) + BX до входа в PM.
; INT 13h здесь не вызывается принципиально: в unreal-режиме BIOS-вызовы
; портят high word'и сегментных дескрипторов.
; ============================================================================

ext2_read_sectors:
    ; --- полный 32-битный LBA старта (DX:AX + EXT2_START_LBA) ---
    add ax, EXT2_START_LBA
    adc dx, 0                       ; DX = старшие 16 бит полного LBA

    push bp
    mov  bp, sp                     ; кадр фиксирован: [bp+...] читается
                                    ; линейно (< 1MB) и в RM, и в PM
    push bx                         ; [bp+4] = BP вызывающего (для rm_enter)
    pushf                           ; [bp+6] = EFLAGS вызывающего (сюда же
                                    ;   впишем CF результата при ошибке)
    push ax                         ; [bp+8] = LBA[0..15]
    push dx                         ; [bp+10]= LBA high
    push cx                         ; [bp+12]= кол-во секторов
    push es                         ; [bp+14]= сегмент буфера (unreal ES)
    push bx                         ; [bp+16]= смещение буфера

    ; --- перенос GDT во временную low-memory область 0x91000 ---
    ; ВАЖНО: временный плоский сегмент грузим в DS, ES НЕ трогаем - иначе
    ; будет испорчено unreal-состояние ES (high word), а именно в ES:BX
    ; пишутся читаемые с диска данные.
    push si
    push di
    mov  ax, 0x9100                 ; временный DS: base=0x91000, high=0
    mov  ds, ax
    mov  si, gdt_start              ; источник: дескрипторы внутри stage2
    mov  di, 0x0008                 ; приёмник: линейный 0x91008 (GDT обязана
    mov  cx, (gdt_end - gdt_start) / 2   ; быть 8-байтно выровнена, иначе
    cld                             ; на реальных CPU возможен GP fault)
    rep movsw                       ; копия GDT -> 0x91008 (ES не затронут)
    mov  word [0x0000], (gdt_end - gdt_start) - 1 ; pseudo-GDTR limit @0x91000
    mov  dword [0x0002], 0x91008                   ; pseudo-GDTR base @0x91002
    pop  di
    pop  si
    cli                             ; IRQ в PM без IDT = triple fault
    lgdt [0x1000]                   ; RM: DS(stage2)<<4 + 0x1000 = 0x91000

    ; --- вход в protected mode ---
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  0x08:.pm

.pm:
    BITS 32
    ; Временные DS/SS := flat (base=0, limit=4GB). ESP не меняется, а весь
    ; кадр и far-pointer лежат в стеке < 1MB, поэтому любые [bp+off] дают
    ; ТОЧНЫЕ линейные адреса (в т.ч. слова >= 0x10000) без обходных окон.
    mov  ax, 0x10
    mov  ds, ax
    mov  ss, ax

    movzx esi, word [bp+16]         ; смещение буфера
    movzx eax, word [bp+14]         ; сегмент буфера (unreal ES = phys>>4)
    shl eax, 4
    add esi, eax                    ; ESI = линейный адрес назначения

    mov  ebx, [bp+8]                ; текущий LBA (полные 32 бита)
    mov  ecx, [bp+12]               ; счётчик оставшихся секторов
    movzx edx, byte [cs:drive]      ; drive: 0x00/0x80 -> primary,
                                    ;         0x01/0x81 -> secondary
    and  edx, 1                     ; EDX = номер канала (0/1)
    shl  edx, 4                     ; bit4 Drive/Head = выбор привода
    mov  ah, byte [bp+11]           ; AH = старший байт полного LBA
    shr  ebx, 8                     ; EBX = LBA >> 8 (24-битное поле регистров)
    sub  edx, 6
    add  edx, 0x1F0                 ; EDX = база канала МИНУС 6:
                                    ; порты base+2..base+7 = EDX+8..EDX+13

    ; --- сохранить контекст для точки входа в RM ---
    mov  [rm_bp], bp                ; линейный адрес кадра (= BP, база flat 0)
    mov  ax, cs
    mov  [rm_cs], ax                ; селектор stage2

.sector_loop:
    push ecx                        ; счётчик секторов на время выдачи команд
    ; --- выбор привода и выдача команды READ SECTORS ---
    ; EDX = база-6; порты: +8=data, +9=error, +10=count, +11=sec,
    ; +12=cyl lo, +13=cyl hi, +14=drive/head, +15=status/command
    mov  al, cl                     ; count (у нас 1..2)
    out  dx, al                     ; [base+2] sector count
    add  edx, 1                     ; EDX = base+2
    mov  al, bl                     ; AL = LBA[8..15]
    out  dx, al                     ; [base+3] sector number
    shr  ebx, 8
    mov  al, bl                     ; AL = LBA[16..23]
    out  dx, al                     ; [base+4] cylinder low
    shr  ebx, 8
    mov  al, bl                     ; AL = LBA[24..31]
    out  dx, al                     ; [base+5] cylinder high
    pop  ecx                        ; восстановить счётчик
    or   al, ah                     ; ah = старший байт полного LBA:
                                    ; bit0->bit4 = канал, биты 1-3 отброшены
    or   al, 0xE0                   ; LBA mode | bit4 = выбор привода
    out  dx, al                     ; [base+6] drive/head
    mov  al, 0x20                   ; CMD: READ SECTORS (PIO)
    out  dx, al                     ; [base+7] command register
    add  edx, 1                     ; EDX = base+7 = status port

.wait_drq:
    in   al, dx                     ; [base+7] status
    test al, 0x08                   ; DRQ?
    jnz  .transfer
    test al, 0x80                   ; BSY?
    jz   .read_error                ; ни BSY, ни DRQ -> ошибка
    jmp  .wait_drq

.transfer:
    ; --- переключение контроллера в 16-битный режим (Device Control, DCR=0) ---
    ; иначе high byte дублируется и rep insw пишет по 2 байта мусора
    add  edx, 2                     ; status+2 = base+9 = control (mirror)
    in   al, dx
    and  al, 0xF7
    out  dx, al
    sub  edx, 2                     ; вернуть EDX = status port

.confirm:
    in   al, dx                     ; подтверждение готовности
    test al, 0x80                   ; BSY=0?
    jnz  .confirm
    test al, 0x08                   ; DRQ=1?
    jz   .read_error
    test al, 0x10                   ; DSC=1?
    jz   .read_error

    ; --- вычитываем сектор в линейно адресуемую память ---
    sub  edx, 7                     ; data port (base+0)
    mov  ecx, 256                   ; 512 байт = 256 слов
    cld
    rep insw                        ; порт -> ESI (flat 4GB, пишет хоть в 0x100000)
    add  esi, 512                   ; следующий сектор буфера
    add  ebx, 1                     ; следующий LBA (EBX = LBA>>24!)
    mov  ecx, [bp+12]               ; перезагрузить счётчик секторов
    dec  ecx
    mov  [bp+12], ecx
    jz   .drain_last
    shl  ebx, 8                     ; вернуть EBX = текущий LBA >> 8
    sub  edx, 7                     ; EDX = base-6, готов к следующей итерации
    jmp  .sector_loop

.drain_last:
    ; --- drain последних данных сектора (4 слова) ---
    push edx                        ; status port (base+7)
    sub  edx, 7                     ; data port (base+0)
    in   ax, dx
    in   ax, dx
    in   ax, dx
    in   ax, dx
    pop  edx                        ; EDX = status port

.drain_wait:
    in   al, dx                     ; финальный статус
    test al, 0x88                   ; BSY | DRQ
    jnz  .drain_wait
    test al, 0x01                   ; ERR?
    jnz  .read_error
    jmp  .read_ok

.read_error:
    mov  al, '?'                    ; debugcon: ошибка чтения сектора
    out  0xE9, al
    or   word [bp+6], 1             ; CF=1 в сохранённый EFLAGS кадра ->
                                    ; popf в rm_enter восстановит его целиком

.read_ok:
.back_rm:
    ; =====================================================================
    ; Возврат в real/unreal mode.
    ; Сегменты восстанавливаются ЕЩЁ В PM (полная 32-битная загрузка
    ; дескрипторов): ES := прежний unreal-сегмент буфера из кадра, DS/SS :=
    ; прежние сегменты stage2 (= CS). Затем high word CS чинится far jump'ом
    ; ДАННЫМИ: far-pointer собирается В РЕГИСТРАХ - в памяти кода нет ни
    ; одного 32-битного immediate, поэтому переход корректен даже при
    ; неполной A20 (все адреса кадра/стека < 1MB).
    ; Точка входа RM = EIP сразу за этим jmp (метка rm_enter ниже).
    ; =====================================================================
    movzx ebx, word [bp+14]         ; прежний unreal-сегмент буфера
    mov   es, bx                    ; полная загрузка дескриптора ES
    mov   ax, cs                    ; прежнее DS/SS stage2 (= CS)
    mov   ds, ax
    mov   ss, ax

    mov   eax, cr0
    and   eax, 0xFE                 ; PE = 0 -> процессор снова в real mode
    lea   ebx, [$+12]               ; полный линейный адрес точки входа RM
                                    ; ($+12 = EIP + 4(push sel)+4(push off)+2)
    mov   ecx, ebx                  ; ECX = тот же адрес
    shr   ebx, 4                    ; BX = селектор (линейная база >> 4)
    push  bx                        ; selector: low = base>>4 (RM-шляпа),
                                    ; high = base>>20 (0 для баз < 1MB)
    push  ecx                       ; offset = ПОЛНЫЙ линейный адрес rm_enter
    jmp   dword [ss:esp]            ; far jump ДАННЫМИ: полная загрузка CS

; --- данные состояния возврата (после конца функции) ---
align 4
rm_bp:     dw 0                     ; линейный адрес кадра (< 1MB)
rm_cs:     dw 0                     ; селектор CS/DS/SS stage2

BITS 16
rm_enter:                           ; точка входа в real mode (сюда приходит
                                    ; far jump из .back_rm); CS/DS/SS/ES уже
                                    ; корректны (восстановлены в PM)
    jmp  $+2                        ; синхронизировать prefetch после
                                    ; 32-битного far jump (латка Intel)
    mov  bp, [rm_bp]                ; BP := кадр ext2_read_sectors:
                                    ; [bp+4]=старый BP, [bp+6]=EFLAGS
    mov  sp, bp                     ; ESP := вершина кадра
    add  sp, 14                     ; снять кадр (BP,EFLAGS,LBAlo,LBAhi,CNT,SEG,OFF)
    popf                            ; CF результата (и IF=состояние вызова)
    ret
