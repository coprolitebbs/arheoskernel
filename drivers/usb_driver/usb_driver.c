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
 * HID-класс и прочие классы просто фиксируются в реестре и не инициализируются как MSC.
 */

#include "include/usb_driver.h"
#include "include/usb_uhci.h"
#include "include/usb_ehci.h"
#include "include/usb_core.h"
#include "include/usb_msc.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

usb_stack_driver_api_t api;
usb_stack_driver_api_t *api_ptr;
description_driver_api_t descr_api;

uhci_hc_t uhci0;
static usb_msc_dev_t g_msc[USB_MAX_MSC_DEVS];

//---------------- Ручной hotplug: состояние ----------------

//Пул USB-адресов (1..127). Адреса глобально уникальны на всю систему:
//uhci_dev_is_ls[]/ehci_dev_speed[] индексируются адресом без разделения
//по контроллерам, поэтому один и тот же адрес не должен выдаваться
//двум устройствам даже на разных HC. Стартовое поведение старого кода
//(addr = 1 + device_count) ломалось после удаления устройства из середины.
static uint8_t usb_addr_used[128];

static uint8_t usb_alloc_addr(void)
{
    for (uint8_t a = 1; a < 128; a++)
    {
        if (!usb_addr_used[a])
        {
            usb_addr_used[a] = 1;
            return a;
        }
    }
    return 0;   //свободных адресов нет
}

static void usb_free_addr(uint8_t a)
{
    if (a && a < 128) usb_addr_used[a] = 0;
}

//Какие контроллеры успели проинициализироваться при старте.
//update трогает только их: re-init HC сбросил бы уже выданные адреса
static uint8_t g_uhci_inited[UHCI_MAX_CONTROLLERS];
static uint8_t g_ehci_inited[4];
static int     g_usb_started = 0;   //0 = контроллеры ещё не инициализировались

//Живое устройство: привязка записи реестра к физическому пути
//(контроллер/корневой порт/хаб) и ресурсам (USB-адрес, MSC-слот, block id).
//По этой таблице update понимает, что исчезло, а что появилось
typedef struct
{
    uint8_t          used;
    usb_hc_type_t    type;       //USB_HC_UHCI / USB_HC_EHCI
    uint8_t          hc_idx;     //индекс в uhci_hcs[] / ehci_hcs[]
    uint8_t          root_port;  //корневой порт контроллера
    uint8_t          hub_addr;   //0 = устройство прямо на корневом порту
    uint8_t          hub_port;   //порт хаба (при hub_addr != 0)
    uint8_t          dev_addr;   //выданный USB-адрес
    int              msc_slot;   //индекс в g_msc[] или -1
    usb_dev_record_t rec;        //запись реестра (в т.ч. block_id)
} usb_live_t;

static usb_live_t g_live[USB_REGISTRY_MAX];

//Индекс контроллера по обёртке usb_hc_t (указатели указывают в массивы)
static int usb_hc_index(const usb_hc_t *w)
{
    if (!w) return -1;
    if (w->type == USB_HC_UHCI) return (int)((uhci_hc_t *)w->hc - uhci_hcs);
    if (w->type == USB_HC_EHCI) return (int)((ehci_hc_t *)w->hc - ehci_hcs);
    return -1;
}

//Найти живое устройство по физическому пути
static usb_live_t *usb_live_find(usb_hc_type_t type, int hc_idx,
                                 uint8_t root_port, uint8_t hub_addr, uint8_t hub_port)
{
    for (int i = 0; i < USB_REGISTRY_MAX; i++)
    {
        usb_live_t *lv = &g_live[i];
        if (!lv->used) continue;
        if (lv->type != type || (int)lv->hc_idx != hc_idx) continue;
        if (lv->root_port != root_port) continue;
        if (lv->hub_addr != hub_addr) continue;
        if (hub_addr != 0 && lv->hub_port != hub_port) continue;
        return lv;
    }
    return NULL;
}

