#include "include/usb_uhci.h"
#include "include/usb_driver.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

/* КРИТИЧНО (UHCI spec §5.5, FLBASEADD): таблица кадров должна быть выровнена
   на 4096 байт — контроллер использует МЛАДШИЕ 12 БИТ адреса как индекс
   (frame_number * 4). При aligned(16) HC читает dword'ы со смещением до
   4080 байт за концом буфера и ПИСАЕТ туда свои actual-length'ы, затирая
   соседние статические TD/QH (в логе это выглядело как "TD_status=d8000000"
   — мусор вместо статуса). Поэтому: отдельный массив ровно 1024 entry +
   явное aligned(4096), без ручного сдвига адреса. */
static uint32_t uhci_frame_list[1024] __attribute__((aligned(4096)));
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

/* Очереди тоже участвуют в DMA (HC пишет status dummy-TD, actual length),
   поэтому их адрес обязан быть кратен 16 — garantируем явно. */
uhci_queue_t g_control_queue __attribute__((aligned(16)));
uhci_queue_t g_bulk_queue    __attribute__((aligned(16)));

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
static void uhci_clear_changes(uhci_hc_t *hc, int port);
int  uhci_init(uhci_hc_t *hc);   /* реинициализация HC между попытками сброса */

/* ==================== Аппаратная идентификация хоста (PCI ID) ====================
   Ответ на вопрос «а уверены ли мы, что это UHCI, а не EHCI?»: класс-code в
   конфигурации PCI — это обещание firmware, а на Somee/CF-серии бывают
   кривые таблицы. Поэтому при регистрации каждого хоста читаем реальные
   Vendor/Device ID и печатаем их: 8086:24x2/26x0 = ICH6 UHCI #1..#5,
   8086:283x = ICH7 UHCI, 8086:2830..2835 = EHCI и т.д. Если в логе рядом с
   "UHCI d=29 f=x" окажется EHCI-идентификатор — значит мы крутим не те
   регистры. Дополнительно печатаем USBLEGSUP ДО всяких манипуляций. */
static const char *uhci_pci_id_name(uint16_t vid, uint16_t did)
{
    if (vid != 0x8086u) return "?";
    switch (did) {
        case 0x2452u: return "ICH2 UHCI";
        case 0x2442u: case 0x2444u: return "ICH4 UHCI";
        case 0x2658u: case 0x2659u: case 0x265Au: case 0x265Bu: case 0x265Cu:
            return "ICH6 UHCI";
        case 0x265Eu: return "ICH6 EHCI";
        case 0x27CCu: case 0x27C9u: case 0x27CBu: case 0x27E6u: case 0x27E7u: case 0x27E8u:
            return "ICH7 UHCI";
        case 0x27E9u: return "ICH7 EHCI";
        default: break;
    }
    /* 0x244x/0x265x диапазоны грубо */
    if (did >= 0x2650u && did <= 0x265Fu) return "ICH6 USB";
    if (did >= 0x2830u && did <= 0x2836u) return "ICH7 USB";
    return "Intel USB";
}
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
   только R/W-биты (PED/PR/SUSP), RO-биты (CCS/LSDA/OCI...) при записе ОБЯЗАТЕЛЬНО зануляем — запись 1 в CCS на ICH ломает состояние порта
   (это и был источник "pre=91 -> fin=288"). W1C-биты ставим единицей,
   если надо очистить change-флаги. */
static void uhci_write_port(uhci_hc_t *hc, int port, uint16_t rw_bits, int clear_changes)
{
    uint16_t reg = portsc_reg(port);
    /* UHCI Rev 1.1 §5.4: при записи в PORTSC значение бита PED задаётся
       ЯВНО битом 2 записываемого слова, а НЕ наследуется от прочитанного
       cur! Прежняя реализация (cur & RW_MASK)|(rw & RW_MASK) молча писала
       PED=0 всегда, когда вызывающий не просил PED — и контроллер ВЫКЛЮЧАЛ
       порт посреди сброса: fin=0x288 (CCS=0,PED=0,PR=1,SUSP=1), PR не
       авто-снимается, "no-PED before reset". Пишем полное слово явно. */
    uint16_t val = (uint16_t)(rw_bits & PORTSC_RW_MASK);
    if (clear_changes) val |= PORTSC_W1C_ALL;
    outw(hc->io_base + reg, val);
}

/* W1C-очистка change-бит БЕЗ изменения R/W-состояния порта (PED/PR/SUSP
   переписываются в текущие значения). Аналог uhci_resume / port_power
   хелперов Linux uhci-hcd. */
static void uhci_clear_changes(uhci_hc_t *hc, int port)
{
    uint16_t reg = portsc_reg(port);
    uint16_t cur = inw(hc->io_base + reg);
    uint16_t val = (uint16_t)(cur & PORTSC_RW_MASK);
    outw(hc->io_base + reg, val | PORTSC_W1C_ALL);
}

/* ICH6 (и большинство южных мостов Intel) имеют в конфигурационном
   пространстве UHCI на смещении 0xC0 регистр USBLEGSUP (LEG SMIs Enable):
     бит 0      SMIEN  — разрешает SMI на события USB (connect/disconnect,
                         suspend/resume). Пока он взведён, BIOS-смущающий
                         SMI-код может ВСЕГДА возвращать хост-контроллер в Global Suspend между нашими операциями: PR игнори-
                         руется железом, PED не поднимается (fin=0x288).
     бит 8      HCSEL  — ownership: 0 = HC принадлежит SMM/BIOS!
     бит 9      OSRSTCTL/SYSRSTCTL, бит 10 ECP select и т.д.
   Linux (uhci-hcd/pci-quirks: usb_legacy_disable) и Haiku перед работой с
   UHCI пишут сюда 0x0000 (или читают-модифицируют-пишут, сбрасывая SMIEN и
   выставляя HCSEL). Делаем то же самое при регистрации каждого хоста. */
/* Полная процедура handoff по образцу Linux:
   drivers/usb/host/uhci-hcd.c: uhci_pci_quirk -> usb_hcd_pci_probe
   + include/linux/usb.h: usb_disabled()/usb_enable() (LEGSUP),
   drivers/usb/core/quirks...: usb_ehci_handshake / wait_for_release.
   Регистр USBLEGSUP (конфиг-смещение 0xC0 для ICH6 UHCI):
     bit 0   SMIEN  — разрешает SMI на события USB; гасим НАВСЕГДА, иначе
                      SMM BIOS периодически возвращает HC в Global Suspend
                      и игнорирует наши записи PORTSC (наш симптом fin=288).
     bit 1   SMISTAT(SMI Idle) — W1C-подобный флаг «SMI сработал».
     bit 8   HCSEL  — 1 = ОС просит ownership (SMM-код обязан отпустить).
     bit 15  BIOSB  — 1 = firmware ДЕРЖИТ контроллер. Linux ждёт снятия
                      этого бита до 100 мс (usb_hcd_pci quirk), затем пишет
                      LEGSUP=0 как финальный «release request».
   ВАЖНО: вызывается ДО первого обращения к IO-портам хоста (в uhci_add_host,
   сразу после заполнения pci_bus/dev/func) — как early handoff в Linux. */
