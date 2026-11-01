#include "include/usb_uhci.h"
#include "include/usb_driver.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

static uint8_t raw_frame_buffer[4096 * 2] __attribute__((aligned(16)));
uint32_t *frame_list_ptr = NULL;
uhci_qh_t control_qh __attribute__((aligned(16)));
uhci_qh_t *control_qh_ptr;
uhci_td_t control_tds[MAX_CONTROL_TDS] __attribute__((aligned(16)));
uhci_td_t *control_tds_ptr;
uint8_t setup_static[32] __attribute__((aligned(16)));
uint8_t *setup_static_ptr;

uhci_qh_t bulk_qh __attribute__((aligned(16)));
uhci_qh_t *bulk_qh_ptr;
uhci_td_t bulk_tds[MAX_BULK_TDS] __attribute__((aligned(16)));
uhci_td_t *bulk_tds_ptr;

uhci_queue_t g_control_queue;
uhci_queue_t g_bulk_queue;

/* A/B-флаг: если 1 — возвращаем старое поведение «wbinvd перед каждым DMA».
   По умолчанию 0: на i386 (non-PAE) DMA когерентен с кэшем, а wbinvd на
   ноутбуках может вызывать SMI и ломать тайминги кадров UHCI. */
int uhci_force_wbinvd = 0;

/* Активный контроллер (см. usb_uhci.h). TD/QH — общая статика, поэтому
   транзакты крутим только через один HC за раз. */
uhci_hc_t *g_uhci_active = NULL;

/* ==================== Пул хост-контроллеров UHCI ====================
   Реестр живёт здесь (usb_uhci.c), чтобы usb_driver.c (probe) и
   select_host видели один и тот же массив без циклической зависимости. */
static uhci_hc_t uhci_hosts[UHCI_MAX_HCS];
static int       uhci_hosts_count = 0;

uhci_hc_t *uhci_get_host(int idx)
{
    if (idx < 0 || idx >= uhci_hosts_count) return NULL;
    return &uhci_hosts[idx];
}
int uhci_get_host_count(void) { return uhci_hosts_count; }

void uhci_select_host(uhci_hc_t *hc)
{
    g_uhci_active = hc;
    /* Останавливаем ВСЕ хост-контроллеры кроме выбранного: их FLBASEADD
       всё ещё указывает на наши же QH — активный чужой HC будет воровать
       TD из очереди и гонять пакеты по своему (пустому) root-hub.
       ВАЖНО: пишем CMD только если RS реально взведён (лишние записи
       USBCMD на ICH могут смахнуть EGSM/RS состояние). */
    for (int i = 0; i < uhci_hosts_count; i++) {
        uhci_hc_t *other = &uhci_hosts[i];
        if (!other || other == hc) continue;
        uint16_t cmd = inw(other->io_base + UHCI_USBCMD);
        if (cmd & USBCMD_RS) outw(other->io_base + UHCI_USBCMD, cmd & ~USBCMD_RS);
    }
}

/* ==================== PCI config helpers (ICH6 legacy fix) ==================== */
/* Прототипы: эти хелперы определены ниже в файле, но используются внутри
   функций, объявленных выше (uhci_disable_legacy / uhci_add_host). */
static uint16_t portsc_reg(int port);
static void uhci_write_port(uhci_hc_t *hc, int port, uint16_t rw_bits, int clear_changes);
int  uhci_init(uhci_hc_t *hc);   /* реинициализация HC между попытками сброса */
static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) |
         ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFCu));
    return inl(0xCFC + (off & 3));
}

static void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val)
{
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) |
         ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFCu));
    outl(0xCFC + (off & 3), val);
}

/* ==================== Root-hub Resume (фикс для CF-18 / ICH6) ====================
   Panasonic CF-18 после выхода из BIOS часто оставляет контроллер в Global
   Suspend (USBCMD.EGSM=1), а порты — в suspend (PORTSC.SUSPEND=1): линии D+
   удерживаются в низком уровне. В таком состоянии даже с PED=1 все транзакты
   уходят в CRC/Time-out ("prclr=0 done ccs=0 ped=0 fin=288"). Функция снимает
   EGSM, выключает suspend порта и подаёт resume-посылку >=20 мс (USB spec
   tRS = 10..20 мс), затем W1C-сит USBSTS.RESUME. Возвращает 1, если что-то
   реально пришлось "будить". */
/* Запись состояния порта СТРОГО по spec: из текущего значения оставляем
   только R/W-биты (PED/PR/SUSP), RO-биты (CCS/LSDA/OCI...) при записи
   ОБЯЗАТЕЛЬНО зануляем — запись 1 в CCS на ICH ломает состояние порта
   (это и был источник "pre=91 -> fin=288"). W1C-биты ставим единицей,
   если надо очистить change-флаги. */
static void uhci_write_port(uhci_hc_t *hc, int port, uint16_t rw_bits, int clear_changes)
{
    uint16_t reg = portsc_reg(port);
    uint16_t cur = inw(hc->io_base + reg);
    uint16_t val = (uint16_t)((cur & PORTSC_RW_MASK) | (rw_bits & PORTSC_RW_MASK));
    if (clear_changes) val |= PORTSC_W1C_ALL;
    outw(hc->io_base + reg, val);
}

/* ICH6 (и большинство южных мостов Intel) имеют в конфигурационном
   пространстве UHCI на смещении 0xC0 регистр USBLEGSUP (LEG SMIs Enable):
     бит 0      SMIEN  — разрешает SMI на события USB (connect/disconnect,
                         suspend/resume). Пока он взведён, BIOS-смущающий
                         SMI-код может ВСЕГДА возвращать хост-контроллер в
                         Global Suspend между нашими операциями: PR игнори-
                         руется железом, PED не поднимается (fin=0x288).
     бит 8      HCSEL  — ownership: 0 = HC принадлежит SMM/BIOS!
     бит 9      OSRSTCTL/SYSRSTCTL, бит 10 ECP select и т.д.
   Linux (uhci-hcd/pci-quirks: usb_legacy_disable) и Haiku перед работой с
   UHCI пишут сюда 0x0000 (или читают-модифицируют-пишут, сбрасывая SMIEN и
   выставляя HCSEL). Делаем то же самое при регистрации каждого хоста. */
static void uhci_disable_legacy(uhci_hc_t *hc)
{
    uint32_t v = pci_cfg_read32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0xC0);
    /* Сбрасываем SMIEN (bit0) и выставляем HCSEL (bit8) + SWRDC (bit9) —
       просим firmware отдать нам контроллер и не трогать его SMI'ами. */
    uint32_t nv = (v & ~0x1u) | (1u << 8);
    pci_cfg_write32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0xC0, nv);
    uint32_t chk = pci_cfg_read32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0xC0);
    api_ptr->load_screen_add_string("UHCImem f=%d LEGSUP %x->%x (chk=%x)",
                                    hc->pci_func, v, nv, chk);
}

