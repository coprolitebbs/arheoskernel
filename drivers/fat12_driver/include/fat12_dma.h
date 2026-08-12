#ifndef FAT12_DMA_H
#define FAT12_DMA_H

#include <stdint.h>

#define FAT12_DMA_BUFFER 0x00080000
#define FAT12_DMA_SIZE   512

int fat12_dma_read(void *buffer, uint16_t size);


#endif
