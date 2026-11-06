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
#include "include/usb_driver.h"
#include "include/usb_uhci.h"
#include "include/usb_ehci.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"
#include "../include_drivers/drv_format.h"



//Диспетчеры на uhci_* / ehci_* по типу контроллера
int usb_hc_control_transfer(usb_hc_t *hc, uint8_t addr,
                            const uint8_t *setup,
                            void *buf, uint16_t len, int in, uint16_t maxpkt)
{
    if (!hc) return -1;
    if (hc->type == USB_HC_UHCI)
        return uhci_control_transfer((uhci_hc_t *)hc->hc, addr, setup, buf, len, in, maxpkt);
    if (hc->type == USB_HC_EHCI)
        return ehci_control_transfer((ehci_hc_t *)hc->hc, addr, setup, buf, len, in, maxpkt);
    return -1;
}

int usb_hc_bulk_transfer(usb_hc_t *hc, uint8_t addr, uint8_t ep,
                         uint16_t maxpkt, uint8_t *toggle,
                         void *buf, uint32_t len, int in)
{
    if (!hc) return -1;
    if (hc->type == USB_HC_UHCI)
        return uhci_bulk_transfer((uhci_hc_t *)hc->hc, addr, ep, maxpkt, toggle, buf, len, in);
    if (hc->type == USB_HC_EHCI)
        return ehci_bulk_transfer((ehci_hc_t *)hc->hc, addr, ep, maxpkt, toggle, buf, len, in);
    return -1;
}

int usb_hc_port_connected(usb_hc_t *hc, int port)
{
    if (!hc) return 0;
    if (hc->type == USB_HC_UHCI)
        return uhci_port_connected((uhci_hc_t *)hc->hc, port);
    if (hc->type == USB_HC_EHCI)
        return ehci_port_connected((ehci_hc_t *)hc->hc, port);
    return 0;
}

int usb_hc_port_reset(usb_hc_t *hc, int port)
{
    if (!hc) return -1;
    if (hc->type == USB_HC_UHCI)
        return uhci_reset_port((uhci_hc_t *)hc->hc, port);
    if (hc->type == USB_HC_EHCI)
        return ehci_port_reset((ehci_hc_t *)hc->hc, port);
    return -1;
}

uint32_t usb_hc_port_status(usb_hc_t *hc, int port)
{
    if (!hc) return 0;
    if (hc->type == USB_HC_UHCI)
        return (uint32_t)uhci_port_status((uhci_hc_t *)hc->hc, port);
    if (hc->type == USB_HC_EHCI)
        return ehci_port_status((ehci_hc_t *)hc->hc, port);
    return 0;
}

int usb_get_descriptor(usb_hc_t *hc, uint8_t addr,
                       uint8_t desc_type, uint8_t desc_index,
                       void *buf, uint16_t want_len,
                       uint16_t max_pkt)
{
    uint8_t setup[8] =
    {
        0x80,                    //device-to-host, standard, device
        USB_REQ_GET_DESCRIPTOR,
        desc_index,              //wValue low
        desc_type,               //wValue high
        0x00, 0x00,
        (uint8_t)(want_len & 0xFF),
        (uint8_t)(want_len >> 8)
    };
    int r = usb_hc_control_transfer(hc, addr, setup, buf, want_len, 1, max_pkt);
    return (r == (int)want_len) ? 0 : -1;
}

int usb_get_device_desc(usb_hc_t *hc, uint8_t addr,
                        usb_device_desc_t *desc, uint16_t want_len,
                        uint16_t max_pkt)
{
    return usb_get_descriptor(hc, addr, USB_DESC_DEVICE, 0,
                              desc, want_len, max_pkt);
}

static int usb_set_address(usb_hc_t *hc, uint8_t new_addr)
{
    uint8_t setup[8] =
    {
        0x00,                    //host-to-device, std, device
        USB_REQ_SET_ADDRESS,
        new_addr, 0x00,          //wValue = адрес
        0x00, 0x00,              //wIndex
        0x00, 0x00               //wLength = 0
    };
    return usb_hc_control_transfer(hc, 0, setup, NULL, 0, 0, 8);
}

