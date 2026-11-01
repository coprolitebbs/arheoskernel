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

#ifndef BLOCK_DRIVER_H
#define BLOCK_DRIVER_H

#include <stdint.h>
#include "../../../include-kernel/bootinfo.h"
#include "../../../include-kernel/block.h"
#include "../../include_drivers/drv_format.h"

#define MAX_BLOCK_DEVICES  16
#define SECTOR_SIZE        512
#define PARENT_NONE        0xFFFFFFFFu

typedef struct {
    char name[16];
    uint32_t sector_count;
    uint32_t flags;
    bool used;
    // транспортная диспетчеризация
    int (*read_fn)(void *priv, uint64_t lba, uint32_t count, void *buf);
    int (*write_fn)(void *priv, uint64_t lba, uint32_t count, const void *buf);
    void *priv;
} block_dev_entry_t;


typedef struct {
    uint16_t base;
    uint8_t slave;
} ata_priv_t;


void driver_main(struct boot_info *boot);

description_driver_api_t *driver_get_api(void);

#endif
