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

#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_FILL_SCREEN     2
#define SYS_EXIT 		    3
#define SYS_YIELD 		    4
#define SYS_DRAW_STRING     5
#define SYS_DRAW_CHAR    6
#define SYS_GET_FB_INFO     7
#define SYS_ALLOC_PAGE      8
#define SYS_FREE_PAGE       9

#define SYS_OPEN           10
#define SYS_READDIR        11
#define SYS_CLOSE          12
#define SYS_READ           13
#define SYS_WRITE          14
#define SYS_MKDIR          15
#define SYS_UNLINK         16
#define SYS_RMDIR          17
#define SYS_STAT           18
#define SYS_SEEK           19

#define SYS_CLEAR_SCREEN   20
//#define SYS_DRAW_CHAR      21

#define SYS_SPAWN          31
#define SYS_WAIT_PID       32
#define SYS_GET_MEM_INFO   33
#define SYS_SET_WALLPAPER  34
#define SYS_SBRK           35

#define SYS_CHDIR          41
#define SYS_GETCWD         42
#define SYS_TELL           43
#define SYS_GET_BOOTINFO   44
#define SYS_KBD_HIT        45
#define SYS_CLEAR_TILE     46

#define SYS_GET_TICKS      47
#define SYS_DUP2           48
#define SYS_ISATTY         49
#define SYS_KBD_SET_MODE   50

#define SYS_TTY_GET_FLAGS         51
#define SYS_TTY_SET_FLAGS         52
#define SYS_LOCALE_GET            53
#define SYS_LOCALE_GET_KEYMAP     54
#define SYS_GET_CONSOLE_GEOMETRY  55
#define SYS_SCROLL_RECT           56
#define SYS_FB_BLIT               57

#define SYS_HALT_OS               99

#define KBD_DRIVER_NOT_READY   -2
#define PROCESS_IS_ACTIVE      -2


#define TTY_ECHO       0x0001
#define TTY_ICANON     0x0002
#define TTY_ISIG       0x0004
#define TTY_KBD_RAW    0x0008


#define TTY_DEFAULT    (TTY_ECHO)



#endif