int usb_get_config_desc(usb_hc_t *hc, uint8_t addr, void *buf,
                        uint16_t want_len, uint16_t max_pkt)
{
    //9-байтовая шапка конфига — узнаём wTotalLength
    usb_config_desc_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (usb_get_descriptor(hc, addr, USB_DESC_CONFIG, 0,
                           &hdr, 9, max_pkt) != 0)
    {
        return -1;                     //Получение заголовка не получилось
    }

    uint16_t total = hdr.wTotalLength;
    if (total < 9 || total > want_len)
    {
        return -2;                     //wTotalLength не влезает в buf
    }

    //Полное тело конфига — ровно total байт
    if (usb_get_descriptor(hc, addr, USB_DESC_CONFIG, 0,
                           buf, total, max_pkt) != 0)
    {
        return -3;                     //Получение тела не получилось
    }
    return 0;
}

int usb_set_configuration(usb_hc_t *hc, uint8_t addr, uint8_t cfg_val)
{
    uint8_t setup[8] =
    {
        0x00,                    //host-to-device, standard, device
        USB_REQ_SET_CONFIG,
        cfg_val, 0x00,
        0x00, 0x00,
        0x00, 0x00
    };
    return usb_hc_control_transfer(hc, addr, setup, NULL, 0, 0, 8);
}


//---------------- Хабы (USB class 09) ----------------
//Порт-ориентированные class-запросы (USB 2.0 spec §11.24):
//  GET_STATUS(port)  : bmRequestType=0xA3 wValue=0 wIndex=port -> 4 байта
//                      (wPortStatus, wPortChange)
//  SET_FEATURE       : bmRequestType=0x23, wValue = селектор фичи
//  CLEAR_FEATURE     : bmRequestType=0x23
//Селекторы фич порта (include/uapi/linux/usb/ch11.h):
//  PORT_RESET=4, PORT_POWER=8; C_PORT_CONNECTION=16(0x10), C_PORT_RESET=20(0x14)
//Биты wPortStatus: bit0 CONNECTION, bit1 ENABLE, bit4 RESET,
//                  bit8 POWER, bit9 LOW_SPEED, bit10 HIGH_SPEED
//Биты wPortChange: bit0 C_CONNECTION, bit4 C_RESET

#define HUB_REQ_GET_STATUS   0x00
#define HUB_REQ_CLEAR_FEAT   0x01
#define HUB_REQ_SET_FEAT     0x03

#define HUB_FEAT_PORT_RESET        4
#define HUB_FEAT_C_PORT_CONNECTION 16
#define HUB_FEAT_C_PORT_RESET      20

#define HUB_ST_CONNECTION 0x0001
#define HUB_ST_RESET      0x0010
#define HUB_ST_LOW_SPEED  0x0200
#define HUB_ST_HIGH_SPEED 0x0400
#define HUB_CH_C_RESET    0x0010

//Класс-запрос к порту/устройству хаба. Возвращает 0 при успехе.
static int hub_class_ctrl(usb_hc_t *hc, uint8_t hub_addr,
                          uint8_t bm_req, uint8_t b_req,
                          uint16_t w_value, uint16_t w_index,
                          void *buf, uint16_t len)
{
    uint8_t setup[8] =
    {
        bm_req,
        b_req,
        (uint8_t)(w_value & 0xFF), (uint8_t)(w_value >> 8),
        (uint8_t)(w_index & 0xFF), (uint8_t)(w_index >> 8),
        (uint8_t)(len & 0xFF),     (uint8_t)(len >> 8)
    };
    int in = (bm_req & 0x80) ? 1 : 0;
    int r = usb_hc_control_transfer(hc, hub_addr, setup, buf, len, in, 8);
    return (r == (int)len) ? 0 : -1;
}

//Прочитать Hub Descriptor (class 0x29), узнать число нижних портов
int usb_hub_probe(usb_hc_t *hc, uint8_t hub_addr, uint8_t *nports_out)
{
    if (!hc || !nports_out) return -1;
    uint8_t hd[8];
    memset(hd, 0, sizeof(hd));
    //GET_DESCRIPTOR: class, device-recipient; wValue = (0x29 << 8) | index
    if (hub_class_ctrl(hc, hub_addr, 0xA0, USB_REQ_GET_DESCRIPTOR,
                       0x2900u, 0, hd, 8) != 0) return -2;
    if (hd[1] != 0x29) return -3;          //не Hub Descriptor
    if (hd[2] == 0 || hd[2] > 15) return -4;
    *nports_out = hd[2];
    return 0;
}

