OUTPUT_FORMAT("binary")
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : ALIGN(4)
    {
        /*
           ЖЕЛЕЗНОЕ ИСПРАВЛЕНИЕ:
           Принудительно ставим секцию .text конкретно из файла kernel.o первой!
           Это гарантирует, что функция _start ляжет ровно на физический адрес 0x100000,
           и jmp 0x08:0x100000 попадет на ее самый первый байт.
        */
        bin/kernel.o(.text*)
        *(.text*)
    }

    .rodata : ALIGN(4)
    {
        *(.rodata*)
    }

    .data : ALIGN(4)
    {
        *(.data*)
    }

    .bss : ALIGN(4)
    {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _kernel_end = .;
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
    }
}
