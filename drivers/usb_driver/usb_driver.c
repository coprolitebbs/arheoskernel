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
 * usb_driver.c - точка входа ELF-драйвера USB (этап 1).
 *
 * Находит UHCI, сбрасывает порт с устройством, делает enumeration
 * и складывает VID:PID:class в description_driver_api_t->debug1..3.
 * Ядро читает их в load_additional_drivers() и выводит в boot-лог.
 */

#include "include/usb_driver.h"
#include "include/usb_uhci.h"
#include "include/usb_core.h"
#include "include/usb_msc.h"
#include "../../include-kernel/lib.h"

usb_stack_driver_api_t api;
usb_stack_driver_api_t *api_ptr;
description_driver_api_t descr_api;

uhci_hc_t       uhci0;
usb_msc_iface_t msc_iface;
uint8_t         msc_dev_addr     = 0;
uint32_t        msc_sector_count = 0;
uint32_t        msc_sector_size  = 0;


/* Обёртки, которые block_driver.c будет дёргать через block_api->read/write.
   priv == NULL, потому что USB-стек — singleton (одно устройство). */
static int usb_msc_read_wrap(void *priv, uint64_t lba, uint32_t count, void *buf){
    (void)priv;
    if (msc_sector_size == 0 || msc_sector_count == 0) return FS_IO_ERROR;
    if (lba + count > msc_sector_count)                return FS_IO_ERROR;
    int r = usb_msc_read10(&uhci0, msc_dev_addr, &msc_iface,
                           (uint32_t)lba, count, msc_sector_size, buf);
    return (r == 0) ? FS_OK : FS_IO_ERROR;
}

static int usb_msc_write_wrap(void *priv, uint64_t lba, uint32_t count, const void *buf){
    (void)priv;
    if (msc_sector_size == 0 || msc_sector_count == 0) return FS_IO_ERROR;
    if (lba + count > msc_sector_count)                return FS_IO_ERROR;
    int r = usb_msc_write10(&uhci0, msc_dev_addr, &msc_iface,
                            (uint32_t)lba, count, msc_sector_size, buf);
    return (r == 0) ? FS_OK : FS_IO_ERROR;
}



int device_usb_start(void){
    api_ptr->vid = 0; api_ptr->pid = 0; api_ptr->dev_class = 0;
    api_ptr->sector_count = 0; api_ptr->sector_size = 0; api_ptr->block_id = 0;

    if (uhci_probe(&uhci0) != 0){ api_ptr->vid = 0xBAD0; return -1; }
    if (uhci_init (&uhci0) != 0){ api_ptr->vid = 0xBAD1; return -1; }

    if (((uint32_t)frame_list_ptr & 0xFFF) != 0 ||
        ((uint32_t)control_qh_ptr  & 0x0F)  != 0 ||
        ((uint32_t)control_tds_ptr & 0x0F)  != 0){
        api_ptr->vid = 0xBAD4;
        return -1;
    }

    for (int port = 0; port < 2; port++){
        if (!uhci_port_connected(&uhci0, port)) continue;

        usb_device_desc_t desc;
        uint8_t dev_addr = 0;
        memset(&desc, 0, sizeof(desc));

        int r = usb_enumerate(&uhci0, port, &desc, &dev_addr);
        if (r != 0){
            api_ptr->vid = 0xBAD2;
            api_ptr->pid = (uint32_t)r;
            continue;
        }

        api_ptr->vid = desc.idVendor;
        api_ptr->pid = desc.idProduct;
        api_ptr->dev_class = desc.bDeviceClass;

        if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 1;

        uint8_t config_buf[256];
        memset(config_buf, 0, sizeof(config_buf));

        uint16_t ep0_max = desc.bMaxPacketSize0;
        if (ep0_max == 0) ep0_max = 8;

        int cfg_r = usb_get_config_desc(&uhci0, dev_addr, config_buf,
                                        sizeof(config_buf), ep0_max);
        if (cfg_r != 0){
            if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;
            api_ptr->dev_class = 0xDEAD0001u | ((uint32_t)(-cfg_r) & 0xFF);
            api_ptr->port = (uint32_t)port;
            return desc.idVendor;
        }

        usb_config_desc_t *cfg = (usb_config_desc_t*)config_buf;
        uint16_t total = cfg->wTotalLength;

        int pr = usb_parse_mass_storage(config_buf, total, &msc_iface);
        if (pr != 0){
            if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;
            api_ptr->dev_class = 0xDEAD0002u | ((uint32_t)(-pr) & 0xFF);
            api_ptr->port = (uint32_t)port;
            return desc.idVendor;
        }

        int sr = usb_set_configuration(&uhci0, dev_addr, cfg->bConfigurationValue);
        if (sr != 0){
            if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;
            api_ptr->dev_class = 0xDEAD0003u;
            api_ptr->port = (uint32_t)port;
            return desc.idVendor;
        }

        int msc_r = usb_msc_init(&uhci0, dev_addr, &msc_iface,
                                 &msc_sector_count, &msc_sector_size);

        if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;

        if (msc_r != 0){
            api_ptr->dev_class = 0xDEAD0004u | ((uint32_t)(-msc_r) & 0xFF);
            api_ptr->port = (uint32_t)port;
            return desc.idVendor;
        }

        msc_dev_addr    = dev_addr;
        api_ptr->sector_count = msc_sector_count;
        api_ptr->sector_size = msc_sector_size;

        /* Регистрируемся как блочное устройство (usb0, usb1, ...) */
        if (api_ptr->block && api_ptr->block->register_device){
            int bid = api_ptr->block->register_device("usb0", msc_sector_count,
                                                       BLK_REMOVABLE,
                                                       usb_msc_read_wrap,
                                                       usb_msc_write_wrap,
                                                       NULL);
            if (bid >= 0){
                api_ptr->block_id = (uint32_t)bid;
            } else {
                api_ptr->block_id = 0xDEADBEEF;
            }
        }
        api_ptr->port = (uint32_t)port;

        return desc.idVendor;
    }

    api_ptr->vid = 0xBAD3;
    return -1;
}


void driver_main(struct boot_info *boot){
    (void)boot;

    uhci_register_ptrs();

    description_driver_api_t *descr_api_ptr = &descr_api;

    api_ptr = &api;
    api_ptr->usb_start = device_usb_start;

    descr_api_ptr->struct_ptr = (void *)&api;

    descr_api_ptr->type       = DESCR_USB_DRIVER;
}

description_driver_api_t *driver_get_api(void){
    return &descr_api;
}