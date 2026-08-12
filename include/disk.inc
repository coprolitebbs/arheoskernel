; ============================================================
; disk.inc
; BIOS disk access layer
; ============================================================

BITS 16

; ------------------------------------------------------------
; Инициализация диска
; ------------------------------------------------------------
disk_init:
    mov ah, 0x00
    int 0x13

    ret

; ------------------------------------------------------------
; Чтение одного сектора по LBA
; AX = LBA, ES:BX = destination
; ------------------------------------------------------------
disk_read_sector:
    push ax
    push bx
    push cx
    push dx
    mov [disk_lba], ax
    ; LBA -> CHS
    xor dx, dx
    div word [disk_spt]      ; AX = LBA / SPT, DX = LBA % SPT
    inc dx                   ; сектор (1-based)
    mov cl, dl               ; сохраняем сектор
    xor dx, dx               ; <--- ОБЯЗАТЕЛЬНО обнулить DX перед вторым делением
    div word [disk_heads]    ; AX = (LBA/SPT)/HEADS = цилиндр, DX = (LBA/SPT)%HEADS = головка
    mov dh, dl               ; головка
    mov ch, al               ; младшие 8 бит цилиндра
    mov al, ah               ; старшие 2 бита цилиндра
    and al, 3
    shl al, 6
    or cl, al                ; объединяем с сектором
    mov dl, [drive]
    mov ah, 0x02
    mov al, 1
    int 0x13
    pop dx
    pop cx
    pop bx
    pop ax

    ret

; ------------------------------------------------------------
; Чтение нескольких секторов
; AX = первый LBA, CX = количество, ES:BX = destination
; ------------------------------------------------------------
disk_read_sectors:
    pusha

.read_loop:
    push ax
    push cx
    call disk_read_sector
    pop cx
    pop ax
    jc .error
    add bx, 512
    inc ax
    loop .read_loop
    popa
    clc

    ret

.error:
    popa
    stc

    ret

; ============================================================
; Variables
; ============================================================
;disk_lba dw 0
;disk_spt dw 9
;disk_heads dw 2
