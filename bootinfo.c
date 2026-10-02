#include "include-kernel/bootinfo.h"

boot_info_t *boot_info = 0;


void bootinfo_init(boot_info_t *info){
    if(!info) return;

    if(info->magic != BOOTINFO_MAGIC){
        boot_info = 0;
        return;
    }
    if(info->version != BOOTINFO_VERSION){
        boot_info = 0;
        return;
    }

    boot_info = info;

}
