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

#ifndef BLOCK_H
#define BLOCK_H

#define BLK_REMOVABLE   0x0001
#define BLK_READONLY    0x0002
#define BLK_PARTITION   0x0004

typedef struct block_device {
    char        name[16];            //"sda", "sda1", "sdb", "usb0"
    uint32_t    sector_size;         //обычно 512
    uint64_t    sector_count;
    uint32_t    flags;               //BLK_REMOVABLE, BLK_READONLY
    void       *private;             //ATA channel info / USB LUN / ...
    struct block_device *parent;     //для партиции — указатель на диск
    uint64_t    partition_offset;    //0 для диска, LBA-смещение для партиции

    int (*read)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
} block_device_t;


typedef struct {
    uint32_t id;                 // 0 = boot-диск, 1.. = остальные
    char     name[16];           // "sda", "sda1", "usb0", "usb0p1"
    uint32_t sector_size;        // 512 / 4096
    uint64_t sector_count;
    uint32_t flags;              // BLK_REMOVABLE | BLK_READONLY | BLK_PARTITION
    uint32_t parent_id;          // для партиции = id диска; для диска = 0xFFFFFFFF
    uint64_t partition_offset;   // для партиции = LBA начала; для диска = 0
} block_info_t;

//Синхронные обёртки — поверх dev->read с учётом partition_offset
//int block_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf);//
//int block_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf);

//Реестр
//block_device_t *block_register(block_device_t *dev);
//void            block_unregister(block_device_t *dev);
//block_device_t *block_find(const char *name);
//block_device_t *block_enum(int index);

#endif
