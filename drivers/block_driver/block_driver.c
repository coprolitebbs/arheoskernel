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

#include "include/block_driver.h"
#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/block.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

block_dev_entry_t devices[MAX_BLOCK_DEVICES];
block_dev_entry_t *devices_ptr;

int devices_count = 0;

ata_priv_t ata_privs[MAX_BLOCK_DEVICES];
ata_priv_t *ata_privs_ptr;

block_driver_api_t api;
block_driver_api_t *api_ptr;
description_driver_api_t descr_api;

//  ----- внутренние функции  -----


// ATA PIO

void ata_soft_reset(uint16_t base)
{
    //Control-регистр канала: 0x3F6 (primary) или 0x376 (secondary)
    uint16_t ctrl = (base == 0x1F0) ? 0x3F6 : 0x376;
    outb(ctrl, 0x04);                    //SRST=1
    for (int i = 0; i < 8; i++) inb(ctrl);  // ≥ 2×400 ns
    outb(ctrl, 0x00);                    //SRST=0
    uint32_t t = 500000;
    while ((inb(base + 7) & 0x80) && --t);  //ждать BSY=0
}


int ata_identify(uint16_t base, uint8_t slave, uint16_t *out_identify)
{
    //Если канал залип — программный сброс
    uint32_t pre = 500000;
    while ((inb(base + 7) & 0x80) && --pre);
    if (pre == 0) ata_soft_reset(base);

    //Если DRQ=1 (данные от предыдущей команды не вычитаны) — вычитать
    if (inb(base + 7) & 0x08)
    {
        __asm__ __volatile__("cld");
        for (int i = 0; i < 256; i++) inw(base);
    }

    outb(base + 6, (slave ? 0xB0 : 0xA0));
    for (int i = 0; i < 4; i++) inb(base + 7);

    outb(base + 2, 0);
    outb(base + 3, 0);
    outb(base + 4, 0);
    outb(base + 5, 0);

    outb(base + 7, 0xEC); //IDENTIFY DEVICE

    for (int i = 0; i < 4; i++) inb(base + 7);

    uint8_t status = inb(base + 7);
    if (status == 0x00 || status == 0xFF)
    {
        //api_ptr->debug1 = base; api_ptr->debug2 = slave;
        //api_ptr->debug3 = 0x100 + status;    //0x100 = "no drive"
        return FS_IO_ERROR; //no drive
    }

    uint32_t timeout = 500000;
    while ((inb(base + 7) & 0x80) && --timeout);
    if (timeout == 0)
    {
        //api_ptr->debug1 = base; api_ptr->debug2 = slave;
        //api_ptr->debug3 = 0x200;             //"BSY stuck"
        return FS_IO_ERROR;
    }

    //api_ptr->debug1 = 0;
    //api_ptr->debug2 = 0;
    //api_ptr->debug3 = 0;
    //api_ptr->debug4 = 0;
    uint8_t lba_mid = inb(base + 4);
    uint8_t lba_hi  = inb(base + 5);

    if (lba_mid != 0 || lba_hi != 0)
    {
        //api_ptr->debug1 = base;
        //api_ptr->debug2 = slave;
        //api_ptr->debug3 = lba_mid;
        //api_ptr->debug4 = lba_hi;

        return FS_IO_ERROR;
    }

    timeout = 500000;
    while (!(inb(base + 7) & 0x08) && --timeout);
    if (timeout == 0)
    {
        //api_ptr->debug1 = base; api_ptr->debug2 = slave;
        //api_ptr->debug3 = 0x300;             // "DRQ timeout"
        return FS_IO_ERROR;
    }
    if (inb(base + 7) & 0x01)
    {
        // api_ptr->debug1 = base; api_ptr->debug2 = slave;
        //api_ptr->debug3 = 0x400;             // "ERR after DRQ"
        return FS_IO_ERROR;
    }

    for (int i = 0; i < 256; i++)
    {
        out_identify[i] = inw(base);
    }
    return FS_OK;
}

int ata_pio_read(uint16_t base, uint8_t slave, uint32_t lba, uint32_t count, void *buf)
{
    uint16_t *ptr = (uint16_t *)buf;
    for (uint32_t i = 0; i < count; i++)
    {
        outb(base + 6, (slave ? 0xF0 : 0xE0) | (((lba + i) >> 24) & 0x0F));
        inb(base + 7);
        inb(base + 7);
        inb(base + 7);
        inb(base + 7);

        outb(base + 2, 1);
        outb(base + 3, (uint8_t)(lba + i));
        outb(base + 4, (uint8_t)((lba + i) >> 8));
        outb(base + 5, (uint8_t)((lba + i) >> 16));
        outb(base + 7, 0x20);

        uint32_t timeout = 500000;
        uint8_t status = 0;
        while (timeout--)
        {
            status = inb(base + 7);
            if (!(status & 0x80) && (status & 0x08)) break;
        }
        if (timeout == 0)
        {
            return FS_IO_ERROR;
        }
        if (status & 0x01)
        {
            return FS_IO_ERROR;
        }

        __asm__ __volatile__("cld");
        insw(base, ptr + (i * 256), 256);
    }
    return FS_OK;
}

