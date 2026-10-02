BITS 32

global driver_entry
extern driver_main

section .text


driver_entry:

    jmp start


align 16


driver_header:
    dd 'DRV1'
    dd 1
    dd driver_header_end-driver_header
    dd start-driver_entry
    dd 0
    dd 0
    db "EXT2 driver",0


driver_header_end:


start:
    ; EBX = boot_info
    push ebx
    call driver_main
    add esp,4

    ret
