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

//Содержит структуры описания дисков общие для ядра и userspace
#ifndef MOUNT_STRUCTURES_H
#define MOUNT_STRUCTURES_H

#include <stdint.h>
#include <stddef.h>

typedef struct
{
    char     mount_point[256];
    uint32_t drive_id;
    uint32_t statfs_supported;   //0/1
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    char     fs_name[16];
} u_mount_info_t;


#endif
