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
 * usb_driver.c - точка входа ELF-драйвера USB.
 *
 * Многопортовый enumeration: контроллер UHCI опрашивает оба корневых
 * порта, каждому найденному устройству выдаётся уникальный USB-адрес,
 * все устройства попадают в devices[] (для lsusb), а Mass Storage
 * дополнительно регистрируется в block_driver через register_device.
 * HID-класс (тачпад/тачскрин на CF-18) и прочие классы просто
 * фиксируются в реестре и не инициализируются как MSC.
 */

#include "include/usb_driver.h"
#include "include/usb_uhci.h"
#include "include/usb_ehci.h"
#include "include/usb_core.h"
#include "include/usb_msc.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

#define USB_MAX_MSC_DEVS  4

usb_stack_driver_api_t api;
usb_stack_driver_api_t *api_ptr;
description_driver_api_t descr_api;

uhci_hc_t uhci0;
static usb_msc_dev_t g_msc[USB_MAX_MSC_DEVS];

static usb_msc_dev_t *usb_alloc_msc_slot(void)
{
    for (int i = 0; i < USB_MAX_MSC_DEVS; i++)
    {
        if (!g_msc[i].in_use)
        {
            memset(&g_msc[i], 0, sizeof(g_msc[i]));
            g_msc[i].in_use = 1;
            return &g_msc[i];
        }
    }
    return NULL;
}

static int usb_msc_read_wrap(void *priv, uint64_t lba, uint32_t count, void *buf)
{
    usb_msc_dev_t *d = (usb_msc_dev_t *)priv;
    if (!d || !d->hc) return FS_IO_ERROR;
    if (d->sector_size == 0 || d->sector_count == 0) return FS_IO_ERROR;
    if (lba + count > d->sector_count)                return FS_IO_ERROR;
    usb_hc_t *w = (usb_hc_t*)d->hc;
    int r = usb_msc_read10(w, d->dev_addr, &d->iface,
                           (uint32_t)lba, count, d->sector_size, buf);
    return (r == 0) ? FS_OK : FS_IO_ERROR;
}

static int usb_msc_write_wrap(void *priv, uint64_t lba, uint32_t count, const void *buf)
{
    usb_msc_dev_t *d = (usb_msc_dev_t *)priv;
    if (!d || !d->hc) return FS_IO_ERROR;
    if (d->sector_size == 0 || d->sector_count == 0) return FS_IO_ERROR;
    if (lba + count > d->sector_count)                return FS_IO_ERROR;
    usb_hc_t *w = (usb_hc_t*)d->hc;
    int r = usb_msc_write10(w, d->dev_addr, &d->iface,
                            (uint32_t)lba, count, d->sector_size, buf);
    return (r == 0) ? FS_OK : FS_IO_ERROR;
}

//Счётчик имён для block-layer: usb0, usb1, ... — уникальные на всю систему
static int g_msc_name_counter = 0;