static void usb_live_add(usb_port_path_t *path, uint8_t dev_addr,
                         const usb_dev_record_t *rec, int msc_slot)
{
    int idx = usb_hc_index(&path->hc);
    if (idx < 0) return;

    for (int i = 0; i < USB_REGISTRY_MAX; i++)
    {
        if (g_live[i].used) continue;
        memset(&g_live[i], 0, sizeof(g_live[i]));
        g_live[i].used      = 1;
        g_live[i].type      = path->hc.type;
        g_live[i].hc_idx    = (uint8_t)idx;
        g_live[i].root_port = path->root_port;
        g_live[i].hub_addr  = path->hub_addr;
        g_live[i].hub_port  = path->hub_port;
        g_live[i].dev_addr  = dev_addr;
        g_live[i].msc_slot  = msc_slot;
        g_live[i].rec       = *rec;
        return;
    }
}

//Снять одно устройство: разрегистрировать block-девайс, освободить MSC-слот
//и USB-адрес. Сама запись реестра пересобирается позже из g_live
static void usb_live_remove_one(usb_live_t *lv)
{
    if (lv->rec.block_id != 0xFFFFFFFFu &&
            api_ptr->block && api_ptr->block->unregister_device)
    {
        api_ptr->block->unregister_device(lv->rec.block_id);
    }

    if (lv->msc_slot >= 0 && lv->msc_slot < USB_MAX_MSC_DEVS)
    {
        //слот не обнуляем целиком: указатель priv в block-слое мог бы ещё
        //где-то болтаться — просто делаем обёртки чтения/записи мёртвыми
        g_msc[lv->msc_slot].in_use       = 0;
        g_msc[lv->msc_slot].sector_count = 0;
        g_msc[lv->msc_slot].sector_size  = 0;
    }

    usb_free_addr(lv->dev_addr);
    lv->used = 0;
}

//Снять устройство, если это хаб — сначала все устройства за ним (они становятся недостижимы)
static void usb_live_remove_recursive(usb_live_t *lv)
{
    if (lv->rec.dev_class == 0x09)
    {
        for (int i = 0; i < USB_REGISTRY_MAX; i++)
        {
            if (!g_live[i].used) continue;
            if (g_live[i].hub_addr != lv->dev_addr) continue;
            if (g_live[i].type != lv->type || g_live[i].hc_idx != lv->hc_idx) continue;
            usb_live_remove_recursive(&g_live[i]);
        }
    }
    usb_live_remove_one(lv);
}

//Пересобрать devices[]/device_count из таблицы живых устройств — именно это ядро потом перекладывает в usb_registry
static void usb_rebuild_registry(void)
{
    api_ptr->device_count = 0;
    memset(api_ptr->devices, 0, sizeof(api_ptr->devices));
    for (int i = 0; i < USB_REGISTRY_MAX; i++)
    {
        if (!g_live[i].used) continue;
        if (api_ptr->device_count >= USB_REGISTRY_MAX) break;
        api_ptr->devices[api_ptr->device_count++] = g_live[i].rec;
    }
}



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

