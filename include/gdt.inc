; =================================================
; GDT
; =================================================

BITS 16

align 8


gdt:
    dq 0
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF

gdt_end:

gdtr:
    dw gdt_end-gdt-1
    dd gdt
