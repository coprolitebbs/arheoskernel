#ifndef SYSCALLS_H
#define SYSCALLS_H

//Номера системных вызовов возьмутся из userspace/include/syscall.h

#define HALT_SUCCESS           0
#define HALT_ERR_OPEN_FILES   -1
#define HALT_ERR_VFS_DENIED   -2

#define PROCESS_IS_ACTIVE      -2

#define LINE_BUFFER_SIZE 1024

#define STR(x) #x
#define XSTR(x) STR(x)

#include "../userspace/include/syscall.h"
#include "task.h"
#include "use_drivers.h"


static const char kbd_ascii_clean[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0,
  '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0,  '*',   0, ' ',
};

static const char kbd_ascii_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',   0,
  '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',   0,  '*',   0, ' ',
};

//Вспомогательные макросы для вызова из ядра (если нужно)
static inline void sys_draw_char(unsigned int x, unsigned int y, char c){
	asm volatile("int $0x80"::"a"(SYS_DRAW_CHAR),"b"(x),"c"(y),"d"(c):"memory");
}

static inline void sys_fill_screen(unsigned char r, unsigned char g, unsigned char b){
    asm volatile("int $0x80"::"a"(SYS_FILL_SCREEN),"b"(r),"c"(g),"d"(b):"memory");
}

void syscall_handler(struct regs *r);

uint32_t tty_get_copied_bytes(void);

#endif
