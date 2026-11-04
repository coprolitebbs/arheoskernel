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

#include "include/usb_ehci.h"
#include "include/usb_driver.h"
#include "include/usb_core.h"
#include "include/usb_uhci.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

//EHCI регистры (смещения от operational base = mmio + CAPLENGTH)
#define EHCI_USBCMD        0x00
#define EHCI_USBSTS        0x04
#define EHCI_USBINTR       0x08
#define EHCI_FRINDEX       0x0C
#define EHCI_CTRLDSSEG     0x10
#define EHCI_PERIODICLIST  0x14
#define EHCI_ASYNCLISTADDR 0x18
#define EHCI_CONFIGFLAG    0x40
#define EHCI_PORTSC(p)     (0x44 + ((p)-1)*4)   // p=1..N

//USBCMD
#define CMD_RUN            (1u << 0)
#define CMD_HCRESET        (1u << 1)
#define CMD_ASE            (1u << 5)    // Async Schedule Enable
//USBSTS
#define STS_HCHALTED       (1u << 12)
//CONFIGFLAG
#define CFG_ROUTE_ALL      1            // CF=1: все порты к EHCI
//PORTSC
#define PSC_CCS            (1u << 0)
#define PSC_CSC            (1u << 1)
#define PSC_PED            (1u << 2)
#define PSC_PEDC           (1u << 3)
#define PSC_PR             (1u << 8)
#define PSC_PO             (1u << 13)   // Port Owner (1=companion, 0=EHCI)

//qTD Token
#define QTD_PID_OUT        0xE1u
#define QTD_PID_IN         0x69u
#define QTD_PID_SETUP      0x2Du
#define QTD_PID_ERR        0xFFu
#define QTD_PID(t)         ((t) & 0xFFu)
#define QTD_CERR(t)        (((t) >> 10) & 0x3u)
#define QTD_BYTES(t)       (((t) >> 16) & 0x7FFFu)
#define QTD_ACTIVE         (1u << 7)
#define QTD_IOC            (1u << 15)
#define QTD_TOGGLE         (1u << 31)  //DCR (Data Toggle Control/Report)
/* Статусные биты токена qTD по EHCI spec (Table 3-5):
   bit 7 Active, bit 8 Halted, bits 9..10 Error Counter,
   bits 12..14 Status (ErrCode), bit 15 IOC. */
#define QTD_STS_HALT       (1u << 8)   //halted on error — по спеке это бит 8
#define QTD_ERRCODE_SHIFT  12          //bits 12..14: 0=no err, 1=timeouts, 2=BDRE, 3=Babble, 4=BusError, 5=STalled
#define QTD_KEC_SHIFT      9           //остаток error counter после retire

static inline uint32_t qtd_errcode(uint32_t token){
    return (token >> QTD_ERRCODE_SHIFT) & 0x7u;
}
static inline uint32_t qtd_kec(uint32_t token){
    return (token >> QTD_KEC_SHIFT) & 0x3u;
}

//QH Endpoint Characteristics
#define QH_EP_MAXPKT(m)    (((uint32_t)(m) & 0x7FFu) << 16)
#define QH_EP_NUM(n)       (((uint32_t)(n) & 0xFu) << 23)
#define QH_EP_SPD_HS       (2u << 27)   // high speed
#define QH_EP_C            (1u << 7)    // control endpoint flag
#define QH_EP_H            (1u << 15)   // head of reclamation

#define EHCI_CTRL_QH_POOL   8

#define EHCI_CTRL_QTD_COUNT 4
#define EHCI_BULK_QTD_COUNT 64

//qTD / QH (все выровнены на 32 байта)
typedef struct {
    volatile uint32_t next;         // low 32 bits (high = 0)
    volatile uint32_t next_hi;
    volatile uint32_t alt;
    volatile uint32_t alt_hi;
    volatile uint32_t token;
    volatile uint32_t token_hi;
    volatile uint32_t buf[5];
    volatile uint32_t buf_hi[5];
} __attribute__((packed, aligned(32))) ehci_qtd_t;

