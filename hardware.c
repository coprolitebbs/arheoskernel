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

#include "include-kernel/hardware.h"
#include "include-kernel/ports_io.h"
#include "include-kernel/comdebug.h"
#include "include-kernel/pmm.h"
#include "include-kernel/lib.h"

//Глобальные переменные, заполняются один раз
static uint32_t *SMI_CMD;
static uint8_t   ACPI_ENABLE;
static uint32_t *PM1a_CNT;
static uint32_t *PM1b_CNT;
static uint16_t  SLP_TYPa;
static uint16_t  SLP_TYPb;
static uint16_t  SLP_EN  = 1 << 13;
static uint16_t  SCI_EN  = 1;
static int acpi_ready = 0;  //1, если всё найдено и можно выключаться


//Переменные PCI-подсистемы ядра

static pci_dev_record_t g_pci_devs[PCI_REGISTRY_MAX];
static int  g_pci_devs_count = 0;
static int  g_pci_scanned    = 0;

//Битовая карта уже просканированных шин — защита от петель мостов
static uint8_t g_bus_scanned[32];



//Внутренние функции


//Проверка сигнатуры и контрольной суммы RSDP
static unsigned int *acpiCheckRSDPtr(unsigned int *ptr) {
    char *sig = "RSD PTR ";
    struct RSDPtr *rsdp = (struct RSDPtr *)ptr;
    uint8_t *bptr;
    uint8_t check = 0;
    int i;

    if (memcmp(sig, rsdp, 8) == 0) {
        // Проверка контрольной суммы первых 20 байт
        bptr = (uint8_t *)ptr;
        for (i = 0; i < (int)sizeof(struct RSDPtr); i++) {
            check += *bptr;
            bptr++;
        }
        if (check == 0) {
            return (unsigned int *)rsdp->RsdtAddress;
        }
    }
    return 0;
}

//Поиск RSDP: сначала в области EBDA, потом в 0xE0000–0xFFFFF
static unsigned int *acpiGetRSDPtr(void) {
    unsigned int *addr;
    unsigned int *rsdp;

    // 1) Область EBDA (сегмент лежит по адресу 0x40E)
    uint16_t ebda_seg = *((uint16_t *)0x40E);
    uint32_t ebda = (uint32_t)ebda_seg * 0x10;
    if (ebda < 0x400 || ebda > 0xA0000) ebda = 0x9FC00; //fallback

    for (addr = (unsigned int *)ebda;
         (uint32_t)addr < ebda + 1024;
         addr += 0x10 / sizeof(addr)) {
        rsdp = acpiCheckRSDPtr(addr);
        if (rsdp) return rsdp;
    }

    //2) Область BIOS ROM 0xE0000–0xFFFFF
    for (addr = (unsigned int *)0x000E0000;
         (uint32_t)addr < 0x00100000;
         addr += 0x10 / sizeof(addr)) {
        rsdp = acpiCheckRSDPtr(addr);
        if (rsdp) return rsdp;
    }

    return 0;
}

//Проверка заголовка таблицы ACPI (сигнатура + контрольная сумма)
static int acpiCheckHeader(unsigned int *ptr, char *sig) {
    if (memcmp(ptr, sig, 4) == 0) {
        char *checkPtr = (char *)ptr;
        int len = *(ptr + 1);  //Length
        char check = 0;
        while (0 < len--) {
            check += *checkPtr;
            checkPtr++;
        }
        if (check == 0) return 0;
    }
    return -1;
}

