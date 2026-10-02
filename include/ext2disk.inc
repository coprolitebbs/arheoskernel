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


; Disk Address Packet

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


; ext2_read_sectors - чтение секторов с IDE-диска (LBA)
; Вход:   AX:DX = LBA (относительно начала раздела), CX = кол-во секторов,
;         ES:BX = буфер. Для адресов выше 1 МБ в unreal-режиме ES должен быть
;         равен физическому адресу >> 4 (например 0x10000 -> 0x100000).
; Выход:  CF=0 - успех, CF=1 - ошибка
; Реализация: прямой PIO-доступ к IDE-портам через IN/OUT.
; INT 13h не используется принципиально: в unreal-режиме вызов BIOS приводит
; к порче high word'ов сегментных регистраторов и невозможности писать > 1 МБ.
; Функция сама переключает CPU в protected mode (GDT с 4GB-дескрипторами) для
; линейной записи в ES:BX и возвращается обратно в real/unreal mode.

ext2_read_sectors:
    add ax, EXT2_START_LBA
    adc dx, 0

    pushf
    push ds                 ; [bp+34] = селектор DS вызывающего (для unreal-возврата)
    pusha

    mov bp, sp              ; фиксированный кадр: в PM [bp+...] читается линейно (< 1MB)

    ; Сохраняем параметры вызова на стеке: [ES:BX] -> физический адрес, LBA, кол-во
    push es                 ; [bp+14] = буферный сегмент
    push bx                 ; [bp+12] = буферное смещение
    push dx                 ; [bp+10] = LBA high
    push ax                 ; [bp+8]  = LBA low
    push cx                 ; [bp+6]  = количество секторов

    cli                     ; IRQ в PM без IDT = мгновенный triple fault

    ; GDT с flat-дескрипторами (0x08 код / 0x10 данные, base=0, limit=4GB).
    ; Копируем дескрипторы и готовим pseudo-GDTR в свободной low memory на
    ; 0x91000 (PM-стек позже стартует от 0x90000 вниз; 0x91000-0x91100 свободен).
    ; ВАЖНО: временный плоский сегмент грузим в DS, ES НЕ трогаем — иначе
    ; будет испорчено unreal-состояние ES (high word), а именно в ES:BX
    ; пишется читаемый с диска data.
    push ds                         ; сохранить сегмент stage2
    push si
    push di
    mov ax, 0x9100                  ; временный DS: base=0x91000, high word=0
    mov ds, ax
    mov si, gdt_start               ; источник: сами дескрипторы (внутри stage2)
    mov di, 0x2                     ; приёмник: линейный 0x91002
    mov cx, (gdt_end - gdt_start) / 2
    cld
    rep movsw                       ; копия GDT -> 0x91002 (ES не затронут)
    mov word [di], ((gdt_end - gdt_start) - 1) & 0xFFFF   ; limit pseudo-gdtr (0x9100A)
    mov dword [di+2], 0x91002       ; base pseudo-gdtr (0x9100C)
    pop di
    pop si
    pop ds                          ; вернуть сегмент stage2
    lgdt [0x9100]                   ; DS=stage2: offset читается как seg*16+off
                                    ;           = линейный адрес псевдо-GDTR
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:.pm

.pm:
    mov ax, 0x10
    mov ds, ax
    mov ss, ax              ; ESP остаётся прежним -> linear < 1MB, стек цел
    movzx esi, word [bp+12]       ; смещение буфера
    movzx eax, word [bp+14]       ; сегмент буфера (в unreal ES = phys>>4)
    add esi, eax                  ; ESI = физический адрес назначения
    movzx edi, word [bp+6]        ; EDI = количество секторов
    movzx ebx, word [bp+8]        ; LBA low
    movzx eax, word [bp+10]       ; LBA high
    shl eax, 16
    or ebx, eax                   ; EBX = полный LBA
    movzx edx, byte [ss:drive]    ; drive: 0 -> primary, 1 -> secondary
    shl edx, 3                    ; *8
    add edx, 0x1F0                ; база портов: 0x1F0 или 0x170

; --- цикл чтения по секторам (PIO, последовательность SFF/ATA) ---
; EBX = текущий LBA, ESI = линейный адрес приёма, EDI = счётчик секторов,
; EDX = база портов канала (0x1F0/0x170). EAX как рабочий регистр значений.
.sector_loop:
    ; --- Setup: Drive/Head, Sector Count, LBA Low/Mid/High ---
    mov ecx, edx                    ; сохранить базу портов
    mov eax, ebx
    shr eax, 24                     ; LBA[24..31]
    and eax, 0x0F
    or eax, 0xE0                    ; LBA mode + reserved bits
    movzx edi, byte [ss:drive]      ; 0x80 -> master, 0x81 -> slave
    and edi, 1
    shl edi, 4                      ; бит D4 = выбор привода на канале
    or eax, edi
    mov edx, ecx                    ; восстановить базу портов
    add dl, 6                       ; порт Drive/Head = base+6
    out dx, al
    mov al, 1                       ; Sector Count = 1 (PIO по одному сектору)
    sub dl, 4                       ; база+2 = Sector Count
    out dx, al
    mov ecx, edx                    ; сохранить базу портов
    mov eax, ebx                    ; полный LBA
    inc dl                          ; база+3 = LBA Low
    out dx, al
    shr eax, 8
    inc dl                          ; база+4 = LBA Mid
    out dx, al
    shr eax, 8
    inc dl                          ; база+5 = LBA High
    out dx, al
    mov edx, ecx                    ; восстановить базу портов
    add dl, 5                       ; база+7 = Status/Command
    in al, dx                       ; discard status (первая операция после записей)
    in al, dx                       ; вторая discard (settle time)