typedef struct {
    volatile uint32_t horiz;
    volatile uint32_t horiz_hi;
    volatile uint32_t ep_char;
    volatile uint32_t ep_cap;
    volatile uint32_t cur_qtd;
    volatile uint32_t cur_qtd_hi;
    volatile uint32_t next_qtd;
    volatile uint32_t next_qtd_hi;
    volatile uint32_t alt_qtd;
    volatile uint32_t alt_qtd_hi;
    volatile uint32_t token;
    volatile uint32_t token_hi;
    volatile uint32_t buf[5];
    volatile uint32_t buf_hi[5];
} __attribute__((packed, aligned(32))) ehci_qh_t;

extern uint32_t usb_virt_to_phys(uint32_t va);
extern usb_stack_driver_api_t *api_ptr;

ehci_hc_t ehci_hcs[4];
int ehci_hc_count = 0;

uint8_t ehci_dev_speed[128] = {0};

// Пул QH для control transfers. QEMU кэширует QH по адресу, поэтому
// переиспользование одного и того же QH не работает — берём следующий
// из пула. 8-ми должно хватить для типичной инициализации (GET_DESC,
// SET_ADDRESS, GET_CONFIG, SET_CONFIG и т.д.).
static ehci_qh_t  *ctrl_qh_pool[EHCI_CTRL_QH_POOL];
static uint32_t    ctrl_qh_pool_phys[EHCI_CTRL_QH_POOL];
static int         ctrl_qh_pool_next = 0;

//Async head (reclamation QH)
static ehci_qh_t  *async_head_ptr  = NULL;
static uint32_t    async_head_phys = 0;

//Control QH — первый из пула (на него смотрит async_head->horiz)
static ehci_qh_t  *ctrl_qh_ptr     = NULL;
static uint32_t    ctrl_qh_phys    = 0;

//Control qTD pool
static ehci_qtd_t *ctrl_qtd_ptr    = NULL;
static uint32_t    ctrl_qtd_phys   = 0;

//Bulk QH + qTD pool
static ehci_qh_t  *bulk_qh_ptr     = NULL;
static uint32_t    bulk_qh_phys    = 0;
static ehci_qtd_t *bulk_qtd_ptr    = NULL;
static uint32_t    bulk_qtd_phys   = 0;



static uint32_t mmio_read(ehci_hc_t *ehc, uint32_t off){
    return *(volatile uint32_t*)(ehc->op_base + off);
}
static void mmio_write(ehci_hc_t *ehc, uint32_t off, uint32_t val){
    *(volatile uint32_t*)(ehc->op_base + off) = val;
}

//---------------- Probe ----------------

/* ICH (PI/LPC) bridge PCI config: dev 31, func 0.
   SMIC (0xD8): bit 11 EHCI_SMI_EN, bit 10 USB_SMI_EN — пока BIOS/OS
   владеет контроллерами через SMM, регистры могут вести себя странно.
   USBPTD (0xE2): "USB Port Detect" — на ICH6/ICH7-M биты 0..3 =
   presence detect портов UHCI#1/#2/#3/EHCI. По ним видно, на каком
   контроллере реально висит флешка, ещё до инициализации HC. */
#define ICH_LPC_DEV      31
#define ICH_LPC_FUNC     0
#define ICH_REG_SMIC     0xD8
#define ICH_REG_USBPTD   0xE2

int ehci_probe_all(void)
{
    uint32_t bar_size = 0;
    ehci_hc_count = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
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

                //Размер BAR по classic size-probing (сохранив оригинал).
                //ICH6 EHCI обычно 4 КБ; если маппим меньше — хвост
                //PORTSC читаться не будет.
                pci_write32((uint8_t)bus, dev, func, 0x10, 0xFFFFFFFFu);
                uint32_t sz_raw = pci_read32((uint8_t)bus, dev, func, 0x10);
                pci_write32((uint8_t)bus, dev, func, 0x10, bar0);
                bar_size = (sz_raw & 0xFFFFFFF0u);
                bar_size = (~bar_size + 1u) & bar_size; //низкий set-бит = размер
                if (bar_size < 0x100) bar_size = 0x1000; //fallback

                //Снять SMI-перехват USB/EHCI у BIOS (ICH-специфично,
                //безопасно: запись в чужой device просто игнорируется).
                uint32_t smic = pci_read32((uint8_t)bus, dev, func, 0xDC);
                smic &= ~((1u << 11) | (1u << 10));
                pci_write32((uint8_t)bus, dev, func, 0xDC, smic);

                //Presence detect с LPC-моста (ICH6): какие порты "живые".
                uint32_t ptd = pci_read32(0, ICH_LPC_DEV, ICH_LPC_FUNC, ICH_REG_USBPTD);
                if (api_ptr && api_ptr->load_screen_add_string)
                    api_ptr->load_screen_add_string("EHCI probe %x:%x.%x VID=%x BAR=%x size=%x SMIC=%x PTD=%x",
                                                    bus, dev, func, (id & 0xFFFF), mmio, bar_size, smic, ptd);

                uint32_t cmd = pci_read32((uint8_t)bus, dev, func, 0x04);
                cmd |= 0x06;   // Memory + Bus Master
                pci_write32((uint8_t)bus, dev, func, 0x04, cmd);

                ehci_hc_t *ehc = &ehci_hcs[ehci_hc_count++];
                memset(ehc, 0, sizeof(*ehc));
                ehc->mmio_base = mmio;
                ehc->mmio_size = bar_size;
            }
        }
    }
    return ehci_hc_count;
}