//Записать устройство в реестр + инициализировать как MSC (если это mass
//storage) и зарегистрировать в блок-слое. Вызывается и для корневых портов,
//и для устройств за хабом.
static void usb_setup_device(usb_hc_t *hcw, int port_for_record,
                             usb_device_desc_t *desc, uint8_t dev_addr)
{
    if (api_ptr->device_count < USB_REGISTRY_MAX){
        usb_dev_record_t *rec = &api_ptr->devices[api_ptr->device_count++];
        memset(rec, 0, sizeof(*rec));
        rec->vid = desc->idVendor; rec->pid = desc->idProduct;
        rec->dev_class = desc->bDeviceClass;
        rec->dev_subclass = desc->bDeviceSubClass;
        rec->dev_protocol = desc->bDeviceProtocol;
        rec->port = (uint8_t)port_for_record;
        rec->block_id = 0xFFFFFFFFu;
    }

    uint16_t ep0 = desc->bMaxPacketSize0 ? desc->bMaxPacketSize0 : 8;
    uint8_t cfg_buf[256]; memset(cfg_buf, 0, sizeof(cfg_buf));
    if (usb_get_config_desc(hcw, dev_addr, cfg_buf,
                            sizeof(cfg_buf), ep0) != 0) return;

    usb_config_desc_t *cfg = (usb_config_desc_t*)cfg_buf;
    usb_msc_iface_t local_msc;
    if (usb_parse_mass_storage(cfg_buf, cfg->wTotalLength, &local_msc) != 0)
        return;

    usb_set_configuration(hcw, dev_addr, cfg->bConfigurationValue);

    usb_msc_dev_t *slot = usb_alloc_msc_slot();
    if (!slot) return;

    slot->dev_addr = dev_addr;
    slot->iface    = local_msc;
    slot->iface.ep0_max_pkt = ep0;
    // *** Копируем usb_hc_t в структуру слота, чтобы read/write_wrap
    //     потом имели валидный указатель. hcw часто на стеке — умрёт. ***
    slot->hc_wrap.type = hcw->type;
    slot->hc_wrap.hc   = hcw->hc;
    slot->hc           = &slot->hc_wrap;

    if (usb_msc_init(&slot->hc_wrap, dev_addr, &slot->iface,
                     &slot->sector_count, &slot->sector_size) == 0
        && api_ptr->block && api_ptr->block->register_device){
        char name[8] = "usb0";
        name[3] = (char)('0' + (g_msc_name_counter % 10));
        int bid = api_ptr->block->register_device(
            name, slot->sector_count, BLK_REMOVABLE,
            usb_msc_read_wrap, usb_msc_write_wrap, (void*)slot);
        //api_ptr->load_screen_add_string("USB msc addr=%d reg %s -> %d",dev_addr, name, bid);
        if (bid >= 0){
            slot->block_id = (uint32_t)bid;
            if (api_ptr->device_count > 0)
                api_ptr->devices[api_ptr->device_count-1].block_id = (uint32_t)bid;
        }
        g_msc_name_counter++;
    }
}

//Перечислить устройство на заданном пути (корневой порт или порт за хабом),
//оформить его; если это хаб (class 09) — рекурсивно обойти его нижние порты.
//Глубина ограничена: depth >= 5 — хабы больше не разворачиваем.
static void usb_enum_device_at(usb_port_path_t *path, const char *tag, int depth)
{
    usb_device_desc_t desc; uint8_t dev_addr = 0;
    memset(&desc, 0, sizeof(desc));
    int r = usb_enumerate(path, 1 + api_ptr->device_count, &desc, &dev_addr);

    /*api_ptr->load_screen_add_string(
        "%s rp%d hub%d.%d enum r=%d vid=%x pid=%x cls=%x mps=%d",
        tag, path->root_port, path->hub_addr, path->hub_port,
        r, desc.idVendor, desc.idProduct, desc.bDeviceClass,
        desc.bMaxPacketSize0);*/

    if (r != 0) return;

    usb_setup_device(&path->hc, path->root_port, &desc, dev_addr);

    if (desc.bDeviceClass == 0x09 && depth < 5){
        //Хаб: он уже в реестре; разворачиваем его нижние порты.
        uint8_t nports = 0;
        if (usb_hub_probe(&path->hc, dev_addr, &nports) != 0){
            //api_ptr->load_screen_add_string("%s hub %d: no hub desc", tag, dev_addr);
            return;
        }
        //api_ptr->load_screen_add_string("%s hub addr=%d ports=%d",tag, dev_addr, nports);
        for (uint8_t hp = 1; hp <= nports; hp++){
            usb_port_path_t cp;
            cp.hc        = path->hc;
            cp.root_port = path->root_port;
            cp.hub_addr  = dev_addr;
            cp.hub_port  = hp;
            if (!usb_path_port_connected(&cp)) continue;
            usb_enum_device_at(&cp, tag, depth + 1);
        }
    } else if (desc.bDeviceClass == 0x09){
        //api_ptr->load_screen_add_string("%s: nested hub at addr=%d skipped",tag, dev_addr);
    }
}

