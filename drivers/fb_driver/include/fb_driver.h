#ifndef FB_DRIVER_H
#define FB_DRIVER_H

#include <stdint.h>
#include "../../../include-kernel/bootinfo.h"
#include "../../include_drivers/drv_format.h"

void driver_main(struct boot_info *boot);

fb_driver_api_t *driver_get_api(void);

#endif
