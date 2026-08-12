BITS 16

; ============================================================
; ext2disk.inc
;
; BIOS INT13 extensions LBA reader
;
; IN:
;
; AX = LBA low
; DX = LBA high
; CX = sectors
; ES:BX = buffer
;
; OUT:
;
; CF=0 OK
; CF=1 error
;
; ============================================================

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


EXT2_START_LBA equ 17

; ============================================================
; Disk Address Packet
;
; ============================================================
align 4

dap:
    db 16
    db 0
    dw 0
    dw 0
    dw 0
    dd 0
    dd 0
times 32 db 0   ;fill

; ============================================================
; ext2_disk_init
;
; ============================================================

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

; ============================================================
; Get CHS geometry
;
; ============================================================

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

; ============================================================
; ext2_read_sectors
;
; ============================================================

ext2_read_sectors:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds
    push cs
    pop ds
    mov byte [dap],16
    mov byte [dap+1],0
    mov [dap+2],cx
    mov [dap+4],bx
    mov [dap+6],es

    add ax,EXT2_START_LBA
    adc dx,0

    mov [dap+8],ax
    mov [dap+10],dx
    mov word [dap+12],0
    mov word [dap+14],0
    mov si,dap
    mov ah,42h
    mov dl,[drive]
    int 13h
    jc .error
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc

    ret

.error:
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc

    ret

; ============================================================
; Messages
; ============================================================

