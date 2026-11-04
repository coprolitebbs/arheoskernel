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

#include "include/fat12_disk.h"
#include "include/fat12_internal.h"
#include "include/fat12_boot.h"
//#include "include/fat12_io.h"
#include "include/fat12_fdc.h"
#include "include/fat12_dma.h"
#include "include/fat12_driver.h"
#include "../include_drivers/drv_format.h"
#include "../../include-kernel/ports_io.h"


//флаг IRQ6
static volatile int floppy_irq_done = 0;

//конверт lba в chs

static void lba_to_chs(fat12_fs_t *vol,uint32_t lba,uint8_t *cylinder,uint8_t *head,uint8_t *sector)
{
    uint32_t spt = vol->sectors_per_track;
    uint32_t heads = vol->heads;

    *cylinder = lba / (heads * spt);
    uint32_t tmp = lba % (heads * spt);
    *head = tmp / spt;
    *sector = (tmp % spt) + 1;
}


//Чтение одного сектора
static int floppy_read_sector(fat12_fs_t *vol,uint32_t lba,void *buffer)
{
    uint8_t cylinder;
    uint8_t head;
    uint8_t sector;
    lba_to_chs(vol,lba,&cylinder,&head,&sector);
    //Настраиваем DMA на 512 байт.
    dma_floppy_read(buffer,512);
    //Выбираем drive 0 и включаем motor.
    outb(FDC_DOR,FDC_DOR_ENABLE | FDC_DOR_DMA_IRQ | FDC_DOR_MOTOR0 | FDC_DOR_DRIVE0);
    io_wait();

    //READ DATA
    //MT  MFM  SK
    // 0   1   0
    int r;

    r = fdc_send_byte(FDC_CMD_READ_DATA | 0x40);
    if (r != FS_OK) return r;

    //Drive/head
    r = fdc_send_byte((head << 2) | 0);
    if (r != FS_OK) return r;

    //Cylinder
    r = fdc_send_byte(cylinder);
    if (r != FS_OK) return r;

    //Head
    r = fdc_send_byte(head);
    if (r != FS_OK) return r;

    //Sector
    r = fdc_send_byte(sector);
    if (r != FS_OK) return r;

    //Sector size:
    //2 = 512 bytes
    r = fdc_send_byte(2);
    if (r != FS_OK) return r;

    // EOT
    //Читаем только один сектор.
    r = fdc_send_byte((uint8_t)vol->sectors_per_track);
    if (r != FS_OK) return r;

    //GAP3
    r = fdc_send_byte(0x1B);
    if (r != FS_OK) return r;

    //Data length.
    r = fdc_send_byte(0xFF);
    if (r != FS_OK) return r;

    //В полноценной реализации здесь ждём IRQ6.
    //Для первого теста просто ждём, пока контроллер закончит busy.
    uint32_t timeout = 2000000;
    while (timeout--)
    {
        uint8_t msr = inb(FDC_MSR);
        if (!(msr & FDC_MSR_CB)) break;
    }
    if (!timeout) return FS_IO_ERROR;
    //Получаем 7 байт результата.
    uint8_t result[7];
    for (int i = 0; i < 7; i++)
    {
        r = fdc_receive_byte(&result[i]);
        if (r != FS_OK) return r;
    }

    //ST0
    if (result[0] & 0xC0) return FS_IO_ERROR;
    //ST1/ST2 errors.
    if (result[1] & 0xBF) return FS_IO_ERROR;
    if (result[2] & 0x7F) return FS_IO_ERROR;

    return FS_OK;
}


//Чтение сектора
int fat12_read_sectors(fat12_fs_t *vol, uint32_t lba, uint32_t count, void *buffer)
{
    if (!vol || !buffer) return FS_IO_ERROR;
    if (count == 0) return FS_OK;

    //Block API
    if (vol->use_block_api && api_ptr->block)
    {
        int r;
        if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 1;
        r = api_ptr->block->read(vol->drive, (uint64_t)lba, count, buffer);
        if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;
        return r;
    }

    //FDC (legacy PIO)
    if (vol->bytes_per_sector != 512) return FS_NOT_SUPPORTED;
    uint8_t *dst = (uint8_t *)buffer;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t current_lba = lba + i;
        uint32_t sectors_per_cylinder = vol->heads * vol->sectors_per_track;
        uint32_t cylinder = current_lba / sectors_per_cylinder;
        uint32_t tmp = current_lba % sectors_per_cylinder;
        uint32_t head = tmp / vol->sectors_per_track;
        uint32_t sector = (tmp % vol->sectors_per_track) + 1;
        int r = fdc_read_sector(vol, cylinder, head, sector, dst + i * vol->bytes_per_sector);
        if (r != FS_OK) return r;
    }
    return FS_OK;
}


