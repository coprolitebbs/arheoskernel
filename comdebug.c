// comdebug.c
#include "include-kernel/lib.h"
#include "include-kernel/comdebug.h"
#include "include-kernel/ports_io.h"

#define COM1_PORT 0x3F8

int com_initialized = 0;

void com_init(void) {
    //Отключаем прерывания
    outb(COM1_PORT + 1, 0x00);
    //Включаем DLAB (доступ к делителю)
    outb(COM1_PORT + 3, 0x80);
    //Устанавливаем делитель на 9600 бод (0x03)
    outb(COM1_PORT + 0, 0x03);
    outb(COM1_PORT + 1, 0x00);
    //8 бит, 1 стоп, без четности, выключаем DLAB
    outb(COM1_PORT + 3, 0x03);
    //Включаем FIFO
    outb(COM1_PORT + 2, 0xC7);
    //Включаем прерывания (опционально, сейчас polling)
    //outb(COM1_PORT + 1, 0x01);
    com_initialized = 1;
}

static void com_wait_ready() {
    while ((inb(COM1_PORT + 5) & 0x20) == 0);
}

void com_putc(char c) {
    if (!com_initialized) com_init();
    com_wait_ready();
    outb(COM1_PORT, c);
    //Если нужен перевод строки, отправляем CR
    if (c == '\n') {
        com_wait_ready();
        outb(COM1_PORT, '\r');
    }
}

void com_puts(const char *s) {
    while (*s) com_putc(*s++);
}

void com_puthex(uint32_t val) {
    const char hex[] = "0123456789ABCDEF";
    com_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        com_putc(hex[(val >> i) & 0xF]);
    }
}

void com_putstr_hex(const char *label, uint32_t val) {
    com_puts(label);
    com_puts(": ");
    com_puthex(val);
    com_puts("\n");
}
