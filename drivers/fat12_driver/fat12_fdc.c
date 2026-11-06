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
//#include "include/fat12_io.h"
#include "include/fat12_fdc.h"
#include "include/fat12_dma.h"
#include "include/fat12_driver.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"
#include "../include_drivers/drv_format.h"

static uint8_t fdc_ksl_we = 0;
static void fdc_lock_sched(void)
{
    if (api_ptr && api_ptr->schedule_lock_ptr && *(volatile uint8_t *)api_ptr->schedule_lock_ptr == 0)
    {
        *(volatile uint8_t *)api_ptr->schedule_lock_ptr = 1;
        fdc_ksl_we = 1;
    }
}
static void fdc_unlock_sched(void)
{
    if (api_ptr && api_ptr->schedule_lock_ptr && fdc_ksl_we)
    {
        *(volatile uint8_t *)api_ptr->schedule_lock_ptr = 0;
        fdc_ksl_we = 0;
    }
}

static void fdc_unlock_scheduler(void)
{
    if (api_ptr && api_ptr->schedule_lock_ptr && fdc_ksl_we)
    {
        *(volatile uint8_t *)api_ptr->schedule_lock_ptr = 0;
        fdc_ksl_we = 0;
    }
}

//wait
int fat12_fdc_wait_irq_async(uint32_t timeout_loops)
{
    if (!api_ptr->floppy_irq_fired_ptr) return FS_IO_ERROR;
    volatile uint32_t loops = timeout_loops;
    while (loops--)
    {
        if (*(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr == 1)
        {
            *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
            return FS_OK;
        }

        uint8_t msr = inb(FDC_MSR);
        if ((msr & FDC_MSR_RQM) && (msr & FDC_MSR_DIO)
                //&&(msr & FDC_MSR_BUSY)
           )
        {
            *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
            return FS_OK;
        }
        if (!(msr & FDC_MSR_BUSY))
        {
            *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
            return FS_OK;
        }

        //небольшая пауза, чтобы не забивать шину
        asm volatile("pause");
    }
    return FS_IO_ERROR;
}




//send command byte
int fdc_send_byte(uint8_t value)
{
    uint32_t timeout = 1000000;
    while (timeout--)
    {
        uint8_t msr = inb(FDC_MSR);
        if ((msr & FDC_MSR_RQM) && !(msr & FDC_MSR_DIO))
        {
            outb(FDC_FIFO, value);
            return FS_OK;
        }
    }
    return FS_IO_ERROR;
}


//receive result byte
int fdc_receive_byte(uint8_t *value)
{
    uint32_t timeout = 1000000;
    while (timeout--)
    {
        uint8_t msr = inb(FDC_MSR);
        //Ждем, пока контроллер будет готов (RQM = 1) и проверяем, что направление данных строго к процессору (DIO = 1)
        if ((msr & FDC_MSR_RQM) && (msr & FDC_MSR_DIO))
        {
            *value = inb(FDC_FIFO);
            return FS_OK;
        }
    }

    return FS_IO_ERROR;
}


//reset FDC
int fdc_reset(void)
{
    uint8_t st0, cylinder;
    int result = FS_IO_ERROR;

    fdc_lock_sched();

    //Reset-импульс. Держим RESET# в низком уровне >= 100 us 2M nop ≈ 4–40 ms в
    // зависимости от CPU, с запасом покрывает спеку
    outb(FDC_DOR, 0x00);
    for (volatile int i = 0; i < 2000000; i++) asm volatile("nop");

    //Отпускаем reset, сразу включаем IRQ и мотор. Мотор нужен до RECALIBRATE,
    //иначе SEEK не выполнится
    if (api_ptr->floppy_irq_fired_ptr)
        *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
    outb(FDC_DOR, FDC_DOR_RESET | FDC_DOR_DMA_IRQ | FDC_DOR_MOTOR0);

    //Ждём IRQ6 о завершении reset (~100 us после release)
    if (fat12_fdc_wait_irq_async(40000000) != FS_OK) goto out;

    //Чистим очередь статусов FDC (4x SENSE_INTERRUPT, как в даташите)
    for (int i = 0; i < 4; i++)
    {
        if (fdc_send_byte(FDC_CMD_SENSE_INTERRUPT) != FS_OK) break;
        if (fdc_receive_byte(&st0) != FS_OK) break;
        if (fdc_receive_byte(&cylinder) != FS_OK) break;
    }

    //Спин-ап диска: 300–500 ms для 3.5". На быстрых CPU (GHz-класс) 50M итераций = ~50–150 ms
    //— этого мало, используем 200M итераций: ~200–600 ms на реальном железе
    for (volatile int delay = 0; delay < 200000000; delay++) asm volatile("nop");

    //RECALIBRATE → голова на цилиндр 0
    if (api_ptr->floppy_irq_fired_ptr)
        *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
    if (fdc_send_byte(FDC_CMD_RECALIBRATE) != FS_OK) goto out;
    if (fdc_send_byte(0x00) != FS_OK) goto out;
    if (fat12_fdc_wait_irq_async(40000000) != FS_OK) goto out;

    //Проверяем, что головка действительно на цилиндре 0
    if (fdc_send_byte(FDC_CMD_SENSE_INTERRUPT) != FS_OK) goto out;
    if (fdc_receive_byte(&st0) != FS_OK) goto out;
    if (fdc_receive_byte(&cylinder) != FS_OK) goto out;
    if (cylinder != 0) goto out;

    result = FS_OK;
out:
    fdc_unlock_sched();
    return result;
}

//motor
static void fdc_motor_on(void)
{
    outb(FDC_DOR, FDC_DOR_RESET | FDC_DOR_DMA_IRQ | FDC_DOR_MOTOR0);
}

//Recalibrate
int fdc_recalibrate(void)
{
    uint8_t st0;
    uint8_t cylinder;

    int result = FS_IO_ERROR;
    fdc_lock_sched();

    int r;
    if (api_ptr->floppy_irq_fired_ptr)
    {
        *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
    }

    r = fdc_send_byte(FDC_CMD_RECALIBRATE);
    if (r != FS_OK)
    {
        result = r;
        goto out;
    }
    r = fdc_send_byte(0);
    if (r != FS_OK)
    {
        result = r;
        goto out;
    }

    if (fat12_fdc_wait_irq_async(40000000) != FS_OK) goto out;

    r = fdc_sense_interrupt(&st0,&cylinder);
    if (r != FS_OK)
    {
        result = r;
        goto out;
    }
    if (cylinder != 0)
    {
        goto out;
    }

    //fdc_unlock_scheduler();

    result = FS_OK;

out:
    fdc_unlock_sched();
    return result;
}


int fdc_sense_interrupt(uint8_t *st0,uint8_t *cylinder)
{
    int r;
    r = fdc_send_byte(FDC_CMD_SENSE_INTERRUPT);
    if (r != FS_OK) return r;

    r = fdc_receive_byte(st0);
    if (r != FS_OK) return r;

    r = fdc_receive_byte(cylinder);
    if (r != FS_OK) return r;

    return FS_OK;
}






//Чтение одного сектора
//CHS:
//cylinder - 0..
//head     - 0/1
//sector   - 1..
//DMA:
//floppy -> DMA channel 2 -> FAT12_DMA_BUFFER
//После успешного чтения данные копируются из DMA-буфера в buffer
int fdc_read_sector(fat12_fs_t *vol, uint32_t cylinder, uint32_t head,
                    uint32_t sector, void *buffer)
{
    if (!vol || !buffer) return FS_IO_ERROR;
    if (vol->drive != 0) return FS_NOT_SUPPORTED;
    if (vol->bytes_per_sector != 512) return FS_NOT_SUPPORTED;
    if (head >= vol->heads) return FS_IO_ERROR;
    if (sector == 0 || sector > vol->sectors_per_track) return FS_IO_ERROR;

    int result = FS_IO_ERROR;
    uint8_t st0, st1, st2, c, h, r, n, seek_st0, seek_cyl;

    fdc_lock_sched();

    //SEEK + WAIT IRQ + SENSE_INTERRUPT
    if (fdc_send_byte(FDC_CMD_SEEK) != FS_OK) goto out;
    if (fdc_send_byte((head << 2) | 0) != FS_OK) goto out;
    if (fdc_send_byte(cylinder) != FS_OK) goto out;

    if (fat12_fdc_wait_irq_async(40000000) != FS_OK) goto out;

    if (fdc_send_byte(FDC_CMD_SENSE_INTERRUPT) != FS_OK) goto out;
    if (fdc_receive_byte(&seek_st0) != FS_OK) goto out;
    if (fdc_receive_byte(&seek_cyl) != FS_OK) goto out;
    if ((seek_st0 & 0xC0) != 0) goto out;

    //SPECIFY
    if (fdc_send_byte(FDC_CMD_SPECIFY) != FS_OK) goto out;
    if (fdc_send_byte(0xDF) != FS_OK) goto out;
    if (fdc_send_byte(0x02) != FS_OK) goto out;

    //MOTOR ON + небольшая стабилизация
    fdc_motor_on();
    io_wait();
    io_wait();
    io_wait();
    for (volatile int d = 0; d < 200000; d++) asm volatile("nop");

    //DMA setup
    if (fat12_dma_read((void *)FAT12_DMA_BUFFER, 512) != FS_OK) goto out;

    if (api_ptr->floppy_irq_fired_ptr)
        *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;

    //READ DATA
    if (fdc_send_byte(FDC_CMD_READ_DATA) != FS_OK) goto out;
    if (fdc_send_byte((uint8_t)((head << 2) | 0)) != FS_OK) goto out;
    c = (uint8_t)cylinder;
    h = (uint8_t)head;
    r = (uint8_t)sector;
    n = 2;
    uint8_t eot = (vol->sectors_per_track > 0) ? (uint8_t)vol->sectors_per_track : 18;
    if (fdc_send_byte(c) != FS_OK) goto out;
    if (fdc_send_byte(h) != FS_OK) goto out;
    if (fdc_send_byte(r) != FS_OK) goto out;
    if (fdc_send_byte(n) != FS_OK) goto out;
    if (fdc_send_byte(eot) != FS_OK) goto out;
    if (fdc_send_byte(0x1B) != FS_OK) goto out;
    if (fdc_send_byte(0xFF) != FS_OK) goto out;

    //ЖДЁМ IRQ6
    if (fat12_fdc_wait_irq_async(40000000) != FS_OK) goto out;

    //Читаем 7 байт статуса
    if (fdc_receive_byte(&st0) != FS_OK) goto out;
    if (fdc_receive_byte(&st1) != FS_OK) goto out;
    if (fdc_receive_byte(&st2) != FS_OK) goto out;
    if (fdc_receive_byte(&c) != FS_OK) goto out;
    if (fdc_receive_byte(&h) != FS_OK) goto out;
    if (fdc_receive_byte(&r) != FS_OK) goto out;
    if (fdc_receive_byte(&n) != FS_OK) goto out;

    if (st0 & 0xC0) goto out;
    if (st1 != 0 || st2 != 0 || n != 2) goto out;

    memcpy(buffer, (void *)FAT12_DMA_BUFFER, 512);
    result = FS_OK;

out:
    fdc_unlock_sched();
    return result;
}


int fdc_write_sector(fat12_fs_t *vol,uint32_t cylinder,uint32_t head,uint32_t sector,const void *buffer)
{
    uint8_t st0, st1, st2, c, h, r, n;
    uint8_t seek_st0, seek_cyl;

    int result = FS_IO_ERROR;

    fdc_lock_sched();

    if (!vol || !buffer || vol->drive != 0) goto out;

    //Позиционирование головки (SEEK)
    if (fdc_send_byte(FDC_CMD_SEEK) != FS_OK) goto out;
    if (fdc_send_byte((head << 2) | 0) != FS_OK) goto out;
    if (fdc_send_byte(cylinder) != FS_OK) goto out;

    if (fat12_fdc_wait_irq_async(40000000) != FS_OK) goto out;

    if (fdc_send_byte(FDC_CMD_SENSE_INTERRUPT) != FS_OK) goto out;
    if (fdc_receive_byte(&seek_st0) != FS_OK) goto out;
    if (fdc_receive_byte(&seek_cyl) != FS_OK) goto out;
    if ((seek_st0 & 0xC0) != 0) goto out;

    //SPECIFY — SRT/HUT/HLT, те же значения, что в fdc_read_sector
    if (fdc_send_byte(FDC_CMD_SPECIFY) != FS_OK) goto out;
    if (fdc_send_byte(0xDF) != FS_OK) goto out;
    if (fdc_send_byte(0x02) != FS_OK) goto out;

    for (volatile int delay = 0; delay < 200000; delay++) asm volatile("nop");

    //Подготовка данных в DMA буфере
    memcpy((void *)FAT12_DMA_BUFFER, buffer, 512);

    //Настройка DMA на запись
    if (fat12_dma_write((void *)FAT12_DMA_BUFFER, 512) != FS_OK) goto out;

    //Врубаем мотор
    fdc_motor_on();
    io_wait();
    io_wait();
    io_wait();

    //Команда WRITE DATA (0x45)
    if (fdc_send_byte(FDC_CMD_WRITE_DATA) != FS_OK) goto out;
    if (fdc_send_byte((head << 2) | 0) != FS_OK) goto out;

    c = (uint8_t)cylinder;
    h = (uint8_t)head;
    r = (uint8_t)sector;
    n = 2; //512 байт
    uint8_t write_eot = (vol && vol->sectors_per_track > 0) ? (uint8_t)vol->sectors_per_track : 18;
    if (fdc_send_byte(c) != FS_OK) goto out;
    if (fdc_send_byte(h) != FS_OK) goto out;
    if (fdc_send_byte(r) != FS_OK) goto out;
    if (fdc_send_byte(n) != FS_OK) goto out;
    if (fdc_send_byte(write_eot) != FS_OK) goto out;

    if (fdc_send_byte(0x1B) != FS_OK) goto out; //GPL
    if (fdc_send_byte(0xFF) != FS_OK) goto out; // DT
    if (fat12_fdc_wait_irq_async(40000000) != FS_OK) goto out;

    //Читаем 7 байт статуса
    if (fdc_receive_byte(&st0) != FS_OK) goto out;
    io_wait();
    if (fdc_receive_byte(&st1) != FS_OK) goto out;
    io_wait();
    if (fdc_receive_byte(&st2) != FS_OK) goto out;
    io_wait();
    if (fdc_receive_byte(&c) != FS_OK) goto out;
    io_wait();
    if (fdc_receive_byte(&h) != FS_OK) goto out;
    io_wait();
    if (fdc_receive_byte(&r) != FS_OK) goto out;
    io_wait();
    if (fdc_receive_byte(&n) != FS_OK) goto out;
    io_wait();


    if (st0 & 0xC0) goto out; //Крах или защита от записи (Write Protected)
    if (st1 != 0 || st2 != 0 || n != 2) goto out;

    result = FS_OK;


out:
    fdc_unlock_sched();

    return result;
}