int usb_hub_port_connected(usb_hc_t *hc, uint8_t hub_addr, uint8_t port)
{
    uint8_t st[4];
    memset(st, 0, sizeof(st));
    if (hub_class_ctrl(hc, hub_addr, 0xA3, HUB_REQ_GET_STATUS,
                       0, port, st, 4) != 0) return 0;
    uint16_t wst = (uint16_t)(st[0] | ((uint16_t)st[1] << 8));
    return (wst & HUB_ST_CONNECTION) ? 1 : 0;
}

//Сброс нижнего порта хаба + определение скорости. Возвращает 0 при успехе.
int usb_hub_port_reset(usb_hc_t *hc, uint8_t hub_addr, uint8_t port,
                       uint8_t *speed_out)
{
    uint8_t st[4];

    //SET_FEATURE(PORT_RESET)
    if (hub_class_ctrl(hc, hub_addr, 0x23, HUB_REQ_SET_FEAT,
                       HUB_FEAT_PORT_RESET, port, NULL, 0) != 0) return -1;

    //Ждём окончания сброса: снимется RESET в статусе или появится C_RESET.
    //Спека: до 20 мс; даём 30 тиков (300 мс) запаса.
    uint32_t t0 = (api_ptr && api_ptr->get_ticks) ? api_ptr->get_ticks() : 0;
    uint16_t wst = 0;
    for (;;){
        memset(st, 0, sizeof(st));
        if (hub_class_ctrl(hc, hub_addr, 0xA3, HUB_REQ_GET_STATUS,
                           0, port, st, 4) != 0) return -2;
        wst = (uint16_t)(st[0] | ((uint16_t)st[1] << 8));
        uint16_t chg = (uint16_t)(st[2] | ((uint16_t)st[3] << 8));
        if (!(wst & HUB_ST_RESET) || (chg & HUB_CH_C_RESET)) break;
        if (api_ptr && api_ptr->get_ticks &&
            (api_ptr->get_ticks() - t0) > 30) break;   //таймаут — выходим с тем, что есть
    }

    //Подчистить change-биты
    hub_class_ctrl(hc, hub_addr, 0x23, HUB_REQ_CLEAR_FEAT,
                   HUB_FEAT_C_PORT_RESET, port, NULL, 0);
    hub_class_ctrl(hc, hub_addr, 0x23, HUB_REQ_CLEAR_FEAT,
                   HUB_FEAT_C_PORT_CONNECTION, port, NULL, 0);

    //Финальный статус: подключение + скорость
    memset(st, 0, sizeof(st));
    if (hub_class_ctrl(hc, hub_addr, 0xA3, HUB_REQ_GET_STATUS,
                       0, port, st, 4) != 0) return -2;
    wst = (uint16_t)(st[0] | ((uint16_t)st[1] << 8));
    if (!(wst & HUB_ST_CONNECTION)) return -3;

    uint8_t speed = 0;                          //FS
    if (wst & HUB_ST_LOW_SPEED)  speed = 1;     //LS
    else if (wst & HUB_ST_HIGH_SPEED) speed = 2;//HS
    if (speed_out) *speed_out = speed;
    return 0;
}

//Есть ли устройство на порту (корневом или за хабом)
int usb_path_port_connected(usb_port_path_t *path)
{
    if (!path) return 0;
    if (path->hub_addr == 0)
        return usb_hc_port_connected(&path->hc, path->root_port);
    return usb_hub_port_connected(&path->hc, path->hub_addr, path->hub_port);
}


