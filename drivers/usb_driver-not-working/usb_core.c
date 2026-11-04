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

#include "include/usb_core.h"
#include "include/usb_driver.h"
#include "include/usb_hcd.h"
#include "include/usb_msc.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"
#include <stdint.h>
#include <stddef.h>


int usb_get_descriptor(usb_hcd_t *hcd, uint8_t addr,
                              uint8_t desc_type, uint8_t desc_index,
                              void *buf, uint16_t want_len,
                              uint16_t max_pkt){
    uint8_t setup[8] = {
        0x80,                    /* device-to-host, standard, device */
        USB_REQ_GET_DESCRIPTOR,
        desc_index,              /* wValue low  */
        desc_type,               /* wValue high */
        0x00, 0x00,
        (uint8_t)(want_len & 0xFF),
        (uint8_t)(want_len >> 8)
    };
    int r = usb_hcd_control_transfer(hcd, addr, setup, buf, want_len, 1, max_pkt);
    if (r != (int)want_len){
        // Сохраняем сырой код возврата: -1 virt→phys, -2 TD error, -3 timeout,
        // или positive при коротком transfer (device вернул меньше)
        api_ptr->debug7 = (uint32_t)r;
    }
    return (r == (int)want_len) ? 0 : -1;
}

int usb_get_device_desc(usb_hcd_t *hcd, uint8_t addr,
                        usb_device_desc_t *desc, uint16_t want_len,
                        uint16_t max_pkt){
    return usb_get_descriptor(hcd, addr, USB_DESC_DEVICE, 0,
                              desc, want_len, max_pkt);
}

int usb_set_address(usb_hcd_t *hcd, uint8_t new_addr){
    uint8_t setup[8] = {
        0x00,                    /* host-to-device, std, device */
        USB_REQ_SET_ADDRESS,
        new_addr, 0x00,          /* wValue = адрес */
        0x00, 0x00,              /* wIndex */
        0x00, 0x00               /* wLength = 0 */
    };
    return usb_hcd_control_transfer(hcd, 0, setup, NULL, 0, 0, 8);
}

int usb_get_config_desc(usb_hcd_t *hcd, uint8_t addr, void *buf,
                        uint16_t want_len, uint16_t max_pkt){
    /* Step 1: 9-байтовая шапка конфига — узнаём wTotalLength */
    usb_config_desc_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (usb_get_descriptor(hcd, addr, USB_DESC_CONFIG, 0,
                           &hdr, 9, max_pkt) != 0){
        return -1;                     /* header fetch failed */
    }

    uint16_t total = hdr.wTotalLength;
    if (total < 9 || total > want_len){
        return -2;                     /* wTotalLength не влезает в buf */
    }

    /* Step 2: полное тело конфига — ровно total байт */
    if (usb_get_descriptor(hcd, addr, USB_DESC_CONFIG, 0,
                           buf, total, max_pkt) != 0){
        return -3;                     /* body fetch failed */
    }
    return 0;
}

int usb_set_configuration(usb_hcd_t *hcd, uint8_t addr, uint8_t cfg_val){
    uint8_t setup[8] = {
        0x00,                    /* host-to-device, standard, device */
        USB_REQ_SET_CONFIG,
        cfg_val, 0x00,
        0x00, 0x00,
        0x00, 0x00
    };
    return usb_hcd_control_transfer(hcd, addr, setup, NULL, 0, 0, 8);
}


int usb_parse_mass_storage(const void *config_buf, uint16_t total_len,
                           usb_msc_iface_t *out){
    if (!config_buf || !out || total_len < 9) return -1;
    memset(out, 0, sizeof(*out));

    uint16_t offset = 0;
    int in_msc_iface = 0;
    int found_iface  = 0;

    while (offset + 2 <= total_len){
        const uint8_t *p = (const uint8_t *)config_buf + offset;
        uint8_t blen = p[0];
        uint8_t btype = p[1];
        if (blen < 2 || offset + blen > total_len) break;

        if (btype == USB_DESC_INTERFACE && blen >= 9){
            const usb_interface_desc_t *iface = (const usb_interface_desc_t*)p;
            if (iface->bInterfaceClass    == USB_CLASS_MASS_STORAGE &&
                iface->bInterfaceSubClass == USB_SUBCLASS_SCSI &&
                iface->bInterfaceProtocol == USB_PROTOCOL_BOT){
                out->interface_num = iface->bInterfaceNumber;
                in_msc_iface = 1;
                found_iface  = 1;
            } else {
                in_msc_iface = 0;
            }
        } else if (btype == USB_DESC_ENDPOINT && blen >= 7 && in_msc_iface){
            const usb_endpoint_desc_t *ep = (const usb_endpoint_desc_t*)p;
            if ((ep->bmAttributes & 0x03) == USB_EP_ATTR_BULK){
                if (ep->bEndpointAddress & 0x80){
                    out->bulk_in_addr    = ep->bEndpointAddress;
                    out->bulk_in_max_pkt = ep->wMaxPacketSize & 0x7FF;
                } else {
                    out->bulk_out_addr    = ep->bEndpointAddress;
                    out->bulk_out_max_pkt = ep->wMaxPacketSize & 0x7FF;
                }
            }
        }
        offset += blen;
    }

    if (!found_iface)                    return -2;
    if (!out->bulk_in_addr || !out->bulk_out_addr) return -3;
    return 0;
}



