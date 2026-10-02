#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h>
void beep(uint32_t count);

uint32_t enumerate_pci_ven_dev_ids(void);
void find_and_prepare_s3_trio64(void);

#endif
