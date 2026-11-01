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
#include "include/usb_hcd.h"
#include "include/usb_driver.h"
#include "../../include-kernel/lib.h"
#include "../../userspace/include/fileattrs.h"


uint32_t msc_tag = 0x12345678;

usb_msc_ctx_t g_msc_ctx[USB_MAX_MSC];
usb_msc_ctx_t *g_msc_ctx_ptr;
//void (*msc_log_ptr)(const char *fmt, ...) = NULL;

int usb_msc_read_wrap(void *priv, uint64_t lba, uint32_t count, void *buf){
    usb_msc_ctx_t *ctx = (usb_msc_ctx_t *)priv;
    if (!ctx || !ctx->used) return FS_IO_ERROR;
    if (lba + count > ctx->sector_count) return FS_IO_ERROR;
    int r = usb_msc_read10(ctx->hcd, ctx->dev_addr, &ctx->iface,
                           (uint32_t)lba, count, ctx->sector_size, buf);
    return (r == 0) ? FS_OK : FS_IO_ERROR;
}

int usb_msc_write_wrap(void *priv, uint64_t lba, uint32_t count, const void *buf){
    usb_msc_ctx_t *ctx = (usb_msc_ctx_t *)priv;
    if (!ctx || !ctx->used) return FS_IO_ERROR;
    if (lba + count > ctx->sector_count) return FS_IO_ERROR;
    int r = usb_msc_write10(ctx->hcd, ctx->dev_addr, &ctx->iface,
                            (uint32_t)lba, count, ctx->sector_size, buf);
    return (r == 0) ? FS_OK : FS_IO_ERROR;
}

/* Выполнить одну BOT-транзакцию.
   cbw_flags: bit 7 = 1 (data IN), 0 (data OUT).
   Возвращает длину данных, реально переданных в data phase (0 если нет data),
   или отрицательный код при ошибке. */