//Включение ACPI (если BIOS этого не сделал)
static int acpiEnable(void) {
    //Проверяем SCI_EN (бит 0 в PM1a_CNT)
    if ((inw((uint32_t)(uintptr_t)PM1a_CNT) & SCI_EN) == 0) {
        if (SMI_CMD != 0 && ACPI_ENABLE != 0) {
            outb((uint32_t)SMI_CMD, ACPI_ENABLE);

            //Ждём до 3 секунд, пока SCI_EN не станет 1
            for (int i = 0; i < 300; i++) {
                if ((inw((uint32_t)(uintptr_t)PM1a_CNT) & SCI_EN) == 1) break;
                for (volatile int j = 0; j < 100000; j++);
            }
            if ((inw((uint32_t)(uintptr_t)PM1a_CNT) & SCI_EN) == 1) return 0;
        }
        return -1;
    }
    return 0;
}


//Внешние функции

//Бип
void beep(uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        //Включаем динамик (биты 0 и 1 в порту 0x61)
        outb(0x61, inb(0x61) | 3);
        // Задержка (длина звука)
        for (volatile int d = 0; d < 10000000; d++) asm volatile("nop");
        //Выключаем динамик
        outb(0x61, inb(0x61) & ~3);
        //Задержка между писками
        for (volatile int d = 0; d < 10000000; d++) asm volatile("nop");
    }
}


uint32_t enumerate_pci_ven_dev_ids(void)
{
    uint32_t res = 0;
    int cnt = 0;

    for (uint8_t slot = 0; slot < 32; slot++)
    {
        //uint32_t id = pci_read_config_dword(0, slot, 0, 0x00);
        uint32_t id = pci_cfg_read_dword(0, slot, 0, 0x00);
        uint16_t vendor = id & 0xFFFF;
        uint16_t device = id >> 16;

        if(vendor != 0x0000FFFF && device != 0x0000FFFF)
        {
            com_puts("[KERNEL] Found PCI device: Num: ");
            com_puthex(slot);
            com_puts(" vendorid: ");
            com_puthex(vendor);
            com_puts(" deviceid: ");
            com_puthex(device);
            com_puts("\n");

            cnt++;
        }

        // Vendor ID S3 = 0x5333, Device ID Trio64 = 0x8811
        if (res == 0 && vendor == 0x5333 && device == 0x8811)
        {
            //uint32_t bar0 = pci_read_config_dword(0, slot, 0, 0x10);
            uint32_t bar0 = pci_cfg_read_dword(0, slot, 0, 0x10);
            res =  bar0 & 0xFFFFFFF0; // Отсекаем флаги PCI, получаем физ. адрес
        }
    }
    return res;
}

