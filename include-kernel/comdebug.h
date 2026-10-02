// include-kernel/debug.h
#ifndef COMDEBUG_H
#define COMDEBUG_H

#include <stdint.h>

void com_init(void);
void com_putc(char c);
void com_puts(const char *s);
void com_puthex(uint32_t val);
void com_putstr_hex(const char *label, uint32_t val);

#endif