//Запись сектора
int fat12_write_sectors(fat12_fs_t *vol, uint32_t lba, uint32_t count, const void *buffer)
{
    if (!vol || !buffer || count == 0) return FS_IO_ERROR;

    //Block API
    if (vol->use_block_api && api_ptr->block)
    {
        int r;
        if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 1;
        r = api_ptr->block->write(vol->drive, (uint64_t)lba, count, buffer);
        if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;
        return r;
    }

    //FDC (legacy PIO)
    if (vol->bytes_per_sector != 512) return FS_NOT_SUPPORTED;
    const uint8_t *src = (const uint8_t *)buffer;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t current_lba = lba + i;
        uint32_t sectors_per_cylinder = vol->heads * vol->sectors_per_track;
        uint32_t cylinder = current_lba / sectors_per_cylinder;
        uint32_t tmp = current_lba % sectors_per_cylinder;
        uint32_t head = tmp / vol->sectors_per_track;
        uint32_t sector = (tmp % vol->sectors_per_track) + 1;
        int r = fdc_write_sector(vol, cylinder, head, sector, src + i * 512);
        if (r != FS_OK) return r;
    }
    return FS_OK;
}



//Инициализация
int fat12_disk_init(fat12_fs_t *vol)
{
    if (!vol) return FS_IO_ERROR;
    vol->sectors_per_track = 18;
    vol->heads = 2;

    //Пробуем block API, принимаем его, только если блок-устройство по адресу vol->drive
    //реально содержит FAT12 boot sector. Это отсекает 86box с пустым/не-FAT12 ATA hda
    //(там блок есть, но не тот) и корректно ловит USB-FDD (там USB-образ как раз FAT12)
    if (api_ptr->block_device_driver_enabled && api_ptr->block)
    {
        block_info_t bi;
        if (api_ptr->block->get_info(vol->drive, &bi) == FS_OK)
        {
            uint8_t probe[512];
            if (api_ptr->block->read(vol->drive, 0, 1, probe) == FS_OK)
            {
                //Лёгкая проверка FAT12/FAT16:
                //   - bps == 512 (отсекает большинство не-дисков)
                //   - root_entries > 0 (отсекает FAT32, у неё 0)
                //   - sectors_per_fat > 0
                //   - boot signature 0xAA55
                uint16_t bps          = probe[0x0B] | ((uint16_t)probe[0x0C] << 8);
                uint16_t root_entries = probe[0x11] | ((uint16_t)probe[0x12] << 8);
                uint16_t spf          = probe[0x16] | ((uint16_t)probe[0x17] << 8);
                uint16_t sig          = probe[510]  | ((uint16_t)probe[511]  << 8);

                if (bps == 512 && root_entries > 0 && spf > 0 && sig == 0xAA55)
                {
                    vol->use_block_api = 1;
                    return FS_OK;
                }
            }
        }
    }

    //Блок либо отсутствует, либо не FAT12 — идём в FDC
    if (vol->drive != 0) return FS_NOT_SUPPORTED;

    vol->use_block_api = 0;
    int r = fdc_reset();
    if (r != FS_OK) return r;
    r = fdc_recalibrate();
    if (r != FS_OK) return r;
    return FS_OK;
}


uint16_t fat12_get_next_cluster(fat12_fs_t *vol,uint16_t cluster)
{
    uint32_t offset;

    if (!vol) return 0xFFF;

    offset = cluster + (cluster / 2);

    if (offset + 1 >= vol->sectors_per_fat * vol->bytes_per_sector)
    {
        return 0xFFF;
    }
    uint16_t value = vol->fat[offset] | ((uint16_t)vol->fat[offset + 1] << 8);
    if (cluster & 1)
        value >>= 4;
    else
        value &= 0x0FFF;

    return value & 0x0FFF;
}
