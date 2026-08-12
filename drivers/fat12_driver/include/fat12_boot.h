#ifndef FAT12_BOOT_H
#define FAT12_BOOT_H

#include <stdint.h>
#include "fat12_internal.h"

int fat12_read_boot_sector(fat12_fs_t *vol);

int fat12_parse_bpb(fat12_fs_t *vol);

#endif
