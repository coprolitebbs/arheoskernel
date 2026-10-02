#ifndef FB_DRIVER_H
#define FB_DRIVER_H

#include <stdint.h>
#include "../../../include-kernel/bootinfo.h"
#include "../../include_drivers/drv_format.h"

#define WP_BUFFER_SIZE (1024 * 768 * 4)

void driver_main(struct boot_info *boot);

fb_driver_api_t *driver_get_api(void);

#endif
