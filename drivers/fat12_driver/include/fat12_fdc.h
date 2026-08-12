#ifndef FAT12_FDC_H
#define FAT12_FDC_H

#include <stdint.h>

//static int fdc_wait_rqm(uint32_t timeout);
int fdc_send_byte(uint8_t value);
int fdc_receive_byte(uint8_t *value);
int fdc_reset(void);
static void fdc_motor_on(void);
int fdc_recalibrate(void);
int fdc_sense_interrupt(uint8_t *st0,uint8_t *cylinder);
int fdc_read_sector(fat12_fs_t *vol,uint32_t cylinder,uint32_t head,uint32_t sector,void *buffer);
#endif
