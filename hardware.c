#include "include-kernel/hardware.h"
#include "include-kernel/ports_io.h"
#include "include-kernel/comdebug.h"
#include "include-kernel/pmm.h"


//Внутренние функции
uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset){
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


//Внешние функции

//Бип
void beep(uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
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


uint32_t enumerate_pci_ven_dev_ids(void){
    uint32_t res = 0;
    int cnt = 0;

    for (uint8_t slot = 0; slot < 32; slot++){
        uint32_t id = pci_read_config_dword(0, slot, 0, 0x00);
        uint16_t vendor = id & 0xFFFF;
        uint16_t device = id >> 16;

        if(vendor != 0x0000FFFF && device != 0x0000FFFF){
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
        if (res == 0 && vendor == 0x5333 && device == 0x8811){
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
void find_and_prepare_s3_trio64(void){
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