int ata_pio_write(uint16_t base, uint8_t slave, uint32_t lba, uint32_t count, const void *buf)
{
    const uint16_t *ptr = (const uint16_t *)buf;
    for (uint32_t i = 0; i < count; i++)
    {
        outb(base + 6, (slave ? 0xF0 : 0xE0) | (((lba + i) >> 24) & 0x0F));
        inb(base + 7);
        inb(base + 7);
        inb(base + 7);
        inb(base + 7);

        uint32_t pre = 100000;
        while ((inb(base + 7) & 0x80) && --pre);

        outb(base + 2, 1);
        outb(base + 3, (uint8_t)(lba + i));
        outb(base + 4, (uint8_t)((lba + i) >> 8));
        outb(base + 5, (uint8_t)((lba + i) >> 16));
        outb(base + 7, 0x30);

        uint32_t timeout = 500000;
        uint8_t status = 0;
        while (timeout--)
        {
            status = inb(base + 7);
            if (!(status & 0x80) && (status & 0x08)) break;
        }
        if (timeout == 0 || (status & 0x01)) return FS_IO_ERROR;

        __asm__ __volatile__("cld");
        outsw(base, ptr + (i * 256), 256);

        timeout = 500000;
        while (timeout--)
        {
            status = inb(base + 7);
            if (!(status & 0x80)) break;
        }
        if (timeout == 0 || (status & 0x01)) return FS_IO_ERROR;
    }
    return FS_OK;
}

int ata_read_wrap(void *priv, uint64_t lba, uint32_t count, void *buf)
{
    ata_priv_t *p = (ata_priv_t*)priv;
    if (lba + count > 0xFFFFFFFFull) return FS_IO_ERROR;
    return ata_pio_read(p->base, p->slave, (uint32_t)lba, count, buf);
}
int ata_write_wrap(void *priv, uint64_t lba, uint32_t count, const void *buf)
{
    ata_priv_t *p = (ata_priv_t*)priv;
    if (lba + count > 0xFFFFFFFFull) return FS_IO_ERROR;
    return ata_pio_write(p->base, p->slave, (uint32_t)lba, count, buf);
}


//Реестр

int register_device(const char *name, uint32_t sector_count,
                    uint16_t base, uint8_t slave, uint32_t flags)
{
    if (devices_count >= MAX_BLOCK_DEVICES) return FS_TOO_MANY;

    int idx = devices_count;

    ata_privs_ptr[idx].base  = base;
    ata_privs_ptr[idx].slave = slave;

    block_dev_entry_t *e = &devices_ptr[idx];
    memset(e, 0, sizeof(*e));

    int i = 0;
    while (name[i] && i < 15)
    {
        e->name[i] = name[i];
        i++;
    }
    e->name[i] = '\0';

    e->sector_count = sector_count;
    e->flags        = flags;
    e->read_fn      = ata_read_wrap;
    e->write_fn     = ata_write_wrap;
    e->priv         = &ata_privs_ptr[idx];
    e->used         = true;

    devices_count++;
    return FS_OK;
}

void ata_probe(uint16_t base, uint8_t slave)
{
    uint16_t identify[256];
    if (ata_identify(base, slave, identify) != FS_OK) return;

    //uint32_t lba28 = identify[60] | ((uint32_t)identify[61] << 16);
    //uint32_t lba48_lo = identify[100] | ((uint32_t)identify[101] << 16);
    //uint32_t lba48_hi = identify[102] | ((uint32_t)identify[103] << 16);
    //uint32_t sectors = lba28 ? lba28 : lba48_lo;   // пока LBA28-only
    //uint32_t sectors = identify[60] | ((uint32_t)identify[61] << 16);
    //if (sectors == 0) return;
    uint32_t sectors = identify[60] | ((uint32_t)identify[61] << 16);
    if (sectors == 0)
    {
        //Попробуем LBA48 (пока только для информации)
        uint32_t l48 = identify[100] | ((uint32_t)identify[101] << 16);
        sectors = l48 ? l48 : 0xFFFFFFFF;   // заглушка: "неизвестно, но диск есть"
    }

    char name[8];
    name[0] = 's';
    name[1] = 'd';
    name[2] = (char)('a' + devices_count);
    name[3] = '\0';

    register_device(name, sectors, base, slave, 0);
    // TODO Phase 3: block_scan_partitions(devices_count - 1);
}



//  -----  внешние функции  -----

int block_read_impl(uint32_t id, uint64_t lba, uint32_t count, void *buf)
{
    if (id >= (uint32_t)devices_count || !devices_ptr[id].used) return FS_IO_ERROR;
    if (!devices_ptr[id].read_fn) return FS_IO_ERROR;
    if (count == 0) return FS_OK;
    return devices_ptr[id].read_fn(devices_ptr[id].priv, lba, count, buf);
}