//---------------- Init ----------------

//Аллокация QH/qTD через DMA (один раз)
int ehci_alloc_structures(void)
{
    uint32_t phys;
    if (async_head_ptr) return 0;

    //async head QH (reclamation, H=1)
    async_head_ptr = (ehci_qh_t*)usb_dma_alloc(sizeof(ehci_qh_t), &phys);
    if (!async_head_ptr || (phys & 0x1F) != 0) return -1;
    async_head_phys = phys;
    memset(async_head_ptr, 0, sizeof(ehci_qh_t));

    // Пул control QH
    for (int i = 0; i < EHCI_CTRL_QH_POOL; i++){
        ctrl_qh_pool[i] = (ehci_qh_t*)usb_dma_alloc(sizeof(ehci_qh_t), &phys);
        if (!ctrl_qh_pool[i] || (phys & 0x1F) != 0) return -2;
        ctrl_qh_pool_phys[i] = phys;
        memset(ctrl_qh_pool[i], 0, sizeof(ehci_qh_t));
    }
    ctrl_qh_pool_next = 0;
    // Первый QH используем как «текущий» (на него смотрит async_head)
    ctrl_qh_ptr  = ctrl_qh_pool[0];
    ctrl_qh_phys = ctrl_qh_pool_phys[0];

    //control qTD pool
    ctrl_qtd_ptr = (ehci_qtd_t*)usb_dma_alloc(EHCI_CTRL_QTD_COUNT * sizeof(ehci_qtd_t), &phys);
    if (!ctrl_qtd_ptr || (phys & 0x1F) != 0) return -3;
    ctrl_qtd_phys = phys;
    memset(ctrl_qtd_ptr, 0, EHCI_CTRL_QTD_COUNT * sizeof(ehci_qtd_t));

    //bulk QH
    bulk_qh_ptr = (ehci_qh_t*)usb_dma_alloc(sizeof(ehci_qh_t), &phys);
    if (!bulk_qh_ptr || (phys & 0x1F) != 0) return -4;
    bulk_qh_phys = phys;
    memset(bulk_qh_ptr, 0, sizeof(ehci_qh_t));

    //bulk qTD pool
    bulk_qtd_ptr = (ehci_qtd_t*)usb_dma_alloc(EHCI_BULK_QTD_COUNT * sizeof(ehci_qtd_t), &phys);
    if (!bulk_qtd_ptr || (phys & 0x1F) != 0) return -5;
    bulk_qtd_phys = phys;
    memset(bulk_qtd_ptr, 0, EHCI_BULK_QTD_COUNT * sizeof(ehci_qtd_t));

    return 0;
}

