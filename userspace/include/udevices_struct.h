#ifndef UDEVICES_STRUCT_H
#define UDEVICES_STRUCT_H

#include <stdint.h>

#define USB_DEV_NAME_MAX    32
#define USB_DEV_SERIAL_MAX  20
#define USB_REGISTRY_MAX    8

/* Каноническое описание USB-устройства.
   Используется одновременно:
     - драйвером (usb_stack_driver_api_t->devices[]),
     - ядром (usb_registry_*),
     - userspace (SYS_USB_GET_DEV через u_usb_dev_t).
   Все стороны договариваются об одном layout. */
typedef struct {
    uint32_t vid;
    uint32_t pid;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint8_t  port;
    uint32_t block_id;                  /* 0xFFFFFFFF если не блок-девайс */
    char     manufacturer[USB_DEV_NAME_MAX];
    char     product[USB_DEV_NAME_MAX];
    char     serial[USB_DEV_SERIAL_MAX];
} __attribute__((packed)) usb_dev_record_t;

/* Пользовательское «зеркало» — тот же layout, другое имя типа,
   чтобы userspace-код не тянул драйверные заголовки. */
typedef usb_dev_record_t u_usb_dev_t;

#endif


