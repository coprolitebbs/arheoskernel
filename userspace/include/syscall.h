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
#define SYS_EXIT                    3
#define SYS_YIELD                   4
#define SYS_DRAW_STRING     5
#define SYS_DRAW_CHAR       6
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
#define SYS_RENAME         20

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
#define SYS_CLEAR_SCREEN          58
#define SYS_CLIPBOARD             59


//реестр USB устройств
#define SYS_USB_COUNT             60   //-> eax = число устройств
#define SYS_USB_GET_DEV           61   //ebx = idx, ecx = usb_dev_info_t*
#define SYS_GET_CURSOR_POS        62

#define SYS_GET_MOUNT_COUNT       63
#define SYS_GET_MOUNT_INFO        64

//Управление курсором редактора строки из Ring-3 (см. systemcall_tty_line_edit)
#define SYS_TTY_LINE_EDIT         65

//Графическая подсистема: карточка активного видеодрайвера, его текущий
//режим, fix-информация и список доступных разрешений (источник — реестр
//graphic_layer.c: fbdrv-fallback или загруженный vesa_fb и т.п.)
#define SYS_GRAPH_INFO            66

//реестр PCI устройств
#define SYS_PCI_COUNT             67   //-> eax = число устройств (ленивое сканирование шин)
#define SYS_PCI_GET_DEV           68   //ebx = idx, ecx = pci_dev_record_t*
#define SYS_PCI_READ_CFG          69   //ebx = PCI_BDF(bus,dev,func), ecx = offset; -> eax = dword или -1

//Ручной USB-hotplug: пересмотреть порты активных USB-контроллеров
//(вставлено/вынуто), перестроить реестр устройств и блок-слой.
//eax = число устройств после обновления или отрицательный код ошибки
#define SYS_USB_RESCAN            70

//op-коды для SYS_GRAPH_INFO (ebx = op)
#define GRAPH_INFO_DRIVER   0   //ecx = u_gr_driver_info_t*
#define GRAPH_INFO_MODE     1   //ecx = индекс режима, edx = u_gr_mode_info_t*
#define GRAPH_INFO_CURRENT  2   //ecx = u_gr_mode_info_t* — текущий режим
#define GRAPH_INFO_FIX      3   //ecx = u_gr_fix_info_t* — fix-информация
#define GRAPH_INFO_SET_MODE 4   //ecx = индекс режима; возврат 0 или код GR_ERR_*

#define SYS_HALT_OS               99

#define KBD_DRIVER_NOT_READY   -2
#define PROCESS_IS_ACTIVE      -2


#define TTY_ECHO       0x0001
#define TTY_ICANON     0x0002
#define TTY_ISIG       0x0004
#define TTY_KBD_RAW    0x0008


#define TTY_DEFAULT    (TTY_ECHO)

//op-коды для SYS_CLIPBOARD (ebx = op, ecx = buf, edx = size)
#define CLIP_OP_GET_SIZE   0   //ecx=edx=0 ; -> eax = размер буфера обмена
#define CLIP_OP_GET        1   //ecx=buf, edx=max; -> eax = скопировано, буфер обрезан
#define CLIP_OP_SET        2   //ecx=buf, edx=size; -> eax = 0
#define CLIP_OP_CLEAR      3   // -> eax = 0


#endif