int device_usb_start(void)
{
    api_ptr->device_count = 0;
    api_ptr->last_error   = 0;
    memset(api_ptr->devices, 0, sizeof(api_ptr->devices));
    for (int i = 0; i < USB_MAX_MSC_DEVS; i++) g_msc[i].in_use = 0;
    g_msc_name_counter = 0;

    if (uhci_alloc_structures() != 0){ api_ptr->last_error = 0xBAD6; return -1; }
    if (ehci_alloc_structures() != 0){ api_ptr->last_error = 0xBAD7; return -1; }

    usb_scan_pci_controllers();

    //--- EHCI: HS-устройства — ПЕРВЫМИ ---
    //Порядок важен на реальном железе (CF-18): EHCI-порт в это время владеет
    //шиной, и никакой фоновый трафик UHCI-companion (его «фантомные»
    //LS-устройства на общих портах) не может вмешаться в перечисление и
    //инициализацию MSC. UHCI-устройства (внутренние LS/FS) опрашиваются
    //после и своим расписанием уже ничего не ломают.
    int ehci_count = ehci_probe_all();
    //api_ptr->load_screen_add_string("EHCI found %d controller(s)", ehci_count);
    for (int eci = 0; eci < ehci_count; eci++){
        ehci_hc_t *ehc = &ehci_hcs[eci];

        int ir = ehci_init(ehc);
        /*api_ptr->load_screen_add_string("EHCI %d init=%d mmio=%x op=%x n_ports=%d",
                                        eci, ir, ehc->mmio_base,
                                        ehc->op_base, (int)ehc->n_ports);*/
        if (ir != 0) continue;

        usb_hc_t hc_wrap = { USB_HC_EHCI, ehc };
        for (int port = 1; port <= (int)ehc->n_ports; port++){
            uint32_t ps = usb_hc_port_status(&hc_wrap, port);
            /*api_ptr->load_screen_add_string("EHCI %d p%d PORTSC=%x pspd=%d",
                                            eci, port, ps, (int)((ps >> 26) & 3));*/
            usb_port_path_t root_path;
            root_path.hc = hc_wrap;
            root_path.root_port = (uint8_t)port;
            root_path.hub_addr  = 0;
            root_path.hub_port  = 0;
            if (!usb_path_port_connected(&root_path)) continue;
            usb_enum_device_at(&root_path, "EHCI", 0);
        }
    }

    //--- UHCI: companion-контроллеры, FS/LS-устройства ---
    int uhci_count = uhci_probe_all();
    for (int hci = 0; hci < uhci_count; hci++){
        uhci_hc_t *uhc = &uhci_hcs[hci];
        if (uhci_init(uhc) != 0) continue;

        usb_hc_t hc_wrap = { USB_HC_UHCI, uhc };
        for (int port = 0; port < 2; port++){
            usb_port_path_t root_path;
            root_path.hc = hc_wrap;
            root_path.root_port = (uint8_t)port;
            root_path.hub_addr  = 0;
            root_path.hub_port  = 0;
            if (!usb_path_port_connected(&root_path)) continue;
            usb_enum_device_at(&root_path, "UHCI", 0);
        }
    }

    if (api_ptr->device_count == 0){ api_ptr->last_error = 0xBAD3; return -1; }
    return 0;
}



void *usb_dma_alloc(uint32_t size, uint32_t *phys_out)
{
    if (!api_ptr || !api_ptr->dma_alloc_ptr) return NULL;
    return api_ptr->dma_alloc_ptr(size, phys_out);
}

void usb_dma_free(void *virt, uint32_t size)
{
    if (!api_ptr || !api_ptr->dma_free_ptr) return;
    api_ptr->dma_free_ptr(virt, size);
}


void driver_main(struct boot_info *boot)
{
    (void)boot;

    api_ptr = &api;
    api_ptr->usb_start = device_usb_start;

    descr_api.struct_ptr = (void *)&api;
    descr_api.type       = DESCR_USB_DRIVER;
}

description_driver_api_t *driver_get_api(void)
{
    return &descr_api;
}
