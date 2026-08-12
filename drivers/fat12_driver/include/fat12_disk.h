#ifndef FAT12_DISK_H
#define FAT12_DISK_H

#include <stdint.h>
#include "fat12_internal.h"


int fat12_disk_init(fat12_fs_t *vol);

int fat12_read_sectors(fat12_fs_t *vol,uint32_t lba,uint32_t count,void *buffer);
int fat12_write_sectors(fat12_fs_t *vol,uint32_t lba,uint32_t count,const void *buffer);
//int fat12_read_file(fat12_fs_t *vol,uint16_t first_cluster,uint32_t file_size,void *buffer);
uint16_t fat12_get_next_cluster(fat12_fs_t *vol,uint16_t cluster);

#endif
