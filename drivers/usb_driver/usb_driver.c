#include "include/usb_driver.h"
#include "include/usb_uhci.h"
#include "include/usb_core.h"
#include "include/usb_msc.h"
#include "include/usb_hub.h"
#include "include/ehci_priv.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

usb_stack_driver_api_t api;
usb_stack_driver_api_t *api_ptr;
description_driver_api_t descr_api;
description_driver_api_t *descr_api_ptr;

/* Пул UHCI-контроллеров переехал в usb_uhci.c (uhci_add_host/get_host/...),
   чтобы select_host мог останавливать чужие HC без дублирования состояния. */

ehci_hc_t       ehci0;
usb_hcd_t g_hcd;

uint32_t g_dev_count = 0;
int      g_msc_idx   = 0;

void usb_legacy_handoff(void) {}

void usb_disable_smm(void)
{
    uint32_t pm_cfg = pci_read32(0, 31, 0, 0x40);
    uint16_t pmbase = pm_cfg & 0xFF80;
    if (pmbase >= 0x1000 && pmbase != 0xFFF8)
    {
        uint32_t en = inl(pmbase + 0x30);
        en &= ~((1u<<5) | (1u<<6) | (1u<<7));
        outl(pmbase + 0x30, en);
    }
    uint32_t ctl = pci_read32(0, 31, 0, 0xB0);
    uint8_t cmd = (ctl >> 8) & 0xFF;
    if (cmd && cmd != 0xFF) outb(cmd, 0xE8);
    for (volatile int i = 0; i < 1000000; i++);
}

void probe_all_uhci_controllers(void)
{
    uhci_reset_host_list();

    /* Полный PCI-скан шины 0 (32 dev × 8 func). Прежний «список слотов ICH6»
       {29,x},{23,1} не находил НИЧЕГО в QEMU с -device piix3-usb-uhci: там
       контроллер сидит на dev 1 func 1, VID 0x8086 DID 0x7123 (PIIX4 UHCI).
       Фильтр — по класс-code, как pci_scan_bus в Linux. */
    for (uint32_t bus = 0; bus < 1; bus++)
    for (uint32_t dev = 0; dev < 32; dev++)
    for (uint32_t func = 0; func < 8; func++)
    {
        uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x00);
        if ((id & 0xFFFF) == 0xFFFF || id == 0) continue;

        uint32_t cls = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x08);
        uint8_t cclass = (cls >> 24) & 0xFF;
        uint8_t csub   = (cls >> 16) & 0xFF;
        uint8_t cprog  = (cls >>  8) & 0xFF;
        if (cclass != PCI_CLASS_SERIAL_BUS || csub != PCI_SUBCLASS_USB) continue;
        if (cprog != PCI_PROGIF_UHCI)      continue;   /* OHCI/EHCI пропускаем */

        uint32_t bar4 = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x20);
        if (!(bar4 & 0x01)) continue;
        uint16_t io = bar4 & 0xFFF0;
        if (io == 0 || io == 0xFFF0) continue;

        uint32_t cmd = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x04);
        pci_write32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x04, (cmd & ~0x7u) | 0x07u);

        uhci_add_host(io, (uint8_t)bus, (uint8_t)dev, (uint8_t)func);
        api_ptr->load_screen_add_string("PCI UHCI b%d d%d f%d %x:%x at IO %x",
                                        bus, dev, func,
                                        (id >> 16) & 0xFFFF, id & 0xFFFF, io);
    }
}


int ehci_release_to_uhci(void) {
    /* EHCI ищем по РЕАЛЬНОМУ class-code (0x0C/0x03/prog-if 0x20), а не по
       жёстко зашитому слоту dev 29 func 7: в QEMU такого слота нет вообще. */
    int found = 0;
    for (uint32_t dev = 0; dev < 32; dev++)
    for (uint32_t func = 0; func < 8; func++)
    {
        uint32_t id = pci_read32(0, (uint8_t)dev, (uint8_t)func, 0x00);
        if ((id & 0xFFFF) == 0xFFFF || id == 0) continue;
        uint32_t cls = pci_read32(0, (uint8_t)dev, (uint8_t)func, 0x08);
        if (((cls >> 24) & 0xFF) != PCI_CLASS_SERIAL_BUS) continue;
        if (((cls >> 16) & 0xFF) != PCI_SUBCLASS_USB) continue;
        if (((cls >>  8) & 0xFF) != PCI_PROGIF_EHCI) continue;

        found = 1;
        api_ptr->load_screen_add_string("EHCI found b0 d%d f%d %x:%x",
                                        dev, func, (id >> 16) & 0xFFFF, id & 0xFFFF);

        pci_write32(0, (uint8_t)dev, (uint8_t)func, 0x60, 0x00000000u);
        pci_write32(0, (uint8_t)dev, (uint8_t)func, 0x6C, 0x00000000u);
        for (volatile int i = 0; i < 10000; i++);

        uint32_t leg = pci_read32(0, (uint8_t)dev, (uint8_t)func, 0x68);
        pci_write32(0, (uint8_t)dev, (uint8_t)func, 0x68, (leg | (1u << 24)) & ~(1u << 16));
        for (volatile int i = 0; i < 20000; i++);

        pci_write32(0, (uint8_t)dev, (uint8_t)func, 0x04, 0x00000000u);
        pci_read32(0, (uint8_t)dev, (uint8_t)func, 0x04); // PCI Flush
    }
    if (!found)
        api_ptr->load_screen_add_string("EHCI not present (pure UHCI host?)");

    uhci_mdelay(300);
    return found ? 0 : -1;
}





