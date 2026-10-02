;Функции чтения

disk_read_sectors:
    pusha

.loop:
    push ax
    push cx
    call disk_read_sector
    pop cx
    pop ax
    jc .error
    add bx, 512
    inc ax
    loop .loop
    popa
    clc

    ret

.error:
    popa
    stc

    ret

;Чтение одного сектора по LBA с повторными попытками
;AX = LBA, ES:BX = destination

disk_read_sector:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov di, 5            ;Количество попыток (5 для надежности)

.retry_loop:
    push di              ;Сохраняем счетчик попыток
    push ax
    push bx
    push cx
    push dx
    push ds

    ;Конвертация LBA в CHS
    ;Важно использовать cs:, так как DS может быть испорчен
    xor dx, dx
    div word [cs:spt]    ;AX = LBA / SPT, DX = LBA % SPT
    inc dx               ;Сектор (1-based)
    mov cl, dl           ;CL = Сектор

    xor dx, dx           ;Обнуляем DX перед следующим делением
    div word [cs:heads]  ;AX = Цилиндр, DX = Головка
    mov ch, al           ;CH = Младшие 8 бит цилиндра
    mov dh, dl           ;DH = Головка

    ;Старшие 2 бита цилиндра в старшие биты CL
    mov al, ah
    and al, 0x03
    shl al, 6
    or cl, al

    mov dl, [cs:drive]   ;Номер диска
    mov ah, 0x02         ;Функция чтения
    mov al, 1            ;Читать 1 сектор

    ;Защита памяти для реального железа
    ;Иначе оно жрет имена файлов на дискетах

    xor si, si
    mov ds, si

    int 0x13             ;Вызов BIOS
    jc .read_error       ;Если CF=1, ошибка

    ;Успех
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    pop di               ;Восстанавливаем счетчик
    jmp .success_exit

.read_error:
    ;Восстанавливаем регистры для следующей попытки
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    pop di               ;Восстанавливаем счетчик попыток

    dec di
    jz .fail_exit        ;Если попытки кончились -> выход с ошибкой

    ;Сброс контроллера дискеты перед повторной попыткой
    ;Это критично для реального железа при временных сбоях
    push ax
    push dx
    mov ah, 0x00         ;Сброс контроллера
    mov dl, [cs:drive]
    int 0x13
    pop dx
    pop ax

    jmp .retry_loop      ;Пробуем снова

.fail_exit:
    ;Ошибка после всех попыток
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc                  ;Устанавливаем флаг ошибки
    ret

.success_exit:
    ;Успешный выход
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc                  ;Снимаем флаг ошибки
    ret

;fat12_find_file

;IN:
;DS:SI = FAT 8.3 filename (11 bytes)

;OUT:
;AX = first cluster
;DX:CX = file size

;CF=0 found
;CF=1 not found

fat12_find_file:
    push bx
    ;push dx
    push di
    push es
; root dir
    mov bx,0x8000
    mov es,bx
    xor bx,bx
; number of entries
    mov dx,[cs:root_ent]

.search:
    cmp byte [es:bx],0
    je .not_found
    cmp byte [es:bx],0xE5
    je .next
;сохраняем имя
    push si
    mov di,bx
    mov cx,11
    cld
    repe cmpsb
    pop si
    je .found

.next:
    add bx,32
    dec dx
    jnz .search

.not_found:
    pop es
    pop di
    ;pop dx
    pop bx
    stc

    ret

.found:

; cluster
    mov ax,[es:bx+26]
; size DWORD
    mov cx,[es:bx+28]
    mov dx,[es:bx+30]
    pop es
    pop di
    ;pop dx
    pop bx
    clc

    ret


;fat12_next_cluster
;IN:
;AX = current cluster

;OUT:
;AX = next cluster

;FAT расположен:
;9000:0000


fat12_next_cluster:
    push bx
    push dx
    push ds

;DS = FAT segment
    mov bx,0x9000
    mov ds,bx
; offset = cluster * 3 / 2
    mov bx,ax
    mov dx,ax
    add ax,ax
    add ax,dx        ;ax = cluster*3
    shr ax,1         ;ax = offset
    mov si,ax
    mov ax,[ds:si]
;проверяем четность исходного кластера
    test bx,1
    jnz .odd

.even:
    and ax,0x0FFF
    jmp .done

.odd:
    shr ax,4

.done:
    pop ds
    pop dx
    pop bx

    ret


;fat12_cluster_to_lba
;IN:
;AX = cluster

;OUT:
;AX = LBA

fat12_cluster_to_lba:
    push bx
    sub ax,2
    mov bx,[cs:spc]
    mul bx
    add ax,[cs:data_start]
    pop bx

    ret



;fat12_load_file
;IN:
;DS:SI = имя файла 11 байт FAT12
;ES:BX = адрес загрузки

;OUT:
;CF=0 успех
;CF=1 ошибка

;Использует:
;fat12_find_file
;fat12_cluster_to_lba
;fat12_next_cluster


fat12_load_file:
    push es
    push ax
    push bx
    push cx
    push dx
    push si
;ищем файл
    push cs
    pop ds
    call fat12_find_file
    jc .error
;AX = первый cluster
    mov [cs:load_cluster],ax
;сохраняем длину
    mov [cs:current_file_size],cx
    mov [cs:current_file_size+2],dx
.load_next:
    mov ax,[cs:load_cluster]
;cluster -> LBA
    call fat12_cluster_to_lba
;Перед чтением BX всегда должен быть равен 0, чтобы BIOS писал строго в начало ES
    xor bx, bx
;читаем один кластер
    mov cx,[cs:spc]
    call disk_read_sectors
    jc .error
;переходим к следующему кластеру
    mov ax,[cs:load_cluster]
    call fat12_next_cluster
    cmp ax,0xFF8
    jae .done
    mov [cs:load_cluster],ax
;следующий адрес загрузки
;ES:BX уже указывает после прочитанного кластера
    mov ax,[cs:spc]
    shl ax,9        ; *512
    shr ax,4
    ;add bx,ax
    mov cx, es
    add cx, ax
    mov es, cx
    jmp .load_next

.done:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop es

    clc

    ret


.error:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    stc

    ret


;Функция сброса контроллера дискеты
disk_reset:
    push ax
    push dx
    mov ah, 0x00      ;Сброс контроллера
    mov dl, [cs:drive]   ;Номер привода (0x00 или 0x01)
    int 0x13
    jc .error
    clc
    pop dx
    pop ax
    ret
.error:
    stc
    pop dx
    pop ax
    ret


; Variables

load_cluster dw 0
kernel_size    dd 0
current_file_size dd 0
