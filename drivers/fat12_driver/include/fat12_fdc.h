#ifndef FAT12_FDC_H
#define FAT12_FDC_H

#include <stdint.h>
#include "fat12_internal.h"

//Floppy Disk Controller (Intel 82077AA / NEC 765 Compatible) Ports
#define FDC_DOR                 0x3F2   /* Digital Output Register (Write Only) */
#define FDC_MSR                 0x3F4   /* Main Status Register (Read Only) */
#define FDC_FIFO                0x3F5   /* Data FIFO / Command Register (R/W) */
#define FDC_CCR                 0x3F7   /* Configuration Control Register (Write Only) */
#define FDC_CTRL                0x3F7

//Digital Output Register (DOR) Bits
#define FDC_DOR_DRIVE0          0x00
#define FDC_DOR_DRIVE1          0x01
#define FDC_DOR_RESET           0x04
#define FDC_DOR_ENABLE          0x04
#define FDC_DOR_DMA_IRQ         0x08
#define FDC_DOR_DMA             0x08

#define FDC_DOR_MOTOR0          0x10
#define FDC_DOR_MOTOR1          0x20

//Main Status Register (MSR) Bits
#define FDC_MSR_BUSY            0x10
#define FDC_MSR_CB              0x10
#define FDC_MSR_NDMA            0x20
#define FDC_MSR_DIO             0x40
#define FDC_MSR_RQM             0x80

//Floppy Controller Commands (0x40 - MFM Mode)
#define FDC_CMD_SPECIFY         0x03
#define FDC_CMD_WRITE_DATA      0x45
#define FDC_CMD_READ_DATA       0x46
#define FDC_CMD_RECALIBRATE     0x07
#define FDC_CMD_SENSE_INTERRUPT 0x08
#define FDC_CMD_SEEK            0x0F
#define FDC_CMD_FORMAT_TRACK    0x4D


int fdc_send_byte(uint8_t value);
int fdc_receive_byte(uint8_t *value);
int fdc_reset(void);
static void fdc_motor_on(void);
int fdc_recalibrate(void);
int fdc_sense_interrupt(uint8_t *st0,uint8_t *cylinder);
int fdc_read_sector(fat12_fs_t *vol,uint32_t cylinder,uint32_t head,uint32_t sector,void *buffer);
int fdc_write_sector(fat12_fs_t *vol,uint32_t cylinder,uint32_t head,uint32_t sector,const void *buffer);

#endif
