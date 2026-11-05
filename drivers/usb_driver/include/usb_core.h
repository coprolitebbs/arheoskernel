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

#ifndef USB_CORE_H
#define USB_CORE_H

#include <stdint.h>
#include "usb_uhci.h"

//bRequest
#define USB_REQ_GET_DESCRIPTOR   0x06
#define USB_REQ_SET_ADDRESS      0x05
#define USB_REQ_SET_CONFIG       0x09

//wValue high byte = descriptor type
#define USB_DESC_DEVICE          0x01
#define USB_DESC_CONFIG          0x02
#define USB_DESC_STRING          0x03
#define USB_DESC_INTERFACE       0x04
#define USB_DESC_ENDPOINT        0x05



#define USB_CLASS_MASS_STORAGE  0x08
#define USB_SUBCLASS_SCSI       0x06
#define USB_PROTOCOL_BOT        0x50

#define USB_EP_ATTR_BULK        0x02

typedef struct
{
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

typedef struct
{
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

typedef struct
{
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_interface_desc_t;

typedef struct
{
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

//Разобранный Mass Storage интерфейс
typedef struct
{
    uint8_t  interface_num;
    uint8_t  bulk_in_addr;
    uint8_t  bulk_out_addr;
    uint16_t bulk_in_max_pkt;
    uint16_t bulk_out_max_pkt;
    //Toggle хранится здесь — по одному на направление, сбрасывается в 0 после SET_CONFIGURATION
    uint8_t  bulk_in_toggle;
    uint8_t  bulk_out_toggle;
    uint16_t ep0_max_pkt;   //для control-transfer recovery (BOT reset)
} usb_msc_iface_t;


//Обобщённый хост-контроллер: UHCI или EHCI
//Позволяет usb_core/usb_msc работать с любым типом контроллера через общий API, не зная деталей
typedef enum { USB_HC_UHCI = 0, USB_HC_EHCI = 1 } usb_hc_type_t;

typedef struct
{
    usb_hc_type_t type;
    void *hc;   //uhci_hc_t* или ehci_hc_t*
} usb_hc_t;

//Путь к порту устройства: либо корневой порт HC, либо порт вниз по хабу
//(один уровень вложенности — QEMU втыкает авто-хаб, когда устройств больше,
//чем корневых портов; реальный хаб — так же)
//Транзакции control/bulk до устройства за хабом идут прозрачно — хаб их
//повторяет; через хаб делаются только port-reset/status/speed
typedef struct
{
    usb_hc_t hc;          //контроллер (по значению)
    uint8_t  root_port;   //корневой порт: 0/1 для UHCI, 1..N для EHCI
    uint8_t  hub_addr;    //0 = устройство прямо на корневом порту
    uint8_t  hub_port;    //1-based порт хаба (при hub_addr != 0)
} usb_port_path_t;

//Хабы (class 09): probe (число портов) + операции над нижележащим портом
int     usb_hub_probe(usb_hc_t *hc, uint8_t hub_addr, uint8_t *nports_out);
int     usb_hub_port_connected(usb_hc_t *hc, uint8_t hub_addr, uint8_t port);
int     usb_hub_port_reset(usb_hc_t *hc, uint8_t hub_addr, uint8_t port,
                           uint8_t *speed_out);   // *speed: 0=FS 1=LS 2=HS

//Есть ли устройство на порту (корневом или за хабом)
int     usb_path_port_connected(usb_port_path_t *path);


//Обёртки: диспетчеризуют на uhci_* или ehci_* по полю type
int usb_hc_control_transfer(usb_hc_t *hc, uint8_t addr,
                            const uint8_t *setup,
                            void *buf, uint16_t len, int in, uint16_t maxpkt);
int usb_hc_bulk_transfer(usb_hc_t *hc, uint8_t addr, uint8_t ep,
                         uint16_t maxpkt, uint8_t *toggle,
                         void *buf, uint32_t len, int in);
int usb_hc_port_connected(usb_hc_t *hc, int port);
int usb_hc_port_reset(usb_hc_t *hc, int port);
uint32_t usb_hc_port_status(usb_hc_t *hc, int port);


//Прототипы
int usb_enumerate(usb_port_path_t *path, uint8_t want_addr,
                  usb_device_desc_t *out_desc, uint8_t *out_addr);

int usb_get_config_desc(usb_hc_t *hc, uint8_t addr, void *buf,
                        uint16_t want_len, uint16_t max_pkt);
int usb_set_configuration(usb_hc_t *hc, uint8_t addr, uint8_t cfg_val);
int usb_parse_mass_storage(const void *config_buf, uint16_t total_len,
                           usb_msc_iface_t *out);

void usb_scan_pci_controllers(void);
int ehci_route_ports_to_uhci(void);
#endif