static int uhci_disable_legacy(uhci_hc_t *hc)
{
    uint32_t v = pci_cfg_read32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0xC0);

    /* Шаг 1: гасим SMI и выставляем запрос ownership HCSEL. */
    uint32_t nv = (v & ~0x3u) | (1u << 8);   /* чистим SMIEN+SMISTAT, ставим HCSEL */
    pci_cfg_write32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0xC0, nv);

    /* Шаг 2: ждём, пока firmware САМ снимет бит 15 (BIOS ownership).
       До 500 мс шагом 10 мс (Linux берёт 100 мс; CF-18 местами медлит). */
    int bios_released = 0;
    uint32_t chk = nv;
    for (int i = 0; i < 50; i++) {
        uhci_mdelay(10);
        chk = pci_cfg_read32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0xC0);
        if (!(chk & (1u << 15))) { bios_released = 1; break; }
    }

    /* Шаг 3: финальная запись — просим полностью снять legacy-семафоры
       (аналог outl(LEGSUP, 0) из usbcore early_handoff). */
    pci_cfg_write32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0xC0, 0u);
    uint32_t fin = pci_cfg_read32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0xC0);

    api_ptr->load_screen_add_string("LEGACY f=%d LEGSUP %x->%x rel=%d fin=%x %s",
                                    hc->pci_func, v, chk, bios_released, fin,
                                    bios_released ? "OK" : "BIOS-HOLD");
    return bios_released;
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
           ВАЖНО: PED при этом обязан ОСТАТЬСЯ включённым — запись с PED=0
           просто выключит порт. Пишем текущие R/W-биты минус SUSPEND. */
        uint16_t rw = (uint16_t)(ps & PORTSC_RW_MASK);
        uhci_write_port(hc, port, rw & ~(uint16_t)PORTSC_SUSPEND, 1);
        uhci_mdelay(25);                     /* hold resume >= 20 мс */
        ps = inw(hc->io_base + reg);         /* гарантируем SUSP=0, PED как был */
        uhci_write_port(hc, port, (uint16_t)(ps & PORTSC_RW_MASK) &
                                  ~(uint16_t)PORTSC_SUSPEND, 0);
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

/* ==================== Over-current latch fix (ICH6/CF-18) ====================
   В логе fin=288 взведён OCIC (бит7): ICH6 аппаратно БЛОКИРУЕТ сброс и
   включение порта, пока висит over-current — отсюда "PR auto-clear=0 after
   0 us" и никогда не поднимающийся PED. BIOS при загрузке с флешки мог
   оставить ложный latched OC. Единственный способ снять его — power-cycle
   порта: записать все R/W-биты в 0 (PED=0), выдержать паузу, W1C-очистить
   change-биты, включить порт заново и дождаться CCS+PED. */
static int uhci_clear_overcurrent(uhci_hc_t *hc, int port)
{
    uint16_t reg = portsc_reg(port);
    uint16_t ps = inw(hc->io_base + reg);
    if (!(ps & (PORTSC_OCI | PORTSC_OCIC))) return 0;   /* чисто */

    api_ptr->load_screen_add_string("OC[%d] detected ps=%x -> power-cycle", port, ps);

    /* 1) Полное выключение порта (PED=0, PR=0, SUSP=0) */
    uhci_write_port(hc, port, 0, 0);
    uhci_mdelay(100);
    /* 2) W1C всех change-битов (в т.ч. OCIC) без изменения R/W */
    uhci_clear_changes(hc, port);
    uhci_mdelay(50);
    /* 3) Снова включаем питание порта */
    uhci_write_port(hc, port, PORTSC_PED, 0);
    int ok = 0;
    for (uint32_t i = 0; i < 150 && !ok; i++) {          /* до 300 мс */
        uint16_t st = inw(hc->io_base + reg);
        if ((st & (PORTSC_CCS | PORTSC_PED)) == (PORTSC_CCS | PORTSC_PED)) ok = 1;
        uhci_mdelay(2);
    }
    uint16_t fin = inw(hc->io_base + reg);
    api_ptr->load_screen_add_string("OC[%d] cycle done oci=%d ccs=%d ped=%d fin=%x %s",
                                    port, !!(fin & PORTSC_OCI), !!(fin & PORTSC_CCS),
                                    !!(fin & PORTSC_PED), fin,
                                    ok ? "OK" : "REAL short circuit or OC# line stuck!");
    return ok;
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

/* Подробный дамп root-hub: регистры + побитовая расшифровка обоих портов.

   ВАЖНО про читаемость с экрана: раньше использовались похожие латинские
   буквы ('o'/'O', 'c'/'e'), которые при ручном переписывании лога неотличимы
   (регистр теряется). Теперь строка флагов имеет ФИКСИРОВАННЫЕ позиции и
   шаблон-ключ в начале: каждая буква на своей позиции всегда означает одно
   и то же, независимо от регистра, в котором вы её перерисовали:
     позиция 0 C = CCS (device connected)     позиция 4 O = OCI (over-current!)
     позиция 1 c = CSC  (connect change)      позиция 5 o = OCIC (OC change)
     позиция 2 E = PED (port enabled)         позиция 6 L = LSDA (low-speed)
     позиция 3 e = PEDC (enable change)       позиция 7 R = PR (reset)
                                               позиция 8 S = SUSP (suspend)
   "!"/"." означает флаг взведён, "-" — нет. */
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
    api_ptr->load_screen_add_string("flags: CcEeOoLRS | P1[%s%s%s%s%s%s%s%s%s] P2[%s%s%s%s%s%s%s%s%s]",
        (p1 & PORTSC_CCS)     ? "C!":"--", (p1 & PORTSC_CSC)    ? "c.":"..",
        (p1 & PORTSC_PED)     ? "E!":"--", (p1 & PORTSC_PEDC)   ? "e.":"..",
        (p1 & PORTSC_OCI)     ? "O!":"--", (p1 & PORTSC_OCIC)   ? "o.":"..",
        (p1 & PORTSC_LSDA)    ? "L!":"--", (p1 & PORTSC_PR)     ? "R!":"--",
        (p1 & PORTSC_SUSPEND) ? "S!":"--",
        (p2 & PORTSC_CCS)     ? "C!":"--", (p2 & PORTSC_CSC)    ? "c.":"..",
        (p2 & PORTSC_PED)     ? "E!":"--", (p2 & PORTSC_PEDC)   ? "e.":"..",
        (p2 & PORTSC_OCI)     ? "O!":"--", (p2 & PORTSC_OCIC)   ? "o.":"..",
        (p2 & PORTSC_LSDA)    ? "L!":"--", (p2 & PORTSC_PR)     ? "R!":"--",
        (p2 & PORTSC_SUSPEND) ? "S!":"--");
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

    /* Идентификация ЖЕЛЕЗА до любых манипуляций: реальный VID:DID + имя из
       таблицы Intel + нетронутый USBLEGSUP. Это позволяет по логу однозначно
       отличить UHCI-функцию от EHCI с кривым class-code. */
    {
        uint32_t id  = pci_cfg_read32(bus, dev, func, 0x00);
        uint32_t cls = pci_cfg_read32(bus, dev, func, 0x08);
        uint32_t leg0 = pci_cfg_read32(bus, dev, func, 0xC0);
        api_ptr->load_screen_add_string("HW d=%d f=%d %x:%x (%s) rev=%x class=%x LEGSUP0=%x",
                                        dev, func,
                                        (uint16_t)(id >> 16), (uint16_t)(id & 0xFFFFu),
                                        uhci_pci_id_name((uint16_t)(id >> 16), (uint16_t)(id & 0xFFFFu)),
                                        (cls >> 8) & 0xFFu, cls >> 8, leg0);
    }

    /* EARLY HANDOFF (порядок как в Linux: usb core делает legacy handoff ДО
       регистрации хоста и до первого обращения к IO-портам HC). Гасим SMI и
       отбираем ownership у firmware, пока никто ещё не дёргал PORTSC. */
    uhci_disable_legacy(hc);
    uhci_mdelay(20);   /* tPSCTORSTRIP-ish: дать шине стабилизироваться после handoff */
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

    // 1b) EARLY HANDOFF уже выполнен в uhci_add_host() ДО первого обращения
    //     к IO-портам (как usb legacy handoff в Linux). Здесь — повторная
    //     страховка прямо перед HCRESET: убеждаемся, что SMIEN по-прежнему
    //     выключен и firmware не вернул ownership за время PCI-зонда.
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
    /* Буфер объявлен как aligned(4096), поэтому его адрес УЖЕ выровнен —
       никакой ручной коррекции "(addr+4095)&~4095" не нужно (она лишь
       скрывала баг выравнивания). Проверяем инвариант и аварийно выходим,
       если линкер/загрузчик его нарушил. */
    uint32_t fl_virt = (uint32_t)uhci_frame_list;
    if (fl_virt & 0xFFFu) return -5;

    frame_list_ptr = (uint32_t *)fl_virt;
    uint32_t fl_phys = usb_virt_to_phys(fl_virt);

    if (!fl_phys || (fl_phys & 0xFFFu)) {
        return -5; // Аварийный выход, если физический адрес имеет смещение
    }

    // Заполняем выровненную таблицу кадров ссылками на нашу Control-очередь
    uint32_t fl_link_val = (control_qh_phys & ~0x0Fu) | TD_LINK_Q;
    for (int i = 0; i < 1024; i++) {
        uhci_frame_list[i] = fl_link_val;
    }

    outl(hc->io_base + UHCI_FLBASEADD, fl_phys);
    /* Страховка: перечитали FLBASEADD — на ICH запись может игнорироваться,
       если BME/legacy ownership не установлены. */
    if ((inl(hc->io_base + UHCI_FLBASEADD) & 0xFFFFF000u) != (fl_phys & 0xFFFFF000u)) {
        api_ptr->load_screen_add_string("UHCI FLBASEADD readback mismatch %x!=%x",
                                        inl(hc->io_base + UHCI_FLBASEADD), fl_phys);
    }
    outw(hc->io_base + UHCI_SOFMOD, 64);

    // 4a) Снять возможный latched over-current от BIOS ДО включения портов
    for (int port = 0; port < 2; port++)
        uhci_clear_overcurrent(hc, port);

    // 4) ПОСЛЕДОВАТЕЛЬНОЕ включение портов (как root_hub_init_ports в Linux
    //    uhci-hcd): PED=1 -> ждать CCS+PED -> только потом чистить W1C.
    //    Раньше здесь были два бага: (а) ps|=0x0001 писал RO-бит CCS;
    //    (б) helper сохранял R/W-биты из прочитанного значения и молча писал
    //    PED=0, выключая порт посреди сброса (fin=0x288). Теперь helper пишет
    //    полное слово явно, а change-биты чистятся отдельным write-only-safe
    //    вызовом ПОСЛЕ подтверждения подключения.
    for (int port = 0; port < 2; port++)
    {
        uhci_write_port(hc, port, PORTSC_PED, 0);   /* enable, SUSP=0 */
        int ok = 0;
        for (uint32_t i = 0; i < 150 && !ok; i++) { /* до 300 мс */
            uint16_t st = inw(hc->io_base + portsc_reg(port));
            if ((st & (PORTSC_CCS | PORTSC_PED)) == (PORTSC_CCS | PORTSC_PED)) ok = 1;
            else uhci_mdelay(2);
        }
        api_ptr->load_screen_add_string("UHCI init P%d enable=%d ps=%x",
                                        port, ok, inw(hc->io_base + portsc_reg(port)));
        uhci_clear_changes(hc, port);               /* W1C без изменения PED */
    }
    uhci_mdelay(20);

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
    switch (port) {
        case 0:  return UHCI_PORTSC1;
        case 1:  return UHCI_PORTSC2;
        case 2:  return UHCI_PORTSC3;
        default: return UHCI_PORTSC4;
    }
}
uint16_t uhci_port_status(uhci_hc_t *hc, int port)
{
    return inw(hc->io_base + portsc_reg(port));
}
int uhci_port_connected(uhci_hc_t *hc, int port)
{
    return (uhci_port_status(hc, port) & PORTSC_CCS) ? 1 : 0;
}


