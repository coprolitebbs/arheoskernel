#ifndef KBD_INTERNAL_H
#define KBD_INTERNAL_H

#include "../../../include-kernel/bootinfo.h"

#include <stdint.h>
#include <stdbool.h>

//Аппаратные порты контроллера клавиатуры i8042 (ностальгический IBM PC/AT)
#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64

#define KBD_BUFFER_SIZE  256

static const char kbd_ascii_map[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0,
  '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0,  '*',   0, ' ',
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,  '-',   0,   0,   0,  '+',   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 0
};

#endif

