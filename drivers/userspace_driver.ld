ENTRY(driver_entry)


SECTIONS
{

    . = 0;


    .text :
    {
        *(.text)
    }


    .rodata :
    {
        *(.rodata)
    }


    .data :
    {
        *(.data)
    }


    .bss :
    {
        *(.bss)
    }

}