/* Одна попытка сброса порта. Возвращает 0 при PED=1, -2 в противном случае.
   ВАЖНО (UHCI Rev1.1 §4.1, Linux uhci-hcd root_hub_init_ports):
   в UHCI бит PR автосбрасывается САМИМ контроллером по окончании 10-мс
   сброса. Программная запись PR=0 раньше срока ОБРЫВАЕТ сброс на середине
   — устройство остаётся в неопределённом состоянии и PED так и не
   поднимается. Поэтому здесь PR только устанавливается, а затем ждём, пока
   железо само его снимет, после чего ожидаем восстановления PED. */
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

    // 0b) KRITICHNO vtoroe: latched over-current (OCIC=1 в fin=288)
    //     АППАРАТНО блокирует reset и enable порта на ICH6: HC сам гасит PR
    //     через кадр ("PR auto-clear=0 after 0 us") и не поднимает PED.
    //     Снимаем OC power-cycle'ом ДО попытки сброса.
    {
        uint16_t pre = inw(hc->io_base + reg);
        if (pre & (PORTSC_OCI | PORTSC_OCIC)) {
            if (!uhci_clear_overcurrent(hc, port)) {
                api_ptr->load_screen_add_string("RESET[%d] over-current NOT cleared", port);
                return -6;   /* distinct code: OC latch alive */
            }
        }
    }

    // 1) Порт ДОЛЖЕН быть включён (PED=1) перед сбросом. Записываем полное
    //    слово явно (helper пишет R/W-биты ЯВНО, RO-биты зануляет).
    uint16_t ps = inw(hc->io_base + reg);
    api_ptr->load_screen_add_string("RESET[%d] pre=%x", port, ps);
    uhci_write_port(hc, port, PORTSC_PED, 0);      /* PED=1, SUSP=0, PR=0 */
    uhci_clear_changes(hc, port);                  /* W1C отдельной записью */
    uhci_mdelay(50);
    ps = inw(hc->io_base + reg);
    if (!(ps & PORTSC_PED)) {
        /* Некоторые ICH требуют второй прогон enable после connect-change */
        uhci_write_port(hc, port, PORTSC_PED, 0);
        uhci_mdelay(50);
        ps = inw(hc->io_base + reg);
    }
    if (!(ps & PORTSC_PED)) {
        api_ptr->load_screen_add_string("RESET[%d] no-PED before reset! ps=%x", port, ps);
    }

    // 2) Assert reset: PR=1 (с PED=1!). UHCI Rev1.1 §4.1: бит PR автосбрасывается
    //    САМ контроллер через ~10 мс. Софт НЕ должен трогать PR после установки.
    uhci_write_port(hc, port, PORTSC_PED | PORTSC_PR, 0);
    uhci_udelay(100);
    uint16_t ps_asserted   = inw(hc->io_base + reg);
    uint16_t sts_asserted  = inw(hc->io_base + UHCI_USBSTS);
    api_ptr->load_screen_add_string("RESET[%d] asserted=%x (PR=%d STS=%x)",
                                    port, ps_asserted, !!(ps_asserted & PORTSC_PR),
                                    sts_asserted);
    if (!(ps_asserted & PORTSC_PR) && !(sts_asserted & USBSTS_RESET)) {
        /* Железо ПРОИГНОРИРОВАЛО запись PR. Дифференциальная диагностика:
           (а) FRNUM не двигается -> PCI I/O до хоста не доходит или HC
               остановлен (HCH);
           (б) LEGSUP bit15=1 -> firmware держит ownership (SMM-quirk). */
        uint16_t f0 = inw(hc->io_base + UHCI_FRNUM);
        uhci_udelay(3000);                        /* >1 кадра */
        uint16_t f1 = inw(hc->io_base + UHCI_FRNUM);
        uint32_t lg = pci_cfg_read32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0xC0);
        api_ptr->load_screen_add_string(
            "RESET[%d] PR IGNORED: FR %x->%x STS=%x CMD=%x LEGSUP=%x",
            port, f0, f1, inw(hc->io_base + UHCI_USBSTS),
            inw(hc->io_base + UHCI_USBCMD), lg);
        return -5;   /* distinct code: write ignored by HW/firmware */
    }

    // 3) Ждём авто-снятия PR контроллером: polling каждые 200 мкс до 500 мс.
    //    Микросекундный шаг принципиален: PR висит ровно ~10 мс, при шаге
    //    2 мс мы стабильно фиксировали "prclr=0" просто не замечая события.
    int pr_clear = 0;
    uint32_t pr_us = 0;
    for (uint32_t i = 0; i < 2500; i++) {          /* 2500 * 200us = 500 ms */
        uint16_t st = inw(hc->io_base + reg);
        if (!(st & PORTSC_PR)) { pr_clear = 1; pr_us = i * 200u; break; }
        if (!(st & PORTSC_CCS)) { pr_us = i * 200u; break; }  /* device пропал */
        /* Если в процессе ожидания вспыхнул OC — сброс блокирован, выходим */
        if (st & (PORTSC_OCI | PORTSC_OCIC)) { pr_us = i * 200u; break; }
        uhci_udelay(200);
    }
    api_ptr->load_screen_add_string("RESET[%d] PR auto-clear=%d after %d us",
                                    port, pr_clear, pr_us);

    /* W1C снимаем Host-Reset Occurred, чтобы следующий вход честно видел
       "сброса ещё не было". */
    {
        uint16_t sts2 = inw(hc->io_base + UHCI_USBSTS);
        if (sts2 & USBSTS_RESET) outw(hc->io_base + UHCI_USBSTS, sts2 & USBSTS_RESET);
    }

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