int usb_parse_mass_storage(const void *config_buf, uint16_t total_len,
                           usb_msc_iface_t *out)
{
    if (!config_buf || !out || total_len < 9) return -1;
    memset(out, 0, sizeof(*out));

    uint16_t offset = 0;
    int in_msc_iface = 0;
    int found_iface  = 0;

    while (offset + 2 <= total_len)
    {
        const uint8_t *p = (const uint8_t *)config_buf + offset;
        uint8_t blen = p[0];
        uint8_t btype = p[1];
        if (blen < 2 || offset + blen > total_len) break;

        if (btype == USB_DESC_INTERFACE && blen >= 9)
        {
            const usb_interface_desc_t *iface = (const usb_interface_desc_t*)p;
            if (iface->bInterfaceClass    == USB_CLASS_MASS_STORAGE &&
                    iface->bInterfaceSubClass == USB_SUBCLASS_SCSI &&
                    iface->bInterfaceProtocol == USB_PROTOCOL_BOT)
            {
                out->interface_num = iface->bInterfaceNumber;
                in_msc_iface = 1;
                found_iface  = 1;
            }
            else
            {
                in_msc_iface = 0;
            }
        }
        else if (btype == USB_DESC_ENDPOINT && blen >= 7 && in_msc_iface)
        {
            const usb_endpoint_desc_t *ep = (const usb_endpoint_desc_t*)p;
            if ((ep->bmAttributes & 0x03) == USB_EP_ATTR_BULK)
            {
                if (ep->bEndpointAddress & 0x80)
                {
                    out->bulk_in_addr    = ep->bEndpointAddress;
                    out->bulk_in_max_pkt = ep->wMaxPacketSize & 0x7FF;
                }
                else
                {
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


int usb_enumerate(usb_port_path_t *path, uint8_t want_addr,
                  usb_device_desc_t *out_desc, uint8_t *out_addr)
{

    if (!path) return -10;
    if (want_addr == 0 || want_addr > 127) return -11;

    //Сброс порта и определение скорости.
    //Корневой порт — через PORTSC HC; порт за хабом — через class-запросы хаба.
    int is_ls = 0;
    uint8_t speed = 0;   // 0=FS, 1=LS, 2=HS
    if (path->hub_addr == 0){
        int rp = usb_hc_port_reset(&path->hc, path->root_port);
        if (rp != 0) return rp;

        //UHCI: LSDA бит 8 (QEMU/ICH) или line-status bits 4-5 (D- высокий — LS)
        uint32_t ps = usb_hc_port_status(&path->hc, path->root_port);
        if (path->hc.type == USB_HC_UHCI){
            is_ls = ((ps & 0x0100u) || ((ps & 0x30) == 0x10)) ? 1 : 0;
            speed = is_ls ? 1 : 0;   // UHCI: FS или LS
        } else {
            //EHCI: скорость — ТОЛЬКО по биту Port Enable (PE, бит 2).
            //PE=1 <=> устройство high-speed (FS/LS EHCI отдаёт companion'у:
            //ehci_port_reset в этом случае вернул бы -3, и мы сюда не дошли).
            //Поля PSPD (bits 27:26) НЕ читать: QEMU их выставляет (=2 для HS),
            //а реальный кремний (ICH6/CF-18) держит PSPD=0 даже для включённого
            //HS-порта. Из-за чтения PSPD устройство классифицировалось как FS,
            //QH собирался с EPS=FS/C=1/mps=8 без split-транзакций, и каждый
            //SETUP получал XactErr (CERR 3->0, HALT) — enum r=-12 при живом
            //устройстве и рабочем контроллере (см. лог CF-18: ok=1105, pspd=0).
            //По EHCI spec и как в Linux (ehci-hub.c скорость нигде не читается):
            //включённый порт EHCI — всегда HS.
            speed = (ps & 0x4u) ? 2 : 0;
            is_ls = 0;
        }

        //Recovery после сброса порта (USB 2.0 §7.1.7.5: >=10 мс до первой
        //транзакции). QEMU прощает отсутствие паузы, реальное устройство
        //после chirp/reset ещё не готово отвечать — первый SETUP без паузы
        //ловит XactErr. 2 тика = 20 мс (1 тик = 10 мс), safety-net — на случай
        //если тики ещё не тикают.
        {
            uint32_t t0r = (api_ptr && api_ptr->get_ticks) ? api_ptr->get_ticks() : 0;
            uint32_t safety = 1000000;
            while (((api_ptr->get_ticks() - t0r) < 2) && (safety-- > 0)) {
                __asm__ __volatile__("pause");
            }
        }
    } else {
        int hr = usb_hub_port_reset(&path->hc, path->hub_addr, path->hub_port, &speed);
        if (hr != 0) return (hr == -3) ? -21 : -22;
        is_ls = (speed == 1) ? 1 : 0;
    }
    (void)is_ls;
    uhci_dev_is_ls[0]    = (uint8_t)is_ls;
    uhci_dev_is_ls[want_addr] = (uint8_t)is_ls;
    ehci_dev_speed[0]    = speed;
    ehci_dev_speed[want_addr] = speed;

    //Первые 8 байт Device Descriptor (bMaxPacketSize0 неизвестен)
    //Устройство ещё отвечает на адрес 0
    usb_device_desc_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (usb_get_device_desc(&path->hc, 0, &tmp, 8, 8) != 0) return -12;

    if (tmp.bDeviceClass == 0x09)
    {
        //return -20;  //root hub, не real device
    }

    uint16_t max_pkt = tmp.bMaxPacketSize0;
    if (max_pkt != 8 && max_pkt != 16 && max_pkt != 32 && max_pkt != 64)
    {
        max_pkt = 8;
    }

    //SET_ADDRESS(want_addr)
    if (usb_set_address(&path->hc, want_addr) != 0) return -13;

    //Пауза ≥ 2 мс после SET_ADDRESS. Используем тики (1 тик = 10 мс),
    //чтобы время было одинаковым на любой частоте CPU. Safety-net
    //по итерациям — на случай если тики ещё не тикают.
    {
        uint32_t t0 = (api_ptr && api_ptr->get_ticks) ? api_ptr->get_ticks() : 0;
        uint32_t safety = 1000000;
        while (((api_ptr->get_ticks() - t0) < 1) && (safety-- > 0)) {
            __asm__ __volatile__("pause");
        }
    }

    //Полный Device Descriptor (18 байт) с НОВЫМ адресом
    usb_device_desc_t full;
    memset(&full, 0, sizeof(full));
    if (usb_get_device_desc(&path->hc, want_addr, &full, 18, max_pkt) != 0) return -14;

    *out_desc = full;
    *out_addr = want_addr;
    return 0;
}


// Диагностика: найти все USB-контроллеры (UHCI/EHCI) и вывести их на экран.
// Ничего не инициализирует, только читает PCI-конфигурацию.
void usb_scan_pci_controllers(void)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                uint32_t class = pci_read32((uint8_t)bus, dev, func, 0x08);
                uint8_t base_cls = (class >> 24) & 0xFF;
                uint8_t sub_cls  = (class >> 16) & 0xFF;
                uint8_t prog_if  = (class >> 8) & 0xFF;

                if (base_cls == PCI_CLASS_SERIAL_BUS &&
                    sub_cls  == PCI_SUBCLASS_USB) {

                    const char *type = "UNKNOWN";
                    if (prog_if == PCI_PROGIF_UHCI) type = "UHCI";
                    else if (prog_if == 0x20)       type = "EHCI";

                    uint32_t bar0 = pci_read32((uint8_t)bus, dev, func, 0x10);
                    uint32_t bar4 = pci_read32((uint8_t)bus, dev, func, 0x20);

                    api_ptr->load_screen_add_string(
                        "USB %s: %x:%x.%x prog_if=%x BAR0=%x BAR4=%x",
                        type, bus, dev, func, prog_if, bar0, bar4);

                    //Диагностика портов UHCI: покажет, на каком именно
                    //UHCI-контроллере висит устройство. На ICH6 три UHCI,
                    //и флешка может быть на #2 или #3, а не на #1.
                    if (prog_if == PCI_PROGIF_UHCI && (bar4 & 0x01)) {
                        uint16_t io = bar4 & 0xFFFC;
                        uint16_t ps0 = inw(io + 0x10);   //PORTSC1
                        uint16_t ps1 = inw(io + 0x12);   //PORTSC2
                        api_ptr->load_screen_add_string(
                            "  UHCI %x:%x.%x ports: p0=%x p1=%x",
                            bus, dev, func, ps0, ps1);
                    }
                }
            }
        }
    }
}


// Путь 2: вернуть физические порты от EHCI к UHCI.
// Ищем EHCI (prog_if = 0x20), включаем MMIO, читаем CONFIGFLAG.
// Если CF=1 — записываем 0. Это глобально переводит все порты на companion-контроллеры (UHCI).
// EHCI при этом НЕ запускаем (USBCMD.RS=0), расписание не трогаем.
// Возвращает 0 при успехе (EHCI найден и CF сброшен, либо EHCI отсутствует),
// отрицательный код при ошибке доступа к MMIO.
int ehci_route_ports_to_uhci(void)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                uint32_t class = pci_read32((uint8_t)bus, dev, func, 0x08);
                uint8_t base_cls = (class >> 24) & 0xFF;
                uint8_t sub_cls  = (class >> 16) & 0xFF;
                uint8_t prog_if  = (class >> 8) & 0xFF;

                if (base_cls == PCI_CLASS_SERIAL_BUS &&
                    sub_cls  == PCI_SUBCLASS_USB &&
                    prog_if  == 0x20) { // EHCI

                    // Включаем Memory Space + Bus Master
                    uint32_t cmd = pci_read32((uint8_t)bus, dev, func, 0x04);
                    cmd |= 0x06;
                    pci_write32((uint8_t)bus, dev, func, 0x04, cmd);

                    uint32_t bar0 = pci_read32((uint8_t)bus, dev, func, 0x10);

                    // BAR0 должен быть memory, не I/O, и не "все единицы"
                    if (bar0 & 0x01) return -1;             // I/O BAR — не наш случай
                    if (bar0 == 0xFFFFFFFF) return -2;      // BAR не размечен BIOS
                    uint32_t mmio_base = bar0 & 0xFFFFFFF0; // маскируем младшие 4 бита
                    if (mmio_base == 0) return -3;

                    if (!api_ptr) return -4;
                    //Для MMIO используем некэшируемый маппинг (PCD+PWT).
                    //Обычный vmm_map_pages кеширует, что для EHCI-регистров
                    //приводит к чтению устаревших значений и зависаниям.
                    if (api_ptr->vmm_map_pages_uncached_ptr) {
                        api_ptr->vmm_map_pages_uncached_ptr(mmio_base, 0x1000);
                    } else if (api_ptr->vmm_map_pages_ptr) {
                        api_ptr->vmm_map_pages_ptr(mmio_base, 0x1000);
                    } else {
                        return -4;
                    }

                    // Читаем CAPLENGTH (первый байт capability-регистров).
                    // Operational base = MMIO base + CAPLENGTH.
                    uint8_t cap_length = *(volatile uint8_t*)(mmio_base + 0x00);

                    // Если BIOS не инициализировал EHCI, регистры могут читаться
                    // как 0xFF или 0x00 — это повод не доверять устройству.
                    if (cap_length == 0xFF || cap_length == 0x00) return -5;
                    if (cap_length > 0x7F) return -6;

                    uint32_t op_base = mmio_base + cap_length;

                    // CONFIGFLAG находится по смещению 0x40 от op_base
                    volatile uint32_t *config_flag = (volatile uint32_t*)(op_base + 0x40);
                    uint32_t cf = *config_flag;

                    if (cf & 0x01) {
                        // CF = 1, порты у EHCI. Сбрасываем — все порты -> UHCI.
                        *config_flag = 0;
                        // Спека: после сброса CF маршрутизация применяется
                        // не мгновенно. Ждём до 10 мс, чтобы изменения устоялись.
                        for (volatile int i = 0; i < 20000; i++) inw(0x80);
                    }

                    // Проверяем PortOwner (бит 13) в PORTSC каждого порта.
                    // Если после сброса CF у порта PortOwner=1 — он уже принадлежит
                    // companion-контроллеру (UHCI). Если 0 — EHCI не отдал его,
                    // и UHCI его не увидит.
                    // PORTSC портов: op_base + 0x44 + (port-1)*4 для port=1..N.
                    // Количество портов (N_PORTS) — в HCSPARAMS (offset 0x04 от MMIO base),
                    // биты 3..0.
                    uint32_t hcs_params = *(volatile uint32_t*)(mmio_base + 0x04);
                    uint32_t n_ports = hcs_params & 0x0F;
                    if (n_ports > 15) n_ports = 15; // sanity

                    int owned_by_uhci = 0;
                    int owned_by_ehci = 0;
                    for (uint32_t p = 1; p <= n_ports; p++) {
                        uint32_t portsc_addr = op_base + 0x44 + (p - 1) * 4;
                        uint32_t portsc = *(volatile uint32_t*)portsc_addr;
                        if (portsc & (1u << 13)) owned_by_uhci++;
                        else                     owned_by_ehci++;
                    }

                    // Диагностика: сколько портов реально отдано UHCI
                    if (api_ptr && api_ptr->load_screen_add_string) {
                        api_ptr->load_screen_add_string(
                            "EHCI route: n_ports=%d owned_by_uhci=%d owned_by_ehci=%d",
                            (int)n_ports, owned_by_uhci, owned_by_ehci);
                    }

                    return (owned_by_uhci > 0) ? 0 : -7;
                }
            }
        }
    }
    return 0; // EHCI не найден — ничего не делаем
}
