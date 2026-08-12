#ifndef FAT12_INTERNAL_H
#define FAT12_INTERNAL_H

#include "../../../include-kernel/bootinfo.h"

#define FAT12_MAX_VOLUMES 8
#define FAT12_MAX_FAT_SIZE 8192
#define FAT12_MAX_OPEN_FILES 16
//#define FAT12_MAX_FAT_SIZE  4608

typedef struct __attribute__((packed))
{
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
} fat12_dirent_t;

_Static_assert(sizeof(fat12_dirent_t) == 32, "bad FAT dirent size");




/*
 * ============================================================
 * FAT12 volume
 * ============================================================
 *
 * Внутреннее описание одного смонтированного FAT12 тома.
 *
 * Драйвер FAT12 ориентирован на дискеты:
 *
 *   5.25" 720 KB
 *   5.25" 1.2 MB
 *   3.5"  720 KB
 *   3.5"  1.44 MB
 *
 * BIOS drive:
 *
 *   0x00 - первый floppy
 *   0x01 - второй floppy
 *
 * ============================================================
 */

typedef struct
{
    /* ========================================================
     * Идентификация тома
     * ======================================================== */

    uint32_t id;

    uint32_t mounted;

    /*
     * BIOS drive number:
     *
     * 0x00 - floppy A:
     * 0x01 - floppy B:
     */
    uint32_t drive;

    /*
     * FAT12 boot sector.
     *
     * Храним копию сектора внутри volume,
     * чтобы каждый том имел собственный BPB.
     */
    uint8_t boot_sector[512];

    /*
     * FAT12 BPB
     */

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;

    uint32_t reserved_sectors;

    uint32_t fat_count;
    uint32_t sectors_per_fat;

    uint32_t root_entries;

    uint32_t total_sectors;

    uint32_t media;


    /*
     * Вычисленные FAT-параметры
     */

    uint32_t fat_size;

    uint32_t root_start;
    uint32_t root_size;
    uint32_t data_start;


    /*
     * Floppy geometry
     */

    uint32_t sectors_per_track;
    uint32_t heads;

    /*
     * Первая FAT.
     */
    uint8_t fat[FAT12_MAX_FAT_SIZE];

    /*
     * --------------------------------------------------------
     * DEBUG
     * --------------------------------------------------------
     */
    fat12_dirent_t rentry;


} fat12_fs_t;



typedef struct
{
    uint32_t used;

    fat12_fs_t *vol;

    uint16_t start_cluster;
    uint32_t file_size;
    uint32_t position;

} fat12_file_t;


#endif
