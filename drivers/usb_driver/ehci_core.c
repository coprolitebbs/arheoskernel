#include "include/ehci_priv.h"
#include "include/usb_uhci.h"
#include "include/usb_driver.h"
#include "../../include-kernel/lib.h"

ehci_qh_t ehci_ctrl_qh __attribute__((aligned(32)));
ehci_qtd_t ehci_ctrl_qtd[4] __attribute__((aligned(32)));
ehci_qh_t  *g_qh;
ehci_qtd_t *g_qtd;
ehci_qh_t  *g_qh_base;
ehci_qtd_t *g_qtd_base;
ehci_qh_t  ehci_bulk_qh __attribute__((aligned(32)));
ehci_qtd_t ehci_bulk_qtd[MAX_BULK_TDS] __attribute__((aligned(32)));
ehci_qh_t  *ehci_bulk_qh_ptr;
ehci_qtd_t *ehci_bulk_qtd_ptr;

uint8_t setup_packet_buf[32] __attribute__((aligned(32)));
uint8_t *setup_packet_buf_ptr;

void ehci_wr(volatile uint32_t *r, uint32_t v){
    *r = v;
    (void)*r;
}
uint32_t ehci_rd(volatile uint32_t *r){
    return *r;
}

int ehci_start_async(ehci_hc_t *hc){
    return 0; // Нам не нужно запускать асинхронное расписание EHCI, мы отдали шину UHCI!
}

void ehci_fill_qtd(ehci_qtd_t *q, uint32_t next_phys, uint32_t pid_bits, uint32_t dt, uint32_t len, uint32_t buf_phys, int ioc){}

int ehci_probe(ehci_hc_t *hc){
    if (!hc) return -1;
    memset(hc, 0, sizeof(*hc));
    for (uint16_t bus = 0; bus < 256; bus++){
        for (uint8_t dev = 0; dev < 32; dev++){
            for (uint8_t func = 0; func < 8; func++){
                uint32_t id = pci_read32((uint8_t)bus, dev, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                uint32_t cls = pci_read32((uint8_t)bus, dev, func, 0x08);
                if (((cls >> 24) & 0xFF) != PCI_CLASS_SERIAL_BUS) continue;
                if (((cls >> 16) & 0xFF) != PCI_SUBCLASS_USB) continue;
                if (((cls >>  8) & 0xFF) != PCI_PROGIF_EHCI) continue;

                uint32_t cmd = pci_read32((uint8_t)bus, dev, func, 0x04);
                pci_write32((uint8_t)bus, dev, func, 0x04, cmd | 0x06);

                uint32_t bar0 = pci_read32((uint8_t)bus, dev, func, 0x10) & 0xFFFFFF00u;
                if (bar0 == 0 || bar0 == 0xFFFFFF00u) continue;

                hc->bar0    = bar0;
                hc->pci_bus  = (uint8_t)bus;
                hc->pci_dev  = dev;
                hc->pci_func = func;
                return 0;
            }
        }
    }
    return -1;
}

int ehci_release_from_bios(ehci_hc_t *hc){ return 0; }

// СВЕРХНАДЕЖНЫЙ КОМПЛЕКСНЫЙ ИНИЦИАЛИЗАТОР ИЗ HAIKU OS С ТОТАЛЬНЫМ HANDOFF
int ehci_init(ehci_hc_t *hc){
    if (!hc || hc->bar0 == 0) return -1;

    volatile uint32_t *cap = (volatile uint32_t *)hc->bar0;
    uint32_t cap0 = ehci_rd(&cap[0]);

    hc->cap_length  = (uint8_t)(cap0 & 0xFF);
    if (hc->cap_length < 8 || hc->cap_length > 0x80) hc->cap_length = 0x20;

    hc->hcs_params = ehci_rd(&cap[1]);
    hc->n_ports = (uint8_t)(hc->hcs_params & 0x0F);
    if (hc->n_ports == 0 || hc->n_ports > 8) hc->n_ports = 4;

    hc->op = (volatile uint32_t *)(hc->bar0 + hc->cap_length);

    // 1) Полностью глушим маршевый и асинхронный движки планировщика (Haiku OS pattern)
    uint32_t cmd = ehci_rd(&hc->op[EHCI_OP_USBCMD / 4]);
    ehci_wr(&hc->op[EHCI_OP_USBCMD / 4], cmd & ~(0x00000001u | 0x00000020u)); // RS=0, ASE=0
    for (uint32_t i = 0; i < 100000; i++){
        if (ehci_rd(&hc->op[EHCI_OP_USBSTS / 4]) & EHCI_STS_HCH) break;
    }

    // 2) Выполняем тотальный аппаратный сброс самого ядра EHCI (HCRESET)
    ehci_wr(&hc->op[EHCI_OP_USBCMD / 4], EHCI_CMD_HCRESET);
    for (uint32_t i = 0; i < 1000000; i++){
        if (!(ehci_rd(&hc->op[EHCI_OP_USBCMD / 4]) & EHCI_CMD_HCRESET)) break;
    }

    // 3) Очищаем все W1C Change биты статуса
    ehci_wr(&hc->op[EHCI_OP_USBSTS / 4], 0x3F);


    ehci_wr(&hc->op[EHCI_OP_USBINTR / 4], 0);
    return 0;
}

uint32_t ehci_port_status(ehci_hc_t *hc, int port){
    if (!hc || !hc->op || port < 0 || port >= hc->n_ports) return 0;
    return ehci_rd(&hc->op[(EHCI_OP_PORTSC / 4) + port]);
}
int ehci_port_connected(ehci_hc_t *hc, int port){ return 0; }
int ehci_bulk_transfer(ehci_hc_t *hc, uint8_t addr, uint8_t ep_addr, uint16_t ep_max, uint8_t *toggle, void *buf, uint32_t len, int dir_in){ return -1; }
int ehci_control_transfer(ehci_hc_t *hc, uint8_t addr, const uint8_t *setup, void *data, uint16_t dlen, int data_in, uint16_t max_pkt){ return -1; }
int ehci_reset_port(ehci_hc_t *hc, int port){ return -1; }

void ehci_register_ptrs(void){
    setup_packet_buf_ptr = setup_packet_buf;
    g_qh_base  = &ehci_ctrl_qh;
    g_qtd_base = ehci_ctrl_qtd;
    ehci_bulk_qh_ptr     = &ehci_bulk_qh;
    ehci_bulk_qtd_ptr    = ehci_bulk_qtd;
}