static int uhci_resume_port(uhci_hc_t *hc, int port)
{
    if (!hc) return 0;
    uint16_t reg = portsc_reg(port);
    int woke = 0;

    uint16_t cmd = inw(hc->io_base + UHCI_USBCMD);
    if (cmd & USBCMD_EGSM) {                 /* выход из Global Suspend */
        outw(hc->io_base + UHCI_USBCMD, cmd & ~(uint16_t)USBCMD_EGSM);
        uhci_mdelay(5);
        woke = 1;
        /* Если EGSM поднялся СРАЗУ после снятия — нас душит SMM/SMI:
           дополнительно лочим legacy-режим прямо сейчас. */
        if (inw(hc->io_base + UHCI_USBCMD) & USBCMD_EGSM) {
            uhci_disable_legacy(hc);
            outw(hc->io_base + UHCI_USBCMD,
                 inw(hc->io_base + UHCI_USBCMD) & ~(uint16_t)USBCMD_EGSM);
            uhci_mdelay(5);
        }
    }

    uint16_t ps = inw(hc->io_base + reg);
    if (ps & PORTSC_SUSPEND) {               /* suspend конкретного порта */
        /* deassert SUSPEND -> HC начнёт J-state (resume) на линии.
           Запись собирается ТОЛЬКО из R/W-бит: CCS/LSDA при записи обязаны
           быть 0 (см. uhci_write_port). Change-биты чистим единицами W1C. */
        uhci_write_port(hc, port, 0, 1);
        uhci_mdelay(25);                     /* hold resume >= 20 мс */
        uhci_write_port(hc, port, 0, 0);     /* гарантируем SUSP=0 */
        woke = 1;
    }

    uint16_t sts = inw(hc->io_base + UHCI_USBSTS);
    if (sts & USBSTS_RESUME) {               /* W1C Resume Detected */
        outw(hc->io_base + UHCI_USBSTS, sts & USBSTS_RESUME);
        woke = 1;
    }
    return woke;
}

