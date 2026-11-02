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


/*
 * usb_core.c - минимальный enumeration endpoint 0.
 * Этап 1: GET_DESCRIPTOR(Device) + SET_ADDRESS + ещё раз GET_DESCRIPTOR.
 */

#include "include/usb_core.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

int usb_get_descriptor(uhci_hc_t *hc, uint8_t addr,
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
    int r = uhci_control_transfer(hc, addr, setup, buf, want_len, 1, max_pkt);
    return (r == (int)want_len) ? 0 : -1;
}

int usb_get_device_desc(uhci_hc_t *hc, uint8_t addr,
                        usb_device_desc_t *desc, uint16_t want_len,
                        uint16_t max_pkt){
    return usb_get_descriptor(hc, addr, USB_DESC_DEVICE, 0,
                              desc, want_len, max_pkt);
}

static int usb_set_address(uhci_hc_t *hc, uint8_t new_addr){
    uint8_t setup[8] = {
        0x00,                    /* host-to-device, std, device */
        USB_REQ_SET_ADDRESS,
        new_addr, 0x00,          /* wValue = адрес */
        0x00, 0x00,              /* wIndex */
        0x00, 0x00               /* wLength = 0 */
    };
    return uhci_control_transfer(hc, 0, setup, NULL, 0, 0, 8);
}

int usb_get_config_desc(uhci_hc_t *hc, uint8_t addr, void *buf,
                        uint16_t want_len, uint16_t max_pkt){
    /* Step 1: 9-байтовая шапка конфига — узнаём wTotalLength */
    usb_config_desc_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (usb_get_descriptor(hc, addr, USB_DESC_CONFIG, 0,
                           &hdr, 9, max_pkt) != 0){
        return -1;                     /* header fetch failed */
    }

    uint16_t total = hdr.wTotalLength;
    if (total < 9 || total > want_len){
        return -2;                     /* wTotalLength не влезает в buf */
    }

    /* Step 2: полное тело конфига — ровно total байт */
    if (usb_get_descriptor(hc, addr, USB_DESC_CONFIG, 0,
                           buf, total, max_pkt) != 0){
        return -3;                     /* body fetch failed */
    }
    return 0;
}

int usb_set_configuration(uhci_hc_t *hc, uint8_t addr, uint8_t cfg_val){
    uint8_t setup[8] = {
        0x00,                    /* host-to-device, standard, device */
        USB_REQ_SET_CONFIG,
        cfg_val, 0x00,
        0x00, 0x00,
        0x00, 0x00
    };
    return uhci_control_transfer(hc, addr, setup, NULL, 0, 0, 8);
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


int usb_enumerate(uhci_hc_t *hc, int port,
                  usb_device_desc_t *out_desc, uint8_t *out_addr){

    /* 1) Сброс порта */
    int rp = uhci_reset_port(hc, port);
    if (rp != 0) return rp;

    /* 2) Первые 8 байт Device Descriptor (bMaxPacketSize0 неизвестен) */
    usb_device_desc_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (usb_get_device_desc(hc, 0, &tmp, 8, 8) != 0) return -12;

    uint16_t max_pkt = tmp.bMaxPacketSize0;
    if (max_pkt != 8 && max_pkt != 16 && max_pkt != 32 && max_pkt != 64){
        max_pkt = 8;
    }

    /* 3) SET_ADDRESS(1) */
    if (usb_set_address(hc, 1) != 0) return -13;

    /* После SET_ADDRESS устройство отвечает уже на адрес 1. USB spec
       требует паузу ≥ 2 ms — busy-wait через порт. */
    for (volatile int i = 0; i < 20000; i++) inw(hc->io_base);

    /* 4) Полный Device Descriptor (18 байт) с новым адресом */
    usb_device_desc_t full;
    memset(&full, 0, sizeof(full));
    if (usb_get_device_desc(hc, 1, &full, 18, max_pkt) != 0) return -14;

    *out_desc = full;
    *out_addr = 1;
    return 0;
}