int register_device_record(usb_hcd_t *hcd, int port_idx, uint8_t dev_addr, const usb_device_desc_t *desc)
{
    if (g_dev_count >= USB_REGISTRY_MAX) return -1;

    int is_first = (g_dev_count == 0);
    usb_dev_record_t *rec = &api_ptr->devices[g_dev_count];
    memset(rec, 0, sizeof(*rec));
    rec->block_id = 0xFFFFFFFFu;
    rec->vid          = desc->idVendor;
    rec->pid          = desc->idProduct;
    rec->dev_class    = desc->bDeviceClass;
    rec->dev_subclass = desc->bDeviceSubClass;
    rec->dev_protocol = desc->bDeviceProtocol;
    rec->port         = (uint8_t)port_idx;

    uint16_t ep0 = desc->bMaxPacketSize0 ? desc->bMaxPacketSize0 : 8;
    if (g_msc_idx < USB_MAX_MSC)
    {
        uint8_t config_buf[256];
        memset(config_buf, 0, sizeof(config_buf));

        int cfg_r = usb_get_config_desc(hcd, dev_addr, config_buf, sizeof(config_buf), ep0);
        if (cfg_r == 0)
        {
            usb_config_desc_t *cfg = (usb_config_desc_t*)config_buf;
            usb_msc_iface_t iface;
            int pr = usb_parse_mass_storage(config_buf, cfg->wTotalLength, &iface);
            if (pr == 0)
            {
                int sr = usb_set_configuration(hcd, dev_addr, cfg->bConfigurationValue);
                if (sr == 0)
                {
                    extern usb_msc_ctx_t g_msc_ctx[];
                    usb_msc_ctx_t *ctx = &g_msc_ctx[g_msc_idx];
                    ctx->hcd = hcd;
                    ctx->iface = iface;
                    ctx->dev_addr = dev_addr;
                    ctx->used = 1;

                    int msc_r = usb_msc_init(hcd, dev_addr, &ctx->iface, &ctx->sector_count, &ctx->sector_size);
                    if (msc_r == 0)
                    {
                        if (api_ptr->block && api_ptr->block->register_device)
                        {
                            char name[8];
                            name[0]='u';
                            name[1]='s';
                            name[2]='b';
                            name[3]=(char)('0'+g_msc_idx);
                            name[4]='\0';
                            int bid = api_ptr->block->register_device(name, ctx->sector_count, BLK_REMOVABLE, usb_msc_read_wrap, usb_msc_write_wrap, ctx);
                            if (bid >= 0) rec->block_id = (uint32_t)bid;
                        }
                        g_msc_idx++;
                    }
                    else
                    {
                        ctx->used = 0;
                    }
                }
            }
        }
    }

    if (is_first)
    {
        api_ptr->vid           = desc->idVendor;
        api_ptr->pid           = desc->idProduct;
        api_ptr->dev_class     = desc->bDeviceClass;
        api_ptr->port          = (uint32_t)port_idx;
        api_ptr->sector_count  = (rec->block_id != 0xFFFFFFFFu) ? g_msc_ctx_ptr[0].sector_count : 0;
        api_ptr->sector_size   = (rec->block_id != 0xFFFFFFFFu) ? g_msc_ctx_ptr[0].sector_size  : 0;
        api_ptr->block_id      = rec->block_id;
    }

    g_dev_count++;
    return 0;
}

