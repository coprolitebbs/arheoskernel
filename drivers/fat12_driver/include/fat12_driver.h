#ifndef FAT12_DRIVER_H
#define FAT12_DRIVER_H

#include <stdint.h>
#include "../../../include-kernel/bootinfo.h"
#include "../../include_drivers/drv_format.h"
#include "fat12_internal.h"


extern fat12_fs_t fat12_volumes[FAT12_MAX_VOLUMES];
extern uint32_t fat12_volume_count;

extern fat12_file_t fat12_files[FAT12_MAX_OPEN_FILES];

void driver_main(struct boot_info *boot);

//int fat12_read_file(void *vol,const char *name,void *buffer,uint32_t size);

fs_driver_api_t *driver_get_api(void);

#endif
