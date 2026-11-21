OUTPUT_FORMAT("binary") /* Делаем flat-бинарник без ELF-заголовков */
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS{
    /* Базовый адрес пространства процессов в Ring 3 */
    /* 0x48000000 = USER_VA_BASE из include-kernel/task.h: выше identity-окна видео Geode GX1 */
    . = 0x48000000;

    .text : AT(0x48000000){
        /*Ставим секцию .text объектника библиотеки на нулевой байт*/
        /* Так как функция _start объявлена первой в ustd.c, она гарантированно встанет на адрес 0x48000000 */
        bin/u_std.o(.text)

        /* И после нее линкуем весь остальной Си-код библиотек и программ */
        *(.text)
        *(.text.*)
    }

    .rodata :{
        *(.rodata)
        *(.rodata.*)
    }

    .data :{
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        LONG(0); /* Насильно заставляем ld физически дописать нули на диск до самого конца BSS */
    }

    /DISCARD/ :{
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
}

