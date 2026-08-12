#include "include-kernel/use_drivers.h"
#include "include-kernel/bootinfo.h"
#include "include-kernel/driver_elf.h"
#include "include-kernel/draw.h"

//указатель на api драйвера framebuffer
fb_driver_api_t *fb = 0;
fs_driver_api_t *f12dr = 0;
fs_driver_api_t *ext2dr = 0;



void drivers_init(void){
    //fb = load_driver_elf(bin_fb_driver_elf, boot_info);

    module_info_t *m = 0;

    m = &boot_info->modules[0];
    fb = (fb_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);

    if(!fb){
        char *err = "FB Driver load error";
        draw_string(10, 20, err);
    }

    m = &boot_info->modules[1];
    f12dr = (fs_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);

    if(!f12dr){
        char *err = "FAT12 Driver load error";
        draw_string(10, 30, err);
    }

    m = &boot_info->modules[2];
    ext2dr = (fs_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);

    if(!ext2dr){
        char *err = "EXT2 Driver load error";
        draw_string(10, 30, err);
    }

}
