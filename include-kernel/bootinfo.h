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

#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>

// Magic/version
#define BOOTINFO_MAGIC   0x4F4F4249
#define BOOTINFO_VERSION 2

#define GRAPHICS_FLAG_LFB 0x01
// Filesystem types
#define FS_UNKNOWN 0
#define FS_FAT12   1
#define FS_FAT16   2
#define FS_EXT2    3
// Module types
#define MODULE_TYPE_NONE        0
#define MODULE_TYPE_DRIVER      1
#define MODULE_TYPE_FILESYSTEM  2
#define MODULE_TYPE_SERVICE     3
#define MODULE_TYPE_PROGRAM     4

//filesystem_info
typedef struct filesystem_info
{
    uint32_t type;
    //общий размер блока
    uint32_t block_size;
    //FAT12/FAT16
    uint32_t bytes_per_sector;

    uint32_t sectors_per_cluster;
    //EXT2
    uint32_t blocks_per_group;

    uint32_t inodes_per_group;

    uint32_t inode_size;
    //FAT параметры
    uint32_t reserved_sectors;

    uint32_t fat_count;

    uint32_t fat_size;

    uint32_t root_start;

    uint32_t root_size;

    uint32_t data_start;
    //EXT2
    uint32_t first_data_block;
} filesystem_info_t;

// module_info
typedef struct module_info
{
    uint32_t start;

    uint32_t size;

    uint32_t type;

    char name[32];

} module_info_t;

// boot_info
typedef struct boot_info
{

    uint32_t magic;

    uint32_t version;

    uint32_t size;
    //framebuffer
    uint32_t framebuffer;

    uint32_t pitch;

    uint32_t width;

    uint32_t height;

    uint32_t bpp;

    uint32_t graphics_flags;
    //memory map
    uint32_t e820_addr;

    uint32_t e820_count;
    //filesystem
    filesystem_info_t *fs;
    //modules
    uint32_t module_count;

    module_info_t *modules;
    //strings
    char *cmdline;

    char *loader_name;
    //boot device
    uint32_t boot_drive;
    //VBE: таблица режимов, собранная загрузчиком (vbe.inc)
    uint32_t vbe_modes;       //физический адрес шапки таблицы (VBE_MODES_ADDR)
    uint32_t vbe_mode_count;
    uint32_t vbe_version;     //версия VBE контроллера, BCD
    uint32_t vbe_current;     //индекс текущего режима в таблице, 0xFFFF = нет
} boot_info_t;

//Global boot information
extern boot_info_t *boot_info;

// Init
void bootinfo_init(boot_info_t *info);

//================= таблица режимов VBE =================
//Строится загрузчиком (include/vbe.inc) в младшей памяти по адресу
//VBE_MODES_ADDR и переживает весь старт ядра: pmm никогда не раздаёт
//память ниже 1 МБ, identity-map накрывает её целиком.
#define VBE_MODES_ADDR    0x5400
#define VBE_MODES_MAX     64
#define VBE_TABLE_MAGIC   0x4D534256   //"VBSM"

//шапка таблицы, 12 байт (см. include/bootinfo.inc)
typedef struct vbe_table_hdr
{
    uint32_t magic;      //VBE_TABLE_MAGIC
    uint16_t version;    //версия VBE контроллера, BCD: 0x0200 = 2.0
    uint16_t count;      //число записей режимов
    uint16_t current;    //индекс режима, выставленного загрузчиком (0xFFFF = нет)
    uint16_t reserved;
} __attribute__((packed)) vbe_table_hdr_t;

//запись режима, 16 байт
//Смещения полей обязаны совпадать с VBE_REC_* из include/bootinfo.inc
typedef struct vbe_mode_rec
{
    uint16_t mode_number;   //номер режима VBE, уже с битом 0x4000 (LFB)
    uint16_t xres;
    uint16_t yres;
    uint8_t  bpp;           //8/16/24/32
    uint8_t  flags;         //бит 0 = LFB (VBE_REC_FLAG_LFB)
    uint16_t pitch;         //длина строки в байтах
    uint16_t reserved;
    uint32_t phys_base;     //адрес линейного фреймбуфера режима
} __attribute__((packed)) vbe_mode_rec_t;

#define VBE_REC_FLAG_LFB 0x01

//указатели на таблицу по адресу boot_info_t->vbe_modes
static inline vbe_table_hdr_t *vbe_table_hdr(uint32_t addr) { return (vbe_table_hdr_t *)addr; }
static inline vbe_mode_rec_t *vbe_table_recs(uint32_t addr) { return (vbe_mode_rec_t *)(addr + sizeof(vbe_table_hdr_t)); }

#endif
