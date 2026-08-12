#ifndef BOOTINFO_H
#define BOOTINFO_H


#include <stdint.h>


// ============================================================
// Magic/version
// ============================================================

#define BOOTINFO_MAGIC   0x4F4F4249
#define BOOTINFO_VERSION 1

// ============================================================
// Filesystem types
// ============================================================

#define FS_UNKNOWN 0
#define FS_FAT12   1
#define FS_FAT16   2
#define FS_EXT2    3



// ============================================================
// Module types
// ============================================================

#define MODULE_TYPE_NONE        0
#define MODULE_TYPE_DRIVER      1
#define MODULE_TYPE_FILESYSTEM  2
#define MODULE_TYPE_SERVICE     3
#define MODULE_TYPE_PROGRAM     4



// ============================================================
// filesystem_info
// ============================================================

typedef struct filesystem_info
{
    uint32_t type;

    // общий размер блока
    uint32_t block_size;

    // FAT12/FAT16

    uint32_t bytes_per_sector;

    uint32_t sectors_per_cluster;

    // EXT2

    uint32_t blocks_per_group;

    uint32_t inodes_per_group;

    uint32_t inode_size;

    // FAT параметры

    uint32_t reserved_sectors;

    uint32_t fat_count;

    uint32_t fat_size;

    uint32_t root_start;

    uint32_t root_size;

    uint32_t data_start;

    // EXT2

    uint32_t first_data_block;


} filesystem_info_t;



// ============================================================
// module_info
// ============================================================

typedef struct module_info
{
    uint32_t start;

    uint32_t size;

    uint32_t type;

    char name[32];

} module_info_t;



// ============================================================
// boot_info
// ============================================================

typedef struct boot_info
{

    uint32_t magic;

    uint32_t version;

    uint32_t size;

    // framebuffer

    uint32_t framebuffer;

    uint32_t pitch;

    uint32_t width;

    uint32_t height;

    uint32_t bpp;

    // memory map

    uint32_t e820_addr;

    uint32_t e820_count;

    // filesystem

    filesystem_info_t *fs;

    // modules

    uint32_t module_count;

    module_info_t *modules;


    // strings

    char *cmdline;

    char *loader_name;

    // boot device

    uint32_t boot_drive;


} boot_info_t;

// ============================================================
// Global boot information
// ============================================================

extern boot_info_t *boot_info;

// ============================================================
// Init
// ============================================================

void bootinfo_init(boot_info_t *info);

#endif
