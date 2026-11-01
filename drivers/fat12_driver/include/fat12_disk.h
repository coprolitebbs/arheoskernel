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

#ifndef FAT12_DISK_H
#define FAT12_DISK_H

#include <stdint.h>
#include "fat12_internal.h"


int fat12_disk_init(fat12_fs_t *vol);

int fat12_read_sectors(fat12_fs_t *vol,uint32_t lba,uint32_t count,void *buffer);
int fat12_write_sectors(fat12_fs_t *vol,uint32_t lba,uint32_t count,const void *buffer);
uint16_t fat12_get_next_cluster(fat12_fs_t *vol,uint16_t cluster);


#endif
