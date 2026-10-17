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

#ifndef FAT12_INTERNAL_H
#define FAT12_INTERNAL_H

#include "../../../include-kernel/bootinfo.h"

#define FAT12_MAX_VOLUMES 8
#define FAT12_MAX_FAT_SIZE 8192
#define FAT12_MAX_OPEN_FILES 16

#define FAT12_FD_FILE  1
#define FAT12_FD_DIR   2

typedef struct __attribute__((packed)){
    char     name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_low;
    uint32_t size;
} __attribute__((packed)) fat12_dirent_t;

_Static_assert(sizeof(fat12_dirent_t) == 32, "bad FAT dirent size");


//FAT12 volume
//   5.25" 720 KB
//   5.25" 1.2 MB
//   3.5"  720 KB
//   3.5"  1.44 MB
// BIOS drive:
//   0x00 - первый floppy
//   0x01 - второй floppy

typedef struct{
    uint32_t id;
    uint32_t mounted;
    // BIOS drive number:
    // 0x00 - floppy A:
    // 0x01 - floppy B:
    uint32_t drive;
    //FAT12 boot sector.
    uint8_t boot_sector[512];
    //FAT12 BPB
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;

    uint32_t reserved_sectors;

    uint32_t fat_count;
    uint32_t sectors_per_fat;

    uint32_t root_entries;

    uint32_t total_sectors;

    uint32_t media;

    uint32_t fat_size;

    uint32_t root_start;
    uint32_t root_size;
    uint32_t data_start;
    //Floppy geometry
    uint32_t sectors_per_track;
    uint32_t heads;

    uint8_t fat[FAT12_MAX_FAT_SIZE];

    fat12_dirent_t rentry;
} fat12_fs_t;



typedef struct{
    uint32_t used;

    fat12_fs_t *vol;

    uint16_t start_cluster;
    uint32_t file_size;
    uint32_t position;

    uint32_t attributes;

    uint32_t entry_id;

    uint16_t parent_dir_cluster;
} fat12_file_t;


#endif
