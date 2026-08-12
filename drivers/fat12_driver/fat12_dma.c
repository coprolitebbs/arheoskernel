#include "include/fat12_dma.h"
#include "include/fat12_io.h"
#include "../include_drivers/drv_format.h"

#define DMA_MASK_REG        0x0A
#define DMA_MODE_REG        0x0B
#define DMA_CLEAR_FF_REG    0x0C

#define DMA_CH2_ADDR        0x04
#define DMA_CH2_COUNT       0x05
#define DMA_PAGE_CH2        0x81

#define DMA_MODE_READ       0x44
#define DMA_MODE_SINGLE     0x00
#define DMA_MODE_CHANNEL2   0x02

#define DMA_MASK_CHANNEL2   0x06

#define DMA_UNMASK_CHANNEL2 0x02

/*
static inline void dma_outb(uint16_t port, uint8_t value)
{
    asm volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}
*/



static void dma_mask_channel2(void)
{
    outb(
        DMA_MASK_REG,
        DMA_MASK_CHANNEL2
    );
}


static void dma_unmask_channel2(void)
{
    outb(
        DMA_MASK_REG,
        DMA_UNMASK_CHANNEL2
    );
}


static void dma_clear_flip_flop(void)
{
    outb(
        DMA_CLEAR_FF_REG,
        0
    );
}


static void dma_set_address(uint32_t address)
{
    dma_clear_flip_flop();

    /*
     * Channel 2 address register.
     *
     * Сначала младший байт,
     * затем старший.
     */

    outb(
        DMA_CH2_ADDR,
        address & 0xFF
    );

    outb(
        DMA_CH2_ADDR,
        (address >> 8) & 0xFF
    );

    /*
     * Page register содержит
     * A16..A23.
     */

    outb(
        DMA_PAGE_CH2,
        (address >> 16) & 0xFF
    );
}


static void dma_set_count(uint16_t count)
{
    dma_clear_flip_flop();

    /*
     * DMA count = количество байт - 1.
     */

    uint16_t value = count - 1;

    outb(
        DMA_CH2_COUNT,
        value & 0xFF
    );

    outb(
        DMA_CH2_COUNT,
        (value >> 8) & 0xFF
    );
}


int fat12_dma_read(void *buffer, uint16_t size)
{
    if (!buffer)
        return FS_IO_ERROR;

    if (size == 0)
        return FS_IO_ERROR;

    if (size > 65536)
        return FS_IO_ERROR;

    /*
     * Пока DMA работает через фиксированный
     * физический буфер.
     */

    dma_mask_channel2();

    dma_set_address(FAT12_DMA_BUFFER);

    dma_set_count(size);

    /*
     * Single transfer,
     * address increment,
     * read from device -> memory,
     * channel 2.
     */

    outb(
        DMA_MODE_REG,
        DMA_MODE_READ |
        DMA_MODE_SINGLE |
        DMA_MODE_CHANNEL2
    );

    dma_unmask_channel2();

    /*
     * Сам DMA ничего не запускает.
     *
     * FDC начнёт DMA transfer после
     * команды READ DATA.
     */

    return FS_OK;
}
