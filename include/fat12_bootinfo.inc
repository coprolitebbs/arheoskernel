; ============================================================
; FAT12 filesystem info builder
; ============================================================

build_fs_info:
    push edi
    mov edi,FSINFO_ADDR

    mov dword [edi+FS_TYPE],FS_FAT12

    mov eax,0
    mov ax,[bps]
    mov [edi+FS_BYTES_PER_SECTOR],eax

    mov eax,0
    mov ax,[spc]
    mov [edi+FS_SECTORS_PER_CLUSTER],eax

    mov eax,0
    mov ax,[rsvd]
    mov [edi+FS_RESERVED_SECTORS],eax

    mov eax,0
    mov ax,[nfats]
    mov [edi+FS_FAT_COUNT],eax

    mov eax,0
    mov ax,[fatsz]
    mov [edi+FS_FAT_SIZE],eax

    mov eax,0
    mov ax,[root_start]
    mov [edi+FS_ROOT_START],eax

    mov eax,0
    mov ax,[root_size]
    mov [edi+FS_ROOT_SIZE],eax

    mov eax,0
    mov ax,[data_start]
    mov [edi+FS_DATA_START],eax

    pop edi

    ret