//Заполнить запись устройства + инициализировать MSC (если это mass storage)
//и зарегистрировать в блок-слое. Не добавляет запись в devices[] — реестр
//пересобирается из g_live (см. usb_rebuild_registry). Вызывается и при старте,
//и при hotplug-rescan.
//rec->port заполняет вызывающий (номер корневого порта)
static void usb_setup_device_rec(usb_hc_t *hcw, usb_device_desc_t *desc,
                                 uint8_t dev_addr, usb_dev_record_t *rec_out,
                                 int *msc_slot_out)
{
    memset(rec_out, 0, sizeof(*rec_out));
    rec_out->vid          = desc->idVendor;
    rec_out->pid          = desc->idProduct;
    rec_out->dev_class    = desc->bDeviceClass;
    rec_out->dev_subclass = desc->bDeviceSubClass;
    rec_out->dev_protocol = desc->bDeviceProtocol;
    rec_out->block_id     = 0xFFFFFFFFu;
    if (msc_slot_out) *msc_slot_out = -1;

    uint16_t ep0 = desc->bMaxPacketSize0 ? desc->bMaxPacketSize0 : 8;
    uint8_t cfg_buf[256];
    memset(cfg_buf, 0, sizeof(cfg_buf));
    if (usb_get_config_desc(hcw, dev_addr, cfg_buf,
                            sizeof(cfg_buf), ep0) != 0) return;

    usb_config_desc_t *cfg = (usb_config_desc_t*)cfg_buf;
    usb_msc_iface_t local_msc;
    if (usb_parse_mass_storage(cfg_buf, cfg->wTotalLength, &local_msc) != 0)
        return;   //не MSC — обычная запись в реестре

    usb_set_configuration(hcw, dev_addr, cfg->bConfigurationValue);

    usb_msc_dev_t *slot = usb_alloc_msc_slot();
    if (!slot) return;

    slot->dev_addr = dev_addr;
    slot->iface    = local_msc;
    slot->iface.ep0_max_pkt = ep0;
    //Копируем usb_hc_t в структуру слота, чтобы read/write_wrap потом имели валидный указатель.
    //hcw часто на стеке — помрёт всё
    slot->hc_wrap.type = hcw->type;
    slot->hc_wrap.hc   = hcw->hc;
    slot->hc           = &slot->hc_wrap;

    //unit attention после вставки снимается внутри usb_msc_init (REQUEST SENSE)
    if (usb_msc_init(&slot->hc_wrap, dev_addr, &slot->iface,
                     &slot->sector_count, &slot->sector_size) == 0
            && api_ptr->block && api_ptr->block->register_device)
    {
        //Имя привязано к MSC-слоту: слоты освобождаются при отключении,
        //поэтому повторная вставка флешки переиспользует то же имя,
        //а collisions с ещё живыми устройствами не бывает
        char name[8] = "usb0";
        int sidx = (int)(slot - g_msc);
        name[3] = (char)('0' + (sidx % 10));
        int bid = api_ptr->block->register_device(
                      name, slot->sector_count, BLK_REMOVABLE,
                      usb_msc_read_wrap, usb_msc_write_wrap, (void*)slot);
        //api_ptr->load_screen_add_string("USB msc addr=%d reg %s -> %d",dev_addr, name, bid);
        if (bid >= 0)
        {
            slot->block_id = (uint32_t)bid;
            rec_out->block_id = (uint32_t)bid;
        }
        if (msc_slot_out) *msc_slot_out = sidx;
    }
}

//Перечислить устройство на заданном пути (корневой порт или порт за хабом),
//оформить его; если это хаб (class 09) — рекурсивно обойти его нижние порты.
//Глубина ограничена: depth >= 5 — хабы больше не разворачиваем.
//Используется и при старте, и при hotplug-rescan (в таблице g_live и в
//devices[] устройство появляется только после успешного перечисления)
static void usb_enum_device_at(usb_port_path_t *path, const char *tag, int depth)
{
    uint8_t want_addr = usb_alloc_addr();
    if (!want_addr) return;   //адреса кончились

    usb_device_desc_t desc;
    uint8_t dev_addr = 0;
    memset(&desc, 0, sizeof(desc));
    if (usb_enumerate(path, want_addr, &desc, &dev_addr) != 0)
    {
        usb_free_addr(want_addr);
        return;
    }

    usb_dev_record_t rec;
    int mslot = -1;
    usb_setup_device_rec(&path->hc, &desc, dev_addr, &rec, &mslot);
    rec.port = path->root_port;

    usb_live_add(path, dev_addr, &rec, mslot);

    if (desc.bDeviceClass == 0x09 && depth < 5)
    {
        //Хаб: он уже в реестре; разворачиваем его нижние порты.
        uint8_t nports = 0;
        if (usb_hub_probe(&path->hc, dev_addr, &nports) != 0)
        {
            //api_ptr->load_screen_add_string("%s hub %d: no hub desc", tag, dev_addr);
            return;
        }
        //api_ptr->load_screen_add_string("%s hub addr=%d ports=%d",tag, dev_addr, nports);
        for (uint8_t hp = 1; hp <= nports; hp++)
        {
            usb_port_path_t cp;
            cp.hc        = path->hc;
            cp.root_port = path->root_port;
            cp.hub_addr  = dev_addr;
            cp.hub_port  = hp;
            if (!usb_path_port_connected(&cp)) continue;
            usb_enum_device_at(&cp, tag, depth + 1);
        }
    }
    else if (desc.bDeviceClass == 0x09)
    {
        //api_ptr->load_screen_add_string("%s: nested hub at addr=%d skipped",tag, dev_addr);
    }
}

