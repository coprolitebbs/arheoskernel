#ifndef FAT12_DMA_H
#define FAT12_DMA_H

#include <stdint.h>


#define DMA_MASK_REG        0x0A
#define DMA_MODE_REG        0x0B
#define DMA_CLEAR_FF_REG    0x0C

#define DMA_CH2_ADDR        0x04
#define DMA_CH2_COUNT       0x05
#define DMA_PAGE_CH2        0x81

#define DMA_MODE_READ       0x04
#define DMA_MODE_WRITE      0x08
#define DMA_MODE_SINGLE     0x40
#define DMA_MODE_CHANNEL2   0x02

#define DMA_MASK_CHANNEL2   0x06

#define DMA_UNMASK_CHANNEL2 0x02

#define FAT12_DMA_BUFFER 0x00070000
#define FAT12_DMA_SIZE   512

void dma_floppy_read(void *buffer,uint16_t count);
int fat12_dma_read(void *buffer, uint16_t size);
int fat12_dma_write(void *buffer, uint16_t size);

#endif
