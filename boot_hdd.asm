BITS 16
ORG 0x7C00

start:




    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    ; ---- Проверка LBA ----
    mov ah, 0x41
    mov bx, 0x55AA
    int 0x13
    jc .no_lba
    cmp bx, 0xAA55
    jne .no_lba
    mov byte [lba_supported], 1
    jmp .lba_ok
.no_lba:
    mov byte [lba_supported], 0
.lba_ok:

    ; ---- Если LBA не поддерживается, получаем геометрию ----
    cmp byte [lba_supported], 1
    je .skip_geometry

    call get_geometry
    jc .geometry_error
    jmp .geometry_ok
.geometry_error:
    mov word [heads_tmp], 16
    mov word [sectors_tmp], 63
    jmp .geometry_ok
.geometry_ok:
    ; heads и sectors уже сохранены в heads_tmp и sectors_tmp

.skip_geometry:




    ; ---- Загрузка hddld.bin (4 сектора) ----
    ;mov ax, 0x7E00 >> 4
    ;mov es, ax
    ;xor bx, bx
    ;mov eax, 17 	   ; LBA = 17
    ;mov cx, 16          ; 4 сектора
    ;call read_sectors

    ; ---- Загрузка первых 16 секторов после MBR ----
    ; ---- Загрузка hddld.bin ----
    mov ax, 0x7E00 >> 4
    mov es, ax
    xor bx, bx

    mov eax, 1        ; LBA начало stage2
    mov cx, 32        ; 32 сектора = 16 КБ

    call read_sectors


    ; ---- Переход на hddld.bin ----
    jmp 0x0000:0x7E00

; ---------- Подпрограмма получения геометрии ----------
get_geometry:
    mov ah, 0x08
    mov dl, 0x80
    int 0x13
    jc .error
    ; DH = max head (0-based), CL[5:0] = max sector (1-based)
    mov [heads], dh
    inc byte [heads]      ; heads = DH + 1
    mov [sectors], cl
    and byte [sectors], 0x3F
    mov al, [heads]
    mov [heads_tmp], al
    mov al, [sectors]
    mov [sectors_tmp], al
    clc
    ret
.error:
    stc
    ret

; ---------- Чтение секторов (LBA или CHS) ----------
read_sectors:
    cmp byte [lba_supported], 1
    je .lba
    ; ---- CHS fallback с динамической геометрией ----
    pusha
    push es
    push bx
    mov si, ax          ; LBA
    mov di, cx          ; количество
    mov ax, [heads_tmp]
    mov [heads_temp], ax
    mov ax, [sectors_tmp]
    mov [sectors_temp], ax
.next_chs:
    mov ax, si
    xor dx, dx
    div word [sectors_temp]   ; ax = LBA / SPT, dx = LBA % SPT
    mov cl, dl
    inc cl
    xor dx, dx
    div word [heads_temp]     ; ax = cylinder, dx = head
    mov dh, dl
    mov ch, al
    mov dl, 0x80
    mov ah, 0x02
    mov al, 1
    int 0x13
    jc .error_chs
    add bx, 512
    inc si
    dec di
    jnz .next_chs
    clc
    pop bx
    pop es
    popa
    ret
.error_chs:
    stc
    pop bx
    pop es
    popa
    ret

.lba:
    push si
    mov si, dap
    mov word [si], 0x10
    mov word [si+2], cx
    mov word [si+4], bx
    mov word [si+6], es
    mov dword [si+8], eax
    mov dword [si+12], 0
    mov dl, 0x80
    mov ah, 0x42
    int 0x13
    pop si
    ret

lba_supported db 0
heads_tmp dw 16
sectors_tmp dw 63
heads db 0
sectors db 0
; Вспомогательные переменные для CHS-чтения
heads_temp dw 0
sectors_temp dw 0
dap times 16 db 0

times 446-($-$$) db 0
times 64 db 0
dw 0xAA55
