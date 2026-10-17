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

#ifndef EXT2_DISK_H
#define EXT2_DISK_H

#include <stdint.h>
#include "ext2_internal.h"
#include "../../../include-kernel/bootinfo.h"
#include "../../include_drivers/drv_format.h"

int ext2_free_single_block(ext2_volume_t *vol, uint32_t block_no);
int ext2_free_block_tree(ext2_volume_t *vol, uint32_t block_no, int depth);
int ext2_truncate_inode(ext2_volume_t *vol, uint32_t inode_no, ext2_inode_t *inode);

int ext2_disk_read_sectors(uint32_t lba, uint32_t count, void *buffer);
int ext2_disk_write_sectors(uint32_t lba, uint32_t count, const void *buf);
int ext2_read_block(ext2_volume_t *vol, uint32_t block, void *buf);
int ext2_read_inode(ext2_volume_t *vol, uint32_t inode_no, ext2_inode_t *out_inode);
int ext2_write_block(ext2_volume_t *vol, uint32_t block, const void *buf);
int ext2_write_inode(ext2_volume_t *vol, uint32_t inode_no, const ext2_inode_t *in_inode);
int ext2_alloc_block(ext2_volume_t *vol, uint32_t group, uint32_t *out_block_no);
int ext2_alloc_inode(ext2_volume_t *vol, uint32_t group, uint32_t *out_inode_no);

#endif