uint32_t usb_virt_to_phys(uint32_t va)
{
    // ЖЕСТКИЙ АППАРАТНЫЙ ФИКС: Убираем ручной обход CR3, который возвращал мусор на реальном ПК!
    // Поскольку код драйвера выполняется в Identity-Mapped (плоской) нижней памяти ядра,
    // виртуальный Си-адрес переменной гарантированно равен её физическому адресу в ОЗУ.
    // Возвращаем flat-адрес напрямую процессору и Bus-Master контроллеру UHCI!
    return va;
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val)
{
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

int uhci_probe(uhci_hc_t *hc)
{
    return 0;    // Больше не нужен, probe делает usb_driver.c
}

int uhci_wait_hcreset(uhci_hc_t *hc)
{
    for (uint32_t i = 0; i < 100000; i++)
    {
        if (!(inw(hc->io_base + UHCI_USBCMD) & USBCMD_HCRESET)) return 0;
    }
    return -1;
}

void uhci_init_queues(void)
{
    memset(&g_control_queue, 0, sizeof(uhci_queue_t));
    memset(&g_bulk_queue, 0, sizeof(uhci_queue_t));

    uint32_t dummy_ctrl_phys = usb_virt_to_phys((uint32_t)&g_control_queue.dummy_td);
    g_control_queue.qh.element = dummy_ctrl_phys & ~0x0F;
    g_control_queue.dummy_td.link = TD_LINK_TERM;
    g_control_queue.dummy_td.status = 0;
    g_control_queue.tail_td = &g_control_queue.dummy_td;

    uint32_t dummy_bulk_phys = usb_virt_to_phys((uint32_t)&g_bulk_queue.dummy_td);
    g_bulk_queue.qh.element = dummy_bulk_phys & ~0x0F;
    g_bulk_queue.dummy_td.link = TD_LINK_TERM;
    g_bulk_queue.dummy_td.status = 0;
    g_bulk_queue.tail_td = &g_bulk_queue.dummy_td;

    memset(control_tds, 0, sizeof(control_tds));
    memset(bulk_tds, 0, sizeof(bulk_tds));
}

/* Утилиты времени. IO 0x43 (PIT command/status) на i386 — write-only,
   но чтение из него не имеет побочных эффектов и даёт ~1 мкс задержку;
   это стандартный «io busy wait» (аналог io_delay / mdelay в Linux). */
static inline void uhci_io_wait(void)
{
    inb(0x43);
}

static inline uint64_t uhci_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Оценка TSC в герцах через PIT-канал 2 (режим one-shot, gate уже поднят
   BIOS'ом). Однократно ~54.6 мс. Если PIT не отвечает — правдоподобная
   оценка для Pentium M CF-18 (~1.2 ГГц).
   Важно: freestanding i686-elf без libgcc НЕ предоставляет __udivdi3
   (64-битное деление), линковка драйвера его не подтягивает ->
   "undefined reference to '__udivdi3'". Поэтому здесь нет ни одного
   деления/умножения 64-битных величин между собой: только сдвиги и
   32-битные mul/div, которые gcc эмитит инструкциями 'imul'/'div'. */
static uint32_t g_tsc_hz = 0;

/* hz = delta * 1193182 / 65535, где delta <= 2.2e8 (см. лимит выше).
   Реализация «на вычитаниях» без 64/64-деления:
   delta*1193182/65535 = delta*18 + delta*(1193182-18*65535)/65535
                       = delta*18 + delta*4852/65535.
   Оба члена считаем 32-битной арифметикой с контролем переполнения. */
static uint32_t uhci_tsc_hz_from_delta(uint32_t delta)
{
    uint32_t whole = delta * 18u;          /* < 4e9, в uint32 помещается */
    /* second = delta*4852/65535. Дробь мала (< delta/13.5), но произведение
       может переполнить 32 бита, поэтому делим на 2^16 приближённо:
       delta*4852/65535 ≈ (delta/65535)*4852 + остаток. Считаем точно
       длинным делением только 32-битного произведения:
       delta <= 2.2e8 => delta*4852 <= 1.07e12 — не влезает в 32 бита.
       Разбиваем: delta = q*65535 + r, тогда
       delta*4852/65535 = q*4852 + r*4852/65535  (r*4852 < 3.2e8 — влезает!) */
    uint32_t q = delta / 65535u;           /* 32/32 div — инлайнится в 'div' */
    uint32_t r = delta - q * 65535u;
    uint32_t frac = (r * 4852u) / 65535u;  /* оба операнда 32-битные */
    return whole + q * 4852u + frac;
}

static uint32_t uhci_tsc_hz(void)
{
    if (g_tsc_hz) return g_tsc_hz;

    uint64_t t0 = uhci_rdtsc();

    outb(0x43, 0xB2);            /* ch2, latch select, binary, one-shot */
    outb(0x42, 0xFF);            /* count = 0xFFFF -> ~54.6 мс */
    outb(0x42, 0xFF);

    uint32_t guard = 0;
    while (!(inb(0x61) & 0x20))  /* бит TIMEOUT.OUT ждёт до переполнения */
        if (++guard > 30000000u) break;

    uint64_t t1 = uhci_rdtsc();

    if (guard >= 30000000u || t1 <= t0) {
        g_tsc_hz = 1200000000u;  /* fallback: типичная частота CF-18 */
    } else {
        uint64_t delta = t1 - t0;
        /* Ограничиваем delta сверху: 4 ГГц * 54.6 мс ≈ 2.18e8 тактов.
           Приведение к uint32 безопасно после клампинга (без 64-бит del). */
        if (delta > 220000000ull) delta = 220000000ull;
        uint32_t hz = uhci_tsc_hz_from_delta((uint32_t)delta);
        if (hz < 100000000u) hz = 100000000u;    /* sanity: <100 МГц */
        if (hz > 4000000000u) hz = 4000000000u;  /* sanity: >4 ГГц */
        g_tsc_hz = hz;
    }
    return g_tsc_hz;
}

/* ТОЧНАЯ задержка в миллисекундах по TSC. Прежняя версия на «250 портовом
   I/O цикле» давала ~100 микросекунд вместо заявленных миллисекунд: сброс
   порта держался ~2 мс вместо требуемых 10+ мс, поэтому контроллер так и
   не выставлял PED (порт оставался disabled, fin=0x93). */
void uhci_mdelay(uint32_t ms)
{
    /* Все операции — 32-битные (gcc эмитит 'div eax', без __udivdi3):
       cpms = tsc_hz / 1000 < 4e6; cycles64 = cpms * ms — только умножение,
       оно раскладывается в 'imul' с расширением, RTL-вызовов нет. */
    uint32_t cpms = uhci_tsc_hz() / 1000u;
    uint64_t cycles = (uint64_t)cpms * (uint64_t)ms;
    uint64_t start  = uhci_rdtsc();
    while (uhci_rdtsc() - start < cycles) {
        uhci_io_wait();
    }
}

/* Точная задержка в микросекундах. udelays нужны для ожидания авто-снятия
   PR: UHCI держит reset ровно ~10 мс и снимает PR сам; polling шагом 2+ мс
   легко проскочить момент или, наоборот, ждать лишние секунды.
   Всё 32-битное: cpu_us = hz[Гц] / 1e6 <= 4000 (int-div константой gcc
   разворачивает в imul+shift, вызовов libgcc нет). */
void uhci_udelay(uint32_t us)
{
    uint32_t cpu_us = uhci_tsc_hz() / 1000000u;
    if (cpu_us == 0) cpu_us = 1;
    uint64_t cycles = (uint64_t)cpu_us * (uint64_t)us;
    uint64_t start  = uhci_rdtsc();
    while (uhci_rdtsc() - start < cycles) {
        uhci_io_wait();
    }
}

/* Подробный дамп root-hub: регистры + пофлагровая расшифровка обоих портов.
   Формат строки: "DUMP tag CMD STS PORT1 PORT2 FR" и далее побитовые флаги
   порта 1 и порта 2: C(connect) c(CSC-change) E(PED) e(PEDC) o(OCI) O(OCIC)
   L(LSDA) R(PR) S(SUSP). Плюс отдельная проверка живости I/O-портов HC. */
void uhci_dump_ports(uhci_hc_t *hc, const char *tag)
{
    if (!hc || !api_ptr || !api_ptr->load_screen_add_string) return;
    uint16_t cmd = inw(hc->io_base + UHCI_USBCMD);
    uint16_t sts = inw(hc->io_base + UHCI_USBSTS);
    uint16_t p1  = inw(hc->io_base + UHCI_PORTSC1);
    uint16_t p2  = inw(hc->io_base + UHCI_PORTSC2);
    uint16_t fr  = inw(hc->io_base + UHCI_FRNUM);
    api_ptr->load_screen_add_string("DUMP %s CMD=%x STS=%x P1=%x P2=%x FR=%x",
                                    tag, cmd, sts, p1, p2, fr);
    api_ptr->load_screen_add_string("  P1[%c%c%c%c%c%c%c%c%c] P2[%c%c%c%c%c%c%c%c%c]",
        (p1 & PORTSC_CCS)     ? 'C':'-', (p1 & PORTSC_CSC)    ? 'c':'-',
        (p1 & PORTSC_PED)     ? 'E':'-', (p1 & PORTSC_PEDC)   ? 'e':'-',
        (p1 & PORTSC_OCI)     ? 'o':'-', (p1 & PORTSC_OCIC)   ? 'O':'-',
        (p1 & PORTSC_LSDA)    ? 'L':'-', (p1 & PORTSC_PR)     ? 'R':'-',
        (p1 & PORTSC_SUSPEND) ? 'S':'-',
        (p2 & PORTSC_CCS)     ? 'C':'-', (p2 & PORTSC_CSC)    ? 'c':'-',
        (p2 & PORTSC_PED)     ? 'E':'-', (p2 & PORTSC_PEDC)   ? 'e':'-',
        (p2 & PORTSC_OCI)     ? 'o':'-', (p2 & PORTSC_OCIC)   ? 'O':'-',
        (p2 & PORTSC_LSDA)    ? 'L':'-', (p2 & PORTSC_PR)     ? 'R':'-',
        (p2 & PORTSC_SUSPEND) ? 'S':'-');
    /* Живость планировщика: FRNUM обязан меняться при RS=1. Если не меняется
       — контроллер остановлен (HCH) или PCI I/O вообще не доходит. */
    if (cmd & USBCMD_RS) {
        uhci_udelay(2500);          /* >2 кадров: счётчик обязан подвинуться */
        uint16_t fr2 = inw(hc->io_base + UHCI_FRNUM);
        if (fr2 == fr)
            api_ptr->load_screen_add_string("DUMP %s DEAD! FR stuck at %x HCH=%d",
                                            tag, fr, !!(sts & USBSTS_HCH));
    }
}


/* Регистрирует UHCI-контроллер по данным PCI-зонда (массив — наверху файла).
   Возвращает 0/-1. */
int uhci_add_host(uint16_t io_base, uint8_t bus, uint8_t dev, uint8_t func)
{
    if (uhci_hosts_count >= UHCI_MAX_HCS) return -1;
    // Защита от дублей той же пары шины/функции
    for (int i = 0; i < uhci_hosts_count; i++)
        if (uhci_hosts[i].pci_bus == bus && uhci_hosts[i].pci_dev == dev &&
            uhci_hosts[i].pci_func == func) return 0;
    uhci_hc_t *hc = &uhci_hosts[uhci_hosts_count++];
    hc->io_base  = io_base;
    hc->pci_bus  = bus;
    hc->pci_dev  = dev;
    hc->pci_func = func;
    hc->current_device_low_speed = 0;
    hc->dbg_td0_status = 0; hc->dbg_td0_token = 0;
    hc->dbg_last_frame = 0; hc->dbg_sts = 0; hc->dbg_portsc = 0; hc->dbg_swstamp = 0;
    return 0;
}

/* Сбрасывает реестр (вызывается перед PCI-сканом при каждом usb_start). */
void uhci_reset_host_list(void)
{
    uhci_hosts_count = 0;
    memset(uhci_hosts, 0, sizeof(uhci_hosts));
    g_uhci_active = NULL;
}

int uhci_init(uhci_hc_t *hc)
{
    if (!hc) return -1;

    // 1) Гарантируем маску 0x07 на шине PCI для удержания Bus Master и Memory Space
    uint32_t pci_cmd = pci_read32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0x04);
    pci_write32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0x04, (pci_cmd & ~0x7u) | 0x07u);

    // 1b) КРИТИЧНО для CF-18/ICH6: отбираем контроллер у firmware.
    //     USBLEGSUP(0xC0): SMIEN=0 — BIOS-EMI больше не сможет загонять HC
    //     обратно в Global Suspend между нашими операциями (это и есть
    //     причина "prclr=0 ccs=0 ped=0 fin=288": PR физически игнорируется,
    //     пока SMM держит ownership). HCSEL=1 + SWRDC — просьба отдать HC ОС.
    uhci_disable_legacy(hc);

    // 2) ПОЛНЫЙ аппаратный сброс контроллера (HCRESET), как делает uhci-hcd в Linux:
    //    "uhci_reset: outw(USBCMD_HCRESET); while (inw(USBCMD) & HCRESET) udelay(...)"
    //    Сброс вычищает BIOS-состояние портов, планировщика и ошибки USBSTS.
    outw(hc->io_base + UHCI_USBCMD, USBCMD_HCRESET);
    {
        uint32_t guard = 0;
        while ((inw(hc->io_base + UHCI_USBCMD) & USBCMD_HCRESET) && guard < 100000u)
            guard++;
        if (guard >= 100000u) return -2;   // HC не отвечает по PCI -> дальше нет смысла
    }

    // 3) Инициализируем скелетные асинхронные очереди (Control & Bulk)
    uhci_init_queues();

    uint32_t control_qh_phys = usb_virt_to_phys((uint32_t)&g_control_queue.qh);
    uint32_t bulk_qh_phys    = usb_virt_to_phys((uint32_t)&g_bulk_queue.qh);
    if (!control_qh_phys || !bulk_qh_phys) return -4;

    // Связываем QH в плоскую иерархию: Control QH -> Bulk QH -> TERM
    g_control_queue.qh.head = (bulk_qh_phys & ~0x0Fu) | TD_LINK_Q;
    g_bulk_queue.qh.head    = TD_LINK_TERM;

    // ==================== FLBASEADD: адрес, кратный 4096 (UHCI spec) ====================
    uint32_t raw_addr = (uint32_t)raw_frame_buffer;
    uint32_t aligned_addr = (raw_addr + 4095u) & ~4095u; // Ручной сдвиг на начало чистой страницы

    frame_list_ptr = (uint32_t *)aligned_addr;
    uint32_t fl_phys = usb_virt_to_phys(aligned_addr);

    if (!fl_phys || (fl_phys & 0xFFFu)) {
        return -5; // Аварийный выход, если физический адрес имеет смещение
    }

    // Заполняем выровненную таблицу кадров ссылками на нашу Control-очередь
    uint32_t fl_link_val = (control_qh_phys & ~0x0Fu) | TD_LINK_Q;
    for (int i = 0; i < 1024; i++) {
        frame_list_ptr[i] = fl_link_val;
    }

    outl(hc->io_base + UHCI_FLBASEADD, fl_phys);
    outw(hc->io_base + UHCI_SOFMOD, 64);

    // 4) После HCRESET все PORTSC чистые. Включаем порты (PED=1, bit2!) и
    //    снимаем ложные change-биты. ПРЕЖДЕ ЗДЕСЬ БЫЛ ФАТАЛЬНЫЙ БАГ:
    //      ps |= 0x0001   /* "Port Select / power-on" */
    // что ставило CCS (bit0) — а это READ-ONLY бит текущего подключения!
    // Его запись на ICH ломает состояние порта (в паре с "ps|CSC|PEDC" без
    // обнуления RO-битов это и есть причина "pre=91 -> fin=288": порт после
    // наших же записей терял CCS/CSC, сброс не запускался). Теперь пишем
    // строго PED через helper, оставляющий в значении только R/W-биты.
    for (int port = 0; port < 2; port++)
    {
        uhci_write_port(hc, port, PORTSC_PED, 1);   /* enable + W1C change */
    }
    uhci_mdelay(100);   /* spec: до 100 мс на подтверждение connect после HC reset */

    outw(hc->io_base + UHCI_USBSTS, 0x003F); // W1C всех статусных флагов
    outw(hc->io_base + UHCI_USBINTR, 0);     // polling-режим, прерывания не нужны

    /* 4b) Явно гарантируем выход из Global Suspend: на CF-18 BIOS/ACPI могут
       оставить EGSM=1 даже после HCRESET-нечувствительных состояний; в этом
       режиме планировщик формально крутится, но трансиверы портов обесточены
       (линии D+/D- в SEFS) — любой пакет даёт CRC/Time-out. */
    {
        uint16_t cmdg = inw(hc->io_base + UHCI_USBCMD);
        if (cmdg & USBCMD_EGSM) {
            outw(hc->io_base + UHCI_USBCMD, cmdg & ~(uint16_t)USBCMD_EGSM);
            api_ptr->load_screen_add_string("U%d: cleared EGSM global suspend",
                                            (int)(hc - uhci_hosts));
        }
    }

    // 5) Вертикальный обход очередей (depth-first) — корректный режим для
    //    multi-TD транзакций (SETUP->DATA->STATUS идут в одном кадре подряд).
    uint16_t cmdv = inw(hc->io_base + UHCI_USBCMD);
    outw(hc->io_base + UHCI_USBCMD, (cmdv & ~USBCMD_CF) | USBCMD_RS);
    inw(hc->io_base + UHCI_USBCMD); // PCI Flush

    for (uint32_t i = 0; i < 100000; i++)
    {
        if (!(inw(hc->io_base + UHCI_USBSTS) & USBSTS_HCH)) break; // УСПЕХ!
    }

    // 6) Даём root-hub осмыслить подключение: ждём CCS/PED по UHCI spec до 300мс.
    //    Возвращаем 0 даже если ничего не подключено — решение о порте принимает upper.
    uhci_mdelay(50);
    uhci_dump_ports(hc, "init");
    return 0;
}




