; ============================================================
; fat12.inc
;
; FAT12 filesystem layer
;
; Требует:
;
; drive        db
; fat_buffer   equ 0x9000
; root_buffer  equ 0x8000
;
; Использует disk.inc
;
; ============================================================


; ------------------------------------------------------------
; FAT12 init
;
; Загружает FAT и ROOT directory
;
; ------------------------------------------------------------

fat12_init:
    ; -------------------------
    ; ROOT directory
    ;
    ; FAT12 layout:
    ;
    ; boot sector  = 1
    ; FAT1         = 1-5
    ; FAT2         = 6-10
    ; ROOT         = 11-24
    ;
    ; -------------------------

    mov ax, ROOT_LBA
    mov cx, ROOT_SECTORS
    mov bx, 0
    mov ax,ROOT_SEG
	mov es,ax
    call disk_read_sectors
    jc .error
; FAT
    mov ax, FAT_LBA
    mov cx, FAT_SECTORS
    mov bx,0
    mov ax,FAT_SEG
	mov es,ax
    call disk_read_sectors
    jc .error
    clc

    ret


.error:

    stc
    ret


; ------------------------------------------------------------
; fat12_find_file
;
; DS:SI = filename 11 chars
;
; return:
;
; AX = first cluster
;
; CF=0 found
;
; ------------------------------------------------------------

fat12_find_file:
    mov ax, ROOT_SEG
    mov es, ax
    xor bx,bx

.next:
    mov di,bx
    mov cx,11
    push si
    repe cmpsb
    pop si
    je .found
    add bx,32
    cmp bx, ROOT_SIZE
    jb .next
    stc

    ret

.found:
    mov ax,[es:bx+0x1A]
    clc

    ret


; ------------------------------------------------------------
; fat12_load_file
;
; AX = first cluster
;
; ES:BX = destination
;
; ------------------------------------------------------------

fat12_load_file:
    mov [current_cluster],ax

.next_cluster:
    mov ax,[current_cluster]
    cmp ax,0xFF8
    jae .done
; cluster -> sector
    sub ax,2
    add ax,DATA_LBA
    call disk_read_sector
    jc .error
    add bx,512
    mov ax,[current_cluster]
    call fat12_next_cluster
    mov [current_cluster],ax
    jmp .next_cluster

.done:
    clc

    ret

.error:
    stc

    ret


; ------------------------------------------------------------
; Получение следующего кластера FAT12
;
; AX = cluster
;
; return AX = next
;
; ------------------------------------------------------------

fat12_next_cluster:
    push bx
    push dx
    push es

    mov bx,ax
; offset = cluster * 3 / 2
    mov dx,ax
    shr ax,1
    add ax,dx
    mov bx,ax
    mov ax,FAT_SEG
    mov es,ax
    mov ax,[es:bx]
    test dx,1
    jnz .odd

.even:
    and ax,0x0FFF
    jmp .done

.odd:
    shr ax,4

.done:
    pop es
    pop dx
    pop bx

    ret


; ============================================================
; FAT12 constants
; ============================================================

FAT_LBA        equ 1
FAT_SECTORS    equ 5

ROOT_LBA       equ 11
ROOT_SECTORS   equ 14

DATA_LBA       equ 25

ROOT_SEG       equ 0x8000 >> 4
FAT_SEG        equ 0x9000 >> 4

ROOT_SIZE      equ 224*32


current_cluster dw 0
