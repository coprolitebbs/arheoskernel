#ifndef FAT12_DRIVER_H
#define FAT12_DRIVER_H

#include <stdint.h>
#include "../../../include-kernel/bootinfo.h"
#include "../../include_drivers/drv_format.h"
#include "fat12_internal.h"

#define FAT12_PATH_MAX 4096


extern fat12_fs_t fat12_volumes[FAT12_MAX_VOLUMES];
extern uint32_t fat12_volume_count;

extern fs_driver_api_t *api_ptr;

void driver_main(struct boot_info *boot);

fs_driver_api_t *driver_get_api(void);

#endif