static uint16_t portsc_reg(int port)
{
    return (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
}
uint16_t uhci_port_status(uhci_hc_t *hc, int port)
{
    return inw(hc->io_base + portsc_reg(port));
}
int uhci_port_connected(uhci_hc_t *hc, int port)
{
    return (uhci_port_status(hc, port) & PORTSC_CCS) ? 1 : 0;
}

/* КОРРЕКТНЫЙ сброс порта по UHCI Universal Host Controller Interface spec
   (UHCI Rev 1.1, §3.6 "Reset Resume" + Linux drivers/usb/host/uhci-hcd.c
   uhci_port_reset):
     1) Сбрасываем change-биты (W1C: CSC/PEDC)
     2) Устанавливаем PR (Port Reset) = 1 и держим НЕ МЕНЕЕ 10 мс
     3) Снимаем PR = 0
     4) Ждём, пока хост-контроллер сам выставит PED=1 (до ~250 мс; если за
        это время PED так и не поднялся — устройство не отвечает / его нет)
   Возвращает 0 при успехе (PED=1), -1/-2 при проблемах. */
/* Одна попытка сброса порта. Возвращает 0 при PED=1, -2 в противном случае.
   ВАЖНО (UHCI Rev1.1 §4.1, Linux uhci-hcd root_hub_init_ports):
   в UHCI бит PR автосбрасывается САМИМ контроллером по окончании 10-мс
   сброса. Программная запись PR=0 раньше срока ОБРЫВАЕТ сброс на середине
   — устройство остаётся в неопределённом состоянии и PED так и не
   поднимается (прежний лог: "done ccs=1 ped=0 fin=93", код -2). Поэтому
   здесь PR только устанавливается, а затем ждём, пока железо само его
   снимет, после чего ожидаем восстановления PED. */
static int uhci_reset_port_once(uhci_hc_t *hc, int port)
{
    if (!hc) return -1;
    uint16_t reg = portsc_reg(port);

    // 0) КРИТИЧНО для CF-18: BIOS часто оставляет контроллер в Global
    //    Suspend (EGSM=1) и порт в suspend (SUSPEND=1). В этом состоянии
    //    PR игнорируется железом. Сначала — полный resume root-hub'а.
    if (uhci_resume_port(hc, port)) {
        api_ptr->load_screen_add_string("RESET[%d] resumed from suspend/smi", port);
    }

    // 1) Порт ДОЛЖЕН быть включён (PED=1) перед сбросом. Записываем строго
    //    R/W-биты (см. helper): раньше сюда попадали CCS/LSDA из прочитанного
    //    значения — запись 1 в эти RO-биты на ICH выключает порт.
    uint16_t ps = inw(hc->io_base + reg);
    api_ptr->load_screen_add_string("RESET[%d] pre=%x", port, ps);
    uhci_write_port(hc, port, PORTSC_PED, 1);      /* PED=1, SUSP=0, W1C */
    uhci_mdelay(50);
    ps = inw(hc->io_base + reg);
    if (!(ps & PORTSC_PED)) {
        /* Некоторые ICH требуют второй прогон enable после connect-change */
        uhci_write_port(hc, port, PORTSC_PED, 1);
        uhci_mdelay(50);
        ps = inw(hc->io_base + reg);
    }
    if (!(ps & PORTSC_PED)) {
        api_ptr->load_screen_add_string("RESET[%d] no-PED before reset! ps=%x", port, ps);
    }

    // 2) Assert reset: PR=1. UHCI Rev1.1 §4.1: бит PR автосбрасывается САМ
    //    контроллер через ~10 мс. Софт НЕ должен трогать PR после установки
    //    (прежняя запись PR=0 раньше срока обрывала сброс на середине).
    uhci_write_port(hc, port, PORTSC_PED | PORTSC_PR, 0);
    uhci_udelay(100);
    uint16_t ps_asserted = inw(hc->io_base + reg);
    api_ptr->load_screen_add_string("RESET[%d] asserted=%x (PR=%d)",
                                    port, ps_asserted, !!(ps_asserted & PORTSC_PR));

    // 3) Ждём авто-снятия PR контроллером: polling каждые 200 мкс до 500 мс.
    //    Микросекундный шаг принципиален: PR висит ровно ~10 мс, при шаге
    //    2 мс мы стабильно фиксировали "prclr=0" просто не замечая события.
    int pr_clear = 0;
    uint32_t pr_us = 0;
    for (uint32_t i = 0; i < 2500; i++) {          /* 2500 * 200us = 500 ms */
        uint16_t st = inw(hc->io_base + reg);
        if (!(st & PORTSC_PR)) { pr_clear = 1; pr_us = i * 200u; break; }
        if (!(st & PORTSC_CCS)) { pr_us = i * 200u; break; }  /* device пропал */
        uhci_udelay(200);
    }
    api_ptr->load_screen_add_string("RESET[%d] PR auto-clear=%d after %d us",
                                    port, pr_clear, pr_us);

    /* USB spec 7.1.7.5: reset-recovery. Linux ждёт 10 мс + до 100 мс PED. */
    uhci_mdelay(10);

    // 4) Ждём PED=1 от железа (polling шагом 1 мс до 300 мс)
    int ped_ok = 0;
    uint32_t ped_ms = 0;
    for (uint32_t i = 0; i < 300; i++) {
        uint16_t st = inw(hc->io_base + reg);
        if ((st & (PORTSC_CCS | PORTSC_PED)) == (PORTSC_CCS | PORTSC_PED)) {
            ped_ok = 1; ped_ms = i; break;
        }
        if (!(st & PORTSC_CCS)) break;  // device физически исчез со шины
        uhci_mdelay(1);
    }

    uint16_t s_fin = inw(hc->io_base + reg);
    api_ptr->load_screen_add_string("RESET[%d] prclr=%d ccs=%d ped=%d(%dms) fin=%x",
                                    port, pr_clear, !!(s_fin & PORTSC_CCS),
                                    !!(s_fin & PORTSC_PED), ped_ms, s_fin);
    uhci_dump_ports(hc, "reset");

    if (!ped_ok) return -2;

    // Скорость устройства: LSDA=1 -> Low-Speed, иначе Full-Speed.
    // (USB-флешки ВСЕГДА full-speed на UHCI; LS означает клавиатуру/мышь.)
    hc->current_device_low_speed = (s_fin & PORTSC_LSDA) ? 1 : 0;
    return 0;
}

/* Публичная обёртка: до 4 попыток сброса. По USB spec hub/HC после deassert
   даёт устройству 10 мс «recovery», и часть флешек активируются только со
   второго-третьего reset'а (особенно если BIOS уже общался с ними и они
   остались в подвешенном состоянии).
   Лестница эскалации (по образцу Linux hub_port_init/hub_reset_port и
   Haiku USBAbsolutHostController::ResetPort):
     1) мягкий повтор сброса;
     2) power-cycle порта: disable (PED=0) -> пауза -> enable (PED=1) ->
        ожидание CCS/PED — это единственный способ "оживить" зависший
        трансивер ICH без сброса всего HC;
     3) полный HCRESET контроллера (uhci_init), как uhci_restart() в Linux. */
int uhci_reset_port(uhci_hc_t *hc, int port)
{
    if (!hc) return -1;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (uhci_reset_port_once(hc, port) == 0)
            return 0;
        api_ptr->load_screen_add_string("RESET[%d]: attempt %d failed, escalation %d",
                                        port, attempt + 1, attempt);
        uhci_mdelay(100);

        if (attempt == 0) {
            /* мягкий retry без вмешательства */
            continue;
        }
        if (attempt == 1) {
            /* Power-cycle порта: запись PED=0 глушит порт, запись PED=1
               включает; RO-биты при этом занулены (helper). */
            uhci_write_port(hc, port, 0, 1);          /* disable */
            uhci_mdelay(100);                          /* spec tPSCTORSTRIP... запас */
            uhci_write_port(hc, port, PORTSC_PED, 1);  /* enable */
            uhci_dump_ports(hc, "pcyc");
            continue;
        }
        /* attempt >= 2: полный ре-инит контроллера (HCRESET+RS+порты) */
        if (uhci_init(hc) != 0) return -4;
        uhci_mdelay(100);   /* CCS должен вернуться сам: pull-up устройства никуда делся не мог */
        if (!(uhci_port_status(hc, port) & PORTSC_CCS))
            api_ptr->load_screen_add_string("RESET[%d]: CCS lost after reinit", port);
    }
    return -2;
}






