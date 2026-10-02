#ifndef USE_DRIVERS_H
#define USE_DRIVERS_H

#include "../drivers/include_drivers/drv_format.h"
#include "bootinfo.h"
#include "driver_elf.h"
#include "draw.h"
#include "isr.h"
#include "vmm.h"
#include "config.h"

#define FILESYSTEMS_API_MAX 2

#define DRV_STATUS_OK             0
#define DRV_STATUS_VFS_ERROR     -1
#define DRV_STATUS_LOAD_ERROR    -2
#define DRV_STATUS_NOT_SUPPORTED -3

typedef struct{
    uint32_t type;
    fs_driver_api_t *api;
} fs_driver_t;

extern fb_driver_api_t *fb;

extern fs_driver_api_t *main_filesystem_fs_api;

extern fs_driver_t fs_apis[FILESYSTEMS_API_MAX];

extern kbd_driver_api_t *kbd_driver_api;

//extern char* additional_drivers_list;

void drivers_init(void);

int load_additional_drivers(cfg_file_t *cfg);

#endif