int block_write_impl(uint32_t id, uint64_t lba, uint32_t count, const void *buf)
{
    if (id >= (uint32_t)devices_count || !devices_ptr[id].used) return FS_IO_ERROR;
    if (!devices_ptr[id].write_fn) return FS_IO_ERROR;
    if (count == 0) return FS_OK;
    if (devices_ptr[id].flags & BLK_READONLY) return FS_ACCESS_DENIED;   //вернули check
    return devices_ptr[id].write_fn(devices_ptr[id].priv, lba, count, buf);
}

int block_get_info(uint32_t id, block_info_t *out)
{
    if (id >= (uint32_t)devices_count || !devices_ptr[id].used) return FS_NOT_FOUND;
    if (!out) return FS_IO_ERROR;

    //memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t*)out)[i] = 0;
    out->id = id;
    int i = 0;
    while (devices_ptr[id].name[i] && i < 15)
    {
        out->name[i] = devices_ptr[id].name[i];
        i++;
    }
    out->name[i]         = '\0';
    out->sector_size     = SECTOR_SIZE;
    out->sector_count    = devices_ptr[id].sector_count;
    out->flags           = devices_ptr[id].flags;
    out->parent_id       = PARENT_NONE;
    out->partition_offset= 0;
    return FS_OK;
}

int block_enum_devices(int index, block_info_t *out)
{
    if (index < 0 || index >= devices_count) return FS_NOT_FOUND;
    return block_get_info((uint32_t)index, out);
}

int block_find_by_name(const char *name, uint32_t *out_id)
{
    if (!name || !out_id) return FS_IO_ERROR;
    for (int i = 0; i < devices_count; i++)
    {
        if (!devices_ptr[i].used) continue;
        if (strcmp(devices_ptr[i].name, name) == 0)
        {
            *out_id = (uint32_t)i;
            return FS_OK;
        }
    }
    return FS_NOT_FOUND;
}

static int block_get_partition(uint32_t disk_id, uint32_t part_num, uint32_t *out_id)
{
    (void)disk_id;
    (void)part_num;
    (void)out_id;
    return FS_NOT_SUPPORTED;
}

int block_scan_partitions(uint32_t disk_id)
{
    (void)disk_id;
    return FS_NOT_SUPPORTED;
}

int block_resolve_boot_drive(uint32_t bios_drive, uint32_t *out_id)
{
    if (!out_id || devices_count == 0) return FS_NOT_FOUND;
    if (bios_drive < 0x80) return FS_NOT_FOUND;

    uint32_t idx = bios_drive - 0x80;
    if (idx >= (uint32_t)devices_count)
    {
        // BIOS-порядок разошёлся с ATA-порядком (типично при boot с USB,
        //когда ATA-дисков нет; Далее USB-драйвер зарегистрирует
        //своё устройство и сюда встанет правильный id)
        *out_id = 0;
        return FS_OK;
    }
    *out_id = idx;
    return FS_OK;
}


int block_register_device(const char *name, uint32_t sector_count, uint32_t flags,
                          int (*read_fn)(void*, uint64_t, uint32_t, void*),
                          int (*write_fn)(void*, uint64_t, uint32_t, const void*),
                          void *priv)
{
    if (!name || !read_fn) return FS_IO_ERROR;
    if (devices_count >= MAX_BLOCK_DEVICES) return FS_TOO_MANY;

    int idx = devices_count;
    block_dev_entry_t *e = &devices_ptr[idx];
    memset(e, 0, sizeof(*e));

    int i = 0;
    while (name[i] && i < 15)
    {
        e->name[i] = name[i];
        i++;
    }
    e->name[i] = '\0';

    e->sector_count = sector_count;
    e->flags        = flags;
    e->read_fn      = read_fn;
    e->write_fn     = write_fn;
    e->priv         = priv;
    e->used         = true;
    devices_count++;
    if (api_ptr) api_ptr->enabled = true;
    return idx;   //валидный id >= 0
}


void block_init(struct boot_info *boot)
{
    (void)boot;
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) devices_ptr[i].used = false;
    devices_count = 0;

    ata_probe(0x1F0, 0); //primary master
    ata_probe(0x1F0, 1); //primary slave
    ata_probe(0x170, 0); //secondary master
    ata_probe(0x170, 1); //secondary slave
}






void driver_main(struct boot_info *boot)
{

    api_ptr = &api;

    devices_ptr = devices;
    ata_privs_ptr = ata_privs;

    block_init(boot);

    description_driver_api_t *descr_api_ptr = &descr_api;
    descr_api_ptr->type = DESCR_BLOCKDEVICE_DRIVER;

    api_ptr->version             = 1;
    api_ptr->enabled             = (devices_count > 0);
    api_ptr->read                = block_read_impl;
    api_ptr->write               = block_write_impl;
    api_ptr->get_info            = block_get_info;
    api_ptr->enum_devices        = block_enum_devices;
    api_ptr->find_by_name        = block_find_by_name;
    api_ptr->get_partition       = block_get_partition;
    api_ptr->scan_partitions     = block_scan_partitions;
    api_ptr->resolve_boot_drive  = block_resolve_boot_drive;
    api_ptr->register_device     = block_register_device;

    descr_api_ptr->struct_ptr = (void *)&api;

    //api_ptr->debug1 = 10;

}


description_driver_api_t *driver_get_api(void)
{
    return &descr_api;
}