//Это для эмулятора 86box. Поскольку в нем не предусмотрена видеокарта как на плате
//с целевым железом (плата TA-NC529V3 со встроенным видео), пришлось добавлять в настройках
//карту [PCI] S3 Trio64 и делать отдельно поиск ее адреса по PCI и определение адреса
//фреймбуффера
void find_and_prepare_s3_trio64(void)
{
    uint32_t s3a = enumerate_pci_ven_dev_ids();
    if(s3a == 0) return;
    //Разблокировка регистров S3 TRIO64
    //Без этого чип игнорирует любые попытки изменить конфигурацию LFB/RAMDAC
    __asm__ volatile("movw $0x3D4, %%dx; movb $0x38, %%al; outb %%al, %%dx; movb $0x48, %%al; movw $0x3D5, %%dx; outb %%al, %%dx" ::: "eax", "edx");
    __asm__ volatile("movw $0x3D4, %%dx; movb $0x39, %%al; outb %%al, %%dx; movb $0xA5, %%al; movw $0x3D5, %%dx; outb %%al, %%dx" ::: "eax", "edx");
    __asm__ volatile("movw $0x3D4, %%dx; movb $0x40, %%al; outb %%al, %%dx; movb $0x01, %%al; movw $0x3D5, %%dx; outb %%al, %%dx" ::: "eax", "edx");

    //Снимаем блокировку с верхних регистров карты (CR50+)
    __asm__ volatile("movw $0x3D4, %%dx; movb $0x33, %%al; outb %%al, %%dx; movb $0x00, %%al; movw $0x3D5, %%dx; outb %%al, %%dx" ::: "eax", "edx");

    //Настройка регистра CR50 (Включение LFB + Окно 4МБ)
    //Биты 2-3 = 11 (Размер линейного окна 4 МБ для покрытия всего разрешения)
    //Бит 0 = 1 (Включить Linear Frame Buffer)
    __asm__ volatile("movw $0x3D4, %%dx; movb $0x50, %%al; outb %%al, %%dx" ::: "eax", "edx");
    uint8_t cr50;
    __asm__ volatile("movw $0x3D5, %%dx; inb %%dx, %%al" : "=a"(cr50) : : "edx");
    cr50 |= 0x0D; // Принудительно выставляем окно и включаем LFB
    __asm__ volatile("movw $0x3D4, %%dx; movb $0x50, %%al; outb %%al, %%dx" ::: "eax", "edx");
    __asm__ volatile("movw $0x3D5, %%dx; movb %0, %%al; outb %%al, %%dx" : : "r"(cr50) : "eax", "edx");

    //Настройка регистра CR58 (Linear Address Control) — критически для S3
    //Бит 4 = 1 (Включить линейную адресацию памяти через PCI)
    __asm__ volatile("movw $0x3D4, %%dx; movb $0x58, %%al; outb %%al, %%dx" ::: "eax", "edx");
    uint8_t cr58;
    __asm__ volatile("movw $0x3D5, %%dx; inb %%dx, %%al" : "=a"(cr58) : : "edx");
    cr58 |= 0x10; // Активируем трансляцию адресов LFB на шину PCI
    __asm__ volatile("movw $0x3D4, %%dx; movb $0x58, %%al; outb %%al, %%dx" ::: "eax", "edx");
    __asm__ volatile("movw $0x3D5, %%dx; movb %0, %%al; outb %%al, %%dx" : : "r"(cr58) : "eax", "edx");

    //Настройка регистра CR67 (Формат вывода пикселей)
    //Значение 0x50 переключает встроенный RAMDAC в режим 16-бит (5-6-5) с прямой адресацией
    __asm__ volatile("movw $0x3D4, %%dx; movb $0x67, %%al; outb %%al, %%dx; movb $0x50, %%al; movw $0x3D5, %%dx; outb %%al, %%dx" ::: "eax", "edx");

    //Адрес фреймбуфера
    fb_virt_addr = s3a;
    fb_phys_addr = s3a;
    com_puts("[KERNEL] find s3_trio64_lfb addr: ");
    com_puthex(fb_phys_addr);
    com_puts("\n");
}

//Вызывается один раз при инициализации ядра (после vmm_init, до включения задач)
void acpi_init(void) {
    unsigned int *rsdp = acpiGetRSDPtr();
    if (!rsdp) return;                       //ACPI недоступен

    if (acpiCheckHeader(rsdp, "RSDT") != 0) return;

    //RSDT: заголовок 36 байт, далее массив 32-битных физических адресов
    int entries = *(rsdp + 1);               //Length всей RSDT
    entries = (entries - 36) / 4;
    unsigned int *entry_ptr = rsdp + 36 / 4;

    struct FACP *facp = 0;

    while (entries-- > 0) {
        if (acpiCheckHeader((unsigned int *)*entry_ptr, "FACP") == 0) {
            facp = (struct FACP *)*entry_ptr;
            break;
        }
        entry_ptr++;
    }
    if (!facp) return;

    //DSDT
    if (acpiCheckHeader((unsigned int *)facp->DSDT, "DSDT") != 0) return;

    //Поиск \_S5 в DSDT (AML-сканер)
    char *S5Addr = (char *)facp->DSDT + 36;       //пропускаем заголовок DSDT
    int dsdtLength = ((uint32_t *)facp->DSDT)[1] - 36;   // Length DSDT (поле Length в заголовке) минус 36

    while (dsdtLength-- > 0) {
        if (memcmp(S5Addr, "_S5_", 4) == 0) break;
        S5Addr++;
    }

    if (dsdtLength <= 0) return; //\_S5 не найден

    //Проверяем, что это действительно Name(\_S5, Package(...))
    if ((*(S5Addr - 1) == 0x08 ||
        (*(S5Addr - 2) == 0x08 && *(S5Addr - 1) == '\\')) &&
        *(S5Addr + 4) == 0x12) {

        S5Addr += 5;
        S5Addr += ((*S5Addr & 0xC0) >> 6) + 2; //PkgLength size

        if (*S5Addr == 0x0A) S5Addr++;          //byteprefix
        SLP_TYPa = *(uint8_t *)S5Addr << 10;
        S5Addr++;

        if (*S5Addr == 0x0A) S5Addr++;          //byteprefix
        SLP_TYPb = *(uint8_t *)S5Addr << 10;

        SMI_CMD      = (uint32_t *)facp->SMI_CMD;
        ACPI_ENABLE  = facp->ACPI_ENABLE;
        PM1a_CNT     = (uint32_t *)facp->PM1a_CNT_BLK;
        PM1b_CNT     = (uint32_t *)facp->PM1b_CNT_BLK;
        acpi_ready   = 1;
    }
}


