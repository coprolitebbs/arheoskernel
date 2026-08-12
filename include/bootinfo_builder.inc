; ============================================================
; build_boot_info
; ============================================================


build_boot_info:

	push edi
	push esi
	push ecx
	push edx
	push eax

    mov edi,BOOTINFO_ADDR

    mov dword [edi+BOOT_MAGIC],BOOTINFO_MAGIC
    mov dword [edi+BOOT_VERSION],BOOTINFO_VERSION
    mov dword [edi+BOOT_SIZE],BOOT_TOTAL_SIZE

    mov eax,[lfb_addr]
    mov [edi+BOOT_FRAMEBUFFER],eax

    xor eax,eax
    mov ax,[pitch]
    mov [edi+BOOT_PITCH],eax

    mov dword [edi+BOOT_WIDTH],1024
    mov dword [edi+BOOT_HEIGHT],768

    xor eax,eax
    mov al,[bytes_pp]
    shl eax,3
    mov [edi+BOOT_BPP],eax

    mov dword [edi+BOOT_E820_ADDR],0x6002

    xor eax,eax
    mov ax,[0x6000]
    mov [edi+BOOT_E820_COUNT],eax

    mov dword [edi+BOOT_FS_INFO],FSINFO_ADDR

    mov dword [edi+BOOT_MODULE_COUNT],3
    mov dword [edi+BOOT_MODULE_TABLE],MODULES_ADDR

    mov dword [edi+BOOT_CMDLINE],0
    mov dword [edi+BOOT_LOADER_NAME],loader_name

    xor eax,eax
    mov al,[drive]
    mov [edi+BOOT_DRIVE],eax

	pop eax
	pop edx
	pop ecx
	pop esi
    pop edi

    ;mov eax,BOOTINFO_ADDR
    ret



; ============================================================
; build_modules_info
; ============================================================

build_modules_info:
    push ds
    push es
    push si
    push di

    xor ax,ax
    mov es,ax

    ; ------------------------------------------------------------
    ; Модуль 0 : драйвер framebuffer
    ; ------------------------------------------------------------

    mov di,MODULES_ADDR

    mov dword [es:di + MODULE_START],0x30000

    mov eax,[fb_driver_size]
    mov [es:di + MODULE_SIZE],eax

    mov dword [es:di + MODULE_TYPE],MODULE_TYPE_DRIVER

    mov di,MODULES_ADDR + MODULE_NAME
    mov si,fb_driver_filename

    call copy_string

    ; ------------------------------------------------------------
    ; Модуль 1 : драйвер файловой системы FAT12
    ; ------------------------------------------------------------

    mov di,MODULES_ADDR + MODULE_INFO_SIZE

    mov dword [es:di + MODULE_START],0x34000

    mov eax,[fat12_driver_size]
    mov [es:di + MODULE_SIZE],eax

    mov dword [es:di + MODULE_TYPE],MODULE_TYPE_FILESYSTEM

    lea di,[di + MODULE_NAME]
    mov si,fat12_driver_filename
    call copy_string


    ; ------------------------------------------------------------
    ; Модуль 2 : драйвер файловой системы EXT2
    ; ------------------------------------------------------------

    mov di,MODULES_ADDR + MODULE_INFO_SIZE + MODULE_INFO_SIZE

    mov dword [es:di + MODULE_START],0x40000

    mov eax,[ext2_driver_size]
    mov [es:di + MODULE_SIZE],eax

    mov dword [es:di + MODULE_TYPE],MODULE_TYPE_FILESYSTEM

    lea di,[di + MODULE_NAME]
    mov si,ext2_driver_filename
    call copy_string


    pop di
    pop si
    pop es
    pop ds

    ret


copy_string:

.next:
    lodsb
    stosb
    test al,al
    jz .done
    jmp .next

.done:

    ret

; ============================================================
; Variables
; ============================================================

fb_driver_size     dd 0
fat12_driver_size  dd 0
ext2_driver_size   dd 0
