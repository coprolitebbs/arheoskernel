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

;Функции чтения

KERNEL_SEG   equ 0x2000
INDIRECT_SEG equ 0x1800

disk_read_sectors:
    push bx
    push cx
    push ax
.loop:
    call disk_read_sector
    jc .error
    add bx, 512
    inc ax
    loop .loop
    pop ax
    pop cx
    pop bx
    clc
    ret
.error:
    pop ax
    pop cx
    pop bx
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

    mov di, 5               ; счётчик попыток в регистре, НЕ на стеке

.retry_loop:
    push ax
    push bx
    push cx
    push dx
    push ds

    ; --- LBA -> CHS ---
    xor dx, dx
    div word [cs:spt]
    inc dx
    mov cl, dl              ; CL = sector (1-based)

    xor dx, dx
    div word [cs:heads]
    mov ch, al              ; CH = cylinder low
    mov dh, dl              ; DH = head

    mov al, ah
    and al, 0x03
    shl al, 6
    or  cl, al              ; CL[6:7] = cylinder high

    mov dl, [cs:drive]
    mov ah, 0x02
    mov al, 1

    xor si, si
    mov ds, si              ; DS = 0 для надёжности BIOS

    int 0x13
    jc .read_error

    ; Успех
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    jmp .success_exit

.read_error:
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax

    dec di
    jz .fail_exit

    ; --- Reset и пауза ---
    push ax
    push dx
    mov ah, 0x00
    mov dl, [cs:drive]
    int 0x13
    pop dx
    pop ax

    ; --- Пауза ~50-100 мс (вложенный цикл, 1 внешняя итерация ≈ 4 мс на 100 МГц) ---
    push bx
    push cx
    mov bx, 20
.pause_outer:
    xor cx, cx
.pause_inner:
    loop .pause_inner
    dec bx
    jnz .pause_outer
    pop cx
    pop bx

    jmp .retry_loop
.fail_exit:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

.success_exit:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret


;fat12_find_file

;IN:
;DS:SI = FAT 8.3 filename (11 bytes)

;OUT:
;AX = first cluster
;DX:CX = file size
;CF=0 found, CF=1 not found

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


fat12_load_kernel_high:
    pushad                      ;Сохраняем абсолютно все регистры общего назначения
    push ds
    push es
    push fs
    push gs

    ;Инициализируем локальные переменные смещения
    mov dword [cs:kernel_load_offset], 0
    mov edi, 0x100000           ;Физический адрес назначения (1 МБ)

.cluster_loop:
    ;Проверяем, не прочитали ли мы весь файл ядра
    mov eax, [cs:kernel_load_offset]
    cmp eax, [cs:kernel_size_pm]
    jae .done

    ;Восстанавливаем DS на сегмент загрузчика (CS)
    push cs
    pop ds

    ;Получаем текущий кластер ядра, переводим его в LBA-сектор диска
    mov ax, [cs:cluster]
    call fat12_cluster_to_lba

    ;Настраиваем буфер реального режима ES:BX = KERNEL_SEG:0x0000 (0x20000 физ.)
    mov bx, KERNEL_SEG          ; 0x2000
    mov es, bx
    xor bx, bx

    ;Читаем строго то количество секторов, которое задано в BPB дискеты
    ;Для 1.44МБ прочитает 1 сектор, для 720КБ прочитает 2 сектора.
    ;Переменная read_count удалена, так как disk_read_sectors принимает счетчик прямо в CX
    mov cx, [cs:spc]
    call disk_read_sectors
    jc .error

    ;Вычисляем размер кластера в байтах (SectorsPerCluster * 512)
    mov ax, [cs:spc]
    shl ax, 9                   ;AX = ax * 512 (Размер кластера в байтах: 512 или 1024)
    movzx ebx, ax               ;Сохраняем размер кластера в EBX

    ;Проверяем, не является ли этот кластер последним (хвостом файла)
    mov eax, [cs:kernel_size_pm]
    sub eax, [cs:kernel_load_offset]

    cmp eax, ebx
    jae .full_cluster
    mov ebx, eax                ;Если остаток файла меньше размера кластера, копируем только остаток
