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
 * usb_uhci.c - UHCI Host Controller: PCI probe (все контроллеры), reset,
 * frame list, порты, control/bulk transfers.
 *
 * Мультиустройство:
 *  - uhci_probe_all() находит ВСЕ UHCI-функции в PCI (на ICH их обычно
 *    два: USB#1 и USB#2, по 2 root-порта на каждый);
 *  - у каждого HC свой Frame List / QH / TD-кольца — кадры разных
 *    контроллеров идут параллельно, устройства на разных HC не мешают
 *    друг другу;
 *  - все root-порты постоянно висят в кадрах (port-QH очереди с TD,
 *    которые HC "дожигает" за кадр). Между кадрами драйвер hot-patch'ит
 *    status последнего активного TD порта — так HC продолжает опрашивать
 *    порт каждый кадр без участия софта (hotplug работает сам);
 *  - control/bulk очереди включаются в расписание только на время
 *    конкретной транзакции (bulk — один кадр, чтобы NAK-пакеты чужих
 *    устройств не съедали слоты нашего кольца);
 *  - перед SET_ADDRESS порт переживает hard reset (PED=0): выключенный
 *    порт не участвует в расписании HC, поэтому TD других устройств его
 *    не видят, а наше устройство отвечает только на адрес 0.
 *
 * Реальное железо (Panasonic CF-18 / ICH3 и аналоги):
 *  - legacy-режим BIOS: USBCMD.CF=0 при захвате — переводим контроллер
 *    в полный контроль SOF (CF=1), иначе HC не генерирует кадры;
 *  - порты после BIOS могут стоять в SUSPEND / SDE (soft disconnect) —
 *    снимаем при инициализации;
 *  - debug-порты (PORTSC.DS) не трогаем reset'ом, чтобы не угонять
 *    EDB-режим отладочного кабеля;
 *  - USBSTS.HSE (Host System Error) проверяется в каждом цикле ожидания:
 *    при системной ошибке HC останавливается и TD вечно висят ACTIVE —
 *    без проверки мы бы просто зависли.
 */

#include "include/usb_uhci.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

/* ---------------- Статические UHCI-структуры на каждый HC ----------------
   Всё в .bss драйвера (identity map: VA == PA). Динамически размечать
   нельзя — HC требует физического адреса и 4K-выравнивания frame list. */

uint8_t   frame_lists[UHCI_MAX_HC][4096] __attribute__((aligned(4096)));
uhci_qh_t port_qhs[UHCI_MAX_HC][2]       __attribute__((aligned(16)));
uhci_qh_t ctrl_qhs[UHCI_MAX_HC]          __attribute__((aligned(16)));
uhci_qh_t bulk_qhs[UHCI_MAX_HC]          __attribute__((aligned(16)));
uhci_td_t ctrl_rings[UHCI_MAX_HC][UHCI_CTRL_RING] __attribute__((aligned(16)));
uhci_td_t bulk_rings[UHCI_MAX_HC][UHCI_BULK_RING] __attribute__((aligned(16)));
uint8_t   setup_statics[UHCI_MAX_HC][8]  __attribute__((aligned(16)));

/* Позиция головы кольца (следующий свободный TD) — только со стороны
   CPU; HC видит лишь link-поля самих TD. */
static uint32_t ctrl_ring_head[UHCI_MAX_HC];
static uint32_t bulk_ring_head[UHCI_MAX_HC];
static uint32_t ctrl_ring_free[UHCI_MAX_HC];
static uint32_t bulk_ring_free[UHCI_MAX_HC];

uhci_hc_t        uhci_hcs[UHCI_MAX_HC];
uhci_hc_t       *uhci_hc_ptrs[UHCI_MAX_HC];
int              uhci_hc_count = 0;

uint32_t uhci_dbg_hse = 0;
uint32_t uhci_dbg_err = 0;
uint32_t uhci_dbg_timeout = 0;

/* UHCI — bus-master, ему нужен физический адрес. Stack/driver .bss
   identity-mapped (VA==PA), но kmalloc-буферы живут по VA != PA.
   Делаем walk по текущим page tables через CR3. */
uint32_t usb_virt_to_phys(uint32_t va){
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= 0xFFFFF000u;

    uint32_t *pd = (uint32_t*)cr3;
    uint32_t pde = pd[va >> 22];
    if (!(pde & 1)) return va;                 /* not present — fallback */

    uint32_t *pt = (uint32_t*)(pde & 0xFFFFF000u);
    uint32_t pte = pt[(va >> 12) & 0x3FF];
    if (!(pte & 1)) return va;

    return (pte & 0xFFFFF000u) | (va & 0xFFF);
}

/* Привязка указателей колец/QH к слоту hc->index. Идемпотентно. */
static void uhci_bind_ptrs(uhci_hc_t *hc){
    int i = hc->index;
    hc->frame_list = frame_lists[i];
    hc->fl_phys    = (uint32_t)frame_lists[i];
    hc->port_qh    = &port_qhs[i][0];
    hc->port_qh1   = &port_qhs[i][1];
    hc->ctrl_qh    = &ctrl_qhs[i];
    hc->bulk_qh    = &bulk_qhs[i];
    hc->ctrl_ring  = ctrl_rings[i];
    hc->bulk_ring  = bulk_rings[i];
}

/* ---------------- PCI ---------------- */

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off){
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)func <<  8)
                  | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val){
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)func <<  8)
                  | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

