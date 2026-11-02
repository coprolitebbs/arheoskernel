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
 * usb_core.c - enumeration endpoint 0 + реестр устройств.
 *
 * Адресация: каждое устройство живёт на паре (hc_idx, port) и получает
 * уникальный dev_addr 1..USB_ADDR_MAX. Перед SET_ADDRESS порт переживает
 * hard reset (PED=0): выключенный порт не участвует в расписании HC,
 * устройство гарантированно отвечает только на адрес 0, а TD уже
 * enumerated устройств (адреса 1..N) его не трогают.
 */

#include "include/usb_core.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

usb_device_t usb_devices[USB_REGISTRY_MAX];
int          usb_device_count = 0;

uhci_hc_t *usb_get_hc(int hc_idx){
    if (hc_idx < 0 || hc_idx >= uhci_hc_count) return NULL;
    return uhci_hc_ptrs[hc_idx];
}

uint8_t usb_alloc_dev_addr(void){
    for (uint8_t a = 1; a <= USB_ADDR_MAX; a++){
        int used = 0;
        for (int i = 0; i < usb_device_count; i++){
            if (usb_devices[i].dev_addr == a){ used = 1; break; }
        }
        if (!used) return a;
    }
    return 0;   /* исчерпано */
}

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


int usb_enumerate(uhci_hc_t *hc, int port, uint8_t addr,
                  usb_device_desc_t *out_desc){
    if (!hc || !out_desc) return -1;

    /* 1) Hard reset: устройство отвечает только на адрес 0, порт выключен.
          Это "сброс адреса перед enumeration" — без него оставшийся с
          прошлого захода адрес уводил бы GET_DESCRIPTOR на несуществующий
          адрес, а TD других устройств перехватывали бы наш трафик на 0. */
    uhci_port_reset_hard(hc, port);

    /* 2) Первые 8 байт Device Descriptor (bMaxPacketSize0 неизвестен) */
    usb_device_desc_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (usb_get_device_desc(hc, 0, &tmp, 8, 8) != 0) return -12;

    uint16_t max_pkt = tmp.bMaxPacketSize0;
    if (max_pkt != 8 && max_pkt != 16 && max_pkt != 32 && max_pkt != 64){
        max_pkt = 8;
    }

    /* 3) SET_ADDRESS(addr). some devices need ≥2ms pause after set address */
    if (usb_set_address(hc, addr) != 0) return -13;
    for (volatile int i = 0; i < 20000; i++) inw(hc->io_base);

    /* 4) Включаем порт (soft reset c PED=1) — без PED HC не будет
          слать токены на адрес устройства в следующих транзакциях. */
    if (uhci_reset_port(hc, port) != 0) return -15;

    /* 5) Полный Device Descriptor (18 байт) с новым адресом */
    if (usb_get_device_desc(hc, addr, out_desc, 18, max_pkt) != 0) return -14;

    return 0;
}

int usb_enumerate_device(usb_device_t *dev, uint8_t addr){
    if (!dev) return -1;

    uhci_hc_t *hc = usb_get_hc(dev->hc_idx);
    if (!hc) return -2;

    int r = usb_enumerate(hc, dev->port, addr, &dev->desc);
    if (r != 0) return r;

    dev->dev_addr = addr;
    dev->ep0_max_pkt = dev->desc.bMaxPacketSize0;
    if (dev->ep0_max_pkt != 8 && dev->ep0_max_pkt != 16 &&
        dev->ep0_max_pkt != 32 && dev->ep0_max_pkt != 64){
        dev->ep0_max_pkt = 8;
    }
    return 0;
}