int usb_msc_bot_transfer(usb_hcd_t *hcd, uint8_t dev_addr,
                                usb_msc_iface_t *iface,
                                const uint8_t *cb, uint8_t cb_len,
                                uint8_t *data, uint32_t data_len,
                                int data_in){
    api_ptr->load_screen_add_string("in usb_msc_bot_transfer:");
    // Фикс релокации: берем адрес локального массива напрямую, минуя указатели!
    usb_msc_ctx_t *ctx = &g_msc_ctx[0];


        // Берем буферы напрямую из нулевого элемента массива, полностью исключая расчеты указателя ctx
    uint8_t *cbw_raw = (uint8_t *)g_msc_ctx[0].static_cbw_buf;
    uint8_t *csw_raw = (uint8_t *)g_msc_ctx[0].static_csw_buf;


    // СТРОГОЕ ПОБАЙТОВОЕ ЗАПОЛНЕНИЕ CBW (Размер по стандарту USB BOT = ровно 31 байт)
    memset(cbw_raw, 0, 32);

    // dCBWSignature = 0x43425355 ('USBC')
    cbw_raw[0] = 0x55; cbw_raw[1] = 0x53; cbw_raw[2] = 0x42; cbw_raw[3] = 0x43;

    // dCBWTag
    uint32_t tag = msc_tag++;
    cbw_raw[4] = (uint8_t)(tag & 0xFF);         cbw_raw[5] = (uint8_t)((tag >> 8) & 0xFF);
    cbw_raw[6] = (uint8_t)((tag >> 16) & 0xFF); cbw_raw[7] = (uint8_t)((tag >> 24) & 0xFF);

    // dCBWDataTransferLength
    cbw_raw[8] = (uint8_t)(data_len & 0xFF);         cbw_raw[9] = (uint8_t)((data_len >> 8) & 0xFF);
    cbw_raw[10] = (uint8_t)((data_len >> 16) & 0xFF); cbw_raw[11] = (uint8_t)((data_len >> 24) & 0xFF);

    // bmCBWFlags, bCBWLUN, bCBWCBLength
    cbw_raw[12] = data_in ? 0x80 : 0x00;
    cbw_raw[13] = 0; // LUN
    cbw_raw[14] = cb_len;

    // CBWCB — SCSI-команда копируется строго с 15-го байта, хвост гарантированно занулен!
    for (int i = 0; i < cb_len && i < 16; i++) {
        cbw_raw[15 + i] = cb[i];
    }

    uint8_t cbw_toggle = iface->bulk_out_toggle;
    api_ptr->load_screen_add_string("CBW Mem: Sig=%x%x%x%x Len=%d Cmd=%x",
                                    cbw_raw[0], cbw_raw[1], cbw_raw[2], cbw_raw[3],
                                    cbw_raw[14], cbw_raw[15]);

    // Передаем строго 31 байт CBW пакета
    int r = usb_hcd_bulk_transfer(hcd, dev_addr, iface->bulk_out_addr,
                               iface->bulk_out_max_pkt, &cbw_toggle,
                               cbw_raw, 31, 0);
    if (r != 31) return -10;
    iface->bulk_out_toggle = cbw_toggle;

    // Фаза передачи данных
    int data_result = 0;
    if (data_len > 0 && data){
        uint8_t dtoggle = data_in ? iface->bulk_in_toggle : iface->bulk_out_toggle;
        r = usb_hcd_bulk_transfer(hcd, dev_addr,
                               data_in ? iface->bulk_in_addr : iface->bulk_out_addr,
                               data_in ? iface->bulk_in_max_pkt : iface->bulk_out_max_pkt,
                               &dtoggle, data, data_len, data_in);
        if (r < 0) return -11;
        if (data_in) iface->bulk_in_toggle  = dtoggle;
        else         iface->bulk_out_toggle = dtoggle;
        data_result = r;
    }

    // Чтение статуса CSW (Строго 13 байт)
    memset(csw_raw, 0, 16);
    uint8_t csw_toggle = iface->bulk_in_toggle;

    r = usb_hcd_bulk_transfer(hcd, dev_addr, iface->bulk_in_addr,
                           iface->bulk_in_max_pkt, &csw_toggle,
                           csw_raw, 13, 1);
    if (r != 13) return -12;
    iface->bulk_in_toggle = csw_toggle;

    // Проверка полей CSW по фиксированным смещениям стандарта USB BOT
    // dCSWSignature = 0x53425355 ('USBS')
    if (csw_raw[0] != 0x55 || csw_raw[1] != 0x53 || csw_raw[2] != 0x42 || csw_raw[3] != 0x43) {
        // Побайтовый дамп первых 12 байт того, что реально записал контроллер UHCI в ОЗУ
        api_ptr->load_screen_add_string("CSW Real Bytes: %x %x %x %x | %x %x %x %x",
                                        csw_raw[0], csw_raw[1], csw_raw[2], csw_raw[3],
                                        csw_raw[4], csw_raw[5], csw_raw[6], csw_raw[7]);
        return -13;
    }


    // dCSWTag
    uint32_t csw_tag = ((uint32_t)csw_raw[7] << 24) | ((uint32_t)csw_raw[6] << 16) |
                       ((uint32_t)csw_raw[5] <<  8) |  (uint32_t)csw_raw[4];
    if (csw_tag != tag) return -14;

    // bCSWStatus
    if (csw_raw[12] != 0) return -15;

    return data_result;
}




int usb_msc_inquiry(usb_hcd_t *hcd, uint8_t dev_addr,
                           usb_msc_iface_t *iface){
    uint8_t cb[6] = { 0x12, 0x00, 0x00, 0x00, 36, 0x00 };
    uint8_t *cb_ptr = cb;

    // Берем буфер из кучи контекста устройства, защищенный от кэша процессора
    usb_msc_ctx_t *ctx = &g_msc_ctx[0];
    memset(ctx->static_data_buf, 0, 64);

    int r = usb_msc_bot_transfer(hcd, dev_addr, iface, cb_ptr, 6, ctx->static_data_buf, 36, 1);
    return (r > 0) ? 0 : -1;
}

