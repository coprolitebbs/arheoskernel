#ifndef SYSCALLS_H
#define SYSCALLS_H

// Номера системных вызовов
#define SYS_DRAW_CHAR    1
#define SYS_FILL_SCREEN  2
#define SYS_EXIT 		 3
#define SYS_YIELD 		 4
#define SYS_GET_BOOTINFO 5
#define SYS_FB_DRAW_CHAR 6
#define SYS_GET_FB_INFO  7
#define SYS_ALLOC_PAGE   8
// будущие: SYS_GET_KEY, SYS_WRITE_SERIAL и т.д.

#include "task.h"
#include "use_drivers.h"





// Вспомогательные макросы для вызова из ядра (если нужно)
static inline void sys_draw_char(unsigned int x, unsigned int y, char c) {

	asm volatile("int $0x80"
                 : : "a"(SYS_DRAW_CHAR), "b"(x), "c"(y), "d"(c)
                 : "memory");

}

static inline void sys_fill_screen(unsigned char r, unsigned char g, unsigned char b) {
    asm volatile("int $0x80"
                 : : "a"(SYS_FILL_SCREEN), "b"(r), "c"(g), "d"(b)
                 : "memory");
}

void syscall_handler(struct regs *r);

#endif