int ehci_init(ehci_hc_t *ehc)
{
    if (!ehc) return -1;

    //Map MMIO (uncached!). Размер — по реальному BAR (ICH6 EHCI = 4 КБ),
    //округлённый вверх до страницы. 0x1000 могло не покрывать PORTSC
    //последних портов на некоторых чипах.
    uint32_t map_size = (ehc->mmio_size >= 0x800) ? ehc->mmio_size : 0x1000;
    map_size = (map_size + 0xFFF) & ~0xFFFu;
    if (api_ptr->vmm_map_pages_uncached_ptr)
        api_ptr->vmm_map_pages_uncached_ptr(ehc->mmio_base, map_size);
    else if (api_ptr->vmm_map_pages_ptr)
        api_ptr->vmm_map_pages_ptr(ehc->mmio_base, map_size);
    else
        return -1;

    uint8_t cap_len = *(volatile uint8_t*)(ehc->mmio_base);
    if (cap_len == 0 || cap_len == 0xFF){
        api_ptr->load_screen_add_string("EHCI cap_len=%x (bad) mmio=%x", cap_len, ehc->mmio_base);
        return -2;
    }
    ehc->op_base = ehc->mmio_base + cap_len;

    uint32_t hcs = *(volatile uint32_t*)(ehc->mmio_base + 0x04);
    ehc->n_ports = hcs & 0x0F;
    if (ehc->n_ports == 0 || ehc->n_ports > 15) ehc->n_ports = 15;

    //Сначала читаем состояние "как оставил BIOS": HCHalted/CFL.
    //Если CFL==1 и RS==1 — BIOS сам держит все порты на EHCI и
    //сбрасывать CF нельзя (companion-контроллеры могут быть выключены).
    uint32_t sts0 = mmio_read(ehc, EHCI_USBSTS);
    uint32_t cmd0 = mmio_read(ehc, EHCI_USBCMD);
    uint32_t cf0  = mmio_read(ehc, EHCI_CONFIGFLAG);
    api_ptr->load_screen_add_string("EHCI pre: CAP=%x HCS=%x CMD=%x STS=%x CF=%x",
                                    cap_len, hcs, cmd0, sts0, cf0);
    int bios_owned = (cf0 & CFG_ROUTE_ALL) && (cmd0 & CMD_RUN);

    //HCRESET
    mmio_write(ehc, EHCI_USBCMD, CMD_HCRESET);
    for (int i = 0; i < 1000000; i++){
        if (!(mmio_read(ehc, EHCI_USBCMD) & CMD_HCRESET)) break;
    }
    //HCHalted должен установиться после reset
    int halted = 0;
    for (int i = 0; i < 1000000; i++){
        if (mmio_read(ehc, EHCI_USBSTS) & STS_HCHALTED){ halted = 1; break; }
    }
    if (!halted){
        api_ptr->load_screen_add_string("EHCI HCREST stuck: CMD=%x STS=%x",
                                        mmio_read(ehc, EHCI_USBCMD),
                                        mmio_read(ehc, EHCI_USBSTS));
        return -4;
    }

    //Очистить статус (W1C — пишем единицы в установленные биты)
    mmio_write(ehc, EHCI_USBSTS, mmio_read(ehc, EHCI_USBSTS));
    mmio_write(ehc, EHCI_USBINTR, 0);
    mmio_write(ehc, EHCI_CTRLDSSEG, 0);

        // 1. async_head: HALT, T-bit в next_qtd, alt_qtd = 1 (T-bit)
    async_head_ptr->horiz   = ctrl_qh_phys;
    async_head_ptr->horiz_hi= 0;
    async_head_ptr->ep_char = QH_EP_H | QH_EP_SPD_HS | QH_EP_MAXPKT(64);
    async_head_ptr->ep_cap  = 0;
    async_head_ptr->cur_qtd = 1;             // T-bit
    async_head_ptr->cur_qtd_hi = 0;
    async_head_ptr->next_qtd= 1;             // T-bit
    async_head_ptr->next_qtd_hi = 0;
    async_head_ptr->alt_qtd = 1;             // T-bit (нет альтернативной цепочки)
    async_head_ptr->alt_qtd_hi = 0;
    async_head_ptr->token   = QTD_ACTIVE;  // HALT (как в Linux)
    async_head_ptr->token_hi= 0;

    // 2. Все QH пула — активны, но пусты.
    // КЛЮЧЕВОЕ: связываем их в ЦЕПОЧКУ, а не каждый на async_head.
    // QEMU обходит кольцо по horiz. Если QH не в кольце — QEMU его не видит.
    // Цепочка: async_head -> qh[0] -> qh[1] -> ... -> qh[7] -> async_head.
    for (int i = 0; i < EHCI_CTRL_QH_POOL; i++){
        ehci_qh_t *qh = ctrl_qh_pool[i];
        // horiz: следующий QH в цепочке, последний замыкается на async_head
        if (i + 1 < EHCI_CTRL_QH_POOL)
            qh->horiz = ctrl_qh_pool_phys[i + 1];
        else
            qh->horiz = async_head_phys;
        qh->horiz_hi = 0;
        qh->ep_char  = QH_EP_C | QH_EP_SPD_HS | QH_EP_MAXPKT(64);
        qh->ep_cap   = 0;
        qh->cur_qtd  = 1;             // T-bit: пустая цепочка
        qh->cur_qtd_hi = 0;
        qh->next_qtd = 1;
        qh->next_qtd_hi = 0;
        qh->alt_qtd  = 1;
        qh->alt_qtd_hi = 0;
        qh->token    = QTD_ACTIVE;
        qh->token_hi = 0;
    }

    for (int i = 0; i < 5; i++){ async_head_ptr->buf[i]=0; async_head_ptr->buf_hi[i]=0; }

    mmio_write(ehc, EHCI_ASYNCLISTADDR, async_head_phys);

    //CF=1: все порты маршрутизируются EHCI.
    //Исключение: если BIOS оставил EHCI запущенным с CFL=1, он сам
    //распорядился портами (возможно, часть осознанно осталась на UHCI) —
    //не трогаем CF, иначе можно "отобрать" порт у живого companion.
    if (bios_owned){
        api_ptr->load_screen_add_string("EHCI: BIOS owns ports (CF=1,RS=1) - keeping CF");
    } else {
        mmio_write(ehc, EHCI_CONFIGFLAG, CFG_ROUTE_ALL);
    }

    //Порты после смены ownership/сброса. Хак EHCI: PORTSC портов,
    //оставшихся у companion-контроллера (PO=1), через MMIO EHCI читать
    //нельзя — там будет мусор. Включаем только порты, реально принадлежащие
    //EHCI; для остальных пишем ConfigRunFlag-зависимый idle (без записи
    //PED в чужой порт).
    for (uint32_t p = 1; p <= ehc->n_ports; p++){
        uint32_t ps = mmio_read(ehc, EHCI_PORTSC(p));
        if (!(ps & PSC_PO)){
            //Порт наш. Если устройство подключено, но порт не включён
            //(BIOS выключил или PHY в chirp) — включаем принудительно.
            if ((ps & PSC_CCS) && !(ps & PSC_PED)){
                mmio_write(ehc, EHCI_PORTSC(p),
                           (ps | PSC_PED) & ~(PSC_CSC | PSC_PEDC));
            }
        }
        api_ptr->load_screen_add_string("EHCI p%d PORTSC=%x pspd=%d po=%d",
                                        (int)p, ps,
                                        (int)((ps >> 26) & 3),
                                        (ps & PSC_PO) ? 1 : 0);
    }

    //Run + Async Schedule Enable
    uint32_t cmd = mmio_read(ehc, EHCI_USBCMD);
    cmd &= ~CMD_HCRESET;
    cmd |= CMD_RUN | CMD_ASE;
    mmio_write(ehc, EHCI_USBCMD, cmd);

    int running = 0;
    for (int i = 0; i < 100000; i++){
        if (!(mmio_read(ehc, EHCI_USBSTS) & STS_HCHALTED)){ running = 1; break; }
    }
    if (!running){
        api_ptr->load_screen_add_string("EHCI didn't start: CMD=%x STS=%x",
                                        mmio_read(ehc, EHCI_USBCMD),
                                        mmio_read(ehc, EHCI_USBSTS));
        return -3;
    }

    api_ptr->load_screen_add_string("EHCI started: CMD=%x STS=%x CFG=%x",
                                    mmio_read(ehc, EHCI_USBCMD),
                                    mmio_read(ehc, EHCI_USBSTS),
                                    mmio_read(ehc, EHCI_CONFIGFLAG));

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

int ehci_port_reset(ehci_hc_t *ehc, int port)
{
    uint32_t reg = EHCI_PORTSC(port);
    uint32_t ps0 = mmio_read(ehc, reg);
    if (api_ptr && api_ptr->load_screen_add_string)
        api_ptr->load_screen_add_string("ehci_pr p%d in=%x pspd=%d",
                                        port, ps0, (int)((ps0 >> 26) & 3));

    //Порт принадлежит companion-контроллеру (PO=1)? EHCI трогать его
    //PORTSC через MMIO не должен;Enumeration такого порта — задача UHCI.
    if (ps0 & PSC_PO){
        if (api_ptr && api_ptr->load_screen_add_string)
            api_ptr->load_screen_add_string("ehci_pr p%d owned by companion", port);
        return -3;
    }

    if (!(ps0 & PSC_CCS)) return -1;

    //Assert PR=1.
    //ВАЖНО: change bits CSC/PEDC в PORTSC — W1C (write 1 to clear).
    //Если в ps0 они =1, запись ps0 (или ps0|PR) их очистит.
    //Если маскировать в 0 (как было раньше) — они никогда не сбросятся,
    //а EHCI hardware при CSC=1 держит порт disabled (PED=0).
    mmio_write(ehc, reg, ps0 | PSC_PR);

    //Держим PR=1 минимум 10 мс + 1 мс после снятия (USB spec TDRSTR/TDDSCN).
    //get_ticks() возвращает тики по 10 мс (PIT 100 Гц)!
    //Раньше ждали "< 5" тиков = 50 мс на assert и settle "< 50" = 500 мс
    //на PED — запас был, но теперь делаем явно: assert 2 тика (20 мс),
    //settle до 10 тиков (100 мс).
    uint32_t t0 = api_ptr->get_ticks();
    while ((api_ptr->get_ticks() - t0) < 2) { (void)mmio_read(ehc, reg); }

    //Deassert PR=0, форсим PED=1. Если CSC/PEDC появились снова — очистятся.
    uint32_t ps = mmio_read(ehc, reg);
    mmio_write(ehc, reg, (ps & ~PSC_PR) | PSC_PED);

    //Ждём PE=1 (после deassert нужно ~1 мс; даём до 100 мс)
    t0 = api_ptr->get_ticks();
    while ((api_ptr->get_ticks() - t0) < 10){
        ps = mmio_read(ehc, reg);
        if (ps & PSC_PED){
            if (api_ptr && api_ptr->load_screen_add_string)
                api_ptr->load_screen_add_string("ehci_pr p%d ok=%x pspd=%d",
                                                port, ps, (int)((ps >> 26) & 3));
            return 0;
        }
        __asm__ __volatile__("pause");
    }
    if (api_ptr && api_ptr->load_screen_add_string)
        api_ptr->load_screen_add_string("ehci_pr p%d timeout=%x", port, ps);
    return -2;
}

//---------------- Control transfer ----------------

int ehci_control_transfer(ehci_hc_t *ehc, uint8_t addr,
                          const uint8_t *setup,
                          void *buf, uint16_t len, int in, uint16_t maxpkt)
{
    if (!ehc || !setup || !ehc->initialized) return -1;
    if (maxpkt == 0 || maxpkt > 64) maxpkt = 8;

    //qTD[0]: SETUP
    ehci_qtd_t *tds = ctrl_qtd_ptr;
    uint32_t phys0 = ctrl_qtd_phys;
    memset(tds, 0, EHCI_CTRL_QTD_COUNT * sizeof(ehci_qtd_t));

    // Берём следующий QH из пула. QEMU кэширует QH по адресу; если
    // переиспользовать один и тот же, он будет видеть старую версию
    // (пустую цепочку) и не обработает наш свежий next_qtd.
    ehci_qh_t *qh = ctrl_qh_pool[ctrl_qh_pool_next];
    uint32_t   qh_phys = ctrl_qh_pool_phys[ctrl_qh_pool_next];
    ctrl_qh_pool_next = (ctrl_qh_pool_next + 1) % EHCI_CTRL_QH_POOL;
    // Переинициализируем QH перед использованием
    qh->horiz    = async_head_phys;
    qh->horiz_hi = 0;
    qh->ep_cap   = 0;
    qh->cur_qtd  = 1;
    qh->cur_qtd_hi = 0;
    qh->alt_qtd  = 1;
    qh->alt_qtd_hi = 0;

    //Копия setup в отдельный буфер? setup_pkt уже в стеке вызывающего,
    //но EHCI нужен физический адрес — используем setup_static-подобный
    //подход. Здесь setup_pkt обычно в config_buf или setup локально.
    //Используем usb_virt_to_phys на setup (обычно identity-mapped для
    //стека драйвера, но может быть и kmalloc).
    uint32_t setup_phys = usb_virt_to_phys((uint32_t)setup);

    int qidx = 0;
    //Setup qTD
    tds[qidx].next = (qidx + 1 < EHCI_CTRL_QTD_COUNT) ? (phys0 + (qidx+1)*sizeof(ehci_qtd_t)) : 1;
    tds[qidx].next_hi = 0;
    tds[qidx].alt = 1; // T-bit
    tds[qidx].alt_hi = 0;
    tds[qidx].token = QTD_PID_SETUP | (3u << 10) | (8u << 16) | QTD_ACTIVE;
    tds[qidx].token_hi = 0;
    tds[qidx].buf[0] = setup_phys;
    qidx++;

    //Data qTD(s) — если есть len > 0
    uint16_t remain = len;
    uint32_t buf_phys = buf ? usb_virt_to_phys((uint32_t)buf) : 0;
    while (remain > 0){
        uint16_t chunk = remain > 16384 ? 16384 : remain;
        tds[qidx].next = (qidx + 1 < EHCI_CTRL_QTD_COUNT) ? (phys0 + (qidx+1)*sizeof(ehci_qtd_t)) : 1;
        tds[qidx].next_hi = 0;
        tds[qidx].alt = 1; // T-bit
        tds[qidx].alt_hi = 0;
        tds[qidx].token = (in ? QTD_PID_IN : QTD_PID_OUT) | (3u << 10) |
                          (((uint32_t)chunk) << 16) | QTD_ACTIVE | (in ? QTD_TOGGLE : 0u);
        tds[qidx].token_hi = 0;
        tds[qidx].buf[0] = buf_phys;
        buf_phys += chunk;
        remain -= chunk;
        qidx++;
        if (qidx >= EHCI_CTRL_QTD_COUNT - 1) break;
    }

    //Status qTD
    tds[qidx].next = 1;   //terminate
    tds[qidx].next_hi = 0;
    tds[qidx].alt = 1; // T-bit
    tds[qidx].alt_hi = 0;
    tds[qidx].token = (in ? QTD_PID_OUT : QTD_PID_IN) | (3u << 10) | (0u << 16) |
                      QTD_ACTIVE | QTD_IOC | QTD_TOGGLE;
    tds[qidx].token_hi = 0;
    tds[qidx].buf[0] = 0;

    //QH: C=1, EP0, адрес в bits 7:0.
    //Speed — по PSPD, сохранённому в usb_enumerate.
    //MPS: для HS control всегда 64; для FS/LS — bMaxPacketSize0 из дескриптора
    //(передаётся сюда как maxpkt от usb_core).
    uint8_t spd = (addr < 128) ? ehci_dev_speed[addr] : 0;
    uint32_t speed_bits;
    uint16_t mps;
    if (spd == 2){          // HS
        speed_bits = (2u << 27);
        mps = 64;
    } else if (spd == 1){   // LS (control LS на EHCI не поддерживается по спеке,
                            // но эмулируем для безопасности)
        speed_bits = (1u << 27);
        mps = (maxpkt && maxpkt <= 8) ? maxpkt : 8;
    } else {                // FS
        speed_bits = (0u << 27);
        mps = (maxpkt >= 8 && maxpkt <= 64) ? maxpkt : 8;
    }

    qh->ep_char  = QH_EP_MAXPKT(mps) | QH_EP_NUM(0) |
                   speed_bits | QH_EP_C |
                   ((uint32_t)addr & 0x7F);

    // КЛЮЧЕВОЕ: QEMU и реальный EHCI смотрят на current_qtd, а не next_qtd.
    // Если cur_qtd имеет T-бит (1), QH считается «пустым» и пропускается.
    // Кладём указатель на цепочку qTD именно в cur_qtd.
    qh->cur_qtd    = phys0;
    qh->cur_qtd_hi = 0;

    // next_qtd оставляем terminated, чтобы после завершения цепочки
    // QH не пытался идти дальше.
    qh->next_qtd    = 1;
    qh->next_qtd_hi = 0;

    qh->token    = QTD_ACTIVE;
    qh->token_hi = 0;

    //Вставить QH в async list (head->horiz = QH)
    async_head_ptr->horiz = ctrl_qh_pool_phys[0];
    __sync_synchronize();

        // Заставляем QEMU перечитать асинхронный список
    mmio_write(ehc, EHCI_ASYNCLISTADDR, async_head_phys);
    __sync_synchronize();

    //Ждать пока QH token снимет Active
    int result = -3;
    uint32_t t_qh = api_ptr->get_ticks();
    while ((api_ptr->get_ticks() - t_qh) < 200){
        uint32_t tok = qh->token;
        if (!(tok & QTD_ACTIVE)){
            //Финальный статус — последний qTD
            uint32_t last_tok = tds[qidx].token;
            if (QTD_PID(last_tok) == QTD_PID_ERR){ result = -2; }
            else if (QTD_CERR(last_tok) != 0){ result = -2; }
            else {
                //actual = orig - remain (bytes)
                uint16_t orig = len;
                uint16_t rem  = QTD_BYTES(last_tok);
                result = (int)(orig - (rem > orig ? orig : rem));
                if (len == 0) result = 0;
            }
            break;
        }
    }

    // Отцеплять QH от кольца НЕ НУЖНО. Оставляем async_head->ctrl_qh.
    // QEMU должен видеть постоянное кольцо.
    __sync_synchronize();

    //Диагностика неудачи — покажет, что реально записано в QH/qTD после завершения
    if (result < 0 && api_ptr && api_ptr->load_screen_add_string){
            api_ptr->load_screen_add_string("ehci_ctrl r=%d qh=%x nq=%x q0=%x q%d=%x",
                                        result, qh->token, qh->next_qtd,
                                        tds[0].token, qidx, tds[qidx].token);
    }

    //Снять active
    for (int i = 0; i <= qidx; i++) tds[i].token &= ~QTD_ACTIVE;

    // Больше ничего не нужно: QH из пула больше не используется.
    // QEMU его кэширует, но мы уже взяли следующий адрес для новой транзакции.

    return result;
}

//---------------- Bulk transfer ----------------

int ehci_bulk_transfer(ehci_hc_t *ehc, uint8_t addr, uint8_t ep,
                       uint16_t maxpkt, uint8_t *toggle,
                       void *buf, uint32_t len, int in)
{
    if (!ehc || !buf || len == 0 || !toggle || !ehc->initialized) return -1;
    if (maxpkt == 0 || maxpkt > 1024) maxpkt = 64;

    uint8_t ep_num = ep & 0x0F;
    uint8_t ep_dir = (ep & 0x80) ? 1 : 0;
    uint32_t chunk_total = len;
    uint32_t chunks = (chunk_total + 16384 - 1) / 16384;
    if (chunks > EHCI_BULK_QTD_COUNT) chunks = EHCI_BULK_QTD_COUNT;

    ehci_qtd_t *tds = bulk_qtd_ptr;
    memset(tds, 0, EHCI_BULK_QTD_COUNT * sizeof(ehci_qtd_t));

    uint32_t buf_phys = usb_virt_to_phys((uint32_t)buf);
    uint32_t remain = chunk_total;
    uint8_t cur_toggle = *toggle ? 1 : 0;
    uint32_t qidx = 0;

    while (remain > 0 && qidx < EHCI_BULK_QTD_COUNT){
        uint32_t chunk = remain > 16384 ? 16384 : remain;
        tds[qidx].next = (qidx + 1 < EHCI_BULK_QTD_COUNT) ? (bulk_qtd_phys + (qidx+1)*sizeof(ehci_qtd_t)) : 1;
        tds[qidx].next_hi = 0;
        tds[qidx].alt = 0xFFFFFFFF;
        tds[qidx].alt_hi = 0;
        tds[qidx].token = (in ? QTD_PID_IN : QTD_PID_OUT) | (3u << 10) |
                          (chunk << 16) | QTD_ACTIVE |
                          (cur_toggle ? QTD_TOGGLE : 0u) |
                          (qidx == 0 ? QTD_IOC : 0);   //IOC на первом = retire после первого? нет, на последнем
        tds[qidx].token &= ~QTD_IOC;   //IOC поставим ниже на последнем
        tds[qidx].token_hi = 0;
        tds[qidx].buf[0] = buf_phys;
        tds[qidx].buf[1] = buf_phys + 4096;
        tds[qidx].buf[2] = buf_phys + 8192;
        tds[qidx].buf[3] = buf_phys + 12288;
        tds[qidx].buf[4] = 0;
        //Для простоты: предполагаем, что chunk не пересекает 4К границу в
        //одном qTD (максимум 16384 = 4 страницы). В реале EHCI требует,
        //чтобы все 4 buffer ptr покрывали диапазон. Упрощаем: 1 буфер,
        //максимум 4096 байт в qTD.
        //Переделываем под реальную структуру: qTD покрывает до 4-5 страниц,
        //но проще резать по 4096 байт.
        break;   //TODO: следующий этап
    }

    //Пока — не реализовано. Заглушка.
    (void)cur_toggle;
    return -1;
}
