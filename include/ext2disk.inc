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

;Низкоуровневые функции Stage 2

BITS 16

lba_supported db 0
heads_tmp     dw 16
sectors_tmp   dw 63
heads         db 0
sectors       db 0

;Буфер для CHS-пути
chs_lba       dd 0
chs_count     dw 0
chs_buf_off   dw 0
chs_cylinder  dw 0
chs_head      db 0
chs_sector    db 0

EXT2_START_LBA equ 64

;Disk Address Packet
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


ext2_disk_init:
    mov ah, 00h
    mov dl, [drive]
    int 13h                    ;reset disk
    mov ah, 41h
    mov bx, 55AAh
    mov dl, [drive]
    int 13h
    jc .chs
    cmp bx, 0AA55h
    jne .chs
    mov byte [lba_supported], 1
    ret
.chs:
    mov byte [lba_supported], 0
    call ext2_get_geometry
    ret


ext2_get_geometry:
    mov ah, 08h
    mov dl, [drive]
    int 13h
    jc .default
    mov [heads], dh
    inc byte [heads]           ;BIOS отдаёт max head, +1 для count
    mov al, cl
    and al, 3Fh
    mov [sectors], al
    ;Sanity: если что-то ноль — дефолт
    cmp byte [heads], 0
    je .default
    cmp byte [sectors], 0
    je .default
    xor ax, ax
    mov al, [heads]
    mov [heads_tmp], ax
    xor ax, ax
    mov al, [sectors]
    mov [sectors_tmp], ax
    ret
.default:
    mov byte [heads], 16
    mov byte [sectors], 63
    mov word [heads_tmp], 16
    mov word [sectors_tmp], 63
    ret


; ext2_read_sectors
;   Вход:  DX:AX = LBA (32 бита), CX = число секторов, ES:BX = буфер
;   Выход: CF=0 успех / CF=1 ошибка
ext2_read_sectors:
    add ax, EXT2_START_LBA
    adc dx, 0

    pushf
    push ds
    pusha

    mov bp, sp
    xor ax, ax
    mov ds, ax

    ;Если LBA когда-либо работало — пробуем его
    cmp byte [lba_supported], 1
    jne .go_chs

    ; --- LBA path (INT 13h AH=42h) ---
    mov si, dap
    mov byte [si], 16
    mov byte [si+1], 0

    mov cx, [bp+12]
    mov bx, [bp+8]

    mov [si+2], cx
    mov [si+4], bx
    mov [si+6], es

    mov ax, [bp+14]
    mov dx, [bp+10]

    mov [si+8], ax
    mov [si+10], dx
    mov word [si+12], 0
    mov word [si+14], 0

    mov dl, [drive]
    mov ah, 42h
    int 13h
    jnc .ok

    ; BIOS не осилил LBA (Geode GX1, старые Award/AMI).
    ; Сбрасываем флаг НАВСЕГДА и уходим на CHS.
    mov byte [lba_supported], 0
    call ext2_get_geometry

.go_chs:
    ; --- CHS path (INT 13h AH=02h) ---
    mov ax, [bp+14]
    mov dx, [bp+10]
    mov [chs_lba], ax
    mov [chs_lba+2], dx
    mov ax, [bp+12]
    mov [chs_count], ax
    mov ax, [bp+8]
    mov [chs_buf_off], ax
    ;ES уже настроен вызывающим

.chs_next:
    cmp word [chs_count], 0
    je .ok

    ; LBA -> C/H/S:  LBA / spt -> temp, remainder = sector-1
    ;                temp / heads -> cyl, remainder = head
    mov ax, [chs_lba]
    mov dx, [chs_lba+2]
    movzx bx, byte [sectors]
    test bx, bx
    jz .error
    div bx
    mov [chs_sector], dl
    inc byte [chs_sector]

    movzx bx, byte [heads]
    test bx, bx
    jz .error
    xor dx, dx
    div bx
    mov [chs_cylinder], ax
    mov [chs_head], dl

    ; Собираем регистры для INT 13h AH=02h:
    ;   CH = cyl low, CL = sector | (cyl high << 6), DH = head, DL = drive
    mov ax, [chs_cylinder]
    mov ch, al
    mov cl, ah
    and cl, 0x03
    shl cl, 6
    mov al, [chs_sector]
    and al, 0x3F
    or  cl, al
    mov dh, [chs_head]
    mov dl, [drive]
    mov bx, [chs_buf_off]
    mov ax, 0x0201             ;AH=02 (read), AL=01 (1 sector)
    int 13h
    jnc .chs_ok

    ; Ошибка — пробуем reset disk и ещё раз
    push ax
    push bx
    push cx
    push dx
    push es
    xor ax, ax
    mov dl, [drive]
    int 13h                    ;AH=00 reset disk
    pop es
    pop dx
    pop cx
    pop bx
    pop ax

    mov ax, 0x0201
    int 13h
    jc .error

.chs_ok:
    inc word [chs_lba]
    adc word [chs_lba+2], 0
    add word [chs_buf_off], 0x0200
    jnc .chs_off_ok
    mov ax, es
    add ax, 0x1000
    mov es, ax
.chs_off_ok:
    dec word [chs_count]
    jmp .chs_next

.ok:
    popa
    pop ds
    popf
    clc
    ret

.error:
    mov al, '?'
    call debug_char
    popa
    pop ds
    popf
    stc
    ret
