#include "include-kernel/driver.h"
#include "include-kernel/lib.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "drivers/include_drivers/drv_format.h"


#define DRIVER_LOAD_ADDR 0x500000



static uint32_t resolve_symbol(
    drv_header_t *hdr,
    uint8_t *dst,
    uint32_t index
)
{
    if(index >= hdr->sym_count)
        return 0;


    drv_symbol_t *symbols =
        (drv_symbol_t*)
        (
            dst +
            hdr->sym_offset
        );


    return
        DRIVER_LOAD_ADDR +
        symbols[index].offset;
}




static void apply_relocations(
    uint8_t *dst,
    drv_header_t *hdr
)
{
    drv_reloc_t *rel =
        (drv_reloc_t*)
        (
            dst +
            hdr->reloc_offset
        );


    for(uint32_t i=0;i<hdr->reloc_count;i++)
    {

        uint32_t *patch =
            (uint32_t*)
            (
                dst +
                rel[i].offset
            );


        if(rel[i].symbol != 0xffffffff)
        {

            uint32_t addr =
                resolve_symbol(
                    hdr,
                    dst,
                    rel[i].symbol
                );


            if(!addr)
                continue;



            if(rel[i].type == DRV_RELOC_ABS32)
            {
                *patch = addr;
            }


            else if(rel[i].type == DRV_RELOC_PC32)
            {
                uint32_t patch_addr =
                    DRIVER_LOAD_ADDR +
                    rel[i].offset;


                *patch =
                    addr -
                    patch_addr -
                    4;
            }


            continue;

        }


        /*
            внутренние ссылки
        */

        *patch += DRIVER_LOAD_ADDR;

    }
}




void *load_driver(
    unsigned char *image
)
{

    drv_header_t *hdr =
        (drv_header_t*)image;



    if(
        hdr->magic[0]!='D' ||
        hdr->magic[1]!='R' ||
        hdr->magic[2]!='V' ||
        hdr->magic[3]!='1'
    )
    {
        draw_string(
            10,
            200,
            "BAD DRIVER"
        );

        return 0;
    }




    // ---- ОТЛАДКА ИСХОДНОГО МАССИВА ----
draw_string(10, 400, "magic: ");
draw_hex_dword(*(uint32_t*)image, 180, 400);          // должно быть 0x31565244 (DRV1)
draw_string(10, 420, "image+0x534: ");
draw_hex_dword(*(uint32_t*)(image + 0x534), 180, 420); // должно быть 0x3A505042 (BPP:)
draw_string(10, 440, "image+0x554: ");
draw_hex_dword(*(uint32_t*)(image + 0x554), 180, 440); // должно быть 0x81A5817E
// ------------------------------------



    uint8_t *dst =
        (uint8_t*)DRIVER_LOAD_ADDR;



    /*
        TEXT
    */

    memcpy(
        dst + hdr->text_offset,
        image + hdr->text_offset,
        hdr->text_size
    );



    /*
        RODATA
    */

    memcpy(
        dst + hdr->rodata_offset,
        image + hdr->rodata_offset,
        hdr->rodata_size
    );



    /*
        DATA
    */

    memcpy(
        dst + hdr->data_offset,
        image + hdr->data_offset,
        hdr->data_size
    );



    /*
        BSS
    */

    memset(dst + hdr->bss_offset, 0, hdr->bss_size);



    /*
        RELOC
    */

    memcpy(
        dst + hdr->reloc_offset,
        image + hdr->reloc_offset,
        hdr->reloc_count *
        sizeof(drv_reloc_t)
    );



    /*
        SYMBOLS
    */

    memcpy(
        dst + hdr->sym_offset,
        image + hdr->sym_offset,
        hdr->sym_count *
        sizeof(drv_symbol_t)
    );



    apply_relocations(
        dst,
        hdr
    );



    uint32_t entry =
        DRIVER_LOAD_ADDR +
        hdr->entry;


/*
    draw_string(
        10,
        220,
        "DRIVER OK"
    );


    draw_hex_dword(
        entry,
        100,
        220
    );*/
/*
    // ---- ОТЛАДКА ----
draw_string(10, 280, "dst        : ");
draw_hex_dword((uint32_t)dst, 180, 280);

// Проверяем, скопировались ли данные шрифта (первые 16 байт с 0x554)
draw_string(10, 300, "font data  : ");
draw_hex_dword(*(uint32_t*)(dst + 0x554), 180, 300);
draw_hex_dword(*(uint32_t*)(dst + 0x558), 260, 300); // следующие 4 байта

// Проверяем, применилась ли релокация на font_data_driver (символ 15)
// Смещение патча из дампа: 0x000000B7 + hdr.text_offset (0x44) = 0xFB
uint32_t patch_addr = (uint32_t)(dst + 0xFB);
draw_string(10, 320, "patch 0xFB : ");
draw_hex_dword(*(uint32_t*)patch_addr, 180, 320);

// Проверяем, что в BSS лежат нули (для framebuffer и др.)
draw_string(10, 340, "bss[0]     : ");
draw_hex_dword(*(uint32_t*)(dst + hdr->bss_offset), 180, 340);
// ---- КОНЕЦ ОТЛАДКИ ----
*/


    return
        (void*)entry;
}
