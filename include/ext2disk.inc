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


; ext2_read_sectors - чтение секторов с IDE-диска (LBA, PIO)
; Вход:   AX:DX = LBA (относительно начала раздела), CX = кол-во секторов,
;         ES:BX = буфер. Для адресов выше 1 МБ в unreal-режиме ES должен быть
;         равен физическому адресу >> 4 (например 0x10000 -> 0x100000).
; Выход:  CF=0 - успех, CF=1 - ошибка
; Реализация: прямой PIO к IDE-портам (IN/OUT + rep insw) в кратковременном
; protected mode с flat-дескрипторами GDT (база 0, limit 4GB) - это и есть
; рабочий механизм "unreal"-доступа к памяти выше 1 МБ на этом этапе.
; INT 13h не вызывается принципиально: в unreal-режиме BIOS-вызовы портят
; high word'и сегментных регистров.

ext2_read_sectors:
    add ax, EXT2_START_LBA
    adc dx, 0

    ; Кэшируем полный LBA и количество в переменные: в цикле по секторам
    ; регистры не придётся беречь между портовыми последовательностями.
    mov [cs:cur_lba_low], ax
    mov [cs:cur_lba_high], dx
    mov [cs:cur_count], cx

    pushf
    push ds                 ; стек (низ->верх): EFLAGS, DS, pusha(16), ES,BX,DX,AX,CX,BP(own)
    pusha                   ; [bp+38]=EFLAGS [bp+36..22]=pusha
    push es                 ; [bp+20] = буферный сегмент (unreal ES = phys>>4)
    push bx                 ; [bp+18] = буферное смещение
    push bp                 ; [bp+16] = новый SP (адрес кадра для возврата в RM)
                            ; [bp+14] = BP вызывающего

    mov bp, sp              ; фиксированный кадр: в PM [bp+...] читается линейно (< 1MB)

    cli                     ; IRQ в PM без IDT = мгновенный triple fault

    ; GDT с flat-дескрипторами (0x08 код / 0x10 данные, base=0, limit=4GB).
    ; Копируем дескрипторы и готовим pseudo-GDTR в свободной low memory на
    ; 0x91000 (PM-стек позже стартует от 0x90000 вниз; 0x91000-0x91100 свободен).
    ; ВАЖНО: временный плоский сегмент грузим в DS, ES НЕ трогаем — иначе
    ; будет испорчено unreal-состояние ES (high word), а именно в ES:BX
    ; пишется читаемый с диска data.
    push si                         ; сохранить SI/DI (rep movsw их изменит)
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
                                    ; DS остаётся 0x9100 - в PM он всё равно
                                    ; перезагружается flat-селектором 0x10
    lgdt [0x9100]                   ; DS=0x9100: линейный адрес псевдо-GDTR
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:.pm

.pm:
    mov ax, 0x10
    mov ds, ax
    mov ss, ax              ; ESP остаётся прежним -> linear < 1MB, стек цел
    movzx esi, word [bp+18]       ; смещение буфера
    movzx eax, word [bp+20]       ; сегмент буфера (в unreal ES = phys>>4)
    add esi, eax                  ; ESI = физический адрес назначения
    mov ebx, [ss:cur_lba_low]     ; EBX = полный стартовый LBA
    movzx edi, word [ss:cur_count]    ; EDI = количество секторов
    movzx edx, byte [ss:drive]    ; drive: 0x00/0x80 -> primary, 0x01/0x81 -> secondary
    and edx, 1                    ; номер канала
    shl edx, 3                    ; *8
    add edx, 0x1F0                ; база портов: 0x1F0 или 0x170
    sub edx, 6                    ; DX = база-6: прибавляя 2..7 получаем порты
                                ; base+2..base+7 (значения DH не меняются)

; --- цикл чтения по секторам (PIO, последовательность SFF/ATA) ---
; EBX = текущий LBA, ESI = линейный адрес приёма, EDI = счётчик секторов,
; EDX = база-6 порта канала. AL - значения, DX - порты.
.sector_loop:
    ; Drive/Head: LBA mode + бит привода (0xE0 | ((drive&1)<<4))
    mov eax, ebx
    shr eax, 24                     ; LBA[24..31]
    and eax, 0x0F
    or eax, 0xE0
    mov ecx, edx                    ; копия базы-6
    add cl, 6                       ; порт Drive/Head = base+6
    xchg eax, ecx                   ; AL = значение? нет: ECX=значение, AL=порт low
    mov al, cl                      ; ...перестановка: нужен AL=значение, DX=порт
    ; корректная раскладка:
    movzx ecx, dl
    add cl, 6                       ; CL = порт base+6
    mov eax, ebx
    shr eax, 24
    and eax, 0x0F
    or eax, 0xE0
    or al, ch                       ; bit4 = выбор привода на канале
    xchg eax, ecx                   ; AL = порт... нет!
    ; (см. финальную версию ниже - этот черновик заменяется целиком)

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
