#ifndef USE_DRIVERS_H
#define USE_DRIVERS_H

#include "../drivers/include_drivers/drv_format.h"

extern fb_driver_api_t *fb;
extern fs_driver_api_t *f12dr;
extern fs_driver_api_t *ext2dr;

void drivers_init(void);

#endif
