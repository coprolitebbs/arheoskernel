; ---------- Функции чтения ----------

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

disk_read_sector:
    push ax
    push bx
    push cx
    push dx

    xor dx, dx
    div word [spt]
    inc dx
    mov cl, dl
    xor dx, dx
    div word [heads]
    mov ch, al
    mov dh, dl
    mov al, ah
    and al, 3
    shl al, 6
    or cl, al
    mov dl, [drive]
    mov ah, 0x02
    mov al, 1
    int 0x13

    pop dx
    pop cx
    pop bx
    pop ax

    ret


; ============================================================
; fat12_find_file
;
; IN:
;
; DS:SI = FAT 8.3 filename (11 bytes)
;
; OUT:
;
; AX = first cluster
; DX:CX = file size
;
; CF=0 found
; CF=1 not found
;
; ============================================================

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
    mov dx,[root_ent]

.search:
    cmp byte [es:bx],0
    je .not_found
    cmp byte [es:bx],0xE5
    je .next
; сохраняем имя
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


; ============================================================
; fat12_next_cluster
;
; IN:
;   AX = current cluster
;
; OUT:
;   AX = next cluster
;
; FAT расположен:
;   9000:0000
;
; ============================================================

fat12_next_cluster:
    push bx
    push dx
    push ds

; DS = FAT segment
    mov bx,0x9000
    mov ds,bx
; offset = cluster * 3 / 2
    mov bx,ax
    mov dx,ax
    add ax,ax
    add ax,dx        ; ax = cluster*3
    shr ax,1         ; ax = offset
    mov si,ax
    mov ax,[ds:si]
; проверяем четность исходного cluster
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


; ============================================================
; fat12_cluster_to_lba
;
; IN:
;   AX = cluster
;
; OUT:
;   AX = LBA
;
; ============================================================

fat12_cluster_to_lba:
    push bx
    sub ax,2
    mov bx,[spc]
    mul bx
    add ax,[data_start]
    pop bx

    ret


; ============================================================
; fat12_load_file
;
; IN:
;   DS:SI = имя файла 11 байт FAT12
;   ES:BX = адрес загрузки
;
; OUT:
;   CF=0 успех
;   CF=1 ошибка
;
; Использует:
;   fat12_find_file
;   fat12_cluster_to_lba
;   fat12_next_cluster
;
; ============================================================

fat12_load_file:
    push es
    push ax
    push bx
    push cx
    push dx
    push si
; ищем файл
    call fat12_find_file
    jc .error
; AX = первый cluster
    mov [load_cluster],ax
; сохраняем длину
    mov [current_file_size],cx
    mov [current_file_size+2],dx
.load_next:
    mov ax,[load_cluster]
; cluster -> LBA
    call fat12_cluster_to_lba
; читаем один кластер
    mov cx,[spc]
    call disk_read_sectors
    jc .error
; переходим к следующему кластеру
    mov ax,[load_cluster]
    call fat12_next_cluster
    cmp ax,0xFF8
    jae .done
    mov [load_cluster],ax
; следующий адрес загрузки
; ES:BX уже указывает после прочитанного кластера
    mov ax,[spc]
    shl ax,9        ; *512
    add bx,ax
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

; ============================================================
; Variables
; ============================================================

load_cluster dw 0
kernel_size    dd 0
current_file_size dd 0
