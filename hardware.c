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


//Внутренние функции
uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    //Вычисляем адрес по спецификации PCI Configuration Mechanism #1
    uint32_t address = (1UL << 31) |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) |
                       (offset & 0xFC);
    uint32_t value;
    //"a" привязывает переменную к EAX, "d" привязывает к DX
    __asm__ volatile ("outl %%eax, %%dx":: "a"(address), "d"((uint16_t)0x0CF8));
    __asm__ volatile ("inl %%dx, %%eax":"=a"(value):"d"((uint16_t)0x0CFC));
    return value;
}



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
        uint32_t id = pci_read_config_dword(0, slot, 0, 0x00);
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
            uint32_t bar0 = pci_read_config_dword(0, slot, 0, 0x10);
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