/* Разобрать status-слова TD в человекочитаемый код ошибки.
   Возвращает: 0 = OK; иначе — отрицательный код, по которому upper-слой
   решает, что делать (NAK -> retry, STALL -> halt, CRC/timeout -> reset). */
static int uhci_td_error_code(uint32_t st)
{
    if (st & TD_CTRL_ACTIVE)   return -10; // всё ещё активно (не должно быть на выходе)
    if (st & TD_CTRL_STALLED)  return -4;  // device STALL (endpoint halted)
    if (st & TD_CTRL_DBUFERR)  return -5;  // host-side data buffer error
    if (st & TD_CTRL_BABBLE)   return -6;  // babble — устройство «болтает» по шине
    if (st & TD_CTRL_CRCTIMEO) return -7;  // CRC/Time-out — аппаратный таймаут фазы
    if (st & TD_CTRL_BITSTUFF) return -8;  // bit-stuffing ошибка на линии
    return 0;
}

int uhci_control_transfer(uhci_hc_t *hc, uint8_t dev_addr, const uint8_t *setup_pkt, void *data_buf, uint16_t data_len, int data_in, uint16_t max_pkt)
{
    if (!hc || !setup_pkt) return -1;
    if (max_pkt == 0 || max_pkt > 64) max_pkt = 8;

    /* Транзакты крутим только через ОДИН контроллер за раз (общие TD/QH). */
    uhci_select_host(hc);

    /* CF-18/ICH6: BIOS или SMI могут оставить HC в Global Suspend / порт в
       suspend между транзактами. На "спящей" линии D+ любой SETUP уходит в
       CRC/Time-out даже при ped=1. Перед КАЖДОЙ цепочкой TD проверяем
       USBSTS.RESUME и бит SUSPEND порта и при необходимости будим root-hub
       (снятие EGSM, deassert SUSPEND, resume >=20 мс, W1C RESUME). */
    {
        uint16_t regp = portsc_reg(0), regp2 = portsc_reg(1);
        uint16_t psx  = inw(hc->io_base + regp) | inw(hc->io_base + regp2);
        uint16_t stsx = inw(hc->io_base + UHCI_USBSTS);
        uint16_t cmdx = inw(hc->io_base + UHCI_USBCMD);
        if ((stsx & USBSTS_RESUME) || (psx & PORTSC_SUSPEND) || (cmdx & USBCMD_EGSM)) {
            /* Определяемsuspend-порт для точечного resume; если непонятно —
               будим оба (resume-посылка на свободном порту безвредна). */
            int woke = 0;
            if (inw(hc->io_base + regp)  & PORTSC_SUSPEND) woke |= uhci_resume_port(hc, 0);
            if (inw(hc->io_base + regp2) & PORTSC_SUSPEND) woke |= uhci_resume_port(hc, 1);
            woke |= uhci_resume_port(hc, 0);   /* снимает EGSM + W1C RESUME */
            if (woke)
                api_ptr->load_screen_add_string("UHCI resumed before xfer sts=%x cmd=%x",
                                                stsx, cmdx);
        }
    }

    uint32_t num_data = 0;
    int has_data = (data_len > 0 && data_buf != NULL);
    if (has_data)
    {
        num_data = (data_len + max_pkt - 1) / max_pkt;
        if (num_data > MAX_CONTROL_TDS - 2) num_data = MAX_CONTROL_TDS - 2;
    }

    /* Готовим ВСЮ цепочку TD до того, как контроллер увидит TD0:
       глушим IOC на промежуточных TD и чистим статусы от ошибок прошлого
       транзакта. В прошлой версии у "мусорных" TD оставался старый status
       без Active-бита — HC обрывал цепочку, а STATUS-TD вообще не участвовал
       в цикле ожидания (в логе печатался stale 0x1a800000). */
    for (uint32_t i = 0; i < MAX_CONTROL_TDS; i++) control_tds[i].status = 0;

    // Копируем setup-пакет строго в выровненный flat-буфер
    for (int i = 0; i < 8; i++) setup_static[i] = setup_pkt[i];
    uint32_t setup_phys = usb_virt_to_phys((uint32_t)setup_static);

    uint32_t td0_phys   = usb_virt_to_phys((uint32_t)&control_tds[0]);
    uint32_t td1_phys   = usb_virt_to_phys((uint32_t)&control_tds[1]);
    uint32_t qh_phys    = usb_virt_to_phys((uint32_t)&g_control_queue.qh);
    uint32_t dummy_phys = usb_virt_to_phys((uint32_t)&g_control_queue.dummy_td);

    uint32_t ls_flag = hc->current_device_low_speed ? TD_CTRL_LS : 0u;

    // --- TD0: SETUP Stage ---
    // VF (bit2) в link — корректный depth-first обход цепочки TD внутри QH.
    control_tds[0].link   = (td1_phys & ~0x0Fu) | TD_LINK_VF;
    control_tds[0].status = TD_CTRL_ACTIVE | TD_CTRL_C_ERR_MASK; // C_ERR=11: 3 попытки HC сам
    if (ls_flag) control_tds[0].status |= ls_flag;
    /* ТОКЕН SETUP: PID(8b) | ADDR(7b, биты 8-14) | ENDP=0 (биты 15-18) | DATA0 | LEN=7 (биты 20-30).
       ЭНДПОИНТ В TOKEN ОБЯЗАТЕЛЕН! Без него поле len=7 сдвигалось на бит левее
       и "засоряло" старший байт токена (setup=0x00085007 вместо 0x002D0007):
       контроллер отправлял на шину мусор, устройство молчало -> CRC/Time-out. */
    control_tds[0].token  = (PID_SETUP << TD_TOKEN_PID_SHIFT) | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT) | (0u << TD_TOKEN_EP_SHIFT) | (7u << TD_TOKEN_LEN_SHIFT);
    control_tds[0].buffer = setup_phys;
    hc->dbg_td0_token = control_tds[0].token;

    // --- TD[1..num_data]: DATA Stage ---
    for (uint32_t i = 0; i < num_data; i++)
    {
        uint32_t idx = 1 + i;
        uint32_t off = i * max_pkt;
        uint32_t chunk = (data_len - off > max_pkt) ? max_pkt : (data_len - off);
        uint32_t toggle = ((i & 1) == 0) ? TD_TOKEN_TOGGLE : 0u; // ep0 DATA: первый пакет PID_DATA1

        uint32_t next_idx = (i + 1 < num_data) ? (idx + 1) : (1 + num_data);
        uint32_t next_td_phys = usb_virt_to_phys((uint32_t)&control_tds[next_idx]);

        control_tds[idx].link = (next_td_phys & ~0x0Fu) | TD_LINK_VF;
        control_tds[idx].status = TD_CTRL_ACTIVE | TD_CTRL_C_ERR_MASK | ls_flag;
        control_tds[idx].token = ((data_in ? PID_IN : PID_OUT) << TD_TOKEN_PID_SHIFT) |
                                 ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT) |
                                 (0u << TD_TOKEN_EP_SHIFT) |
                                 toggle | ((chunk - 1) << TD_TOKEN_LEN_SHIFT);

        uint32_t pa = usb_virt_to_phys((uint32_t)((uint8_t *)data_buf + off));
        if (pa == 0) return -1;
        control_tds[idx].buffer = pa;
    }

    // --- TD[sidx]: STATUS Stage ---
    uint32_t sidx = 1 + num_data;
    uint32_t status_pid = has_data ? (data_in ? PID_OUT : PID_IN) : PID_IN;

    control_tds[sidx].link   = TD_LINK_TERM;
    control_tds[sidx].status = TD_CTRL_ACTIVE | TD_CTRL_IOC | TD_CTRL_C_ERR_MASK | ls_flag;
    // STATUS stage всегда PID_DATA1 (toggle=1), длина 0 => поле length = 0x7FF
    control_tds[sidx].token = (status_pid << TD_TOKEN_PID_SHIFT) |
                              ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT) |
                              (0u << TD_TOKEN_EP_SHIFT) |
                              TD_TOKEN_TOGGLE | (0x7FFu << TD_TOKEN_LEN_SHIFT);
    control_tds[sidx].buffer = setup_phys;

    uint32_t status_td_status_off = (uint32_t)&control_tds[sidx].status - (uint32_t)&control_tds[0];

    api_ptr->load_screen_add_string("UHCI DMA Map: QH=%x TD0=%x TD_stat_off=%x", qh_phys, td0_phys, status_td_status_off);

    // Синхронизация записи дескрипторов перед тем, как HC их увидит
    usb_dma_sync_before();
    if (uhci_force_wbinvd) __asm__ __volatile__("wbinvd" ::: "memory");

    // Сбрасываем хвост очереди и врезаем транзакцию в асинхронный планировщик
    g_control_queue.tail_td = &g_control_queue.dummy_td;
    g_control_queue.qh.element = td0_phys & ~0x0Fu;
    __asm__ __volatile__("" ::: "memory");

    uint32_t timeout = 5000000;   // ~несколько секунд даже на медленном CPU
    uint32_t spins = 0;
    int result = -3;

    volatile uint32_t *status_slot = (volatile uint32_t *)&control_tds[sidx].status;

    while (timeout--)
    {
        spins++;
        /* Ждём ЗАВЕРШЕНИЯ ВСЕЙ транзакции: последний TD цепочки (STATUS stage).
       Раньше цикл смотрел только на TD0 — при ошибке SETUP он выходил раньше,
       чем HC успевал пометить ошибкой хвостовой TD. */
        uint32_t st_tail = *status_slot;
        if (!(st_tail & TD_CTRL_ACTIVE))
        {
            /* Хвост завершён без Active — проходим по всей цепочке в поисках
               реального кода ошибки любого из TD. */
            int ec = 0;
            for (uint32_t k = 0; k <= sidx; k++)
            {
                ec = uhci_td_error_code(control_tds[k].status);
                if (ec) break;
            }
            result = ec ? ec : (int)data_len; // УСПЕХ!
            break;
        }
        /* Если TD0 уже упал с ошибкой, а хвост всё ещё Active — HC оборвал
           цепочку: дождись сброса Active на TD0 или выходи по общему таймауту. */
    }

    // Диагностика для лога загрузки: что реально произошло на шине
    hc->dbg_swstamp    = spins;
    hc->dbg_last_frame = inw(hc->io_base + UHCI_FRNUM);
    hc->dbg_sts        = inw(hc->io_base + UHCI_USBSTS);
    hc->dbg_portsc     = inw(hc->io_base + UHCI_PORTSC1);
    api_ptr->load_screen_add_string("XFER done spins=%d sts=%x p1=%x p2=%x td0=%x tdt=%x",
                                    spins, hc->dbg_sts,
                                    inw(hc->io_base + UHCI_PORTSC1),
                                    inw(hc->io_base + UHCI_PORTSC2),
                                    control_tds[0].status, control_tds[sidx].status);
    __asm__ __volatile__("" ::: "memory");
    hc->dbg_td0_status = control_tds[0].status;

    /* Если хвостовой TD так и остался ACTIVE, а TD0 завершён с ошибкой —
       HC оборвал цепочку на SETUP. Подтягиваем код ошибки с TD0, чтобы
       upper-слой получил честный результат (-4 STALL / -7 CRC/TO и т.д.). */
    if (result == -3)
    {
        int ec0 = uhci_td_error_code(control_tds[0].status);
        if (!(control_tds[0].status & TD_CTRL_ACTIVE) && ec0) result = ec0;
    }

    // Если TD0 так и остался ACTIVE — движок не тронул очередь вообще
    // (HC остановлен / FLBASEADD неверен / нет питания порта).
    if (result == -3 && (hc->dbg_td0_status & TD_CTRL_ACTIVE)) {
        api_ptr->load_screen_add_string("UHCI DEAD? RS=%x STS=%x FR=%x",
                                        inw(hc->io_base + UHCI_USBCMD) & 1,
                                        hc->dbg_sts, hc->dbg_last_frame);
        // Глубокая реанимация: полный HCRESET + перезапуск движка
        outw(hc->io_base + UHCI_USBCMD, USBCMD_HCRESET);
        for (uint32_t i = 0; i < 100000 && (inw(hc->io_base + UHCI_USBCMD) & USBCMD_HCRESET); i++);
        outw(hc->io_base + UHCI_USBSTS, 0x003F);
        outw(hc->io_base + UHCI_USBCMD, USBCMD_RS);
    }

    // Восстанавливаем исходный пустой элемент очереди QH
    g_control_queue.qh.element = dummy_phys & ~0x0Fu;
    g_control_queue.tail_td = &g_control_queue.dummy_td;

    // Тотальный сброс кэша для синхронизации входящих данных флешки
    usb_dma_sync_after();
    if (uhci_force_wbinvd) __asm__ __volatile__("wbinvd" ::: "memory");

    if (result > 0 && data_in && data_buf)
    {
        volatile uint8_t *cp = (volatile uint8_t *)data_buf;
        api_ptr->load_screen_add_string("USB DATA READ OK! Bytes: %x %x %x %x | %x %x %x %x",
                                        cp[0], cp[1], cp[2], cp[3], cp[4], cp[5], cp[6], cp[7]);
    }

    api_ptr->debug8 = control_tds[0].status;
    api_ptr->debug9 = control_tds[sidx].status;
    return result;
}