int usb_probe_device_at_addr(usb_hcd_t *hcd, int port, uint8_t addr,
                             usb_device_desc_t *out_desc)
{
    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 1;

    static uint8_t buf8[8]   __attribute__((aligned(16)));
    static uint8_t buf18[18] __attribute__((aligned(16)));

    for (int retry = 0; retry < 3; retry++) {
        // ClearFeature(ENDPOINT_HALT) — снять возможный STALL с ep0
        //usb_clear_ep_halt(hcd, addr, 0);
        //for (int i = 0; i < 5; i++) usb_hcd_delay_2ms(hcd);    // 10 ms

        memset(buf8, 0, 8);
        int r1 = usb_get_device_desc(hcd, addr, (void *)buf8, 8, 8);
        if (r1 != 0 || buf8[0] == 0) continue;
        uint8_t mps = buf8[7];
        if (mps != 8 && mps != 16 && mps != 32 && mps != 64) mps = 8;

        memset(buf18, 0, 18);
        int r2 = usb_get_device_desc(hcd, addr, (void *)buf18, 18, mps);
        if (r2 != 0) continue;

        usb_device_desc_t *d = (usb_device_desc_t *)buf18;
        if (d->bLength != 18 || d->bDescriptorType != USB_DESC_DEVICE) continue;

        api_ptr->load_screen_add_string("probe(%d) OK vid=%x pid=%x",
                                        addr, d->idVendor, d->idProduct);
        *out_desc = *d;
        if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;
        return 0;
    }

    api_ptr->load_screen_add_string("probe(%d) FAIL", addr);
    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;
    return -1;
}