//Пересмотреть один корневой порт: добавить новое / снять исчезнувшее.
//Общая логика для EHCI и UHCI
static void usb_update_root_port(usb_hc_t *hcw, int hc_idx,
                                 int port, int *added, int *removed)
{
    int conn = usb_hc_port_connected(hcw, port);
    usb_live_t *lv = usb_live_find(hcw->type, hc_idx,
                                   (uint8_t)port, 0, 0);

    if (!conn && lv)
    {
        usb_live_remove_recursive(lv);
        (*removed)++;
    }
    else if (conn && !lv)
    {
        usb_port_path_t rp;
        rp.hc        = *hcw;
        rp.root_port = (uint8_t)port;
        rp.hub_addr  = 0;
        rp.hub_port  = 0;
        usb_enum_device_at(&rp, "hotplug", 0);
        (*added)++;
    }
}

//Ручной hotplug-rescan (SYS_USB_RESCAN -> lsusb -update).
//Только уже инициализированные контроллеры: их порты пересматриваются по CCS,
//новые устройства перечисляются (порт сбрасывается, устройство получает адрес),
//исчезнувшие — снимаются вместе с block-регистрацией и MSC-слотом.
//Структуры расписания/HC НЕ переинициализируются: re-init сбросил бы адреса
//уже подключённых устройств.
//Возвращает число устройств после обновления или отрицательный код
int device_usb_start(void);   //полная инициализация (определена ниже)

int device_usb_update(void)
{
    if (!api_ptr) return -1;

    //Контроллеры ещё не инициализировались (старт не удался или драйвер
    //перезагружен) — полный старт
    if (!g_usb_started) return device_usb_start();

    int added = 0, removed = 0;

    //Блокируем вытеснение на время перечисления (как долгая операция с диском):
    //конкурентный MSC-read из другой задачи крутил бы те же QH/TD-кольца
    uint8_t we = 0;
    volatile uint8_t *lock = api_ptr->schedule_lock_ptr;
    if (lock && *lock == 0)
    {
        *lock = 1;
        we = 1;
    }

    //--- EHCI: корневые порты (порядок как при старте: сначала EHCI) ---
    for (int eci = 0; eci < ehci_hc_count && eci < 4; eci++)
    {
        if (!g_ehci_inited[eci]) continue;
        ehci_hc_t *ehc = &ehci_hcs[eci];
        usb_hc_t hcw = { USB_HC_EHCI, ehc };
        for (int port = 1; port <= (int)ehc->n_ports; port++)
            usb_update_root_port(&hcw, eci, port, &added, &removed);
    }

    //--- UHCI: корневые порты ---
    for (int hci = 0; hci < uhci_hc_count && hci < UHCI_MAX_CONTROLLERS; hci++)
    {
        if (!g_uhci_inited[hci]) continue;
        uhci_hc_t *uhc = &uhci_hcs[hci];
        usb_hc_t hcw = { USB_HC_UHCI, uhc };
        for (int port = 0; port < uhci_hc_ports(uhc); port++)
            usb_update_root_port(&hcw, hci, port, &added, &removed);
    }

    //--- Хабы: пересмотреть нижние порты живых хабов ---
    for (int i = 0; i < USB_REGISTRY_MAX; i++)
    {
        usb_live_t *hub = &g_live[i];
        if (!hub->used || hub->rec.dev_class != 0x09) continue;

        usb_hc_t hcw;
        hcw.type = hub->type;
        hcw.hc   = (hub->type == USB_HC_UHCI) ? (void *)&uhci_hcs[hub->hc_idx]
                   : (void *)&ehci_hcs[hub->hc_idx];

        uint8_t nports = 0;
        if (usb_hub_probe(&hcw, hub->dev_addr, &nports) != 0) continue;

        for (uint8_t hp = 1; hp <= nports; hp++)
        {
            int conn = usb_hub_port_connected(&hcw, hub->dev_addr, hp);
            usb_live_t *lv = usb_live_find(hub->type, hub->hc_idx,
                                           hub->root_port, hub->dev_addr, hp);
            if (!conn && lv)
            {
                usb_live_remove_recursive(lv);
                removed++;
            }
            else if (conn && !lv)
            {
                usb_port_path_t cp;
                cp.hc        = hcw;
                cp.root_port = hub->root_port;
                cp.hub_addr  = hub->dev_addr;
                cp.hub_port  = hp;
                usb_enum_device_at(&cp, "hotplug", 1);
                added++;
            }
        }
    }

    usb_rebuild_registry();

    if (we) *lock = 0;

    //Сводка последнего rescan: (added << 8) | removed — для отладки
    api_ptr->last_error = (uint32_t)((added << 8) | removed);
    return (int)api_ptr->device_count;
}