int usb_msc_read_capacity(usb_hcd_t *hcd, uint8_t dev_addr,
                                 usb_msc_iface_t *iface,
                                 uint32_t *out_sectors, uint32_t *out_size){
    uint8_t cb[10] = { 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t *cb_ptr = cb;

    usb_msc_ctx_t *ctx = &g_msc_ctx[0];
    memset(ctx->static_data_buf, 0, 64);

    int r = usb_msc_bot_transfer(hcd, dev_addr, iface, cb_ptr, 10, ctx->static_data_buf, 8, 1);
    if (r != 8) return -1;

    volatile uint8_t *buf = (volatile uint8_t *)ctx->static_data_buf;
    uint32_t last_lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                        ((uint32_t)buf[2] <<  8) |  (uint32_t)buf[3];
    uint32_t blk_size = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                        ((uint32_t)buf[6] <<  8) |  (uint32_t)buf[7];

    *out_sectors = last_lba + 1;
    *out_size    = blk_size;
    return 0;
}


int usb_msc_init(usb_hcd_t *hcd, uint8_t dev_addr,
                 usb_msc_iface_t *iface,
                 uint32_t *out_sector_count, uint32_t *out_sector_size){

    if (!hcd || !iface || !out_sector_count || !out_sector_size) return -1;

    /* Toggle для bulk endpoints после SET_CONFIGURATION сбрасывается в DATA0 */
    iface->bulk_in_toggle  = 0;
    iface->bulk_out_toggle = 0;

    if (usb_msc_inquiry(hcd, dev_addr, iface) != 0) return -2;

    if (usb_msc_read_capacity(hcd, dev_addr, iface,
                              out_sector_count, out_sector_size) != 0)
        return -3;

    return 0;
}


/* Читает `sectors` секторов, начиная с LBA `lba`.
   Внутри режет на CBW-батчи, которые влезают в MAX_BULK_TDS*max_pkt. */
int usb_msc_read10(usb_hcd_t *hcd, uint8_t dev_addr, usb_msc_iface_t *iface,
                   uint32_t lba, uint32_t sectors, uint32_t sector_size,
                   void *buf){
    if (!hcd || !iface || !buf || sectors == 0 || sector_size == 0) return -1;

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
        int r = usb_msc_bot_transfer(hcd, dev_addr, iface, cb, 10,
                                     ptr + done * sector_size, data_len, 1);
        if (r != (int)data_len) return -2;
        done += batch;
    }
    return 0;
}



int usb_msc_write10(usb_hcd_t *hcd, uint8_t dev_addr, usb_msc_iface_t *iface,
                    uint32_t lba, uint32_t sectors, uint32_t sector_size,
                    const void *buf){
    if (!hcd || !iface || !buf || sectors == 0 || sector_size == 0) return -1;

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
        int r = usb_msc_bot_transfer(hcd, dev_addr, iface, cb, 10,
                                     (uint8_t *)ptr + done * sector_size,
                                     data_len, 0);
        if (r != (int)data_len) return -2;
        done += batch;
    }
    return 0;
}

int usb_msc_bot_reset(usb_hcd_t *hcd, uint8_t addr)
{
    uint8_t setup[8] = {
        0x21,           /* host-to-device, class, interface */
        0xFF,           /* Bulk-Only Mass Storage Reset */
        0x00, 0x00,
        0x00, 0x00,     /* interface 0 */
        0x00, 0x00
    };
    return usb_hcd_control_transfer(hcd, addr, setup, NULL, 0, 0, 8);
}



void umsc_register_ptrs(void){
    g_msc_ctx_ptr = g_msc_ctx;
    //msc_log_ptr = (void (*)(const char *, ...))func_addr;
}