void acpi_poweroff(void) {
    if (!acpi_ready) return;

    acpiEnable();  //если BIOS не включил ACPI

    //Записываем SLP_TYPa | SLP_EN в PM1a_CNT
    outw((uint32_t)(uintptr_t)PM1a_CNT, SLP_TYPa | SLP_EN);

    //Если есть PM1b_CNT — повторяем для него
    if (PM1b_CNT != 0) {
        outw((uint32_t)(uintptr_t)PM1b_CNT, SLP_TYPb | SLP_EN);
    }
}


//PCI-подсистема ядра: доступ к конфигурационному пространству (механизм #1),
//сканирование шин (включая PCI-to-PCI мосты) и реестр устройств для userspace

//--------------- Доступ к конфигурационному пространству ---------------

//Чтение выровненного dword. offset маскируется по границе dword
uint32_t pci_cfg_read_dword(uint32_t bus, uint32_t dev, uint32_t func, uint8_t offset){
    uint32_t address = 0x80000000u |
                       (bus    << 16) |
                       (dev    << 11) |
                       (func   <<  8) |
                       (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS_PORT, address);
    return inl(PCI_CONFIG_DATA_PORT);
}

void pci_cfg_write_dword(uint32_t bus, uint32_t dev, uint32_t func, uint8_t offset, uint32_t value){
    uint32_t address = 0x80000000u |
                       (bus    << 16) |
                       (dev    << 11) |
                       (func   <<  8) |
                       (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS_PORT, address);
    outl(PCI_CONFIG_DATA_PORT, value);
}

uint16_t pci_cfg_read_word(uint32_t bus, uint32_t dev, uint32_t func, uint8_t offset){
    uint32_t v = pci_cfg_read_dword(bus, dev, func, offset);
    return (uint16_t)(v >> ((offset & 2) * 8));
}

uint8_t pci_cfg_read_byte(uint32_t bus, uint32_t dev, uint32_t func, uint8_t offset){
    uint32_t v = pci_cfg_read_dword(bus, dev, func, offset);
    return (uint8_t)(v >> ((offset & 3) * 8));
}

//--------------- Реестр ---------------

int pci_registry_count(void){
    return g_pci_devs_count;
}

int pci_registry_get(int idx, pci_dev_record_t *out){
    if (idx < 0 || idx >= g_pci_devs_count || !out) return -1;
    *out = g_pci_devs[idx];
    return 0;
}

static int pci_registry_add(const pci_dev_record_t *rec){
    if (!rec) return -1;
    if (g_pci_devs_count >= PCI_REGISTRY_MAX) return -1;
    g_pci_devs[g_pci_devs_count++] = *rec;
    return g_pci_devs_count - 1;
}