int device_usb_start(void)
{
    api_ptr->device_count = 0;
    api_ptr->last_error   = 0;
    memset(api_ptr->devices, 0, sizeof(api_ptr->devices));

    //Полный сброс состояния hotplug-подсистемы
    memset(g_live, 0, sizeof(g_live));
    memset(usb_addr_used, 0, sizeof(usb_addr_used));
    memset(g_uhci_inited, 0, sizeof(g_uhci_inited));
    memset(g_ehci_inited, 0, sizeof(g_ehci_inited));
    for (int i = 0; i < USB_MAX_MSC_DEVS; i++) g_msc[i].in_use = 0;

    if (uhci_alloc_structures() != 0)
    {
        api_ptr->last_error = 0xBAD6;
        return -1;
    }
    if (ehci_alloc_structures() != 0)
    {
        api_ptr->last_error = 0xBAD7;
        return -1;
    }

    usb_scan_pci_controllers();

    //--- EHCI: HS-устройства — первыми ---
    //Порядок важен на реальном железе: EHCI-порт в это время владеет
    //шиной, и никакой фоновый трафик UHCI-companion (его фантомные
    //LS-устройства на общих портах) не может вмешаться в перечисление и
    //инициализацию MSC. UHCI-устройства (внутренние LS/FS) опрашиваются после
    int ehci_count = ehci_probe_all();
    //api_ptr->load_screen_add_string("EHCI found %d controller(s)", ehci_count);
    for (int eci = 0; eci < ehci_count; eci++)
    {
        ehci_hc_t *ehc = &ehci_hcs[eci];

        int ir = ehci_init(ehc);
        /*api_ptr->load_screen_add_string("EHCI %d init=%d mmio=%x op=%x n_ports=%d",
                                        eci, ir, ehc->mmio_base,
                                        ehc->op_base, (int)ehc->n_ports);*/
        if (ir != 0) continue;
        g_ehci_inited[eci] = 1;

        usb_hc_t hc_wrap = { USB_HC_EHCI, ehc };
        for (int port = 1; port <= (int)ehc->n_ports; port++)
        {
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
    for (int hci = 0; hci < uhci_count; hci++)
    {
        uhci_hc_t *uhc = &uhci_hcs[hci];
        if (uhci_init(uhc) != 0) continue;
        g_uhci_inited[hci] = 1;

        usb_hc_t hc_wrap = { USB_HC_UHCI, uhc };
        for (int port = 0; port < 2; port++)
        {
            usb_port_path_t root_path;
            root_path.hc = hc_wrap;
            root_path.root_port = (uint8_t)port;
            root_path.hub_addr  = 0;
            root_path.hub_port  = 0;
            if (!usb_path_port_connected(&root_path)) continue;
            usb_enum_device_at(&root_path, "UHCI", 0);
        }
    }

    //Реестр драйвера теперь всегда собирается из таблицы живых устройств —
    //и после старта, и после каждого rescan
    usb_rebuild_registry();

    //Статус старта для device_usb_update: разрешён rescan только если
    //хоть один контроллер реально проинициализировался
    for (int i = 0; i < 4; i++) if (g_ehci_inited[i]) g_usb_started = 1;
    for (int i = 0; i < UHCI_MAX_CONTROLLERS; i++) if (g_uhci_inited[i]) g_usb_started = 1;

    if (api_ptr->device_count == 0)
    {
        api_ptr->last_error = 0xBAD3;
        return -1;
    }
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
    api_ptr->usb_start  = device_usb_start;
    api_ptr->usb_update = device_usb_update;

    descr_api.struct_ptr = (void *)&api;
    descr_api.type       = DESCR_USB_DRIVER;
}

description_driver_api_t *driver_get_api(void)
{
    return &descr_api;
}
