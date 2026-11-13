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
 * EHCI-драйвер для ArheOS
 *
 *  1. Раскладка QH/qTD приведена к EHCI spec (Linux: struct ehci_qh_hw /
 *     struct ehci_qtd в ehci.h)
 *  2. Токен qTD: PID 2-битный код в bits 9:8 (0=OUT,1=IN,2=SETUP).
 *     Halted = бит 6
 *     (Linux: QTD_PID(tok) == (tok>>8)&3, QTD_STS_HALT = 1<<6.)
 *  3. Все ссылки QH->QH в async-кольце имеют Type-биты = 01b (бит 1)
 *     (Linux: QH_NEXT() | Q_TYPE_QH, Q_TYPE_QH = 1<<1.) QEMU игнорирует
 *     тип, реальное железо — нет
 *  4. Idle-состояние overlay = HALT (бит 6), как у reclamation head в
 *     Linux (ehci_init: hw->hw_token = QTD_STS_HALT)
 *  5. BIOS->OS handoff по EECP (USBLEGSUP/USBLEGCTLSTS в PCI config
 *     space), как в Linux pci-quirks.c ehci_bios_handoff()
 *  6. Все DMA-структуры и transfer-буферы гоняются через uncached
 *     bounce-память (usb_dma_alloc мапит uncached в ядре)
 *     обычные буферы вызова (стек драйвера) копируются в bounce
 *  7. Перед каждой модификацией QH/qTD асинхронное расписание
 *     останавливается (ASE=0, ждём ASS=0) и стартуется заново —
 *     безопасно и для QEMU, и для реального HC
 *  8. Реализован bulk transfer (нужен для MSC/BOT)
 */

#include "include/usb_ehci.h"
#include "include/usb_driver.h"
#include "include/usb_core.h"
#include "include/usb_uhci.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"


ehci_hc_t ehci_hcs[4];
int ehci_hc_count = 0;

uint8_t ehci_dev_speed[128] = {0};

//Пул QH для control transfers (вращается; см. комментарий ниже)
static ehci_qh_t  *ctrl_qh_pool[EHCI_CTRL_QH_POOL];
static uint32_t    ctrl_qh_pool_phys[EHCI_CTRL_QH_POOL];
static int         ctrl_qh_pool_next = 0;

//Async head (reclamation QH) — token = HALT, как у Linux
static ehci_qh_t  *async_head_ptr  = NULL;
static uint32_t    async_head_phys = 0;

//Control qTD pool
static ehci_qtd_t *ctrl_qtd_ptr    = NULL;
static uint32_t    ctrl_qtd_phys   = 0;

//Bulk QH + qTD pool
static ehci_qh_t  *bulk_qh_ptr     = NULL;
static uint32_t    bulk_qh_phys    = 0;
static ehci_qtd_t *bulk_qtd_ptr    = NULL;
static uint32_t    bulk_qtd_phys   = 0;

//Periodic frame list (1024 x 4B = страница). PSE=0, но база обязана
//указывать на валидную таблицу — Linux всегда пишет frame_list перед run
static volatile uint32_t *ehci_frame_list  = NULL;
static uint32_t    ehci_frame_list_phys = 0;

//Uncached bounce-буферы для transfer-данных. Все данные, которые HC
//читает/пишет DMA, идут только через них: буферы вызывающего кода
//(стек драйвера, кэшируемая RAM) копируются туда-обратно
static uint8_t    *ctl_bounce_ptr  = NULL;
static uint32_t    ctl_bounce_phys = 0;
static uint8_t    *bulk_bounce_ptr  = NULL;
static uint32_t    bulk_bounce_phys = 0;

static uint32_t mmio_read(ehci_hc_t *ehc, uint32_t off)
{
    return *(volatile uint32_t*)(ehc->op_base + off);
}
static void mmio_write(ehci_hc_t *ehc, uint32_t off, uint32_t val)
{
    *(volatile uint32_t*)(ehc->op_base + off) = val;
}

//Пауза на n тиков (1 тик PIT = 10 мс, pit_init: 1193180/100 = 100 Гц).
//Тики не зависят от частоты CPU. Safety-net по итерациям pause — если
//тики ещё не тикают (PIT/IRQ0 не готовы), выходим по счётчику.
static void ehci_msleep(uint32_t ticks)
{
    if (ticks == 0) return;
    uint32_t t0 = api_ptr->get_ticks();
    uint32_t safety = 300000u * ticks;
    while (((api_ptr->get_ticks() - t0) < ticks) && (safety-- > 0))
        __asm__ __volatile__("pause");
}

//---------------- Probe ----------------