int device_usb_start(void) {
    api_ptr->vid = 0; api_ptr->pid = 0; api_ptr->dev_class = 0;
    api_ptr->sector_count = 0; api_ptr->sector_size = 0;
    api_ptr->block_id = 0xFFFFFFFFu; api_ptr->device_count = 0;
    api_ptr->last_error = 0; api_ptr->debug1 = 0; api_ptr->debug2 = 0; api_ptr->debug3 = 0;
    api_ptr->debug4 = 0; api_ptr->debug5 = 0; api_ptr->debug6 = 0; api_ptr->debug7 = 0; api_ptr->debug8 = 0; api_ptr->debug9 = 0;

    g_dev_count = 0; g_msc_idx = 0;
    memset(g_msc_ctx_ptr, 0, sizeof(g_msc_ctx));

    // 1. Изолируем Legacy семафоры BIOS
    usb_disable_smm();

    // 2. Выполняем тотальное аппаратное отключение EHCI на уровне шины PCI
    ehci_release_to_uhci();
    api_ptr->load_screen_add_string("EHCI Hard-Isolated on PCI. Monopolizing bus for UHCI.");

    // Даем линиям D+/D- честные 300мс на электрическую стабилизацию под контролем UHCI
    uhci_mdelay(300);

    // 3. Зондируем шину PCI и собираем ВСЕ подчиненные Companion-контроллеры UHCI
    probe_all_uhci_controllers();

    int hosts_n = uhci_get_host_count();
    if (hosts_n == 0) {
        api_ptr->vid = 0xBAD0;
        return -1;
    }

    uint8_t next_addr = 1;

    // Поочередно инициализируем и обыскиваем КАЖДЫЙ контроллер UHCI из пула
    for (int u = 0; u < hosts_n; u++) {
        uhci_hc_t *hc = uhci_get_host(u);
        if (!hc) continue;

        api_ptr->load_screen_add_string("U%d: Initializing Companion at IO %x...", u, hc->io_base);
        if (uhci_init(hc) != 0) {
            api_ptr->load_screen_add_string("U%d: Hardware init failed, skipping", u);
            continue;
        }

       uint16_t f0 = inw(hc->io_base + UHCI_FRNUM);
      uhci_mdelay(10);
      uint16_t f1 = inw(hc->io_base + UHCI_FRNUM);
      api_ptr->load_screen_add_string("U%d SCHED %x->%x %s", u, f0, f1,
                                (f0 != f1) ? "ALIVE" : "DEAD");
      if (f0 == f1) { uhci_init(hc); }   /* один повтор */

        /* Диагностика сразу после инициализации: USBSTS каждого хоста.
           HCH=1 — планировщик так и не запустился (FLBASEADD/PCI проблема);
           RESUME/ERR bits — BIOS/SMM снова усыпил HC между handoff и init. */
        api_ptr->load_screen_add_string("U%d STS=%x CMD=%x after init", u,
                                        inw(hc->io_base + UHCI_USBSTS),
                                        inw(hc->io_base + UHCI_USBCMD));

        g_hcd.type = HCD_UHCI;
        g_hcd.priv = hc;

        // Обходим 2 корневых порта текущего запущенного UHCI хоста
        for (int port = 0; port < 2; port++) {
            uint16_t pre_status = uhci_port_status(hc, port);

            // Если на данном конкретном порту контроллера НЕТ устройства (CCS == 0), пропускаем его
            if (!(pre_status & PORTSC_CCS)) continue;

            api_ptr->load_screen_add_string("U%d Port %d: Device attached! State: %x. Resetting...", u, port, pre_status);

            int rp = uhci_reset_port(hc, port);
            uint16_t post_status = uhci_port_status(hc, port);

            api_ptr->load_screen_add_string("U%d Port %d: Reset code=%d, Post-state=%x", u, port, rp, post_status);

            if (!(post_status & PORTSC_PED)) {
                api_ptr->load_screen_add_string("U%d Port %d: Port activation fail (PED=0), skip", u, port);
                continue;
            }

            // Порт открыт! Запускаем асинхронное перечисление очередей
            static usb_device_desc_t desc __attribute__((aligned(16)));
            uint8_t dev_addr = 0;
            memset(&desc, 0, sizeof(usb_device_desc_t));

            int r = usb_enumerate(&g_hcd, port, next_addr, &desc, &dev_addr);
            api_ptr->load_screen_add_string("U%d Port %d: Enumerate code=%d, TD0=%x TD_status=%x",
                                            u, port, r, api_ptr->debug8, api_ptr->debug9);

            if (r != 0) continue;

            next_addr++;
            register_device_record(&g_hcd, port, dev_addr, &desc);
            if (g_dev_count >= USB_REGISTRY_MAX) break;
        }
        if (g_dev_count >= USB_REGISTRY_MAX) break;
    }

    api_ptr->device_count = g_dev_count;
    if (g_dev_count == 0 && api_ptr->vid == 0) api_ptr->vid = 0xBAD3;
    return (g_dev_count > 0) ? 0 : -1;
}

void driver_main(struct boot_info *boot)
{
    api_ptr = &api;
    (void)boot;
    uhci_register_ptrs();
    ehci_register_ptrs();
    umsc_register_ptrs();
    api_ptr->usb_start = device_usb_start;
    descr_api_ptr = &descr_api;
    descr_api_ptr->type       = DESCR_USB_DRIVER;
    descr_api_ptr->struct_ptr     = (void *)&api;
}

description_driver_api_t *driver_get_api(void)
{
    return descr_api_ptr;
}
