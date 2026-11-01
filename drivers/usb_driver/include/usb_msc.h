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

#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdint.h>
#include "usb_core.h"
#include "usb_hcd.h"

#define USB_MAX_MSC  4

#define USB_MSC_CBW_SIG 0x43425355u
#define USB_MSC_CSW_SIG 0x53425355u

typedef struct {
    usb_hcd_t *hcd;
    usb_msc_iface_t iface;
    uint8_t dev_addr;
    uint8_t used;
    uint32_t sector_count;
    uint32_t sector_size;
    // Безопасные не-стековые буферы для DMA Bus-Master
    uint8_t static_cbw_buf[32] __attribute__((aligned(32)));
    uint8_t static_csw_buf[32] __attribute__((aligned(32)));
    uint8_t static_data_buf[64] __attribute__((aligned(32)));
} usb_msc_ctx_t;

typedef struct {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];
} __attribute__((packed)) usb_msc_cbw_t;

typedef struct {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;
} __attribute__((packed)) usb_msc_csw_t;



extern usb_msc_ctx_t g_msc_ctx[USB_MAX_MSC];
extern usb_msc_ctx_t *g_msc_ctx_ptr;
extern int g_msc_idx; // Импортируем модульный счетчик из usb_driver.c
extern void msc_init_log_func(void *func_addr);

/* Возвращает 0 при успехе (INQUIRY + READ CAPACITY пройдены),
   отрицательное — если нет. sector_count и sector_size валидны
   только при успехе. */
int usb_msc_init(usb_hcd_t *hcd, uint8_t dev_addr,
                 usb_msc_iface_t *iface,
                 uint32_t *out_sector_count, uint32_t *out_sector_size);

int usb_msc_read10(usb_hcd_t *hcd, uint8_t dev_addr, usb_msc_iface_t *iface,
                   uint32_t lba, uint32_t sectors, uint32_t sector_size,
                   void *buf);

int usb_msc_write10(usb_hcd_t *hcd, uint8_t dev_addr, usb_msc_iface_t *iface,
                    uint32_t lba, uint32_t sectors, uint32_t sector_size,
                    const void *buf);

int usb_msc_read_wrap(void *priv, uint64_t lba, uint32_t count, void *buf);
int usb_msc_write_wrap(void *priv, uint64_t lba, uint32_t count, const void *buf);

int usb_msc_bot_reset(usb_hcd_t *hcd, uint8_t addr);

void umsc_register_ptrs(void);

#endif
