#ifndef EXT2_DRIVER_H
#define EXT2_DRIVER_H

#include <stdint.h>
#include "../../../include-kernel/bootinfo.h"
#include "../../include_drivers/drv_format.h"

#define EXT2_PATH_MAX 4096

//extern fs_driver_api_t api;
extern fs_driver_api_t *api_ptr;

void driver_main(struct boot_info *boot);

fs_driver_api_t *driver_get_api(void);

#endif