int uhci_hc_index(uhci_hc_t *hc){
    if (!hc) return -1;
    return (int)hc->index;
}

/* ---------------- Probe всех UHCI ---------------- */

int uhci_probe_all(void){
    int n = 0;

    for (uint16_t bus = 0; bus < 256 && n < UHCI_MAX_HC; bus++){
        for (uint8_t dev = 0; dev < 32 && n < UHCI_MAX_HC; dev++){

            /* function 0 обязана отвечать, иначе слот пуст */
            uint32_t id0 = pci_read32((uint8_t)bus, dev, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF || (id0 & 0xFFFF) == 0) continue;

            for (uint8_t func = 0; func < 8 && n < UHCI_MAX_HC; func++){
                uint32_t id = (func == 0) ? id0
                            : pci_read32((uint8_t)bus, dev, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) continue;

                uint32_t class = pci_read32((uint8_t)bus, dev, func, 0x08);
                uint8_t prog_if  = (class >>  8) & 0xFF;
                uint8_t sub_cls  = (class >> 16) & 0xFF;
                uint8_t base_cls = (class >> 24) & 0xFF;

                if (base_cls == PCI_CLASS_SERIAL_BUS &&
                    sub_cls  == PCI_SUBCLASS_USB &&
                    prog_if  == PCI_PROGIF_UHCI){

                    uint32_t bar4 = pci_read32((uint8_t)bus, dev, func, 0x20);
                    if (!(bar4 & 0x01)) continue;    /* не I/O BAR */
                    uint16_t io_base = bar4 & 0xFFFC;
                    if (io_base == 0) continue;

                    /* Включить I/O Space + Bus Master */
                    uint32_t cmd = pci_read32((uint8_t)bus, dev, func, 0x04);
                    cmd |= 0x05;
                    pci_write32((uint8_t)bus, dev, func, 0x04, cmd);

                    uhci_hc_t *hc = &uhci_hcs[n];
                    memset(hc, 0, sizeof(*hc));
                    hc->index    = (uint8_t)n;
                    hc->io_base  = io_base;
                    hc->pci_bus  = (uint8_t)bus;
                    hc->pci_dev  = dev;
                    hc->pci_func = func;
                    hc->ports    = 2;          /* UHCI spec: 2 порта на контроллер */
                    hc->valid    = 1;

                    uhci_bind_ptrs(hc);

                    uhci_hc_ptrs[n] = hc;
                    n++;
                    /* Не останавливаемся: собираем ВСЕ контроллеры.
                       На чипсетах типа ICH (Panasonic CF-18) их два,
                       и флешка может висеть только на втором. */
                }
            }
        }
    }
    uhci_hc_count = n;
    return (n > 0) ? 0 : -1;
}

/* ---------------- Init ---------------- */

int uhci_wait_hcreset(uhci_hc_t *hc){
    for (uint32_t i = 0; i < 1000000; i++){
        if (!(inw(hc->io_base + UHCI_USBCMD) & USBCMD_HCRESET)) return 0;
    }
    return -1;
}