int uhci_bulk_transfer(uhci_hc_t *hc, uint8_t dev_addr, uint8_t ep_addr, uint16_t ep_max_pkt, uint8_t *toggle, void *buf, uint32_t len, int dir_in)
{
    if (!hc || !buf || len == 0 || !toggle) return -1;
    if (ep_max_pkt == 0 || ep_max_pkt > 1024) ep_max_pkt = 64;

    /* Только один контроллер может крутить общие TD за раз. */
    uhci_select_host(hc);

    /* Будим root-hub перед bulk-транзакцией (см. комментарий в
       uhci_control_transfer): SMI/BIOS могли загнать порт в suspend между
       перечислением и чтением флешки — иначе каждый CBWDTO/CBWRET уходит
       в CRC/Time-out. */
    {
        uint16_t regp = portsc_reg(0), regp2 = portsc_reg(1);
        uint16_t psx  = inw(hc->io_base + regp) | inw(hc->io_base + regp2);
        uint16_t stsx = inw(hc->io_base + UHCI_USBSTS);
        uint16_t cmdx = inw(hc->io_base + UHCI_USBCMD);
        if ((stsx & USBSTS_RESUME) || (psx & PORTSC_SUSPEND) || (cmdx & USBCMD_EGSM)) {
            int woke = 0;
            if (inw(hc->io_base + regp)  & PORTSC_SUSPEND) woke |= uhci_resume_port(hc, 0);
            if (inw(hc->io_base + regp2) & PORTSC_SUSPEND) woke |= uhci_resume_port(hc, 1);
            woke |= uhci_resume_port(hc, 0);
            if (woke)
                api_ptr->load_screen_add_string("UHCI BULK resumed sts=%x cmd=%x", stsx, cmdx);
        }
    }

    uint32_t num_tds = (len + ep_max_pkt - 1) / ep_max_pkt;
    if (num_tds > MAX_BULK_TDS)
    {
        num_tds = MAX_BULK_TDS;
        len = num_tds * ep_max_pkt;
    }
    uint8_t ep_num = ep_addr & 0x0F;
    uint8_t cur_toggle = *toggle ? 1 : 0;
    uint8_t *b = (uint8_t *)buf;
    uint32_t ls_flag = hc->current_device_low_speed ? TD_CTRL_LS : 0u;
    for (uint32_t i = 0; i < num_tds; i++)
    {
        uint32_t off   = i * ep_max_pkt;
        uint32_t chunk = (len - off > ep_max_pkt) ? ep_max_pkt : (len - off);
        uint32_t toggle_bit = cur_toggle ? TD_TOKEN_TOGGLE : 0u;
        uint32_t next_td_phys = (i + 1 < num_tds) ? (usb_virt_to_phys((uint32_t)&bulk_tds[i + 1]) & ~0x0Fu) : TD_LINK_TERM;
        bulk_tds[i].link   = next_td_phys;
        bulk_tds[i].status = TD_CTRL_ACTIVE | TD_CTRL_C_ERR_MASK | ls_flag | (i == num_tds - 1 ? TD_CTRL_IOC : 0u);
        bulk_tds[i].token  = ((uint32_t)(dir_in ? PID_IN : PID_OUT) << TD_TOKEN_PID_SHIFT) | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT) | ((uint32_t)ep_num << TD_TOKEN_EP_SHIFT) | toggle_bit | ((uint32_t)(chunk - 1) << TD_TOKEN_LEN_SHIFT);
        uint32_t pa = usb_virt_to_phys((uint32_t)(b + off));
        if (pa == 0) return -1;
        bulk_tds[i].buffer = pa;
        cur_toggle ^= 1;
    }
    uint32_t first_bulk_td_phys = usb_virt_to_phys((uint32_t)&bulk_tds[0]);
    uint32_t dummy_td_phys      = usb_virt_to_phys((uint32_t)&g_bulk_queue.dummy_td);
    if (!first_bulk_td_phys || !dummy_td_phys) return -1;
    /* Хвост цепочки замыкаем на dummy TD очереди (VF — depth-first внутри QH),
       как это делает Linux: qh->element всегда валиден для планировщика. */
    bulk_tds[num_tds - 1].link = (dummy_td_phys & ~0x0Fu) | TD_LINK_VF;
    usb_dma_sync_before();
    if (uhci_force_wbinvd) __asm__ __volatile__("wbinvd" ::: "memory");
    g_bulk_queue.tail_td->link = (first_bulk_td_phys & ~0x0Fu) | TD_LINK_VF;
    g_bulk_queue.tail_td = &bulk_tds[num_tds - 1];
    uint32_t timeout = 5000000;
    int result = -3;
    while (timeout--)
    {
        __asm__ __volatile__("" ::: "memory");
        uint32_t st = bulk_tds[num_tds - 1].status;
        if (!(st & TD_CTRL_ACTIVE))
        {
            int ec = 0;
            for (uint32_t k = 0; k < num_tds; k++)
            {
                ec = uhci_td_error_code(bulk_tds[k].status);
                if (ec) break;
            }
            if (!ec)
            {
                *toggle = cur_toggle;
                result = (int)len;
            }
            else
            {
                result = ec;
                // STALL bulk-endpoint'а лечится ClearFeature(ENDPOINT_HALT) upper-слоем
            }
            break;
        }
    }
    if (result == -3) {
        hc->dbg_swstamp = timeout;
        hc->dbg_sts = inw(hc->io_base + UHCI_USBSTS);
        api_ptr->load_screen_add_string("BULK TIMEOUT sts=%x td0=%x", hc->dbg_sts, bulk_tds[0].status);
    }
    usb_dma_sync_after();
    if (uhci_force_wbinvd) __asm__ __volatile__("wbinvd" ::: "memory");
    return result;
}
void uhci_register_ptrs(void)
{
    //frame_list_ptr    = frame_list;
    frame_list_ptr    = (uint32_t *)raw_frame_buffer;
    control_qh_ptr    = &control_qh;
    control_tds_ptr   = control_tds;
    setup_static_ptr  = setup_static;
    bulk_qh_ptr       = &bulk_qh;
    bulk_tds_ptr      = bulk_tds;
}