int ehci_probe_all(void)
{
    ehci_hc_count = 0;
    for (uint16_t bus = 0; bus < 256; bus++)
    {
        for (uint8_t dev = 0; dev < 32; dev++)
        {
            for (uint8_t func = 0; func < 8; func++)
            {
                if (ehci_hc_count >= 4) return ehci_hc_count;

                uint32_t id = pci_read32((uint8_t)bus, dev, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                uint32_t class = pci_read32((uint8_t)bus, dev, func, 0x08);
                uint8_t base_cls = (class >> 24) & 0xFF;
                uint8_t sub_cls  = (class >> 16) & 0xFF;
                uint8_t prog_if  = (class >> 8) & 0xFF;

                if (base_cls != PCI_CLASS_SERIAL_BUS ||
                        sub_cls  != PCI_SUBCLASS_USB ||
                        prog_if  != 0x20) continue;

                uint32_t bar0 = pci_read32((uint8_t)bus, dev, func, 0x10);
                if ((bar0 & 0x01) || (bar0 == 0xFFFFFFFF)) continue;
                uint32_t mmio = bar0 & 0xFFFFFFF0;
                if (!mmio) continue;

                uint32_t cmd = pci_read32((uint8_t)bus, dev, func, 0x04);
                cmd |= 0x06;   //Memory Space + Bus Master (DMA обязателен)
                pci_write32((uint8_t)bus, dev, func, 0x04, cmd);

                //Размер BAR по classic size-probing
                pci_write32((uint8_t)bus, dev, func, 0x10, 0xFFFFFFFFu);
                uint32_t sz_raw = pci_read32((uint8_t)bus, dev, func, 0x10);
                pci_write32((uint8_t)bus, dev, func, 0x10, bar0);
                uint32_t bar_size = (sz_raw & 0xFFFFFFF0u);
                bar_size = (~bar_size + 1u) & bar_size;
                if (bar_size < 0x100) bar_size = 0x1000;

                //Presence detect с LPC-моста (ICH6): какие порты живые
                uint32_t ptd = pci_read32(0, 31, 0, 0xE2);
                if (api_ptr && api_ptr->load_screen_add_string)
                    api_ptr->fake_load_screen_add_string(
                        "EHCI probe %x:%x.%x VID=%x BAR=%x size=%x PTD=%x",
                        bus, dev, func, (id & 0xFFFF), mmio, bar_size, ptd);
                //for (volatile int i = 0; i < 4000; i++) __asm__ __volatile__("pause"); //дать железу устояться
                //ehci_msleep(8);

                ehci_hc_t *ehc = &ehci_hcs[ehci_hc_count++];
                memset(ehc, 0, sizeof(*ehc));
                ehc->mmio_base = mmio;
                ehc->mmio_size = bar_size;
                ehc->pci_bus  = (uint8_t)bus;
                ehc->pci_dev  = dev;
                ehc->pci_func = func;
            }
        }
    }
    return ehci_hc_count;
}

//IAA Doorbell: заставляет HC перечитать async-кольцо с нуля и подтвердить это флагом STS.IAA.
//Нужен после каждой смены async_head->horiz: ICH6 кэширует QH по адресу и без IAA может не заметить линковку/анлинковку.
static void ehci_async_advance(ehci_hc_t *ehc)
{
    uint32_t cmd = mmio_read(ehc, EHCI_USBCMD);
    mmio_write(ehc, EHCI_USBCMD, cmd | (1u << 6));  //IAAC (bit 6)
    uint32_t t0 = api_ptr->get_ticks();
    while ((api_ptr->get_ticks() - t0) < 100)
    {
        uint32_t sts = mmio_read(ehc, EHCI_USBSTS);
        if (sts & (1u << 5))                 //IAA (bit 5) — HC прошёл точку
        {
            mmio_write(ehc, EHCI_USBSTS, (1u << 5));  //W1C
            return;
        }
        __asm__ __volatile__("pause");
    }
    if (api_ptr && api_ptr->load_screen_add_string)
        api_ptr->load_screen_add_string("EHCI IAA timeout STS=%x",
                                        mmio_read(ehc, EHCI_USBSTS));
}

//---------------- DMA structures ----------------

//Idle-состояние QH: overlay с HALT (бит 6): hw->hw_token = QTD_STS_HALT.
static void ehci_qh_set_idle(ehci_qh_t *qh)
{
    qh->cur_qtd  = LINK_T;
    qh->next_qtd = LINK_T;
    qh->alt_qtd  = LINK_T;
    qh->token    = QTD_STS_HALT;
    for (int i = 0; i < 5; i++)
    {
        qh->buf[i] = 0;
        qh->buf_hi[i] = 0;
    }
}

int ehci_alloc_structures(void)
{
    uint32_t phys;
    if (async_head_ptr) return 0;

    //async head QH (reclamation, H=1)
    async_head_ptr = (ehci_qh_t*)usb_dma_alloc(sizeof(ehci_qh_t), &phys);
    if (!async_head_ptr || ((uint32_t)async_head_ptr & 0x1F) != 0) return -1;
    async_head_phys = phys;
    memset(async_head_ptr, 0, sizeof(ehci_qh_t));

    //Пул control QH
    for (int i = 0; i < EHCI_CTRL_QH_POOL; i++)
    {
        ctrl_qh_pool[i] = (ehci_qh_t*)usb_dma_alloc(sizeof(ehci_qh_t), &phys);
        if (!ctrl_qh_pool[i] || ((uint32_t)ctrl_qh_pool[i] & 0x1F) != 0) return -2;
        ctrl_qh_pool_phys[i] = phys;
        memset(ctrl_qh_pool[i], 0, sizeof(ehci_qh_t));
    }
    ctrl_qh_pool_next = 0;

    //control qTD pool
    ctrl_qtd_ptr = (ehci_qtd_t*)usb_dma_alloc(EHCI_CTRL_QTD_COUNT * sizeof(ehci_qtd_t), &phys);
    if (!ctrl_qtd_ptr || ((uint32_t)ctrl_qtd_ptr & 0x1F) != 0) return -3;
    ctrl_qtd_phys = phys;
    memset(ctrl_qtd_ptr, 0, EHCI_CTRL_QTD_COUNT * sizeof(ehci_qtd_t));

    //bulk QH
    bulk_qh_ptr = (ehci_qh_t*)usb_dma_alloc(sizeof(ehci_qh_t), &phys);
    if (!bulk_qh_ptr || ((uint32_t)bulk_qh_ptr & 0x1F) != 0) return -4;
    bulk_qh_phys = phys;
    memset(bulk_qh_ptr, 0, sizeof(ehci_qh_t));

    //bulk qTD pool
    bulk_qtd_ptr = (ehci_qtd_t*)usb_dma_alloc(EHCI_BULK_QTD_COUNT * sizeof(ehci_qtd_t), &phys);
    if (!bulk_qtd_ptr || ((uint32_t)bulk_qtd_ptr & 0x1F) != 0) return -5;
    bulk_qtd_phys = phys;
    memset(bulk_qtd_ptr, 0, EHCI_BULK_QTD_COUNT * sizeof(ehci_qtd_t));

    //Periodic frame list: 1024 записи, все Terminate. PSE выключен, но база должна быть валидной
    ehci_frame_list = (volatile uint32_t*)usb_dma_alloc(4096, &phys);
    if (!ehci_frame_list) return -6;
    ehci_frame_list_phys = phys;
    for (int i = 0; i < 1024; i++) ehci_frame_list[i] = LINK_T;

    //Uncached bounce-буферы для transfer-данных
    ctl_bounce_ptr = (uint8_t*)usb_dma_alloc(4096, &phys);
    if (!ctl_bounce_ptr) return -7;
    ctl_bounce_phys = phys;

    bulk_bounce_ptr = (uint8_t*)usb_dma_alloc(EHCI_BULK_POOL_SIZE, &phys);
    if (!bulk_bounce_ptr) return -8;
    bulk_bounce_phys = phys;
    if (bulk_bounce_phys & 0xFFF) return -9;   // нужен page-aligned (qTD по 4KB)

    //Кольцо пустое: async_head замыкается на себя. Control/bulk QH
    //в кольцо не входят — они линкуются только на время transfer'а
    //(см. ehci_control_transfer/ehci_bulk_transfer). Это устраняет
    //залипание ASS=1 на ICH6: HC в reclamation на async_head, ASE=1,
    //никаких пауз не требуется
    async_head_ptr->horiz    = async_head_phys | Q_TYPE_QH;
    async_head_ptr->ep_char = QH_EP_H | QH_EP_SPD_HS | QH_EP_MAXPKT(64);
    async_head_ptr->ep_cap  = QH_CAP_MULT(1);
    //async_head — reclamation point (H=1), но не HALTED: пустой, но "живой".
    //Иначе QEMU/ICH6 считают его idle и не идут в horiz (новый линкованный QH никогда не обрабатывается).
    async_head_ptr->cur_qtd  = LINK_T;
    async_head_ptr->next_qtd = LINK_T;
    async_head_ptr->alt_qtd  = LINK_T;
    async_head_ptr->token    = 0;
    for (int i = 0; i < 5; i++)
    {
        async_head_ptr->buf[i] = 0;
        async_head_ptr->buf_hi[i] = 0;
    }

    for (int i = 0; i < EHCI_CTRL_QH_POOL; i++)
    {
        ehci_qh_t *qh = ctrl_qh_pool[i];
        qh->horiz    = async_head_phys | Q_TYPE_QH;   //всегда возврат на head
        qh->ep_char  = QH_EP_SPD_HS | QH_EP_MAXPKT(64);
        qh->ep_cap   = QH_CAP_MULT(1);   //Mult=0 зарезервирован (Linux ставит 1)
        ehci_qh_set_idle(qh);
    }

    bulk_qh_ptr->horiz    = async_head_phys | Q_TYPE_QH;
    bulk_qh_ptr->ep_char  = QH_EP_SPD_HS | QH_EP_MAXPKT(512);
    bulk_qh_ptr->ep_cap   = QH_CAP_MULT(1);
    ehci_qh_set_idle(bulk_qh_ptr);
    //Bulk QH тоже в кольце: head -> ... -> bulk -> head
    //хвост control-пула замыкаем на bulk, bulk — на head
    ctrl_qh_pool[EHCI_CTRL_QH_POOL-1]->horiz = bulk_qh_phys | Q_TYPE_QH;

    return 0;
}

//---------------- Async schedule on/off ----------------

//Остановить асинхронное расписание и дождаться ASS=0. Все модификации
//QH/qTD делаются только между pause/resume — иначе реальный HC может
//прочитать наполовину изменённый QH (Linux трогает hw-поля только у
//QH_STATE_IDLE, мы добиваемся того же остановкой расписания)
static int ehci_async_pause(ehci_hc_t *ehc)
{
    uint32_t cmd = mmio_read(ehc, EHCI_USBCMD);
    if (!(cmd & CMD_ASE)) return 0;
    mmio_write(ehc, EHCI_USBCMD, cmd & ~CMD_ASE);
    uint32_t t0 = api_ptr->get_ticks();
    while ((api_ptr->get_ticks() - t0) < 100)        // 1 сек
    {
        if (!(mmio_read(ehc, EHCI_USBSTS) & STS_ASS)) return 0;
        __asm__ __volatile__("pause");
    }
    if (api_ptr && api_ptr->load_screen_add_string)
        api_ptr->load_screen_add_string("EHCI ASS pause timeout STS=%x",
                                        mmio_read(ehc, EHCI_USBSTS));
    return 0;   //Не -1: ASE уже снят, HC новых транзакций не запустит
}

static int ehci_async_resume(ehci_hc_t *ehc)
{
    uint32_t cmd = mmio_read(ehc, EHCI_USBCMD);
    if (cmd & CMD_ASE) return 0;
    mmio_write(ehc, EHCI_ASYNCLISTADDR, 0);
    __asm__ __volatile__("mfence" ::: "memory");
    mmio_write(ehc, EHCI_ASYNCLISTADDR, async_head_phys);
    __asm__ __volatile__("mfence" ::: "memory");
    mmio_write(ehc, EHCI_USBCMD, cmd | CMD_ASE);
    uint32_t t0 = api_ptr->get_ticks();
    while ((api_ptr->get_ticks() - t0) < 100)        // 1 сек
    {
        if (mmio_read(ehc, EHCI_USBSTS) & STS_ASS) return 0;
        __asm__ __volatile__("pause");
    }
    if (api_ptr && api_ptr->load_screen_add_string)
        api_ptr->load_screen_add_string("EHCI ASS resume timeout STS=%x",
                                        mmio_read(ehc, EHCI_USBSTS));
    return 0;   //не -1, HC всё равно начнёт обход, как только сможет
}

//---------------- BIOS handoff (Linux pci-quirks.c) ----------------

//USBLEGSUP лежит в PCI CONFIG SPACE по байтовому смещению EECP из
//HCCPARAMS. bit16 = HC BIOS Owned (RO), bit24 = HC OS Owned.
//После передачи владения всегда пишем 0 в USBLEGCTLSTS (отключаем
//все EHCI SMI). Если BIOS не отпустил за 1с — форсируем
static void ehci_bios_handoff(ehci_hc_t *ehc, uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t hcc  = *(volatile uint32_t*)(ehc->mmio_base + EHCI_HCCPARAMS);
    uint8_t  eecp = (hcc >> 8) & 0xFF;
    int      count = 64;

    while (eecp && count--)
    {
        uint32_t cap = pci_read32(bus, dev, func, eecp);
        if ((cap & 0xFF) == 1)     //Capability ID 1 = USB Legacy Support
        {
            if (cap & (1u << 16))  //BIOS владеет контроллером
            {
                //Просим владение: OS Owned Semaphore (bit 24)
                pci_write32(bus, dev, func, eecp, cap | (1u << 24));
                uint32_t t0 = api_ptr->get_ticks();
                uint32_t t;
                do
                {
                    cap = pci_read32(bus, dev, func, eecp);
                    t = api_ptr->get_ticks();
                }
                while ((cap & (1u << 16)) && (t - t0) < 100);   // ~1 сек

                if (cap & (1u << 16))
                {
                    //BIOS не среагировал (нет SMM-обработчика) — форсируем
                    pci_write32(bus, dev, func, eecp, cap & ~(1u << 16));
                    api_ptr->load_screen_add_string("EHCI handoff forced (BIOS bug)");
                }
                else
                {
                    api_ptr->load_screen_add_string("EHCI handoff ok");
                    //ehci_msleep(12);   // 80 мс
                }
                //Linux: если контроллером владел BIOS — CF=0, порты
                //временно уходят на companion до нашего CF=1
                mmio_write(ehc, EHCI_CONFIGFLAG, 0);
            }
            //Отключить ВСЕ EHCI SMI (иначе SMI-шторм без обработчика)
            pci_write32(bus, dev, func, eecp + 4, 0);
            return;
        }
        eecp = (cap >> 8) & 0xFF;  //следующая extended capability
    }
}

//---------------- Init ----------------

int ehci_init(ehci_hc_t *ehc)
{
    if (!ehc) return -1;

    //Map MMIO uncached (размер — по реальному BAR)
    uint32_t map_size = (ehc->mmio_size + 0xFFF) & ~0xFFFu;
    if (map_size < 0x1000) map_size = 0x1000;
    if (api_ptr->vmm_map_pages_uncached_ptr)
        api_ptr->vmm_map_pages_uncached_ptr(ehc->mmio_base, map_size);
    else if (api_ptr->vmm_map_pages_ptr)
        api_ptr->vmm_map_pages_ptr(ehc->mmio_base, map_size);
    else
        return -1;

    uint8_t cap_len = *(volatile uint8_t*)(ehc->mmio_base + EHCI_CAPLENGTH);
    if (cap_len == 0 || cap_len == 0xFF)
    {
        api_ptr->load_screen_add_string("EHCI cap_len=%x (bad) mmio=%x",
                                        cap_len, ehc->mmio_base);
        return -2;
    }
    ehc->op_base = ehc->mmio_base + cap_len;

    uint32_t hcs = *(volatile uint32_t*)(ehc->mmio_base + EHCI_HCSPARAMS);
    ehc->n_ports = hcs & 0x0F;
    if (ehc->n_ports == 0 || ehc->n_ports > 15) ehc->n_ports = 15;

    api_ptr->load_screen_add_string("EHCI build=%d", EHCI_DRV_BUILD);

    api_ptr->load_screen_add_string("EHCI pre: CAP=%x HCS=%x CMD=%x STS=%x CF=%x",
                                    cap_len, hcs,
                                    mmio_read(ehc, EHCI_USBCMD),
                                    mmio_read(ehc, EHCI_USBSTS),
                                    mmio_read(ehc, EHCI_CONFIGFLAG));
    //ehci_msleep(20);   // 80 мс

    //1. BIOS -> OS handoff до halt/reset
    ehci_bios_handoff(ehc, ehc->pci_bus, ehc->pci_dev, ehc->pci_func);

    //2. Остановить HC перед сбросом (Linux ehci_halt)
    uint32_t cmd = mmio_read(ehc, EHCI_USBCMD);
    cmd &= ~(CMD_RUN | CMD_ASE | CMD_PSE);
    mmio_write(ehc, EHCI_USBCMD, cmd);
    int halted = 0;
    for (uint32_t i = 0; i < 2000000; i++)
    {
        if (mmio_read(ehc, EHCI_USBSTS) & STS_HCHALTED)
        {
            halted = 1;
            break;
        }
    }
    if (!halted)
    {
        api_ptr->load_screen_add_string("EHCI halt stuck: CMD=%x STS=%x",
                                        mmio_read(ehc, EHCI_USBCMD),
                                        mmio_read(ehc, EHCI_USBSTS));
        return -3;
    }

    //3. HCRESET (Linux ehci_reset: handshake 250ms)
    mmio_write(ehc, EHCI_USBCMD, CMD_HCRESET);
    int rst_done = 0;
    for (uint32_t i = 0; i < 4000000; i++)
    {
        if (!(mmio_read(ehc, EHCI_USBCMD) & CMD_HCRESET))
        {
            rst_done = 1;
            break;
        }
    }
    if (!rst_done)
    {
        api_ptr->load_screen_add_string("EHCI HCRESET stuck: CMD=%x STS=%x",
                                        mmio_read(ehc, EHCI_USBCMD),
                                        mmio_read(ehc, EHCI_USBSTS));
        return -4;
    }

    //4. Базовая настройка
    mmio_write(ehc, EHCI_USBSTS, mmio_read(ehc, EHCI_USBSTS)); //W1C сброс
    mmio_write(ehc, EHCI_USBINTR, 0);      //режим опроса
    mmio_write(ehc, EHCI_CTRLDSSEG, 0);    //всё < 4GB

    //5. Списки расписаний
    mmio_write(ehc, EHCI_PERIODICLIST, ehci_frame_list_phys);
    mmio_write(ehc, EHCI_ASYNCLISTADDR, async_head_phys);

    //6. Запуск: RS + ASE, ITC = 1ms (Linux: temp = 1 << 16)
    cmd = mmio_read(ehc, EHCI_USBCMD);
    cmd &= ~(CMD_HCRESET | CMD_PSE);
    cmd &= ~(0x7u << CMD_ITC_SHIFT);        // очистить ITC
    cmd |= CMD_RUN | CMD_ASE | (1u << CMD_ITC_SHIFT);   //ITC=1 (2 us)
    mmio_write(ehc, EHCI_USBCMD, cmd);

    int running = 0;
    for (uint32_t i = 0; i < 4000000; i++)
    {
        if (!(mmio_read(ehc, EHCI_USBSTS) & STS_HCHALTED))
        {
            running = 1;
            break;
        }
    }
    if (!running)
    {
        api_ptr->load_screen_add_string("EHCI didn't start: CMD=%x STS=%x",
                                        mmio_read(ehc, EHCI_USBCMD),
                                        mmio_read(ehc, EHCI_USBSTS));
        return -5;
    }

    //7. CF=1: порты уходят с companion-контроллеров на EHCI (Linux ehci_run)
    mmio_write(ehc, EHCI_CONFIGFLAG, CFG_ROUTE_ALL);
    for (volatile int i = 0; i < 20000; i++) inw(0x80); //дать железу устояться

    //8. Порты: включить питание, сбросить change-биты, показать состояние
    for (uint32_t p = 1; p <= ehc->n_ports; p++)
    {
        uint32_t ps = mmio_read(ehc, EHCI_PORTSC(p));
        uint32_t nw = (ps | PSC_W1C) & ~PSC_PR;
        if (!(nw & PSC_PP)) nw |= PSC_PP;          // Port Power
        if (!(ps & PSC_PO)) mmio_write(ehc, EHCI_PORTSC(p), nw);
        ps = mmio_read(ehc, EHCI_PORTSC(p));
        api_ptr->load_screen_add_string("EHCI p%d PORTSC=%x pspd=%d po=%d",
                                        (int)p, ps,
                                        (int)((ps >> PSC_PSPD_SHIFT) & 3),
                                        (ps & PSC_PO) ? 1 : 0);
        //ehci_msleep(8);
    }

    api_ptr->load_screen_add_string("EHCI started: CMD=%x STS=%x CFG=%x",
                                    mmio_read(ehc, EHCI_USBCMD),
                                    mmio_read(ehc, EHCI_USBSTS),
                                    mmio_read(ehc, EHCI_CONFIGFLAG));
    //ehci_msleep(12);


    ehc->initialized = 1;
    return 0;
}

//---------------- Ports ----------------

uint32_t ehci_port_status(ehci_hc_t *ehc, int port)
{
    return mmio_read(ehc, EHCI_PORTSC(port));
}

int ehci_port_connected(ehci_hc_t *ehc, int port)
{
    return (ehci_port_status(ehc, port) & PSC_CCS) ? 1 : 0;
}

//Сброс порта по USB2 spec / Linux ehci-hub.c:
//  PR=1 на >=10ms (USB_TDRSTR), PR=0, затем ждём PED до 250ms
//  HS-устройство после сброса само выставит PED; FS/LS устройство
//  EHCI отдаст companion-контроллеру (PO=1) — тогда возвращаем -3
int ehci_port_reset(ehci_hc_t *ehc, int port)
{
    uint32_t reg = EHCI_PORTSC(port);
    uint32_t ps0 = mmio_read(ehc, reg);

    if (api_ptr && api_ptr->load_screen_add_string)
        api_ptr->load_screen_add_string("ehci_pr p%d in=%x pspd=%d",
                                        port, ps0,
                                        (int)((ps0 >> PSC_PSPD_SHIFT) & 3));
    //int vd2 = (int)((ps0 >> PSC_PSPD_SHIFT) & 3);
    ehci_msleep(12);

    if (!(ps0 & PSC_CCS)) return -1;

    //Порт принадлежит companion? EHCI его не трогает
    if (ps0 & PSC_PO)
    {
        if (api_ptr && api_ptr->load_screen_add_string)
            api_ptr->load_screen_add_string("ehci_pr p%d owned by companion", port);
        return -3;
    }

    //Change-биты W1C очищаем одновременно со стартом сброса
    mmio_write(ehc, reg, (ps0 | PSC_PR | PSC_W1C) & ~PSC_PED);

    //Держим PR=1 минимум 2 тика = 20 мс (spec требует >=10ms)
    uint32_t t0 = api_ptr->get_ticks();
    while ((api_ptr->get_ticks() - t0) < 2)
    {
        __asm__ __volatile__("pause");
    }

    //Снимаем PR (спека: software снимает reset). PED не трогаем — HS-устройство могло уже включиться
    uint32_t ps = mmio_read(ehc, reg);
    mmio_write(ehc, reg, (ps & ~PSC_PR) | PSC_W1C);

    //Ждём PED (HS-устройство chirp'ует и включается). До 25 тиков = 250 мс
    t0 = api_ptr->get_ticks();
    while ((api_ptr->get_ticks() - t0) < 25)
    {
        ps = mmio_read(ehc, reg);
        if (ps & PSC_PED)
        {
            mmio_write(ehc, reg, ps | PSC_W1C);  //подчистить change-биты
            if (api_ptr && api_ptr->load_screen_add_string)
                api_ptr->load_screen_add_string("ehci_pr p%d ok=%x pspd=%d",
                                                port, ps,
                                                (int)((ps >> PSC_PSPD_SHIFT) & 3));

            //ehci_msleep(12); // 80 мс
            return 0;
        }
    }
    if (api_ptr && api_ptr->load_screen_add_string)
        api_ptr->load_screen_add_string("ehci_pr p%d timeout=%x", port, ps);
    return -2;
}

//---------------- qTD helpers ----------------

//Заполнить buf[0..4] qTD: одна страница на указатель, buf[0] — точный адрес
static void ehci_qtd_fill_bufs(ehci_qtd_t *td, uint32_t phys)
{
    td->buf[0] = phys;   //низкие 12 бит = смещение (QEMU: bufptr[0] & ~0xfffff000)
    for (int i = 1; i < 5; i++)
        td->buf[i] = (phys + (uint32_t)i * EHCI_QTD_PAGE) & 0xFFFFF000u;
}

//Собрать цепочку qTD по 4KB из bounce-пула.
//bphys — физадрес начала данных (page-aligned), len — длина
//Возвращает индекс последнего qTD
static int ehci_build_qtd_chain(ehci_qtd_t *tds, uint32_t pool_phys,
                                uint32_t bphys, uint32_t len,
                                int in, uint32_t first_toggle,
                                int is_control)
{
    uint32_t remain = len;
    uint32_t off = 0;
    uint32_t toggle = first_toggle ? QTD_TOGGLE : 0;
    int qidx = 0;

    if (len == 0)
    {
        //Только status-stage случай обрабатывается вызывающим кодом
        return -1;
    }

    while (remain > 0 && qidx < EHCI_BULK_QTD_COUNT - 1)
    {
        uint32_t chunk = (remain > EHCI_QTD_PAGE) ? EHCI_QTD_PAGE : remain;
        memset(&tds[qidx], 0, sizeof(ehci_qtd_t));
        tds[qidx].alt = LINK_T;                       //короткое чтение -> halt
        ehci_qtd_fill_bufs(&tds[qidx], bphys + off);
        uint32_t tok = (in ? QTD_PID_IN : QTD_PID_OUT) | QTD_CERR_SET |
                       (chunk << 16) | QTD_STS_ACTIVE;
        if (qidx == 0) tok |= toggle;   //toggle только на первый qTD (дальше HC сам)
        tds[qidx].token = tok;
        tds[qidx].next = pool_phys + (uint32_t)(qidx + 1) * sizeof(ehci_qtd_t);
        remain -= chunk;
        off += chunk;
        qidx++;
        if (is_control) break;   //control data — один qTD (<=3584 байт)
    }

    tds[qidx - 1].next = LINK_T;   //последний — terminate
    return qidx - 1;
}

//---------------- Control transfer ----------------

int ehci_control_transfer(ehci_hc_t *ehc, uint8_t addr,
                          const uint8_t *setup,
                          void *buf, uint16_t len, int in, uint16_t maxpkt)
{
    if (!ehc || !setup || !ehc->initialized) return -1;
    if (len > EHCI_CTL_DATA_MAX) return -6;

    //Скорость устройства (заполняется в usb_enumerate: для EHCI root-портов
    //— по биту Port Enable; PSPD bits 27:26 реальный ICH6 не выставляет)
    uint8_t spd = (addr < 128) ? ehci_dev_speed[addr] : 0;
    uint32_t speed_bits;
    uint16_t mps;
    if (spd == 2)           //HS: control EP0 всегда 64
    {
        speed_bits = QH_EP_SPD_HS;
        mps = 64;
    }
    else if (spd == 1)      //LS за root-hub EHCI не бывает (нужен TT)
    {
        speed_bits = QH_EP_SPD_LS;
        mps = 8;
    }
    else                    //FS
    {
        speed_bits = QH_EP_SPD_FS;
        mps = (maxpkt >= 8 && maxpkt <= 64) ? maxpkt : 8;
    }

    //Bounce: setup + данные в некэшированную память
    uint32_t setup_phys = ctl_bounce_phys + 0;
    uint32_t data_phys  = ctl_bounce_phys + 16;

    //Остановить async-расписание
    //if (ehci_async_pause(ehc) != 0) { ehci_async_resume(ehc); return -7;}

    //Одна повторная попытка при таймауте: разовый сбой (устройство ещё
    //не готово после сброса порта, фоновый трафик на общем порту) не должен
    //убивать перечисление. Повторный SETUP легален — по USB-спеке он
    //сбрасывает control-состояние устройства. Диагностика печатается по
    //последней попытке
    int ehci_retried = 0;
ehci_retry:
    memcpy((void*)setup_phys, setup, 8);
    if (len > 0 && !in && buf)
        memcpy((void*)data_phys, buf, len);

    //Готовим qTD-цепочку: SETUP -> DATA -> STATUS
    ehci_qtd_t *tds = ctrl_qtd_ptr;
    memset(tds, 0, EHCI_CTRL_QTD_COUNT * sizeof(ehci_qtd_t));

    int qidx = 0;
    //SETUP (всегда DATA0; при D=1 в QH toggle определяется битом 31 = 0)
    tds[qidx].alt = LINK_T;
    ehci_qtd_fill_bufs(&tds[qidx], setup_phys);
    tds[qidx].token = QTD_PID_SETUP | QTD_CERR_SET | (8u << 16) | QTD_STS_ACTIVE;
    tds[qidx].next = ctrl_qtd_phys + (uint32_t)(qidx + 1) * sizeof(ehci_qtd_t);
    qidx++;

    //DATA (DATA1; zero-length data stage пропускаем)
    int data_idx = -1;
    if (len > 0)
    {
        tds[qidx].alt = LINK_T;
        ehci_qtd_fill_bufs(&tds[qidx], data_phys);
        tds[qidx].token = (in ? QTD_PID_IN : QTD_PID_OUT) | QTD_CERR_SET |
                          ((uint32_t)len << 16) | QTD_STS_ACTIVE | QTD_TOGGLE;
        tds[qidx].next = ctrl_qtd_phys + (uint32_t)(qidx + 1) * sizeof(ehci_qtd_t);
        data_idx = qidx;
        qidx++;
    }

    //STATUS (направление обратное data-стадии, всегда DATA1)
    tds[qidx].alt = LINK_T;
    tds[qidx].token = (in ? QTD_PID_OUT : QTD_PID_IN) | QTD_CERR_SET |
                      QTD_STS_ACTIVE | QTD_TOGGLE | QTD_IOC;
    tds[qidx].next = LINK_T;
    int status_idx = qidx;
    qidx++;

    //alt_next у control-qTD указывает на STATUS-qTD (как в Linux ehci-q.c):
    //короткий пакет data-стадии переводит HC на status-стадию, а не в
    //Terminate. Любое чтение, где устройство отдаёт меньше запрошенного
    //(напр. конфиг-дескриптор), короткое по определению
    {
        uint32_t status_phys = ctrl_qtd_phys +
                               (uint32_t)status_idx * sizeof(ehci_qtd_t);
        tds[0].alt = status_phys;
        if (data_idx >= 0) tds[data_idx].alt = status_phys;
    }

    //Берём QH из пула и настраиваем overlay
    int qh_idx = ctrl_qh_pool_next;
    ehci_qh_t *qh      = ctrl_qh_pool[qh_idx];
    uint32_t   qh_phys = ctrl_qh_pool_phys[qh_idx];
    ctrl_qh_pool_next = (ctrl_qh_pool_next + 1) % EHCI_CTRL_QH_POOL;
    //qh->horiz уже указывает на async_head (задан в alloc_structures),
    //не трогаем — QH после завершения вернётся в пустое кольцо
    qh->ep_char = QH_EP_ADDR(addr) | QH_EP_NUM(0) | speed_bits |
                  QH_EP_D |                 // toggle из DCR qTD (как Linux для control)
                  QH_EP_MAXPKT(mps) | QH_EP_RL(4);
    if (spd != 2) qh->ep_char |= QH_EP_C;   // C=1 только для FS/LS control
    qh->ep_cap   = QH_CAP_MULT(1);
    //Overlay — точное зеркало tds[0]. Реальный HC после исполнения
    //overlay-транзакции делает cur_qtd := overlay.next_qtd, поэтому
    //next_qtd обязан указывать на следующий qTD (tds[0].next), а не на
    //tds[0] — иначе на реальном железе SETUP закрутится в цикл
    qh->cur_qtd  = ctrl_qtd_phys;
    qh->next_qtd = tds[0].next;
    qh->alt_qtd  = tds[0].alt;
    qh->token    = tds[0].token;
    for (int i = 0; i < 5; i++)
    {
        qh->buf[i]    = tds[0].buf[i];
        qh->buf_hi[i] = 0;
    }

    //__sync_synchronize();
    __asm__ __volatile__("mfence" ::: "memory");

    //Линкуем QH в async-кольцо
    async_head_ptr->horiz    = qh_phys | Q_TYPE_QH;
    __asm__ __volatile__("mfence" ::: "memory");
    ehci_async_advance(ehc);   //HC перечитает кольцо и увидит QH

    if (api_ptr && api_ptr->load_screen_add_string)
    {
        api_ptr->load_screen_add_string("ehci link: ah=%x qh=%x qh0=%x sts=%x",
                                        async_head_ptr->horiz,
                                        qh_phys,
                                        tds[0].token,
                                        mmio_read(ehc, EHCI_USBSTS));
    }
    //ehci_msleep(8);

    int result;
    uint32_t t0 = api_ptr->get_ticks();
    for (;;)
    {
        uint32_t tok = tds[status_idx].token;
        if (!(tok & QTD_STS_ACTIVE))
        {
            result = 0;
            break;
        }
        //Цепочка умерла раньше статусной стадии (XactErr/STALL/... на
        //SETUP или DATA) — ждать статус бессмысленно, выходим сразу
        //result=0: разбор реально случившегося сделает общий пост-цикл
        //ниже по tds[0..status_idx) — он же напечатает диагностику
        {
            uint32_t tk0 = tds[0].token;
            if ((tk0 & QTD_STS_HALT) && !(tk0 & QTD_STS_ACTIVE))
            {
                result = 0;
                break;
            }
        }
        if (data_idx >= 0)
        {
            uint32_t tkd = tds[data_idx].token;
            if ((tkd & QTD_STS_HALT) && !(tkd & QTD_STS_ACTIVE))
            {
                result = 0;
                break;
            }
        }
        if ((api_ptr->get_ticks() - t0) >= 500)     //5 сек
        {
            result = -3;
            break;
        }
        __asm__ __volatile__("pause");
    }

    //Unlink: возвращаем async_head в пустое кольцо
    async_head_ptr->horiz    = async_head_phys | Q_TYPE_QH;
    __asm__ __volatile__("mfence" ::: "memory");
    ehci_async_advance(ehc);   //HC гарантированно отошёл от QH

    if (result == 0)
    {
        //Сначала проверяем промежуточные qTD
        for (int i = 0; i < status_idx; i++)
        {
            uint32_t tok = tds[i].token;
            if (tok & (QTD_STS_ACTIVE))
            {
                result = -3;    //не дошёл
                break;
            }
            if (tok & QTD_STS_HALT)
            {
                if (QTD_CERR(tok))       result = -2;  //STALL
                else if (tok & QTD_STS_XACT) result = -3;  //таймаут/CRC
                else if (tok & QTD_STS_DBE)  result = -4;
                else if (tok & QTD_STS_BABBLE) result = -5;
                else                     result = -3;
                break;
            }
        }
    }

    //Таймаут — одна повторная попытка
    if (result == -3 && !ehci_retried)
    {
        ehci_retried = 1;
        if (api_ptr && api_ptr->load_screen_add_string)
            api_ptr->load_screen_add_string("ehci_ctrl: timeout, retry once");
        ehci_msleep(1);
        goto ehci_retry;
    }



    if (result == 0 && len > 0 && in && buf)
    {
        uint32_t last = tds[data_idx].token;
        uint16_t rem  = (uint16_t)QTD_BYTES(last);
        result = (rem > len) ? (int)len : (int)(len - rem);
        if (result > 0) memcpy(buf, (void*)data_phys, (uint32_t)result);
        else result = 0;
    }
    else if (result == 0 && len == 0)
    {
        result = 0;
    }

    //Убираем за собой: QH в idle, qTD деактивировать
    //Сначала зафиксируем реальное состояние, до очистки
    if (result < 0)
    {
        if(api_ptr && api_ptr->load_screen_add_string)
            api_ptr->load_screen_add_string("ehci_ctrl(pre) r=%d spd=%d qh=%x q0=%x qd=%x qs=%x",
                                            result, (int)spd,
                                            qh->token,
                                            tds[0].token,
                                            (data_idx >= 0) ? tds[data_idx].token : 0,
                                            tds[status_idx].token);

        //Состояние HC: жив ли async-план (ASS=bit15, HCHalted=bit12,
        //HostSysErr=bit4), докуда дошёл HC внутри QH (cur/next), чем для него выглядит QH (ep_char)
        //и куда пишутся данные (dbuf)
        api_ptr->load_screen_add_string("ehci_ctrl(hc) cmd=%x sts=%x cur=%x nxt=%x ep=%x dbuf=%x",
                                        mmio_read(ehc, EHCI_USBCMD),
                                        mmio_read(ehc, EHCI_USBSTS),
                                        qh->cur_qtd,
                                        qh->next_qtd,
                                        qh->ep_char,
                                        (data_idx >= 0) ? tds[data_idx].buf[0] : 0);
        //Дамп bounce-памяти: что реально лежало в DRAM, когда HC читал
        //setup-пакет (эталон: 80 06 00 01 00 00 08 00), и что устройство
        //успело передать в data-стадию. Если setup в памяти не равен эталону —
        //DMA-память кто-то портит; если равен, а rcvd нулевой — устройство
        //реально не отвечает на IN.
        {
            uint8_t *sb = (uint8_t*)setup_phys;
            api_ptr->load_screen_add_string("ehci_ctrl(setup) %x %x %x %x %x %x %x %x",
                                            sb[0], sb[1], sb[2], sb[3],
                                            sb[4], sb[5], sb[6], sb[7]);
            if (data_idx >= 0)
            {
                uint8_t *db = (uint8_t*)data_phys;
                api_ptr->load_screen_add_string("ehci_ctrl(rcvd) %x %x %x %x %x %x %x %x",
                                                db[0], db[1], db[2], db[3],
                                                db[4], db[5], db[6], db[7]);
            }
        }
        //Порты на момент таймаута: не отнял ли порт companion (PO=1,
        //бит 13), не отвалился ли (CCS=0, бит 0), не упал ли (PED=0, бит 2)
        {
            uint32_t q1 = (ehc->n_ports >= 1) ? mmio_read(ehc, EHCI_PORTSC(1)) : 0;
            uint32_t q2 = (ehc->n_ports >= 2) ? mmio_read(ehc, EHCI_PORTSC(2)) : 0;
            uint32_t q3 = (ehc->n_ports >= 3) ? mmio_read(ehc, EHCI_PORTSC(3)) : 0;
            uint32_t q4 = (ehc->n_ports >= 4) ? mmio_read(ehc, EHCI_PORTSC(4)) : 0;
            uint32_t q5 = (ehc->n_ports >= 5) ? mmio_read(ehc, EHCI_PORTSC(5)) : 0;
            uint32_t q6 = (ehc->n_ports >= 6) ? mmio_read(ehc, EHCI_PORTSC(6)) : 0;
            api_ptr->load_screen_add_string("ehci_ctrl(ports) %x %x %x %x %x %x",
                                            q1, q2, q3, q4, q5, q6);
        }
    }

    //Убираем за собой: QH в idle, qTD деактивировать
    ehci_qh_set_idle(qh);
    for (int i = 0; i < EHCI_CTRL_QTD_COUNT; i++)
        tds[i].token &= ~QTD_STS_ACTIVE;
    //__sync_synchronize();
    __asm__ __volatile__("mfence" ::: "memory");

    //ehci_async_resume(ehc);

    if (result < 0)
    {
        if(api_ptr && api_ptr->load_screen_add_string)
            api_ptr->load_screen_add_string("ehci_ctrl r=%d st=%x d=%x",
                                            result,
                                            tds[status_idx].token,
                                            (data_idx >= 0) ? tds[data_idx].token : 0);
    }
    return result;
}

//---------------- Bulk transfer ----------------

//Bulk только для HS-устройств (FS за root-hub EHCI требует split-транзакций
//через companion — это путь UHCI-драйвера)
int ehci_bulk_transfer(ehci_hc_t *ehc, uint8_t addr, uint8_t ep,
                       uint16_t maxpkt, uint8_t *toggle,
                       void *buf, uint32_t len, int in)
{
    if (!ehc || !buf || len == 0 || !toggle || !ehc->initialized) return -1;
    if (maxpkt == 0 || maxpkt > 512) maxpkt = 512;

    uint8_t ep_num = ep & 0x0F;
    uint32_t total_done = 0;
    uint32_t total_packets = 0;
    uint32_t remain = len;
    uint32_t off = 0;
    int result = 0;

    while (remain > 0)
    {
        uint32_t chunk = (remain > EHCI_BULK_POOL_SIZE) ? EHCI_BULK_POOL_SIZE : remain;

        //Bounce: OUT — копируем данные в uncached, IN — пишем HC прямо туда
        uint32_t bphys = bulk_bounce_phys;
        if (!in) memcpy((void*)bphys, (uint8_t*)buf + off, chunk);

        //if (ehci_async_pause(ehc) != 0){ result = -7; break; }

        //Цепочка qTD по 4KB (bounce-пул page-aligned => 1 страница на qTD)
        ehci_qtd_t *tds = bulk_qtd_ptr;
        memset(tds, 0, EHCI_BULK_QTD_COUNT * sizeof(ehci_qtd_t));
        int last = ehci_build_qtd_chain(tds, bulk_qtd_phys, bphys, chunk,
                                        in, *toggle ? 1 : 0, 0);
        if (last < 0)
        {
            result = -2;
            break;
        }

        //Bulk QH: D=0 — hardware сам ведёт toggle по qTD-цепочке,
        //стартовое значение кладём в overlay token (бит 31)
        ehci_qh_t *qh = bulk_qh_ptr;
        uint32_t spd_bits = QH_EP_SPD_FS;
        if (ehci_dev_speed[addr] == 2) spd_bits = QH_EP_SPD_HS;
        else if (ehci_dev_speed[addr] == 1) spd_bits = QH_EP_SPD_LS;
        qh->ep_char = QH_EP_ADDR(addr) | QH_EP_NUM(ep_num) | spd_bits |
                      QH_EP_MAXPKT(maxpkt) | QH_EP_RL(4);
        qh->ep_cap   = QH_CAP_MULT(1);
        //Overlay — точное зеркало tds[0]: next_qtd = tds[0].next
        qh->cur_qtd  = bulk_qtd_phys;
        qh->next_qtd = tds[0].next;
        qh->alt_qtd  = tds[0].alt;
        qh->token    = tds[0].token;  //DATA0/DATA1 на первый пакет
        for (int i = 0; i < 5; i++)
        {
            qh->buf[i]    = tds[0].buf[i];
            qh->buf_hi[i] = 0;
        }

        //__sync_synchronize();
        __asm__ __volatile__("mfence" ::: "memory");

        //Линкуем bulk QH
        async_head_ptr->horiz    = bulk_qh_phys | Q_TYPE_QH;
        __asm__ __volatile__("mfence" ::: "memory");
        ehci_async_advance(ehc);

        int done = -3;
        uint32_t t0 = api_ptr->get_ticks();
        for (;;)
        {
            uint32_t tok_last  = tds[last].token;
            uint32_t tok_first = tds[0].token;
            if (!(tok_last & QTD_STS_ACTIVE))
            {
                done = 0;
                break;
            }
            if ((tok_first & QTD_STS_HALT) && !(tok_first & QTD_STS_ACTIVE))
            {
                done = -2;
                break;
            }
            if ((api_ptr->get_ticks() - t0) >= 1000)
            {
                done = -3;
                break;
            }
            __asm__ __volatile__("pause");
        }

        //Unlink
        async_head_ptr->horiz    = async_head_phys | Q_TYPE_QH;
        __asm__ __volatile__("mfence" ::: "memory");
        ehci_async_advance(ehc);

        if (done == 0)
        {
            uint32_t lt = tds[last].token;
            if (lt & QTD_STS_HALT)
            {
                if (QTD_CERR(lt))            result = -2;  //STALL
                else if (lt & QTD_STS_XACT)  result = -3;
                else if (lt & QTD_STS_DBE)   result = -4;
                else if (lt & QTD_STS_BABBLE) result = -5;
                else                          result = -3;
                done = result;
            }
            else
            {
                uint32_t rem = QTD_BYTES(lt);
                uint32_t transferred = chunk - ((rem > chunk) ? chunk : rem);
                if (in) memcpy((uint8_t*)buf + off, (void*)bphys, transferred);
                total_done += transferred;
                total_packets += (transferred + maxpkt - 1) / maxpkt;
                if (transferred < chunk)
                {
                    //Короткая передача — для MSC это ошибка данных
                    done = -6;
                }
            }
        }

        //Снять active, QH в idle, расписание — крутится дальше
        for (int i = 0; i < EHCI_BULK_QTD_COUNT; i++)
            tds[i].token &= ~QTD_STS_ACTIVE;
        ehci_qh_set_idle(qh);
        //__sync_synchronize();
        __asm__ __volatile__("mfence" ::: "memory");
        //ehci_async_resume(ehc);

        if (done < 0)
        {
            if (result == 0) result = done;
            break;
        }

        remain -= chunk;
        off += chunk;
    }

    //Каждый пакет переключает toggle: перевычисляем арифметически
    *toggle ^= (uint8_t)(total_packets & 1);

    if (result < 0) return result;
    return (int)total_done;
}