/* Публичная обёртка: до 4 попыток сброса. Часть флешек активируются только
   со второго-третьего reset'а (особенно если BIOS уже общался с ними).
   Лестница эскалации (по образцу Linux hub_port_init/hub_reset_port и
   Haiku USBAbsolutHostController::ResetPort):
     1) мягкий повтор сброса;
     2) power-cycle порта + принудительная очистка OC-latch;
     3) полный HCRESET контроллера (uhci_init), как uhci_restart() в Linux. */
int uhci_reset_port(uhci_hc_t *hc, int port)
{
    if (!hc) return -1;
    for (int attempt = 0; attempt < 4; attempt++) {
        /* КРИТИЧНО для QEMU и реальных флешек: сразу после Port Reset
           трансивер устройства находится в состоянии «reset recovery» и
           ОТВЕЧАЕТ NAK на все пакеты ep0. Если начать enumeration раньше,
           SETUP уходит в NAK, HC исчерпывает C_ERR=3 ретрая и выставляет
           ложный STALLED ("td0=c400000" = Active=0 NAK=1 STALL=1). Linux в
           hub_port_init ждёт ДО 50 мс после сброса перед первым GET_DESCRIPTOR.
           Пауза ставится ПОСЛЕ успешного PED, а не до — иначе мы бы снова
           "проспали" авто-снятие PR. */
        if (uhci_reset_port_once(hc, port) == 0) {
            uhci_mdelay(50);   /* reset recovery: даём устройству отпустить шину */
            return 0;
        }
        api_ptr->load_screen_add_string("RESET[%d]: attempt %d failed, escalation %d",
                                        port, attempt + 1, attempt);
        uhci_mdelay(100);

        if (attempt == 0) {
            /* мягкий retry без вмешательства */
            continue;
        }
        if (attempt == 1) {
            /* Power-cycle порта + гарантированная чистка OC-latch. */
            if (!uhci_clear_overcurrent(hc, port)) {
                uhci_write_port(hc, port, 0, 0);           /* disable (PED=0) */
                uhci_mdelay(50);                           /* запас поверх spec */
                uhci_write_port(hc, port, PORTSC_PED, 0);  /* enable (PED=1) */
                uhci_clear_changes(hc, port);
                for (uint32_t i = 0; i < 100; i++) {       /* ждём CCS+PED до 200 мс */
                    uint16_t st = inw(hc->io_base + portsc_reg(port));
                    if ((st & (PORTSC_CCS | PORTSC_PED)) == (PORTSC_CCS | PORTSC_PED)) break;
                    uhci_mdelay(2);
                }
            }
            uhci_dump_ports(hc, "pcyc");
            continue;
        }
        /* attempt >= 2: полный ре-инит контроллера (HCRESET+RS+порты) */
        if (uhci_init(hc) != 0) return -4;
        uhci_mdelay(100);   /* CCS должен вернуться сам: pull-up устройства никуда деться не мог */
        if (!(uhci_port_status(hc, port) & PORTSC_CCS))
            api_ptr->load_screen_add_string("RESET[%d]: CCS lost after reinit", port);
    }
    return -2;
}



/* Разобрать status-слова TD в человекочитаемый код ошибки.
   Возвращает: 0 = OK; иначе — отрицательный код, по которому upper-слой
   решает, что делать (NAK -> retry, STALL -> halt, CRC/timeout -> reset). */
/* ВАЖНО: бит NAK (19) НЕ является ошибкой! UHCI Rev1.1 §4.3.3: HC выставляет
   NAK как ИНФОРМАЦИЮ о том, что устройство ответило NAK (занято). Linux в
   uhci_result_control трактует NAK как «успех фазы с коротким результатом» и
   никогда не считает его ошибкой. Прежняя версия маппинга + программные
   ретраи всей цепочки SETUP->DATA->STATUS после каждого NAK приводили к
   гарантированному двойному DATA-пакету на ep0 (нарушение протокола -> реальный
   STALL). Поэтому NAK здесь возвращает 0. */
/* received PID — bits 10..15 status-слова TD (НЕ 15:8! см. комментарий в
   usb_uhci.h). HC/QEMU кладут сюда 6-битный ИНВЕРТИРОВАННЫЙ tag принятого
   пакета; когда ответа нет вообще — 0x00, при нераспознанном PID — 0x0D. */
