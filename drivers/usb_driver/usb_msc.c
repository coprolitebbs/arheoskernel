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


#include "include/usb_msc.h"
#include "../../include-kernel/lib.h"

uint32_t msc_tag = 0x12345678;

/* Выполнить одну BOT-транзакцию.
   cbw_flags: bit 7 = 1 (data IN), 0 (data OUT).
   Возвращает длину данных, реально переданных в data phase (0 если нет data),
   или отрицательный код при ошибке. */
int usb_msc_bot_transfer(uhci_hc_t *hc, uint8_t dev_addr,
                                usb_msc_iface_t *iface,
                                const uint8_t *cb, uint8_t cb_len,
                                uint8_t *data, uint32_t data_len,
                                int data_in){

    usb_msc_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature          = USB_MSC_CBW_SIG;
    cbw.dCBWTag                = msc_tag++;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags             = data_in ? 0x80 : 0x00;
    cbw.bCBWLUN                = 0;
    cbw.bCBWCBLength           = cb_len;
    for (int i = 0; i < cb_len && i < 16; i++) cbw.CBWCB[i] = cb[i];

    /* 1) CBW на Bulk OUT */
    //CBW всегда идёт с DATA0
    uint8_t cbw_toggle = 0;
    int r = uhci_bulk_transfer(hc, dev_addr, iface->bulk_out_addr,
                               iface->bulk_out_max_pkt, &cbw_toggle,
                               &cbw, sizeof(cbw), 0);
    if (r != (int)sizeof(cbw)) return -10;

    /* 2) Data phase */
        int data_result = 0;
    if (data_len > 0 && data){
        //Data-In стартует с DATA1, Data-Out с DATA0
        uint8_t dtoggle = data_in ? 1 : 0;
        r = uhci_bulk_transfer(hc, dev_addr,
                               data_in ? iface->bulk_in_addr : iface->bulk_out_addr,
                               data_in ? iface->bulk_in_max_pkt : iface->bulk_out_max_pkt,
                               &dtoggle, data, data_len, data_in);
        if (r < 0) return -11;
        data_result = r;
    }

    /* 3) CSW на Bulk IN */
        //CSW всегда стартует с DATA1
    usb_msc_csw_t csw;
    memset(&csw, 0, sizeof(csw));
    uint8_t cs = 1;
    r = uhci_bulk_transfer(hc, dev_addr, iface->bulk_in_addr,
                           iface->bulk_in_max_pkt, &cs,
                           &csw, sizeof(csw), 1);
    if (r != (int)sizeof(csw)) return -12;

    if (csw.dCSWSignature != USB_MSC_CSW_SIG)      return -13;
    if (csw.dCSWTag       != cbw.dCBWTag)          return -14;
    if (csw.bCSWStatus    != 0)                    return -15;

    return data_result;
}

int usb_msc_inquiry(uhci_hc_t *hc, uint8_t dev_addr,
                           usb_msc_iface_t *iface){
    uint8_t cb[6] = {
        0x12,        /* INQUIRY */
        0x00,        /* EVPD=0, CmdDt=0 */
        0x00,        /* PageCode=0 */
        0x00,        /* AllocationLength hi */
        36,          /* AllocationLength lo */
        0x00
    };
    uint8_t buf[36];
    memset(buf, 0, sizeof(buf));
    int r = usb_msc_bot_transfer(hc, dev_addr, iface, cb, 6, buf, 36, 1);
    return (r > 0) ? 0 : -1;
}

int usb_msc_read_capacity(uhci_hc_t *hc, uint8_t dev_addr,
                                 usb_msc_iface_t *iface,
                                 uint32_t *out_sectors, uint32_t *out_size){
    uint8_t cb[10] = {
        0x25,        /* READ CAPACITY(10) */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* LBA=0, PMI=0 */
        0x00, 0x00                                  /* reserved */
    };
    uint8_t buf[8];
    memset(buf, 0, sizeof(buf));
    int r = usb_msc_bot_transfer(hc, dev_addr, iface, cb, 10, buf, 8, 1);
    if (r != 8) return -1;

    /* Big-endian: last_lba(4) + block_length(4) */
    uint32_t last_lba = ((uint32_t)buf[0] << 24) |
                        ((uint32_t)buf[1] << 16) |
                        ((uint32_t)buf[2] <<  8) |
                         (uint32_t)buf[3];
    uint32_t blk_size = ((uint32_t)buf[4] << 24) |
                        ((uint32_t)buf[5] << 16) |
                        ((uint32_t)buf[6] <<  8) |
                         (uint32_t)buf[7];

    *out_sectors = last_lba + 1;
    *out_size    = blk_size;
    return 0;
}