.full_cluster:

    ;Установка UNREAL MODE для регистра GS
    cli
    push ds

    mov eax, [cs:gdtr+2]
    cmp eax, 0x7E00
    jae .unreal_ok
    add eax, 0x7E00
    mov [cs:gdtr+2], eax
.unreal_ok:
    lgdt [cs:gdtr]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    mov ax, 0x10
    mov gs, ax                  ;Взвели супер-лимит 4 ГБ в GS

    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax

.unreal_rm_clear:
    pop ds
    sti

    ;Копирование кластера в память выше 1Мб
    push ecx
    push si

    mov si, 0x0000              ;Смещение источника в KERNEL_SEG
    mov ecx, ebx                ;Количество байт для копирования (из EBX)

.copy_byte:
    mov al, [es:si]
    mov [gs:edi], al            ;Запись выше 1 МБ через GS
    inc si
    inc edi
    dec ecx
    jnz .copy_byte

    pop si
    pop ecx

    ;Инкрементируем счетчик загруженных байт на точное скопированное число (EBX)
    add [cs:kernel_load_offset], ebx

    ;Переходим к следующему кластеру по таблице FAT
    push ds
    push cs
    pop ds
    mov ax, [cs:cluster]
    call fat12_next_cluster
    pop ds

    cmp ax, 0xFF8
    jae .done
    mov [cs:cluster], ax
    jmp .cluster_loop

.done:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    clc
    ret

.error:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    stc
    ret



dump_kernel:
    pushad
    push ds
    push es
    push fs
    ; Настраиваем DS на сегмент кода Stage 2 (CS), чтобы прочитать переменную
    mov ax, cs
    mov ds, ax

    ; Печатаем маркер размера [S2_SIZE]:
    mov al, '['
    call debug_char
    mov al, 'S'
    call debug_char
    mov al, '2'
    call debug_char
    mov al, '_'
    call debug_char
    mov al, 'S'
    call debug_char
    mov al, 'I'
    call debug_char
    mov al, 'Z'
    call debug_char
    mov al, 'E'
    call debug_char
    mov al, ']'
    call debug_char
    mov al, ' '
    call debug_char

    ;Считываем 32-битное значение размера файла ядра
    mov ebx, [cs:kernel_size_pm]

    ;Выводим ebx в HEX-виде в порт 0xE9 (8 нибблов)
    mov ecx, 8
.size_hex_loop:
    rol ebx, 4
    mov al, bl
    and al, 0x0F
    cmp al, 10
    jb .sz_digit
    add al, 'A' - 10
    jmp .sz_out
.sz_digit:
    add al, '0'
.sz_out:
    call debug_char
    loop .size_hex_loop

    ; Перевод строки после вывода размера
    mov al, 13
    call debug_char
    mov al, 10
    call debug_char

    cli
    lgdt [cs:gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp .pm_flush
.pm_flush:
    mov ax, 0x10
    mov fs, ax
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp .rm_flush
.rm_flush:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    sti

    mov esi, 0x100000
    mov cx, 64

.next_byte:
    mov al, [fs:esi]
    mov dl, al
    shr al, 4
    call .nibble_to_hex
    call .output_char
    mov al, dl
    and al, 0x0F
    call .nibble_to_hex
    call .output_char
    mov al, ' '
    call .output_char
    inc esi
    mov eax, esi
    sub eax, 0x100000
    and eax, 15
    jnz .loop_end
    mov al, 13
    call .output_char
    mov al, 10
    call .output_char
.loop_end:
    dec cx
    jnz .next_byte

    pop fs
    pop es
    pop ds
    popad
    ret

.nibble_to_hex:
    cmp al, 10
    jb .is_digit
    add al, 'A' - 10
    ret
.is_digit:
    add al, '0'
    ret

.output_char:
    call debug_char
    push ax
    push bx
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop bx
    pop ax
    ret


; Variables

load_cluster dw 0
;kernel_size    dd 0
current_file_size dd 0
kernel_load_offset dd 0

