/*
 * Copyright (C) 2026 ARHeOS by Dmitry H
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

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