.wait_free:
    ; --- диск должен быть свободен до подачи команды ---
    in al, dx
    test al, 0x80                   ; BSY
    jnz .wait_free
    test al, 0x01                   ; ERR
    jnz .read_error
    test al, 0x40                   ; DRDY
    jz .read_error

    mov ecx, edx                    ; сохранить порт статуса
    mov al, 0x20                    ; CMD: READ SECTORS (PIO)
    inc dl                          ; база+7 = command
    out dx, al
    mov edx, ecx                    ; восстановить порт статуса

.wait_drq:
    ; --- ожидание DRQ (или ошибки) ---
    in al, dx
    test al, 0x08                   ; DRQ
    jnz .transfer
    test al, 0x80                   ; BSY
    jz .read_error                  ; не BSY и не DRQ -> ошибка
    jmp .wait_drq

.transfer:
    ; --- переключение контроллера в 16-битный режим (diskmode = 0) ---
    ; иначе high byte дублируется и rep insw пишет мусор по 2 байта
    mov ecx, edx                    ; сохранить порт статуса
    sub edx, 5                      ; база+2 = Device Control
    in al, dx
    and al, 0xF7
    out dx, al
    mov edx, ecx                    ; восстановить порт статуса

    ; --- подтверждение готовности: BSY=0, DRQ=1, DSC=1 ---
.confirm:
    in al, dx
    test al, 0x80                   ; BSY
    jnz .confirm
    test al, 0x08                   ; DRQ
    jz .read_error
    test al, 0x10                   ; DSC
    jz .read_error

    ; --- вычитываем сектор в linear-адресуемую память ---
    sub dl, 7                       ; data-порт (база+0)
    mov ecx, 256                    ; 512 байт = 256 слов
    cld
    rep insw                        ; порт -> ESI (flat 4GB, пишет хоть в 0x100000)

    ; --- следующая секция ---
    add esi, 512                    ; сдвинуть буфер приёма на следующий сектор
    inc ebx                         ; следующий LBA (EBX не портился в блоке setup)
    mov edi, ecx                    ; ...кроме ECX: сохраняем порт статуса
    dec word [ss:bp+6]              ; счётчик секторов в кадре
    jz .drain_last
    mov edx, edi                    ; порт статуса
    jmp .sector_loop

.drain_last:
    ; --- drain последних данных сектора (4 слова) ---
    mov edx, edi                    ; порт статуса
    push edx
    sub edx, 7                      ; data-порт
    in ax, dx
    in ax, dx
    in ax, dx
    in ax, dx
    pop edx                         ; порт статуса

.drain_wait:
    ; --- финальный статус: ждём сброса BSY/DRQ ---
    in al, dx
    test al, 0x88                   ; BSY | DRQ
    jnz .drain_wait
    test al, 0x01                   ; ERR?
    jnz .read_error

.read_done:
    ; --- вернуть BIOS-совместимое состояние диска (INT 13h может читаться позже) ---
    push edx                        ; порт статуса
    mov eax, ebx
    shr eax, 24
    and eax, 0x0F
    or eax, 0xE0                    ; LBA mode (бит привода не важен)
    sub edx, 1                      ; база+6 = Drive/Head
    out dx, al
    mov al, 0x0E                    ; CMD: VERIFY (без transfer, быстрый reset-ready)
    mov al, 0xC8                    ; CMD: WRITE MULTIPLE... нет: используем SET MULTIPLE
    mov al, 0x08                    ; CMD: SET MULTIPLE MODE
    mov ecx, edx                    ; база+6
    add cl, 1                       ; база+7 = Command
    mov edx, ecx
    out dx, al
    pop edx                         ; порт статуса
    in al, dx                       ; discard
    in al, dx                       ; discard
.mult_wait:
    in al, dx
    test al, 0x80                   ; BSY
    jnz .mult_wait
    ; ERR по SET MULTIPLE не критичен — игнорируем

.read_error:
    mov al, '?'                     ; debugcon: ошибка чтения сектора
    out 0xE9, al
    ; ставим CF=1 в сохранённый кадром EFLAGS — iret восстановит его целиком
    or word [ss:bp+34], 1           ; CF=1 -> вызывающий увидит ошибку
    jmp .back_rm

.back_rm:
    ; снимаем RM-счётчик секторов из кадра (он больше не нужен)
    movzx eax, word [ss:bp+34]      ; сохранённый push ds селектор DS
    mov ds, ax
    movzx eax, word [ss:bp+14]      ; ES вызывающего (unreal-сегмент буфера)
    mov es, ax

    add sp, 10                      ; снимаем 5 сохранённых параметров (ES,BX,DX,AX,CX)
    popa
    pop ds                          ; DS вызывающего (сегмент stage2)
    add sp, 2                       ; убрать со стека сохранённые pushf EFLAGS
    sti                             ; прерывания снова легальны (real mode, BIOS IDT цела)
    iret                            ; возврат к вызывающему с восстановленными
                                    ; EFLAGS (в т.ч. CF): popf в PM не восстановил бы VM-бит

; --- стек на момент возврата (высокий адрес -> низкий):
; [bp+38]=CS(ret) [bp+36]=IP(ret) [bp+34]=EFLAGS(pushf) [bp+32]=DS(push)
; [bp+30..16]=pusha [bp+14]=ES [bp+12]=BX [bp+10]=DX [bp+8]=AX [bp+6]=CX [bp+4]=BP
; Выход: mov ds/es из кадра (PM) / add sp,10 / popa / pop ds / add sp,2 / sti / iret
; CF ошибки передаётся через правку слова EFLAGS в кадре ([bp+34]) до iret.