/* Остановить HC до состояния "RS=0, HCH=1" — нужно для честного
   configure-port flow (иначе HC крутит кадры с чужими ссылками). */
static void uhci_halt(uhci_hc_t *hc){
    outw(hc->io_base + UHCI_USBCMD, 0);
    for (uint32_t i = 0; i < 1000000; i++){
        if (inw(hc->io_base + UHCI_USBSTS) & USBSTS_HCH) break;
    }
}

int uhci_init(uhci_hc_t *hc){
    if (!hc) return -1;

    uhci_bind_ptrs(hc);

    /* 0) Legacy-режим BIOS (USBCMD.CF=0): HC принимает часть настроек
          от firmware и может не генерировать SOF. Переводим в полный
          программный контроль. На CF-18 (ICH3) это обязательно. */
    uint16_t cmd0 = inw(hc->io_base + UHCI_USBCMD);
    if (!(cmd0 & USBCMD_CF)){
        uhci_halt(hc);
        outw(hc->io_base + UHCI_USBCMD, cmd0 | USBCMD_CF);
    }

    /* 1) Global Reset — сбрасывает все порты, снимает devices с шины.
          Делаем только если контроллер был запущен (BIOS-legacy или
          наш прошлый запуск); на холодном HC GRESET иногда залипает. */
    uint16_t sts0 = inw(hc->io_base + UHCI_USBSTS);
    if ((cmd0 & USBCMD_RS) || !(sts0 & USBSTS_HCH)){
        uhci_halt(hc);
        outw(hc->io_base + UHCI_USBCMD, USBCMD_GRESET | USBCMD_CF);
        for (volatile int i = 0; i < 30000; i++) inw(hc->io_base + UHCI_USBCMD); /* ~10 ms */
        outw(hc->io_base + UHCI_USBCMD, USBCMD_CF);
    }

    /* 2) Host Controller Reset */
    outw(hc->io_base + UHCI_USBCMD, USBCMD_HCRESET | USBCMD_CF);
    if (uhci_wait_hcreset(hc) != 0) return -2;

    /* 3) Отключить все прерывания — работаем polling'ом */
    outw(hc->io_base + UHCI_USBINTR, 0);

    /* 4) Очистить статус (W1C) */
    outw(hc->io_base + UHCI_USBSTS, 0x007F);

    /* 5) Кольца TD: забить terminate-статусом, обнулить головы */
    ctrl_ring_head[hc->index] = 0;
    bulk_ring_head[hc->index] = 0;
    ctrl_ring_free[hc->index] = UHCI_CTRL_RING;
    bulk_ring_free[hc->index] = UHCI_BULK_RING;
    for (int i = 0; i < UHCI_CTRL_RING; i++){
        hc->ctrl_ring[i].link = TD_LINK_TERM;
        hc->ctrl_ring[i].status = 0;
    }
    for (int i = 0; i < UHCI_BULK_RING; i++){
        hc->bulk_ring[i].link = TD_LINK_TERM;
        hc->bulk_ring[i].status = 0;
    }

    /* 7) Frame List — все слоты terminate (пока без очередей) */
    uint32_t *fl = (uint32_t*)hc->frame_list;
    for (int i = 0; i < 1024; i++) fl[i] = TD_LINK_TERM;
    outl(hc->io_base + UHCI_FLBASEADD, hc->fl_phys);
    outw(hc->io_base + UHCI_FRNUM, 0);
    outw(hc->io_base + UHCI_SOFMOD, 64);   /* 64 байта на кадр */

    /* 8) Снять порты с suspend/soft-disconnect, которые мог оставить BIOS */
    for (int p = 0; p < hc->ports; p++){
        uint16_t reg = (p == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
        uint16_t s = inw(hc->io_base + reg);
        if (s & (PORTSC_SUSPEND | PORTSC_SDE)){
            outw(hc->io_base + reg, s & ~(PORTSC_SUSPEND | PORTSC_SDE));
        }
    }

    /* 9) Запуск HC */
    cmd0 = inw(hc->io_base + UHCI_USBCMD);
    outw(hc->io_base + UHCI_USBCMD, cmd0 | USBCMD_RS | USBCMD_CF);

    /* 10) Дождаться, что HC снял HCH */
    for (uint32_t i = 0; i < 1000000; i++){
        if (!(inw(hc->io_base + UHCI_USBSTS) & USBSTS_HCH)){
            hc->running = 1;
            return 0;
        }
    }
    return -3;
}

int uhci_init_all(void){
    int ok = 0;
    for (int i = 0; i < uhci_hc_count; i++){
        if (uhci_init(uhci_hc_ptrs[i]) == 0) ok++;
    }
    if (ok == 0) return -1;
    uhci_ports_schedule_all();
    return ok;
}

/* ---------------- Ports ---------------- */

static uint16_t portsc_reg(int port){
    return (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
}

uint16_t uhci_port_status(uhci_hc_t *hc, int port){
    return inw(hc->io_base + portsc_reg(port));
}

int uhci_port_connected(uhci_hc_t *hc, int port){
    return (uhci_port_status(hc, port) & PORTSC_CCS) ? 1 : 0;
}

void uhci_port_resume(uhci_hc_t *hc, int port){
    uint16_t reg = portsc_reg(port);
    uint16_t s = inw(hc->io_base + reg);
    if (s & (PORTSC_SUSPEND | PORTSC_SDE)){
        outw(hc->io_base + reg, s & ~(PORTSC_SUSPEND | PORTSC_SDE));
    }
}

/* Hard reset: PR=1, PED=0, suspend off, W1C change bits.
   Порт ВЫКЛЮЧЕН на всё время assert+hold — HC не пытается ничего
   транслировать, пока мы держим reset. Отдельно от "soft" reset
   (uhci_reset_port), который форсирует PED=1 для стабилизации. */
void uhci_port_reset_hard(uhci_hc_t *hc, int port){
    uint16_t reg = portsc_reg(port);
    uint16_t s = inw(hc->io_base + reg);

    /* Debug-порт (EDB-кабель) не трогаем — reset угонит его в странный режим */
    if (s & PORTSC_DS) return;

    outw(hc->io_base + reg, (s & ~(PORTSC_PED | PORTSC_SUSPEND))
                          | PORTSC_PR | PORTSC_W1C);

    /* Держим ≥ 50 ms */
    for (volatile int i = 0; i < 60000; i++) inw(hc->io_base + reg);

    /* Deassert: читаем свежий PORTSC, снимаем PR, PED остаётся 0 */
    uint16_t cur = inw(hc->io_base + reg);
    outw(hc->io_base + reg, (cur & ~(PORTSC_PR | PORTSC_SUSPEND)) | PORTSC_W1C);

    /* ≥ 10 ms восстановления (USB spec: reset recovery) */
    for (volatile int i = 0; i < 120000; i++) inw(hc->io_base + reg);

    /* Почистить change bits, которые выставились в ходе reset */
    uint16_t fin = inw(hc->io_base + reg);
    outw(hc->io_base + reg, fin | PORTSC_W1C);
}

/* "Soft" reset с включением порта: используется для стабилизации
   уже enumerated устройства (переподключение). Возвращает 0, если
   порт включился. */
int uhci_reset_port(uhci_hc_t *hc, int port){
    uint16_t reg = portsc_reg(port);

    uint16_t s = inw(hc->io_base + reg);
    if (!(s & PORTSC_CCS)) return -1;    /* нет устройства */

    uhci_port_reset_hard(hc, port);

    /* Теперь включаем порт: PED=1 */
    uint16_t cur = inw(hc->io_base + reg);
    outw(hc->io_base + reg, (cur & ~PORTSC_PR) | PORTSC_PED | PORTSC_W1C);

    /* Ждём, пока HC сам не поднимет PED (успешная инициализация FS/LS) */
    for (uint32_t i = 0; i < 2000000; i++){
        s = inw(hc->io_base + reg);
        if (s & PORTSC_PED) return 0;
    }
    return -2;
}

/* ---------------- Планировщик кадров ----------------
   Структура очереди каждого HC:

     frame slot -> port_qh -> [port TD p0] -> port_qh1 -> [port TD p1]
                              (element)          (head)
        ctrl_qh / bulk_qh подключаются ЭЛЕМЕНТОМ следующего QH
        (element bit) только на время транзакции:

     ... p1 element -> ctrl_qh -> [ctrl TDs]   (во время control xfer)
     ... p1 element -> bulk_qh -> [bulk TDs]   (во время bulk xfer)

   Port TD — бесконечные IN-токены на адрес 0/ep0 c IOC=0: HC каждый
   кадр пытается опросить порт; без устройства transaction завершается
   PID error (CRCTIMEO), status залипает. Перед следующим кадром
   driver hot-patch'ит status обратно в ACTIVE — опрос продолжается.
   Это стандартный трюк UHCI-стеков (Linux usb-uhci port chain). */

static uhci_td_t *port_tds[UHCI_MAX_HC][2] __attribute__((aligned(16)));
static uhci_td_t port_td_store[UHCI_MAX_HC][2] __attribute__((aligned(16)));

static void port_td_init(uhci_hc_t *hc, int port){
    int i = hc->index;
    uhci_td_t *td = &port_td_store[i][port];
    port_tds[i][port] = td;

    td->link   = TD_LINK_TERM;
    td->status = 0;
    /* IN token, addr 0, ep 0, max length, no IOC: HC "съедает" за кадр */
    td->token  = ((uint32_t)PID_IN << TD_TOKEN_PID_SHIFT)
               | (0u << TD_TOKEN_ADDR_SHIFT)
               | (0u << TD_TOKEN_EP_SHIFT)
               | ((uint32_t)0x7FF << TD_TOKEN_LEN_SHIFT);
    td->buffer = hc->fl_phys;   /* любой валидный буфер (dummy) */
}

/* Переподключить head порта к его port-TD */
static void port_qh_link(uhci_hc_t *hc, int port){
    uhci_qh_t *qh = (port == 0) ? hc->port_qh : hc->port_qh1;
    qh->head = ((uint32_t)port_tds[hc->index][port]) & ~0x0Fu;
}

void uhci_ports_schedule_all(void){
    for (int h = 0; h < uhci_hc_count; h++){
        uhci_hc_t *hc = uhci_hc_ptrs[h];
        if (!hc || !hc->valid) continue;

        port_td_init(hc, 0);
        port_td_init(hc, 1);

        uhci_qh_t *p0 = hc->port_qh;
        uhci_qh_t *p1 = hc->port_qh1;
        uhci_qh_t *cq = hc->ctrl_qh;
        uhci_qh_t *bq = hc->bulk_qh;

        /* Цепочка портов; element у последнего — terminate (очереди выкл) */
        port_qh_link(hc, 0);
        port_qh_link(hc, 1);
        p0->element = (((uint32_t)p1) & ~0x0Fu) | TD_LINK_Q;
        p1->element = TD_LINK_TERM;

        cq->head    = TD_LINK_TERM;
        cq->element = TD_LINK_TERM;
        bq->head    = TD_LINK_TERM;
        bq->element = TD_LINK_TERM;

        uint32_t first = (((uint32_t)p0) & ~0x0Fu) | TD_LINK_Q;
        uint32_t *fl = (uint32_t*)hc->frame_list;
        for (int i = 0; i < 1024; i++) fl[i] = first;
    }
}

void uhci_ports_deschedule_all(void){
    for (int h = 0; h < uhci_hc_count; h++){
        uhci_hc_t *hc = uhci_hc_ptrs[h];
        if (!hc) continue;
        uint32_t *fl = (uint32_t*)hc->frame_list;
        for (int i = 0; i < 1024; i++) fl[i] = TD_LINK_TERM;
    }
}

/* Hot-patch port TD: вернуть ACTIVE, если HC их "дожог".
   Вызывать раз в ~1 мс из основного цикла опроса. */
void uhci_ports_poll_refresh(void){
    for (int h = 0; h < uhci_hc_count; h++){
        uhci_hc_t *hc = uhci_hc_ptrs[h];
        if (!hc || !hc->running) continue;
        for (int p = 0; p < 2; p++){
            uhci_td_t *td = port_tds[hc->index][p];
            if (!(td->status & TD_CTRL_ACTIVE)){
                td->status = 0;
                td->link   = TD_LINK_TERM;
            }
        }
    }
}

/* Подключить/отключить ctrl-очередь HC к расписанию.
   enable=1: хвост цепочки портов (element последнего активного порта)
             указывает на ctrl_qh; head ctrl_qh уже заполнен вызывающим.
   enable=0: элемент снова terminate. */
void uhci_ctrl_queue_enable(uhci_hc_t *hc, int enable){
    uhci_qh_t *tail = hc->port_qh1;   /* всегда есть (2 порта) */
    if (enable){
        tail->element = (((uint32_t)hc->ctrl_qh) & ~0x0Fu) | TD_LINK_Q;
    } else {
        tail->element = TD_LINK_TERM;
        hc->ctrl_qh->head = TD_LINK_TERM;
    }
}

void uhci_bulk_queue_enable(uhci_hc_t *hc, int enable){
    uhci_qh_t *tail = hc->port_qh1;
    if (enable){
        tail->element = (((uint32_t)hc->bulk_qh) & ~0x0Fu) | TD_LINK_Q;
    } else {
        tail->element = TD_LINK_TERM;
        hc->bulk_qh->head = TD_LINK_TERM;
    }
}

/* Проверка живости HC во время busy-wait.
   Возвращает 0 = OK; <0 = проблема (HSE/Error/остановлен). */
int uhci_hc_healthy(uhci_hc_t *hc){
    uint16_t sts = inw(hc->io_base + UHCI_USBSTS);
    if (sts & USBSTS_HSE){
        outw(hc->io_base + UHCI_USBSTS, USBSTS_HSE);
        uhci_dbg_hse++;
        return -1;
    }
    if (sts & USBSTS_ERROR){
        outw(hc->io_base + UHCI_USBSTS, USBSTS_ERROR);
        uhci_dbg_err++;
        /* Error — штатное событие (TD с ошибкой), не fatal */
    }
    if (!(inw(hc->io_base + UHCI_USBCMD) & USBCMD_RS)) return -2;
    return 0;
}

/* Выделить n подряд идущих TD из кольца. Кольцо замкнуто: последний
   TD всегда имеет валидный link, голова "съеденных" TD крутится HC
   бесконечно, пока очередь выключена — поэтому выделяем только когда
   свободных гарантированно больше, чем нужно. */
static uhci_td_t *ring_alloc(uhci_hc_t *hc, int is_bulk, uint32_t n){
    uint32_t size = is_bulk ? UHCI_BULK_RING : UHCI_CTRL_RING;
    uint32_t *head = is_bulk ? &bulk_ring_head[hc->index]
                             : &ctrl_ring_head[hc->index];
    uint32_t *free_cnt = is_bulk ? &bulk_ring_free[hc->index]
                                 : &ctrl_ring_free[hc->index];

    if (*free_cnt < n) return NULL;

    uhci_td_t *ring = is_bulk ? hc->bulk_ring : hc->ctrl_ring;
    uint32_t start = *head;
    *head = (*head + n) % size;
    *free_cnt -= n;

    /* Замкнуть кольцо: последний выдаваемого участка ссылается на голову */
    for (uint32_t i = 0; i < n; i++){
        uint32_t idx = (start + i) % size;
        uint32_t nxt = (start + i + 1) % size;
        ring[idx].link = ((uint32_t)&ring[nxt]) & ~0x0Fu;
    }
    return &ring[start];
}

static void ring_release(uhci_hc_t *hc, int is_bulk, uint32_t n){
    uint32_t size = is_bulk ? UHCI_BULK_RING : UHCI_CTRL_RING;
    uint32_t *head = is_bulk ? &bulk_ring_head[hc->index]
                             : &ctrl_ring_head[hc->index];
    uint32_t *free_cnt = is_bulk ? &bulk_ring_free[hc->index]
                                 : &ctrl_ring_free[hc->index];
    *head = (*head + n) % size;
    *free_cnt += n;
}

/* ---------------- Control transfer ---------------- */

int uhci_control_transfer(uhci_hc_t *hc, uint8_t dev_addr,
                          const uint8_t *setup_pkt,
                          void *data_buf, uint16_t data_len,
                          int data_in, uint16_t max_pkt){

    if (!hc || !setup_pkt) return -1;
    if (max_pkt == 0 || max_pkt > 64) max_pkt = 8;

    int has_data = (data_len > 0 && data_buf != NULL);
    uint32_t num_data = 0;
    if (has_data){
        num_data = (data_len + max_pkt - 1) / max_pkt;
        if (num_data > UHCI_CTRL_RING - 2)
            num_data = UHCI_CTRL_RING - 2;
    }
    uint32_t total_tds = 1 + num_data + 1;

    uhci_td_t *tds = ring_alloc(hc, 0, total_tds);
    if (!tds) return -4;    /* кольцо переполнено — HC не успевает */

    uint8_t *setup_buf = setup_statics[hc->index];
    for (int i = 0; i < 8; i++) setup_buf[i] = setup_pkt[i];

    uint8_t *buf = (uint8_t *)data_buf;

    /* --- TD[0]: SETUP, DATA0, 8 байт --- */
    tds[0].status = TD_CTRL_ACTIVE;
    tds[0].token  = ((uint32_t)PID_SETUP << TD_TOKEN_PID_SHIFT)
                  | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                  | (0u << TD_TOKEN_EP_SHIFT)
                  | 0u                                              /* DATA0 */
                  | ((uint32_t)(8 - 1) << TD_TOKEN_LEN_SHIFT);
    tds[0].buffer = (uint32_t)setup_buf;   /* статика = VA==PA */

    /* --- TD[1..num_data]: Data stage, чанки по max_pkt ---
       Toggle в data stage начинается с DATA1 и чередуется. */
    for (uint32_t i = 0; i < num_data; i++){
        uint32_t idx   = 1 + i;
        uint32_t off   = i * max_pkt;
        uint32_t chunk = (data_len - off > max_pkt) ? max_pkt
                                                    : (data_len - off);
        uint32_t toggle = ((i & 1) == 0) ? TD_TOKEN_TOGGLE : 0u;

        tds[idx].status = TD_CTRL_ACTIVE;
        tds[idx].token  = ((uint32_t)(data_in ? PID_IN : PID_OUT) << TD_TOKEN_PID_SHIFT)
                        | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                        | (0u << TD_TOKEN_EP_SHIFT)
                        | toggle
                        | ((uint32_t)(chunk - 1) << TD_TOKEN_LEN_SHIFT);
        tds[idx].buffer = usb_virt_to_phys((uint32_t)(buf + off));
    }

    /* --- Status TD: направление противоположно data, всегда DATA1 --- */
    uint32_t sidx       = 1 + num_data;
    uint32_t status_pid = has_data ? (data_in ? PID_OUT : PID_IN) : PID_IN;

    tds[sidx].status = TD_CTRL_ACTIVE;   /* IOC не ставим: прерываний нет */
    tds[sidx].token  = (status_pid << TD_TOKEN_PID_SHIFT)
                     | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                     | (0u << TD_TOKEN_EP_SHIFT)
                     | TD_TOKEN_TOGGLE
                     | ((uint32_t)0x7FF << TD_TOKEN_LEN_SHIFT);
    tds[sidx].buffer = (uint32_t)setup_buf;

    /* --- Включить ctrl-очередь в расписание --- */
    hc->ctrl_qh->head = ((uint32_t)tds) & ~0x0Fu;
    uhci_ctrl_queue_enable(hc, 1);

    /* --- Ожидание --- */
    uint32_t timeout = 20000000;
    int result = -3;
    while (timeout--){
        uint32_t st = tds[sidx].status;
        if (!(st & TD_CTRL_ACTIVE)){
            uint32_t bad = TD_CTRL_STALLED | TD_CTRL_DBUFERR |
                           TD_CTRL_BABBLE  | TD_CTRL_CRCTIMEO |
                           TD_CTRL_BITSTUFF;
            if (st & bad){ result = -2; break; }
            int data_ok = 1;
            for (uint32_t i = 0; i < num_data; i++){
                if (tds[1 + i].status & bad){ data_ok = 0; break; }
            }
            result = data_ok ? (int)data_len : -2;
            break;
        }
        if ((timeout & 0xFFFF) == 0){
            if (uhci_hc_healthy(hc) < 0){ result = -5; break; }
        }
    }
    if (result == -3) uhci_dbg_timeout++;

    uhci_ctrl_queue_enable(hc, 0);
    ring_release(hc, 0, total_tds);

    (void)total_tds;
    return result;
}

/* ---------------- Bulk transfer ---------------- */

int uhci_bulk_transfer(uhci_hc_t *hc, uint8_t dev_addr, uint8_t ep_addr,
                       uint16_t ep_max_pkt, uint8_t *toggle,
                       void *buf, uint32_t len, int dir_in){

    if (!hc || !buf || len == 0 || !toggle) return -1;
    if (ep_max_pkt == 0 || ep_max_pkt > 1024) ep_max_pkt = 64;

    uint32_t num_tds = (len + ep_max_pkt - 1) / ep_max_pkt;
    if (num_tds > UHCI_BULK_RING){
        num_tds = UHCI_BULK_RING;
        len = num_tds * ep_max_pkt;    /* режем запрос до влезающего */
    }

    uhci_td_t *tds = ring_alloc(hc, 1, num_tds);
    if (!tds) return -4;

    uint8_t ep_num = ep_addr & 0x0F;
    uint8_t cur_toggle = *toggle ? 1 : 0;
    uint8_t *b = (uint8_t *)buf;

    for (uint32_t i = 0; i < num_tds; i++){
        uint32_t off   = i * ep_max_pkt;
        uint32_t chunk = (len - off > ep_max_pkt) ? ep_max_pkt : (len - off);
        uint32_t toggle_bit = cur_toggle ? TD_TOKEN_TOGGLE : 0u;

        tds[i].status = TD_CTRL_ACTIVE;
        tds[i].token  = ((uint32_t)(dir_in ? PID_IN : PID_OUT) << TD_TOKEN_PID_SHIFT)
                      | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                      | ((uint32_t)ep_num << TD_TOKEN_EP_SHIFT)
                      | toggle_bit
                      | ((uint32_t)(chunk - 1) << TD_TOKEN_LEN_SHIFT);
        tds[i].buffer = usb_virt_to_phys((uint32_t)(b + off));

        cur_toggle ^= 1;
    }
    /* Последний TD замыкает кольцо на голову — см. ring_alloc */

    hc->bulk_qh->head = ((uint32_t)tds) & ~0x0Fu;
    uhci_bulk_queue_enable(hc, 1);

    /* Ждём завершения ВСЕХ TD (не только последнего: кольцо крутится).
       Успех = ни одного бита ошибки и все ACTIVE сняты. */
    uint32_t rounds = 0;
    uint32_t timeout = 20000000;
    int result = -3;
    const uint32_t bad = TD_CTRL_STALLED | TD_CTRL_DBUFERR |
                         TD_CTRL_BABBLE  | TD_CTRL_CRCTIMEO |
                         TD_CTRL_BITSTUFF;
    while (timeout--){
        uint32_t active_left = 0, err = 0;
        for (uint32_t k = 0; k < num_tds; k++){
            uint32_t st = tds[k].status;
            if (st & TD_CTRL_ACTIVE) active_left++;
            if (st & bad) err = 1;
        }
        if (err){ result = -2; break; }
        if (active_left == 0){
            /* Все TD обработаны. Но HC мог прокрутить кольцо повторно
               (NAK-переоценка) — сверяемся только когда хотя бы один
               раунд истёк. Простейшая защита: ждём ещё один кадр. */
            if (++rounds >= 2){
                *toggle = cur_toggle;
                result = (int)len;
                break;
            }
        }
        if ((timeout & 0xFFFF) == 0){
            if (uhci_hc_healthy(hc) < 0){ result = -5; break; }
        }
    }
    if (result == -3) uhci_dbg_timeout++;

    uhci_bulk_queue_enable(hc, 0);
    /* Снять ACTIVE со всех TD до возврата в пул (иначе следующий
       владелец кольца увидит мусор) */
    for (uint32_t k = 0; k < num_tds; k++){
        tds[k].status = 0;
        tds[k].link   = TD_LINK_TERM;
    }
    ring_release(hc, 1, num_tds);

    return result;
}