int usb_enumerate(usb_hcd_t *hcd, int port, uint8_t new_addr,
                  usb_device_desc_t *out_desc, uint8_t *out_addr){

    // Сбрасываем диагностику, чтобы значения от предыдущего порта не путали
    api_ptr->debug7 = 0;
    api_ptr->debug8 = 0;
    api_ptr->debug9 = 0;

    // Reset уже выполнен вызывающим (в device_usb_start) — здесь не дублируем.
    // Только для случая, когда usb_enumerate вызывается из hub_on_device_cb
    // (port < 0), reset тоже не нужен — его сделал hub.
    (void)port;

    static uint8_t local_enum_dma_buf[32] __attribute__((aligned(16)));
    uint8_t *dma_buffer = (uint8_t *)local_enum_dma_buf;
    memset(dma_buffer, 0, 32);

    // Снимаем возможный Halt с ep0 (BIOS мог оставить endpoint в STALL)
    //usb_clear_ep_halt(hcd, 0, 0);

    // --- 1) GET_DESCRIPTOR(8) на адресе 0 ---
    /* Ретраи: сразу после port reset часть флешек NAK'ует ep0 ещё несколько
       сотен миллисекунд (внутренняя инициализация контроллера памяти).
       Linux в hub_port_init делает до 10 попыток GET_DESCRIPTOR с паузами. */
    int gd_r = -1;
    for (int attempt = 0; attempt < 5 && gd_r != 0; attempt++) {
        gd_r = usb_get_device_desc(hcd, 0, (void *)dma_buffer, 8, 8);
        if (gd_r != 0) {
            api_ptr->load_screen_add_string("GET_DESC(8) retry %d r=%d", attempt + 1, gd_r);
            for (int i = 0; i < 25; i++) usb_hcd_delay_2ms(hcd);   // ~50 мс
        }
    }

    /* A118: РАДИКАЛЬНОЕ ВОССТАНОВЛЕНИЕ. Серия из 5 GET_DESCRIPTOR провалилась
       со статусом ISP|ISC|CRC+BITSTUFF при C_ERR=0 — это сигнатура «HC сам
       сдался после лимита попыток», а не «устройство молчит». На обеих машинах
       она НЕ меняется от программных ретраев, потому что состояние ep0/шины
       сбрасывается только USB-reset'ом. Флешка на CF-18 до загрузки ОС
       обслуживалась BIOS'ом в legacy-режиме и могла остаться с несогласованным
       тогглом ep0; в QEMU аналогично — устройство подключено к виртуальному
       хабу уже «живым». Делаем ровно то, что hub_port_init() в Linux после
       исчерпания попыток: GCR-сброс шины + re-enable + port reset, и ОДИН
       финальный GET_DESCRIPTOR. */
    if (gd_r != 0 && hcd && hcd->type == HCD_UHCI && port >= 0) {
        int rr = uhci_hard_recovery((uhci_hc_t *)hcd->priv, port);
        api_ptr->load_screen_add_string("HARD REC r=%d", rr);
        if (rr == 0) {
            memset(dma_buffer, 0, 32);
            gd_r = usb_get_device_desc(hcd, 0, (void *)dma_buffer, 8, 8);
            api_ptr->load_screen_add_string("GET_DESC(8) after rec r=%d", gd_r);
        }
    }

    if (gd_r != 0) {
        /* КОМПАКТНЫЙ ФИНАЛЬНЫЙ ЛОГ (A118): раньше печатались debug4/5 (оба
           PORTSC — дубль) и setup/data/stat (TD0==TD_status — тоже дубль из-за
           чтения TD0 до обработки). Теперь одна строка = полный дамп цепочки:
           st(TD0)/st(DATA-TD)/st(STATUS-TD) + спины + токен TD0 + PORTSC.
           По ней окончательно различаем: NAK-шторм vs шум на wire vs HC стоит. */
        api_ptr->debug4 = usb_hcd_port_status(hcd, port);
        api_ptr->load_screen_add_string("GD8 FAIL r=%d td0=%x tdd=%x tds=%x",
                                        (int)api_ptr->debug7,
                                        api_ptr->debug1, api_ptr->debug2, api_ptr->debug3);
        return -12;
    }

    uint16_t max_pkt = dma_buffer[7];
    api_ptr->load_screen_add_string("Device EP0 MaxPkt=%d", max_pkt);
    if (max_pkt != 8 && max_pkt != 16 && max_pkt != 32 && max_pkt != 64) {
        max_pkt = 8;
    }

    for (int i = 0; i < 10; i++) usb_hcd_delay_2ms(hcd);

    // --- 2) SET_ADDRESS(new_addr) ---
    uint8_t setup[8] = {
        0x00, USB_REQ_SET_ADDRESS,
        new_addr, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    int sa_r = usb_hcd_control_transfer(hcd, 0, setup, NULL, 0, 0, max_pkt);
    if (sa_r != 0) {
        // Clear halt и retry (может, SETUP попал на halted endpoint)
        usb_clear_ep_halt(hcd, 0, 0);
        sa_r = usb_hcd_control_transfer(hcd, 0, setup, NULL, 0, 0, max_pkt);
    }
    api_ptr->debug7 = (uint32_t)sa_r;
    api_ptr->load_screen_add_string("SET_ADDR(%d) r=%d dbg8=%x dbg9=%x",
                                    new_addr, sa_r,
                                    api_ptr->debug8, api_ptr->debug9);

    for (int i = 0; i < 15; i++) usb_hcd_delay_2ms(hcd);

    // --- 3) GET_DESCRIPTOR(18) уже по новому адресу ---
    usb_device_desc_t full;
    memset(&full, 0, sizeof(full));
    if (usb_get_device_desc(hcd, new_addr, &full, 18, max_pkt) != 0) {
        api_ptr->load_screen_add_string("GET_DESC(18) r=%d dbg8=%x dbg9=%x",
                                        (int)api_ptr->debug7,
                                        api_ptr->debug8, api_ptr->debug9);
        return -14;
    }

    api_ptr->load_screen_add_string("DEV OK: VID=%x PID=%x class=%x",
                                    full.idVendor, full.idProduct,
                                    full.bDeviceClass);

    *out_desc = full;
    *out_addr = new_addr;
    return 0;
}

/* Читает STRING descriptor с устройства и декодирует UTF-16LE → ASCII.
   Не-ASCII codepoints заменяются на '?'. Возвращает длину строки
   (без NUL) или -1. */
int usb_get_string_desc(usb_hcd_t *hcd, uint8_t addr, uint8_t index,
                        uint16_t lang_id, char *out, uint16_t max_len,
                        uint16_t max_pkt){
    if (!hcd || !out || max_len < 2 || index == 0) return -1;

    /* Шаг 1: 2 байта шапки — bLength + bDescriptorType */
    uint8_t hdr[2];
    uint8_t setup[8] = {
        0x80,                         /* device-to-host, standard, device */
        USB_REQ_GET_DESCRIPTOR,
        index,                        /* wValue low  = string index */
        USB_DESC_STRING,              /* wValue high = string type  */
        (uint8_t)(lang_id & 0xFF),
        (uint8_t)(lang_id >> 8),
        2, 0                          /* wLength = 2 */
    };
    int r = usb_hcd_control_transfer(hcd, addr, setup, hdr, 2, 1, max_pkt);
    if (r != 2) return -1;

    uint8_t bLength = hdr[0];
    if (hdr[1] != USB_DESC_STRING || bLength < 2) return -1;

    /* Шаг 2: полная строка UTF-16LE */
    uint8_t buf[128];
    if (bLength > sizeof(buf)) bLength = sizeof(buf);
    setup[6] = bLength;
    setup[7] = 0;
    r = usb_hcd_control_transfer(hcd, addr, setup, buf, bLength, 1, max_pkt);
    if (r != bLength) return -1;

    /* Декодируем: bLength/2 - 1 UTF-16 codepoints */
    uint16_t cp_count = (bLength - 2) / 2;
    uint16_t oi = 0;
    for (uint16_t i = 0; i < cp_count && oi < max_len - 1; i++){
        uint16_t cp = buf[2 + i*2] | ((uint16_t)buf[3 + i*2] << 8);
        if (cp >= 0x20 && cp < 0x7F){
            out[oi++] = (char)cp;
        } else if (cp == 0){
            break;
        } else {
            /* Не-ASCII: подставляем '?'. Полноценный UTF-8 можно
               добавить позже, но для lsusb ASCII достаточно. */
            out[oi++] = '?';
        }
    }
    out[oi] = '\0';
    return (int)oi;
}

int usb_clear_ep_halt(usb_hcd_t *hcd, uint8_t addr, uint8_t ep)
{
    uint8_t setup[8] = {
        0x02,           /* host-to-device, standard, endpoint */
        0x01,           /* CLEAR_FEATURE */
        0x00, 0x00,     /* wValue = ENDPOINT_HALT */
        ep, 0x00,       /* wIndex = endpoint */
        0x00, 0x00
    };
    return usb_hcd_control_transfer(hcd, addr, setup, NULL, 0, 0, 8);
}

/*
int usb_control_msg(usb_device_t *dev, uint8_t request_type, uint8_t request,
                    uint16_t value, uint16_t index, void *data, uint16_t len, int data_in)
{
    uint8_t setup[8];
    setup[0] = request_type;
    setup[1] = request;
    setup[2] = (uint8_t)(value & 0xFF);
    setup[3] = (uint8_t)(value >> 8);
    setup[4] = (uint8_t)(index & 0xFF);
    setup[5] = (uint8_t)(index >> 8);
    setup[6] = (uint8_t)(len & 0xFF);
    setup[7] = (uint8_t)(len >> 8);

    int ret = -1;
    for (int retry = 0; retry < 5; retry++)
    {
        ret = uhci_control_transfer((uhci_hc_t *)dev->hc_private, dev->address, setup,
                                    data, len, data_in, dev->ep0_max_pkt);
        if (ret >= 0)
            return ret;

        api_ptr->load_screen_add_string("CTRL(%x/%x) r=%d try %d", request, request_type, ret, retry + 1);

        if (retry < 4)
        {
            uhci_mdelay(20 + 30 * retry);
            if (retry >= 2 && dev->hc_private) {
                uhci_hc_t *hcx = (uhci_hc_t *)dev->hc_private;
                uhci_reset_port(hcx, dev->port_num);
                uhci_mdelay(20);
            }
        }
    }
    return ret;
}*/
