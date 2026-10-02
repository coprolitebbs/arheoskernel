#include "include/fat12_boot.h"
#include "include/fat12_internal.h"
#include "include/fat12_disk.h"
#include "include/fat12_driver.h"
#include "../include_drivers/drv_format.h"
#include "../../include-kernel/lib.h"


int fat12_read_boot_sector(fat12_fs_t *vol){
    int result;

    result = fat12_read_sectors(vol,0,1,vol->boot_sector);
    if (result != FS_OK) return result;

    return FS_OK;
}



static uint16_t fat12_get_u16(const uint8_t *p){
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}



int fat12_parse_bpb(fat12_fs_t *vol){
    if (!vol) return FS_IO_ERROR;
    //BIOS Parameter Block
    vol->bytes_per_sector = fat12_get_u16(&vol->boot_sector[0x0B]);

    vol->sectors_per_cluster = vol->boot_sector[0x0D];

    vol->reserved_sectors = fat12_get_u16(&vol->boot_sector[0x0E]);

    vol->fat_count = vol->boot_sector[0x10];

    vol->root_entries = fat12_get_u16(&vol->boot_sector[0x11]);

    vol->total_sectors = fat12_get_u16(&vol->boot_sector[0x13]);

    vol->media = vol->boot_sector[0x15];

    vol->sectors_per_fat = fat12_get_u16(&vol->boot_sector[0x16]);

    vol->sectors_per_track = fat12_get_u16(&vol->boot_sector[0x18]);

    vol->heads = fat12_get_u16(&vol->boot_sector[0x1A]);

    if (vol->bytes_per_sector != 512) return FS_NOT_SUPPORTED;
    if (vol->sectors_per_cluster == 0) return FS_CORRUPTED;
    if (vol->reserved_sectors == 0) return FS_CORRUPTED;
    if (vol->fat_count == 0) return FS_CORRUPTED;
    if (vol->sectors_per_fat == 0) return FS_CORRUPTED;
    if (vol->root_entries == 0) return FS_CORRUPTED;
    if (vol->total_sectors == 0) return FS_CORRUPTED;
    if (vol->sectors_per_track == 0) return FS_CORRUPTED;
    if (vol->heads == 0) return FS_CORRUPTED;

    //Layout:
    //reserved
    //FAT #1
    //FAT #2
    //...
    //root directory
    //data

    vol->root_start = vol->reserved_sectors + vol->fat_count * vol->sectors_per_fat;
    uint32_t root_bytes = vol->root_entries * 32;
    vol->root_size = (root_bytes + vol->bytes_per_sector - 1) / vol->bytes_per_sector;
    vol->data_start = vol->root_start + vol->root_size;
    if (vol->root_start >= vol->total_sectors) return FS_CORRUPTED;
    if (vol->data_start >= vol->total_sectors) return FS_CORRUPTED;

    return FS_OK;
}
