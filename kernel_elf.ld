ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : ALIGN(4)
    {
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
    }
}