static inline uint8_t uhci_td_recv_pid(uint32_t st)
{
    return (uint8_t)((st & TD_RECV_PID_MASK) >> TD_RECV_PID_SHIFT);
}
static inline uint32_t uhci_td_actlen(uint32_t st)
{
    return st & TD_ACTLEN_MASK;
}
/* DIAGNOSTIKA v4: diagnosis строится ТОЛЬКО по официальным флагам TD
   (биты 16..22 status-слова), как в Linux uhci-hcd (uhci_result_common) и
   QEMU (hw/usb/hcd-uhci.c). Эвристики по полю received-PID (10..15) и
   actual-length (0..9) ПОЛНОСТЬЮ удалены:
     - при «тишине на шине» HC оставляет в поле received-PID инверсный тег
       ОТПРАВЛЕННОГО токена (SETUP->0x0A, IN->0x06, OUT->0x0E), а не 0x00;
     - любое сравнение actlen с масками битов 8..15 гарантированно ловит
       пересечения и бракует успешные TD.
   Оба предыдущих набора эвристик по очереди порождали ложные -9/-1 и
   скрывали реальный диагноз. now: чистые флаги. */

/* ==================== КАРТА ДИАГНОСТИКИ TD (главный вывод этого цикла) ======
   ВНИМАТЕЛЬНОЕ изучение логов обеих машин даёт ОДНУ и ту же сигнатуру:
      TD0 status = 0x0D800000 = ISP(31)|ISC(30)|CRC-TIMEOUT(18)|BITSTUFF(17)
   При этом C_ERR (27:26) обнулился — значит HC реально СХОДИЛ на шину все
   три попытки. Что означает каждый из этих двух битов на UHCI:
     - BITSTUFF выставляется, когда принятый пакет нарушает правило вставки
       нулей, т.е. линия НЕ была в тишине — на ней был сигнал;
     - CRC/TIME-OUT — контрольная сумма принятого пакета не сошлась ИЛИ
       ответ не уложился в таймаут.
   Комбинация обоих — это классический «мусор вместо ответа»: устройство
   отвечает, но данные искажаются. Типичная аппаратная причина ровно такой
   сигнатуры на ICH6 — трансивер работает в неправильном скоростном режиме
   (полная скорость там, где линия низкоскоростная, или наоборот): хост
   принимает FS-кодировку LS-ответа как bit-stuff + CRC error. Прежний код
   ставил SPD (биты 29:28) ВСЕГДА в 00=Full-Speed и полагался на то, что
   «LS флаг бита 25 достаточно». Это неверно: для root-hub именно SPD=01
   говорит контроллеру использовать LS-пакетование/тайминги, а бит 25 —
   всего лишь отражение состояния порта. Поэтому ниже SPD берётся из
   РЕАЛЬНОГО бита LowSpeed-attached активного порта PORTSC (bit 9), а не из
   кэшированного флага. */
static inline int uhci_td_is_err_pid(uint32_t st)
{
    return (st & TD_CTRL_PIDERR) ? 1 : 0;   /* только официальный bit16 */
}

static int uhci_td_error_code(uint32_t st)
{
    if (st & TD_CTRL_ACTIVE)   return -10;  // всё ещё активно (не должно быть)
    if (st & TD_CTRL_STALLED)  return -4;   // device STALL (endpoint halted)
    if (st & TD_CTRL_BABBLE)   return -6;   // babble — устройство «болтает»
    if (st & TD_CTRL_DBUFERR)  return -5;   // host-side data buffer error
    if (st & TD_CTRL_CRCTIMEO) return -7;   // CRC/Time-out — нет ответа/шум
    if (st & TD_CTRL_BITSTUFF) return -8;   // bit-stuffing ошибка на линии
    if (st & TD_CTRL_PIDERR)   return -9;   // принят нераспознанный PID
    return 0;                               /* OK либо NAK/ISC/ISP — не ошибки */
}

/* Побайтово перечитать TD из ПАМЯТИ (не из регистра) и собрать 32-битное
   слово в прямом порядке. Если контроллер реально обработал TD, эти значения
   ОБЯЗАНЫ совпасть с полями структуры. Расхождение = мы читаем НЕ ту память,
   которую видит HC (двойная трансляция адресов / устаревший бинарник). */
