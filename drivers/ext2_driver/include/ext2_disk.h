#ifndef EXT2_DISK_H
#define EXT2_DISK_H

#include <stdint.h>
#include "ext2_internal.h"
#include "../../../include-kernel/bootinfo.h"
#include "../../include_drivers/drv_format.h"

int ext2_disk_read_sectors(uint32_t lba, uint32_t count, void *buffer);
int ext2_disk_write_sectors(uint32_t lba, uint32_t count, const void *buf);
int ext2_read_block(ext2_volume_t *vol, uint32_t block, void *buf);
int ext2_read_inode(ext2_volume_t *vol, uint32_t inode_no, ext2_inode_t *out_inode);
int ext2_write_block(ext2_volume_t *vol, uint32_t block, const void *buf);
int ext2_write_inode(ext2_volume_t *vol, uint32_t inode_no, const ext2_inode_t *in_inode);
int ext2_alloc_block(ext2_volume_t *vol, uint32_t group, uint32_t *out_block_no);
int ext2_alloc_inode(ext2_volume_t *vol, uint32_t group, uint32_t *out_inode_no);

#endif