//Чтение конфиг-пространства устройства, уже присутствующего в реестре.
//Userspace не может читать произвольные (bus,dev,func) — только перечисленные.
int pci_registry_read_cfg(uint32_t bdf, uint8_t offset, uint32_t *out_value){
    if (!out_value) return -1;
    if (offset > PCI_CFG_OFF_MAX) return -1;

    for (int i = 0; i < g_pci_devs_count; i++){
        uint32_t dev_bdf = PCI_BDF(g_pci_devs[i].bus, g_pci_devs[i].dev, g_pci_devs[i].func);
        if (dev_bdf != bdf) continue;

        uint32_t v = pci_cfg_read_dword(g_pci_devs[i].bus, g_pci_devs[i].dev,
                                        g_pci_devs[i].func, offset);
        //Выравнивание внутри dword, если offset не кратен 4
        *out_value = v >> ((offset & 3) * 8);
        return 0;
    }
    return -1;
}

//--------------- Сканирование ---------------

static void pci_record_device(uint32_t bus, uint32_t dev, uint32_t func){
    pci_dev_record_t rec;
    memset(&rec, 0, sizeof(rec));

    uint32_t id = pci_cfg_read_dword(bus, dev, func, PCI_CFG_VENDOR_ID);
    rec.vendor_id = (uint16_t)(id & 0xFFFF);
    rec.device_id = (uint16_t)(id >> 16);

    uint32_t rev_class = pci_cfg_read_dword(bus, dev, func, PCI_CFG_REV_CLASS);
    rec.rev         = (uint8_t)(rev_class & 0xFF);
    rec.prog_if     = (uint8_t)((rev_class >> 8)  & 0xFF);
    rec.subclass    = (uint8_t)((rev_class >> 16) & 0xFF);
    rec.class_code  = (uint8_t)((rev_class >> 24) & 0xFF);

    rec.bus  = (uint8_t)bus;
    rec.dev  = (uint8_t)dev;
    rec.func = (uint8_t)func;

    uint8_t htype = pci_cfg_read_byte(bus, dev, func, PCI_CFG_HEADER_TYPE);
    rec.header_type = htype & ~PCI_HTYPE_MULTIFUNC;

    rec.irq = pci_cfg_read_byte(bus, dev, func, PCI_CFG_IRQ_LINE);

    rec.cmd    = pci_cfg_read_word(bus, dev, func, PCI_CFG_CMD);
    rec.status = pci_cfg_read_word(bus, dev, func, PCI_CFG_STATUS);

    //BAR-ы: обычное устройство — 6, мост — 2
    int bars = (rec.header_type == PCI_HTYPE_BRIDGE) ? 2 : PCI_BARS_MAX;
    for (int b = 0; b < PCI_BARS_MAX; b++){
        if (b < bars)
            rec.bars[b] = pci_cfg_read_dword(bus, dev, func, PCI_CFG_BAR0 + 4 * b);
        else
            rec.bars[b] = 0;
    }

    rec.rom_base = pci_cfg_read_dword(bus, dev, func, PCI_CFG_ROM_BASE);

    if (rec.header_type == PCI_HTYPE_NORMAL || rec.header_type == PCI_HTYPE_BRIDGE){
        uint32_t subsys = pci_cfg_read_dword(bus, dev, func, PCI_CFG_SUBSYSTEM);
        rec.subsys_vendor_id = (uint16_t)(subsys & 0xFFFF);
        rec.subsys_id        = (uint16_t)(subsys >> 16);
    }

    //Мост: запоминаем номер вторичной шины для рекурсии
    rec.bridge_secondary = 0xFF;
    if (rec.header_type == PCI_HTYPE_BRIDGE &&
        rec.class_code == PCI_CLASS_BRIDGE &&
        rec.subclass == PCI_SUBCLASS_PCI_BRIDGE){
        rec.bridge_secondary = pci_cfg_read_byte(bus, dev, func, PCI_CFG_BR_SECONDARY);
    }

    //COM-лог найденного устройства
    com_puts("[PCI] ");
    if (bus < 0x10) com_puts("0");
    com_puthex(bus);
    com_puts(":");
    if (dev < 0x10) com_puts("0");
    com_puthex(dev);
    com_puts(".");
    com_puthex(func);
    com_puts(" vid:did ");
    com_puthex(rec.vendor_id);
    com_puts(":");
    com_puthex(rec.device_id);
    com_puts(" class ");
    com_puthex(rec.class_code);
    com_puts("/");
    com_puthex(rec.subclass);
    com_puts("/");
    com_puthex(rec.prog_if);
    if (rec.irq != 0xFF){
        com_puts(" irq ");
        com_puthex(rec.irq);
    }
    com_puts("\n");

    pci_registry_add(&rec);
}