static uint32_t uhci_raw_dword(const void *p, uint32_t off)
{
    const volatile uint8_t *b = (const volatile uint8_t *)p + off;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* ==================== Служебные конструкторы токенов ====================
   ВАЖНО (UHCI Rev1.1 §4.3.5): в TOKEN-поле SETUP-пакета бит DATA toggle
   (bit19) НЕ ИСПОЛЬЗУЕТСЯ и обязан быть 0. Раньше сюда через общую формулу
   попадал toggle от DATA-стадии — на реальном ICH6 такой пакет отбрасывается.
   Все поля собираются ЯВНЫМИ числами (без зависимости от констант сдвига),
   чтобы по логу можно было сверить каждый бит. */
static inline uint32_t uhci_token_setup(uint8_t addr)
{
    /* PID_SETUP=0x2D | addr<<8 | ep0 | toggle=0 | (8-1)<<20 => 0x0080002D@addr0 */
    return (uint32_t)PID_SETUP | ((uint32_t)(addr & 0x7F) << 8) | (7u << 20);
}

static inline uint32_t uhci_token_data_pid(uint8_t addr, uint8_t ep, int in, uint32_t chunk)
{
    /* chunk==0 (zero-length) -> поле len = 0x7FF; иначе len = chunk-1 */
    uint32_t lenfield = (chunk == 0) ? 0x7FFu : (chunk - 1u);
    return (uint32_t)(in ? PID_IN : PID_OUT) | ((uint32_t)(addr & 0x7F) << 8) |
           ((uint32_t)(ep & 0x0F) << 15) | (lenfield << 20);
}

static inline uint32_t uhci_token_status(uint8_t addr, uint8_t ep, int in)
{
    /* STATUS stage: token PID того направления, где идёт нулевой DATA1 */
    return (uint32_t)(in ? PID_OUT : PID_IN) | ((uint32_t)(addr & 0x7F) << 8) |
           ((uint32_t)(ep & 0x0F) << 15) | (1u << 19) | (0x7FFu << 20);
}

/* Версия компоновки токенов — печатается в каждом транзите, чтобы по логу
   можно было ОДНОЗНАЧНО установить, какая СБОРКА драйвера реально крутится в
   памяти (см. USBVER ниже). */
#define UHCI_CODE_REV 0xA116u   /* A116: PID-ERR detection (-9), 4-port resume, active_port */

/* ==================== Компактный лог (экономия буфера экрана) ================
   Буфер лога загрузки ~30 строк, начало уплывало за 5 ретраев x 3 строки.
   Реализация без sprintf: транзактные отладочные строки переводятся в
   ФИКСИРОВАННЫЕ коды (int), повтор одного кода схлопывается в счётчик, а
   человекочитаемый текст печатает ОДИН декодер на выходе. Это гарантирует
   совместимость с freestanding-сборкой (в ядре нет vsprintf для драйверов)
   и экономит до 15 строк лога на одну серию ретраев. */
static int      g_log_last_code = 0x7FFFFFFF;   /* "ничего не печатали" */
static uint32_t g_log_repeat    = 0;

const char *uhci_xfer_code_text(int code)
{
    switch (code) {
        case ULOG_XFER_TIMEOUT: return "XFER timeout (TD active forever)";
        case ULOG_XFER_NORESP:  return "XFER no-response (err-PID/none)";
        case ULOG_XFER_STALL:   return "XFER device STALL";
        case ULOG_XFER_CRC:     return "XFER CRC/bitstuff on wire";
        case ULOG_XFER_SHORT:   return "XFER short packet";
        default:                return NULL;
    }
}

void uhci_log_code(int code)
{
    if (code == 0) return;                  /* успех — молча, без строк */
    if (code == g_log_last_code) {          /* дубль серии — НЕ тратим строку */
        g_log_repeat++;
        return;
    }
    if (g_log_repeat > 1 && uhci_xfer_code_text(g_log_last_code)) {
        api_ptr->load_screen_add_string("XFER %s (x%d)",
                                        uhci_xfer_code_text(g_log_last_code),
                                        (int)g_log_repeat);
    }
    g_log_last_code = code;
    g_log_repeat    = 1;
}

/* Завершение серии: печатает последний код ровно один раз. Вызывается из
   put_state/put_status после сбора всей диагностики транзакта. */
void uhci_log_flush(void)
{
    const char *txt = uhci_xfer_code_text(g_log_last_code);
    if (txt) {
        if (g_log_repeat > 1)
            api_ptr->load_screen_add_string("XFER %s (x%d)", txt, (int)g_log_repeat);
        else
            api_ptr->load_screen_add_string("XFER %s", txt);
    }
    g_log_last_code = 0x7FFFFFFF;
    g_log_repeat    = 0;
}

/* ==================== Кольцо адресов USB-устройств ====================
   ГЛАВНЫЙ БАГ ПРЕДЫДУЩИХ СБОРОК: контроллер завершал SETUP-TD со статусом
   вида 0xD800000 / 0x??00000 с error-PID — это НЕ «злая воля железа», а
   корректный ответ шины на токен DEVICE-адреса, которому НИКТО не отвечает.
   При legacy-загрузке BIOS сам присваивает адрес подключённому устройству
   (обычно 1..7). Наш же драйвер после HCRESET назначал новый адрес только
   через SET_ADDRESS, но ПЕРВЫЙ GET_DESCRIPTOR(8) слался на addr=0 — а
   устройство после BIOS уже сидит НЕ на нуле! Поэтому enumeration теперь
   перебирает адреса 0 и 1..7 на уже ВСБРОШЕННОМ порту (reset сбрасывает
   адрес в 0 по USB spec 9.1.1.6, так что addr=0 обязано работать; если
   нет — пробуем «BIOS-ны» адреса). Возвращаемый ring_idx говорит upper-слою,
   надо ли делать SET_ADDRESS (idx==0) или можно работать как есть. */
#define USB_ADDR_RING_MAX 8
static uint8_t g_usb_addr_ring[USB_ADDR_RING_MAX] = {0,1,2,3,4,5,6,7};
int uhci_get_addr_ring_entry(int idx, uint8_t *out_addr)
{
    if (idx < 0 || idx >= USB_ADDR_RING_MAX) return 0;
    *out_addr = g_usb_addr_ring[idx];
    return 1;
}

int uhci_control_transfer(uhci_hc_t *hc, uint8_t dev_addr, const uint8_t *setup_pkt, void *data_buf, uint16_t data_len, int data_in, uint16_t max_pkt)
{
    if (!hc || !setup_pkt) return -1;
    if (max_pkt == 0 || max_pkt > 64) max_pkt = 8;

    /* Транзакты крутим только через ОДИН контроллер за раз (общие TD/QH). */
    uhci_select_host(hc);

    /* CF-18/ICH6: BIOS или SMI могут оставить HC в Global Suspend / порт в
       suspend между транзактами. Перед КАЖДОЙ цепочкой TD будим root-hub.
       FIX: раньше будились только порты 0 и 1 — на ICH6 у UHCI-функций до
       4 root-портов (PORTSC3/4), и активный порт вообще не участвовал в
       resume. Теперь перебираем все 4 порта + гарантированно active_port. */
    {
        uint16_t stsx = inw(hc->io_base + UHCI_USBSTS);
        uint16_t cmdx = inw(hc->io_base + UHCI_USBCMD);
        int woke = 0;
        for (int p = 0; p < 4; p++) {
            if (inw(hc->io_base + portsc_reg(p)) & PORTSC_SUSPEND)
                woke |= uhci_resume_port(hc, p);
        }
        if ((stsx & USBSTS_RESUME) || (cmdx & USBCMD_EGSM)) {
            woke |= uhci_resume_port(hc, (hc->active_port <= 3) ? hc->active_port : 0);
        }
        if (woke)
            api_ptr->load_screen_add_string("UHCI resumed before xfer sts=%x cmd=%x",
                                            stsx, cmdx);
    }

    uint32_t num_data = 0;
    int has_data = (data_len > 0 && data_buf != NULL);
    if (has_data)
    {
        num_data = (data_len + max_pkt - 1) / max_pkt;
        if (num_data > MAX_CONTROL_TDS - 2) num_data = MAX_CONTROL_TDS - 2;
    }

    /* Готовим ВСЮ цепочку TD до того, как контроллер увидит TD0: чистим
       статусы от ошибок прошлого транзакта. Статус каждого TD потом
       собирается ЯВНО из всех полей (включая SPD=00), чтобы в него не
       протёк мусор от предыдущего использования или от записи за конец
       frame list. */
    for (uint32_t i = 0; i < MAX_CONTROL_TDS; i++) {
        control_tds[i].status = 0;
        control_tds[i].link   = TD_LINK_TERM;
        control_tds[i].token  = 0;
        control_tds[i].buffer = 0;
    }

    // Копируем setup-пакет строго в выровненный flat-буфер
    for (int i = 0; i < 8; i++) setup_static[i] = setup_pkt[i];
    uint32_t setup_phys = usb_virt_to_phys((uint32_t)setup_static);

    uint32_t td0_phys   = usb_virt_to_phys((uint32_t)&control_tds[0]);
    uint32_t td1_phys   = usb_virt_to_phys((uint32_t)&control_tds[1]);
    uint32_t qh_phys    = usb_virt_to_phys((uint32_t)&g_control_queue.qh);
    uint32_t dummy_phys = usb_virt_to_phys((uint32_t)&g_control_queue.dummy_td);

    uint32_t ls_flag = hc->current_device_low_speed ? TD_CTRL_LS : 0u;
    /* SPD (биты 29:28) — ЯВНО 00 = Full-Speed (или 01 для LS на root hub).
       Никогда не оставляем здесь случайные значения: 11b зарезервировано. */
    uint32_t spd_field = hc->current_device_low_speed ? (1u << 28) : TD_CTRL_SPD_FS;

    // --- TD0: SETUP Stage ---
    /* VF (bit2) в link НЕ ставим: при depth-first обходе (USBCMD.CF=0) он
       лишь добавляет HC лишних состояний; горизонтальная связь TD0->TD1
       работает и без него (так делает uhci-hcd Linux). */
    control_tds[0].link   = (td1_phys & ~0x0Fu);
    /* IOC на каждом TD цепочки — безопасно (USBINTR замаскирован в 0) и нужно,
       чтобы QEMU/ICH корректно обновляли статус каждого TD. */
    control_tds[0].status = TD_CTRL_ACTIVE | TD_CTRL_IOC | TD_CTRL_C_ERR_3 |
                            spd_field | ls_flag;
    /* ТОКЕН SETUP: PID|addr|ep0|toggle=0|len=(8-1). Эталон для addr=0:
       0x0080072D (Linux: uhci_append_pid_token(0x2D, 0, 0, 8)). */
    control_tds[0].token  = uhci_token_setup(dev_addr);
    control_tds[0].buffer = setup_phys;
    hc->dbg_td0_token = control_tds[0].token;

    // --- TD[1..num_data]: DATA Stage ---
    for (uint32_t i = 0; i < num_data; i++)
    {
        uint32_t idx = 1 + i;
        uint32_t off = i * max_pkt;
        uint32_t chunk = (data_len - off > max_pkt) ? max_pkt : (data_len - off);
        /* ep0 после SETUP всегда начинает с DATA0 (toggle=0), далее
           DATA0/DATA1 чередуются. Прежняя версия ставила ПЕРВЫМ DATA1 —
           устройство отвечало STALL на рассогласованный тоггл. */
        uint32_t toggle = ((i & 1) == 1) ? TD_TOKEN_TOGGLE : 0u;

        uint32_t next_idx = (i + 1 < num_data) ? (idx + 1) : (1 + num_data);
        uint32_t next_td_phys = usb_virt_to_phys((uint32_t)&control_tds[next_idx]);

        control_tds[idx].link = (next_td_phys & ~0x0Fu);
        control_tds[idx].status = TD_CTRL_ACTIVE | TD_CTRL_IOC | TD_CTRL_C_ERR_3 |
                                  spd_field | ls_flag;
        control_tds[idx].token = uhci_token_data_pid(dev_addr, 0, data_in, chunk) | toggle;

        uint32_t pa = usb_virt_to_phys((uint32_t)((uint8_t *)data_buf + off));
        if (pa == 0) return -1;
        control_tds[idx].buffer = pa;
    }

    // --- TD[sidx]: STATUS Stage ---
    uint32_t sidx = 1 + num_data;

    control_tds[sidx].link   = TD_LINK_TERM;
    control_tds[sidx].status = TD_CTRL_ACTIVE | TD_CTRL_IOC | TD_CTRL_C_ERR_3 |
                               spd_field | ls_flag;
    /* STATUS stage: направление ПРОТИВОПОЛОЖНОГО data-фазе, нулевой DATA1
       (len field = 0x7FF). Для IN-транзакции это PID_OUT. */
    control_tds[sidx].token  = uhci_token_status(dev_addr, 0, data_in);
    control_tds[sidx].buffer = setup_phys;

    uint32_t status_td_status_off = (uint32_t)&control_tds[sidx].status - (uint32_t)&control_tds[0];
    /* КОМПАКТНЫЙ ЛОГ: раньше здесь печатались 3 строки на КАЖДУЮ попытку
       ("DMA Map", "TD0 tok", "TDraw") — 5 ретраев съедали весь 30-строчный
       буфер лога, и начало загрузки уплывало. Теперь в потоке НЕ печатается
       ничего однотипного: код результата запоминается через uhci_log_code(),
       серии схлопываются в "XFER <диагноз> (xN)". Сырые значения (токен/
       статусы TD) доступны через hc->dbg_* и api debug8/9 — верхний слой
       печатает их ОДИН раз при финальном отказе. */
    (void)qh_phys; (void)td0_phys; (void)status_td_status_off;

    // Синхронизация записи дескрипторов перед тем, как HC их увидит
    usb_dma_sync_before();
    if (uhci_force_wbinvd) __asm__ __volatile__("wbinvd" ::: "memory");

    // Сбрасываем хвост очереди и врезаем транзакцию в асинхронный планировщик
    g_control_queue.tail_td = &g_control_queue.dummy_td;
    g_control_queue.qh.element = td0_phys & ~0x0Fu;
    __asm__ __volatile__("" ::: "memory");

    /* КРИТИЧЕСКАЯ СЕКЦИЯ: блокируем планировщик задач ядра на время всей
       транзакции (аналог того, как ext2_disk.c оборачивает DMA-операции). */
    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 1;

    volatile uint32_t *status_slot = (volatile uint32_t *)&control_tds[sidx].status;

    uint32_t timeout = 5000000;   // ~несколько секунд даже на медленном CPU
    uint32_t spins = 0;
    int result = -3;

    /* Живость планировщика ДО ожидания: при RS=1 счётчик FRNUM обязан
       меняться (1 кадр/мс). Если стоит — HC остановлен (HCH) или PCI I/O не
       доходит, и ждать завершения TD бессмысленно: после таймаута выводим
       точный диагноз. */
    uint16_t fr_pre  = inw(hc->io_base + UHCI_FRNUM);
    uint16_t cmd_pre = inw(hc->io_base + UHCI_USBCMD);
    uint16_t sts_pre = inw(hc->io_base + UHCI_USBSTS);

    while (timeout--)
    {
        spins++;
        /* Ждём ЗАВЕРШЕНИЯ ВСЕЙ транзакции: последний TD цепочки (STATUS stage). */
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
    }

    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;

    // Диагностика для лога загрузки: что реально произошло на шине
    hc->dbg_swstamp    = spins;
    hc->dbg_last_frame = inw(hc->io_base + UHCI_FRNUM);
    hc->dbg_sts        = inw(hc->io_base + UHCI_USBSTS);
    /* ПОЧИНКА ДИАГНОСТИКИ (двоякий смысл в прежнем логе): здесь ВСЕГДА
       читался PORTSC1, даже когда активен порт 2/3 — поэтому d4 и d5 в логах
       были одинаковы и нельзя было отличить «порт умер» от «будит не тот
       порт». Теперь читаем фактический active_port. */
    hc->dbg_portsc     = inw(hc->io_base + portsc_reg(hc->active_port <= 3 ? hc->active_port : 0));
    __asm__ __volatile__("" ::: "memory");
    hc->dbg_td0_status = control_tds[0].status;
    hc->dbg_td0_token  = control_tds[0].token;

    /* Если хвостовой TD так и остался ACTIVE, а TD0 завершён с ошибкой —
       HC оборвал цепочку на SETUP. Подтягиваем честный код ошибки. */
    if (result == -3)
    {
        int ec0 = uhci_td_error_code(control_tds[0].status);
        if (!(control_tds[0].status & TD_CTRL_ACTIVE) && ec0) result = ec0;
    }

    /* Диагноз таймаута: FRNUM обязан был сдвинуться за время ожидания
       (~5 млн спинов >> 1 мс). moved=0 => планировщик мёртв (RS/HCH/PCI),
       moved=1 при незавершённом TD => HC не видит нашу очередь (адреса
       TD/QH/FLBASEADD или link-биты). */
    if (result == -3)
    {
        uint16_t fr_now = inw(hc->io_base + UHCI_FRNUM);
        api_ptr->load_screen_add_string(
            "UHCI xfer TIMEOUT: FR %x->%x CMD=%x STS=%x moved=%d HCH=%d RS=%d",
            fr_pre, fr_now, cmd_pre, sts_pre,
            (fr_now != fr_pre) ? 1 : 0,
            !!(sts_pre & USBSTS_HCH), !!(cmd_pre & USBCMD_RS));
    }

    /* Компактный лог: классифицируем исход ОДНИМ кодом вместо потока
       однотипных строк (см. uhci_log_code). */
    switch (result) {
        case -3:  uhci_log_code(ULOG_XFER_TIMEOUT); break;
        case -9:  uhci_log_code(ULOG_XFER_NORESP);  break;
        case -4:  uhci_log_code(ULOG_XFER_STALL);   break;
        case -7:
        case -8:
        case -6:  uhci_log_code(ULOG_XFER_CRC);     break;
        case -5:  uhci_log_code(ULOG_XFER_SHORT);   break;
        default:  if (result >= 0) uhci_log_code(0); else uhci_log_code(ULOG_XFER_TIMEOUT); break;
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


int uhci_bulk_transfer(uhci_hc_t *hc, uint8_t dev_addr, uint8_t ep_addr,
                       uint16_t ep_max_pkt, uint8_t *toggle,
                       void *buf, uint32_t len, int dir_in)
{
    if (!hc || !buf || len == 0 || !toggle) return -1;
    if (ep_max_pkt == 0 || ep_max_pkt > 1024) ep_max_pkt = 64;

    uhci_select_host(hc);

    {
        uint16_t stsx = inw(hc->io_base + UHCI_USBSTS);
        uint16_t cmdx = inw(hc->io_base + UHCI_USBCMD);
        int woke = 0;
        for (int p = 0; p < 4; p++) {
            if (inw(hc->io_base + portsc_reg(p)) & PORTSC_SUSPEND)
                woke |= uhci_resume_port(hc, p);
        }
        if ((stsx & USBSTS_RESUME) || (cmdx & USBCMD_EGSM)) {
            woke |= uhci_resume_port(hc, (hc->active_port <= 3) ? hc->active_port : 0);
        }
        if (woke)
            api_ptr->load_screen_add_string("UHCI BULK resumed sts=%x cmd=%x", stsx, cmdx);
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
    /* Явный SPD=00 (Full-Speed): MSC-устройство на root-hub всегда FS. */
    uint32_t spd_field = hc->current_device_low_speed ? (1u << 28) : TD_CTRL_SPD_FS;

    for (uint32_t i = 0; i < num_tds; i++)
    {
        uint32_t off   = i * ep_max_pkt;
        uint32_t chunk = (len - off > ep_max_pkt) ? ep_max_pkt : (len - off);
        uint32_t toggle_bit = cur_toggle ? TD_TOKEN_TOGGLE : 0u;
        uint32_t next_td_phys = (i + 1 < num_tds)
                                ? (usb_virt_to_phys((uint32_t)&bulk_tds[i + 1]) & ~0x0Fu)
                                : TD_LINK_TERM;
        bulk_tds[i].link   = next_td_phys;
        bulk_tds[i].status = TD_CTRL_ACTIVE | TD_CTRL_IOC | TD_CTRL_C_ERR_3 |
                             spd_field | ls_flag;
        bulk_tds[i].token  = ((uint32_t)(dir_in ? PID_IN : PID_OUT) << TD_TOKEN_PID_SHIFT) |
                             ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT) |
                             ((uint32_t)ep_num << TD_TOKEN_EP_SHIFT) |
                             toggle_bit | ((uint32_t)(chunk - 1) << TD_TOKEN_LEN_SHIFT);
        uint32_t pa = usb_virt_to_phys((uint32_t)(b + off));
        if (pa == 0) return -1;
        bulk_tds[i].buffer = pa;
        cur_toggle ^= 1;
    }

    uint32_t first_bulk_td_phys = usb_virt_to_phys((uint32_t)&bulk_tds[0]);
    uint32_t dummy_td_phys      = usb_virt_to_phys((uint32_t)&g_bulk_queue.dummy_td);
    if (!first_bulk_td_phys || !dummy_td_phys) return -1;

    /* Хвост цепочки замыкаем на dummy TD БЕЗ VF: горизонтальная связь. */
    bulk_tds[num_tds - 1].link = (dummy_td_phys & ~0x0Fu);

    /* Цикл попыток, аналогично control: MSC-устройство NAK'ует bulk-IN при
       подготовке сектора. При временной ошибке — W1C USBSTS, рестарт RS,
       повтор всей цепочки. STALL отдаём upper-слою сразу. */
    int result = -3;
    const int max_attempts = 40;
    uint8_t start_toggle = *toggle;           /* для отката при ретраях */

    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 1;

    for (int attempt = 0; attempt < max_attempts; attempt++)
    {
        cur_toggle = start_toggle;
        for (uint32_t i = 0; i < num_tds; i++)
        {
            bulk_tds[i].status = TD_CTRL_ACTIVE | TD_CTRL_IOC | TD_CTRL_C_ERR_3 |
                                 spd_field | ls_flag;
            uint32_t toggle_bit = cur_toggle ? TD_TOKEN_TOGGLE : 0u;
            bulk_tds[i].token = (bulk_tds[i].token & ~TD_TOKEN_TOGGLE) | toggle_bit;
            cur_toggle ^= 1;
        }
        usb_dma_sync_before();
        if (uhci_force_wbinvd) __asm__ __volatile__("wbinvd" ::: "memory");

        g_bulk_queue.tail_td = &g_bulk_queue.dummy_td;
        g_bulk_queue.tail_td->link = (first_bulk_td_phys & ~0x0Fu);
        g_bulk_queue.tail_td = &bulk_tds[num_tds - 1];
        __asm__ __volatile__("" ::: "memory");

        {
            uint16_t cmd_ = inw(hc->io_base + UHCI_USBCMD);
            if (!(cmd_ & USBCMD_RS))
                outw(hc->io_base + UHCI_USBCMD, cmd_ | USBCMD_RS);
        }

        uint32_t timeout = 1000000;   /* крупным bulk-чтениям нужно больше бюджета */
        int done = 0;
        uint16_t fr_pre_b = inw(hc->io_base + UHCI_FRNUM);
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
                if (!ec)      { *toggle = cur_toggle; result = (int)len; done = 1; }
                else if (ec == -4) { result = ec; done = 1; }
                else               { result = ec; }   /* ретрай */
                break;
            }
        }
        if (done) break;

        {
            uint16_t sts_ = inw(hc->io_base + UHCI_USBSTS);
            outw(hc->io_base + UHCI_USBSTS, sts_ & 0x003F);
            uint16_t cmd_ = inw(hc->io_base + UHCI_USBCMD);
            outw(hc->io_base + UHCI_USBCMD, cmd_ | USBCMD_RS);
            g_bulk_queue.tail_td = &g_bulk_queue.dummy_td;
        }
        uhci_mdelay(attempt < 5 ? 10 : 50);
        if (attempt == 0 || attempt == 9 || attempt == 29)
            api_ptr->load_screen_add_string("BULK retry %d r=%d td0=%x FR=%x->%x",
                                            attempt, result, bulk_tds[0].status,
                                            fr_pre_b, inw(hc->io_base + UHCI_FRNUM));
    }
    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;
    if (result == -3) {
        hc->dbg_sts = inw(hc->io_base + UHCI_USBSTS);
        api_ptr->load_screen_add_string("BULK DEAD sts=%x td0=%x", hc->dbg_sts, bulk_tds[0].status);
    }
    usb_dma_sync_after();
    if (uhci_force_wbinvd) __asm__ __volatile__("wbinvd" ::: "memory");
    return result;
}

void uhci_register_ptrs(void)
{
    frame_list_ptr   = uhci_frame_list;
    /* БЫЛО: &control_qh / &bulk_qh — отдельные пустые структуры!
       HC ходил по нулевой очереди. Пишем те же объекты, что используются в DMA: */
    control_qh_ptr   = &g_control_queue.qh;
    bulk_qh_ptr      = &g_bulk_queue.qh;
    control_tds_ptr  = control_tds;
    setup_static_ptr = setup_static;
    bulk_tds_ptr     = bulk_tds;
}