int usb_msc_init(uhci_hc_t *hc, uint8_t dev_addr,
                 usb_msc_iface_t *iface,
                 uint32_t *out_sector_count, uint32_t *out_sector_size){

    if (!hc || !iface || !out_sector_count || !out_sector_size) return -1;

    /* Toggle для bulk endpoints после SET_CONFIGURATION сбрасывается в DATA0 */
    iface->bulk_in_toggle  = 0;
    iface->bulk_out_toggle = 0;

    if (usb_msc_inquiry(hc, dev_addr, iface) != 0) return -2;

    if (usb_msc_read_capacity(hc, dev_addr, iface,
                              out_sector_count, out_sector_size) != 0)
        return -3;

    return 0;
}


/* Читает `sectors` секторов, начиная с LBA `lba`.
   Внутри режет на CBW-батчи, которые влезают в MAX_BULK_TDS*max_pkt. */
int usb_msc_read10(uhci_hc_t *hc, uint8_t dev_addr, usb_msc_iface_t *iface,
                   uint32_t lba, uint32_t sectors, uint32_t sector_size,
                   void *buf){
    if (!hc || !iface || !buf || sectors == 0 || sector_size == 0) return -1;

    uint32_t max_batch = (MAX_BULK_TDS * iface->bulk_in_max_pkt) / sector_size;
    if (max_batch == 0) return -1;

    uint8_t *ptr = (uint8_t *)buf;
    uint32_t done = 0;

    while (done < sectors){
        uint32_t batch = sectors - done;
        if (batch > max_batch) batch = max_batch;
        if (batch > 65535)     batch = 65535;   /* предел SCSI READ(10) */

        uint32_t cur_lba = lba + done;
        uint32_t data_len = batch * sector_size;

        uint8_t cb[10] = {
            0x28,                                     /* READ(10) */
            0x00,
            (uint8_t)((cur_lba >> 24) & 0xFF),
            (uint8_t)((cur_lba >> 16) & 0xFF),
            (uint8_t)((cur_lba >>  8) & 0xFF),
            (uint8_t)( cur_lba        & 0xFF),
            0x00,
            (uint8_t)((batch >> 8) & 0xFF),
            (uint8_t)( batch       & 0xFF),
            0x00
        };
        int r = usb_msc_bot_transfer(hc, dev_addr, iface, cb, 10,
                                     ptr + done * sector_size, data_len, 1);
        if (r != (int)data_len) return -2;
        done += batch;
    }
    return 0;
}



int usb_msc_write10(uhci_hc_t *hc, uint8_t dev_addr, usb_msc_iface_t *iface,
                    uint32_t lba, uint32_t sectors, uint32_t sector_size,
                    const void *buf){
    if (!hc || !iface || !buf || sectors == 0 || sector_size == 0) return -1;

    uint32_t max_batch = (MAX_BULK_TDS * iface->bulk_out_max_pkt) / sector_size;
    if (max_batch == 0) return -1;

    const uint8_t *ptr = (const uint8_t *)buf;
    uint32_t done = 0;

    while (done < sectors){
        uint32_t batch = sectors - done;
        if (batch > max_batch) batch = max_batch;
        if (batch > 65535)     batch = 65535;

        uint32_t cur_lba = lba + done;
        uint32_t data_len = batch * sector_size;

        uint8_t cb[10] = {
            0x2A,                                     /* WRITE(10) */
            0x00,
            (uint8_t)((cur_lba >> 24) & 0xFF),
            (uint8_t)((cur_lba >> 16) & 0xFF),
            (uint8_t)((cur_lba >>  8) & 0xFF),
            (uint8_t)( cur_lba        & 0xFF),
            0x00,
            (uint8_t)((batch >> 8) & 0xFF),
            (uint8_t)( batch       & 0xFF),
            0x00
        };
        int r = usb_msc_bot_transfer(hc, dev_addr, iface, cb, 10,
                                     (uint8_t *)ptr + done * sector_size,
                                     data_len, 0);
        if (r != (int)data_len) return -2;
        done += batch;
    }
    return 0;
}