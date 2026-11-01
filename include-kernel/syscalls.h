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

#ifndef SYSCALLS_H
#define SYSCALLS_H

//Номера системных вызовов возьмутся из userspace/include/syscall.h

#define HALT_SUCCESS           0
#define HALT_ERR_OPEN_FILES   -1
#define HALT_ERR_VFS_DENIED   -2

#define PROCESS_IS_ACTIVE      -2

#define LINE_BUFFER_SIZE 1024
#define MAX_SYSCALLS 100

#define STR(x) #x
#define XSTR(x) STR(x)

#include "../userspace/include/syscall.h"
#include "task.h"
#include "use_drivers.h"

//Структура векторов системных вызовов
typedef struct {
    //uint16_t number;
    void (*func)(struct regs *r);
} syscall_vectors_table_t;

extern syscall_vectors_table_t syscalls_table[MAX_SYSCALLS];

//Вспомогательные макросы для вызова из ядра (если нужно)
static inline void sys_draw_char(unsigned int x, unsigned int y, char c){
	asm volatile("int $0x80"::"a"(SYS_DRAW_CHAR),"b"(x),"c"(y),"d"(c):"memory");
}

static inline void sys_fill_screen(unsigned char r, unsigned char g, unsigned char b){
    asm volatile("int $0x80"::"a"(SYS_FILL_SCREEN),"b"(r),"c"(g),"d"(b):"memory");
}

void syscall_handler(struct regs *r);

uint32_t tty_get_copied_bytes(void);

void syscalls_table_init(void);

#endif