static void pci_scan_bus(uint32_t bus, int depth){
    if (bus > 255) return;
    if (g_bus_scanned[bus >> 3] & (1u << (bus & 7))) return; //шина уже пройдена
    g_bus_scanned[bus >> 3] |= (1u << (bus & 7));

    if (depth > PCI_SCAN_MAX_DEPTH) return;

    for (uint32_t dev = 0; dev < 32; dev++){
        uint32_t id = pci_cfg_read_dword(bus, dev, 0, PCI_CFG_VENDOR_ID);
        if ((id & 0xFFFF) == 0xFFFF) continue; //слот пуст

        uint8_t htype = pci_cfg_read_byte(bus, dev, 0, PCI_CFG_HEADER_TYPE);
        uint32_t func_max = (htype & PCI_HTYPE_MULTIFUNC) ? 8 : 1;

        for (uint32_t func = 0; func < func_max; func++){
            uint32_t fid = pci_cfg_read_dword(bus, dev, func, PCI_CFG_VENDOR_ID);
            if ((fid & 0xFFFF) == 0xFFFF) continue; //функции нет

            if (g_pci_devs_count >= PCI_REGISTRY_MAX) return;

            pci_record_device(bus, dev, func);

            //Рекурсия в мосты
            uint8_t fht  = pci_cfg_read_byte(bus, dev, func, PCI_CFG_HEADER_TYPE);
            uint32_t cls = pci_cfg_read_dword(bus, dev, func, PCI_CFG_REV_CLASS);
            if ((fht & ~PCI_HTYPE_MULTIFUNC) == PCI_HTYPE_BRIDGE &&
                ((cls >> 24) & 0xFF) == PCI_CLASS_BRIDGE &&
                ((cls >> 16) & 0xFF) == PCI_SUBCLASS_PCI_BRIDGE){
                uint8_t secondary = pci_cfg_read_byte(bus, dev, func, PCI_CFG_BR_SECONDARY);
                if (secondary != 0 && secondary != bus){
                    pci_scan_bus(secondary, depth + 1);
                }
            }
        }
    }
}

//Полное сканирование. Повторные вызовы — no-op
int pci_scan_all(void){
    if (g_pci_scanned) return g_pci_devs_count;
    g_pci_scanned = 1;

    memset(g_bus_scanned, 0, sizeof(g_bus_scanned));
    g_pci_devs_count = 0;

    //Детект PCI: если на шине 0 нет ни одного устройства,
    //считаем PCI-хост отсутствующим (или механизм #1 не работает)
    uint32_t id0 = pci_cfg_read_dword(0, 0, 0, PCI_CFG_VENDOR_ID);
    if ((id0 & 0xFFFF) == 0xFFFF){
        com_puts("[PCI] Mechanism #1: no devices on bus 0, PCI host absent?\n");
        return 0;
    }

    pci_scan_bus(0, 0);

    com_puts("[PCI] scan done, ");
    com_puthex(g_pci_devs_count);
    com_puts(" devices\n");
    return g_pci_devs_count;
}

