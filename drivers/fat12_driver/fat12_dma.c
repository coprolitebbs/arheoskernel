#include "include/fat12_dma.h"
//#include "include/fat12_io.h"
#include "../include_drivers/drv_format.h"
#include "../../include-kernel/ports_io.h"


static void dma_mask_channel2(void){
    outb(DMA_MASK_REG,DMA_MASK_CHANNEL2);
}


static void dma_unmask_channel2(void){
    outb(DMA_MASK_REG,DMA_UNMASK_CHANNEL2);
}


static void dma_clear_flip_flop(void){
    outb(DMA_CLEAR_FF_REG,0);
}


static void dma_set_address(uint32_t address){
    dma_clear_flip_flop();
    //Сначала младший байт, затем старший
    outb(DMA_CH2_ADDR,address & 0xFF);
    outb(DMA_CH2_ADDR,(address >> 8) & 0xFF);
    //Page register содержит A16..A23
    outb(DMA_PAGE_CH2,(address >> 16) & 0xFF);
}


static void dma_set_count(uint16_t count){
    dma_clear_flip_flop();
    //DMA count = количество байт - 1
    uint16_t value = count - 1;
    outb(DMA_CH2_COUNT,value & 0xFF);
    outb(DMA_CH2_COUNT,(value >> 8) & 0xFF);
}


//DMA setup
void dma_floppy_read(void *buffer,uint16_t count){
    uint32_t addr = (uint32_t)buffer;
    //Запрещаем канал 2
    outb(DMA_MASK_REG,0x06);
    //Сбрасываем flip-flop
    outb(DMA_CLEAR_FF_REG,0);
    //Адрес
    outb(DMA_CH2_ADDR,(uint8_t)(addr & 0xFF));
    outb(DMA_CH2_ADDR,(uint8_t)((addr >> 8) & 0xFF));
    //Page register
    outb(DMA_PAGE_CH2,(uint8_t)((addr >> 16) & 0xFF));
    //Снова сбрасываем flip-flop
    outb(DMA_CLEAR_FF_REG,0);
    //count = количество байт - 1
    count--;
    outb(DMA_CH2_COUNT,(uint8_t)(count & 0xFF));
    outb(DMA_CH2_COUNT,(uint8_t)((count >> 8) & 0xFF));

    //Режим channel 2 single address increment read from device > memory
    outb(DMA_MODE_REG,0x46);
    //Разрешаем канал 2
    outb(DMA_MASK_REG,0x02);
}



int fat12_dma_read(void *buffer, uint16_t size){
    if (!buffer) return FS_IO_ERROR;
    if (size == 0) return FS_IO_ERROR;
    if (size > 65536) return FS_IO_ERROR;
    //Пока DMA работает через фиксированный физический буфер
    dma_mask_channel2();
    dma_set_address(FAT12_DMA_BUFFER);
    dma_set_count(size);
    //Single transfer, address increment, read from device -> memory, channel 2
    outb(DMA_MODE_REG, DMA_MODE_READ | DMA_MODE_SINGLE | DMA_MODE_CHANNEL2);
    dma_unmask_channel2();
    //Сам DMA ничего не запускает. FDC начнёт DMA transfer послекоманды READ DATA

    return FS_OK;
}


int fat12_dma_write(void *buffer, uint16_t size){
    if (!buffer || size == 0) return FS_IO_ERROR;
    // Запрещаем канал 2
    outb(0x0A, 0x06);
    // Сброс флип-флоп
    outb(0x0C, 0);
    // Адрес (младший, затем старший)
    outb(0x04, (uint32_t)FAT12_DMA_BUFFER & 0xFF);
    outb(0x04, ((uint32_t)FAT12_DMA_BUFFER >> 8) & 0xFF);
    // Page
    outb(0x81, ((uint32_t)FAT12_DMA_BUFFER >> 16) & 0xFF);
    outb(0x0C, 0);
    // Count - 1
    uint16_t val = size - 1;
    outb(0x05, val & 0xFF);
    outb(0x05, (val >> 8) & 0xFF);
    // Режим: 0x4A (Запись из RAM на Диск)
    outb(0x0B, 0x4A);
    // Разрешаем канал 2
    outb(0x0A, 0x02);
    return FS_OK;
}
