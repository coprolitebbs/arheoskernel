#ifndef UDEVICES_STRUCT_H
#define UDEVICES_STRUCT_H

#include <stdint.h>

#define USB_DEV_NAME_MAX    32
#define USB_DEV_SERIAL_MAX  20
#define USB_REGISTRY_MAX    8


#define PCI_REGISTRY_MAX 64
#define PCI_BARS_MAX     6

//Упаковка/распаковка (bus, dev, func) в один 32-битный дескриптор
#define PCI_BDF(bus, dev, func) (((uint32_t)(bus) << 16) | ((uint32_t)(dev) << 8) | (uint32_t)(func))
#define PCI_BDF_BUS(bdf)  (uint8_t)(((bdf) >> 16) & 0xFF)
#define PCI_BDF_DEV(bdf)  (uint8_t)(((bdf) >> 8)  & 0xFF)
#define PCI_BDF_FUNC(bdf) (uint8_t)((bdf) & 0xFF)


//Каноническое описание USB-устройства
//Используется одновременно:
//- драйвером (usb_stack_driver_api_t->devices[]),
//- ядром (usb_registry_*),
//- userspace (SYS_USB_GET_DEV через u_usb_dev_t)
typedef struct
{
    uint32_t vid;
    uint32_t pid;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint8_t  port;
    uint32_t block_id;                  //0xFFFFFFFF если не блок-девайс
    char     manufacturer[USB_DEV_NAME_MAX];
    char     product[USB_DEV_NAME_MAX];
    char     serial[USB_DEV_SERIAL_MAX];
} __attribute__((packed)) usb_dev_record_t;

//Каноническое описание PCI-устройства
//Используется одновременно:
//   - ядром (pci.c: реестр результатов сканирования шин),
//   - userspace (SYS_PCI_GET_DEV через u_pci_dev_t).
typedef struct
{
    uint16_t vendor_id;        //0x00
    uint16_t device_id;        //0x02
    uint16_t subsys_vendor_id; //0x2C, 0 если заголовок не 0x00/0x01
    uint16_t subsys_id;
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint8_t  rev;              //0x08, байт 0
    uint8_t  class_code;       //0x08, байт 3
    uint8_t  subclass;         //0x08, байт 2
    uint8_t  prog_if;          //0x08, байт 1
    uint8_t  header_type;      //0x0E без бита multifunction
    uint8_t  irq;              //0x3C, 0xFF = не подключено
    uint8_t  bridge_secondary; //0x19 для мостов 06/04, 0xFF иначе
    uint32_t bars[PCI_BARS_MAX]; //сырые регистры BAR (флаги внутри)
    uint32_t rom_base;         //сырой регистр 0x30
    uint16_t cmd;              //0x04
    uint16_t status;           //0x06
} __attribute__((packed)) pci_dev_record_t;


//Пользовательское зеркало — тот же layout, другое имя типа, чтобы userspace-код не тянул драйверные заголовки
typedef usb_dev_record_t u_usb_dev_t;


//Пользовательское зеркало — тот же layout, другое имя типа, чтобы userspace-код не тянул ядерные заголовки
typedef pci_dev_record_t u_pci_dev_t;

#endif


