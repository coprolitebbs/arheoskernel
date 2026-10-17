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

#ifndef DRV_FORMAT_H
#define DRV_FORMAT_H

#include <stdint.h>
#include "../../include-kernel/bootinfo.h"
#include "../../userspace/include/fileattrs.h"

#define DRV_MAGIC "DRV1"

#define DRV_RELOC_ABS32 1
#define DRV_RELOC_PC32  2

//Общий ABI драйверов файловых систем

#define FS_DRIVER_ABI_VERSION 1

#define KBD_DRIVER_NOT_READY   -2

#define DESCR_FRAMEBUFFER_DRIVER   1
#define DESCR_FILESYSTEM_DRIVER    2
#define DESCR_KEYBOARD_DRIVER      3

typedef struct {
    uint8_t type;
    void *struct_ptr;
} description_driver_api_t;


typedef struct{
    void (*put_pixel)(int,int,uint32_t);
    void (*fill_rect)(int,int,int,int,uint32_t);
    void (*clear)(uint32_t);
    void (*draw_char)(uint32_t,uint32_t,unsigned char,uint32_t);
    void (*draw_string)(uint32_t,uint32_t,const char*,uint32_t);
    void (*draw_hex)(uint32_t,int,int);
    void (*draw_codepoint)(uint32_t x, uint32_t y, uint32_t codepoint, uint32_t color);

    void* (*malloc_ptr)(uint32_t size);
    void  (*free_ptr)(void *ptr);

    void (*set_fg)(uint32_t color);
    void (*set_bg)(uint32_t color);
    uint32_t (*get_fg)(void);
    uint32_t (*get_bg)(void);
    void (*scroll_up)(uint32_t start_y, uint32_t end_y, uint32_t line_height);

    void (*set_wallpaper)(const void *src_user_buffer, uint32_t size);
    void (*clear_tile)(int x, int y, uint32_t fallback_color);

    int (*scroll_rect)(int x, int y, int w, int h, int dy, uint32_t fill);
    int (*blit)(int x, int y, int w, int h, const void *src);

    const uint8_t (*font)[8];
    //const void *active_locale;
    const void *(*get_active_locale_ptr)(void);
    const void *(*font_lookup_ptr)(const void *font, uint32_t cp);

} fb_driver_api_t;


typedef struct {
    //Функция-коллбэк, вызывается ядром из обработчика IRQ1
    //Передает сырой скан-код из порта 0x60 в драйвер
    void (*irq_callback)(uint8_t scancode);

    //Функция чтения символа вызывается ядром из системного вызова SYS_READ (stdin)
    //Возвращает транслированный ASCII-символ. Должна блокировать таск (через yield), если буфер пуст.
    uint32_t (*get_char_blocked)(void);

    //Функция проверки наличия символов (неблокирующий опрос, аналог kbhit)
    int (*has_chars)(void);

    //Экспортные указатели ядра для внутренних нужд драйвера
    void *(*malloc_ptr)(uint32_t size);
    void  (*free_ptr)(void *ptr);
    void  (*yield_ptr)(void);       //Указатель на yield() ядра для асинхронного сна

    //uint32_t debug_val1;
} kbd_driver_api_t;



typedef struct{
    uint32_t id;
    uint32_t size;
    uint32_t attributes;
    uint32_t private_data;
} fs_node_t;

typedef struct{
    char name[256];
    uint32_t id;
    uint32_t size;
    uint32_t attributes;
    uint32_t inode;
    uint32_t type;
} fs_dirent_t;


typedef struct{
    uint32_t size;
    uint32_t attributes;
    uint32_t create_time;
    uint32_t modify_time;
} fs_stat_t;

typedef struct{
    int (*mount)(uint32_t drive);
    int (*unmount)(void *volume);
    int (*lookup)(void *vol,const char *name,void *out,uint32_t *out_entry_id);
    int (*open)(void *vol,const char *path,uint32_t flags);
    int (*touch)(void *vol, const char *path);
    int (*close)(int fd);
    int (*read)(int fd,void *buffer,uint32_t size);
    int (*write)(int fd,const void *buffer,uint32_t size);
    int (*unlink)(void *vol, const char *path);
    int (*seek)(int fd,uint32_t position);
    int (*tell)(int fd);
    int (*readdir)(int dir,fs_dirent_t *entry);
    int (*mkdir)(void *vol, const char *path);
    int (*rmdir)(void *vol, const char *path);
    int (*stat)(void *vol,const char *path,fs_stat_t *st);
    void *(*get_volume)(uint32_t id);

    volatile uint8_t *floppy_irq_fired_ptr;  //irq6 flag
    //void *dma_buffer;

    void *(*malloc_ptr)(uint32_t size);
    void  (*free_ptr)(void *ptr);

    //Указатель на запрет вызова shedule() из обработчика прерываний - нужно для
    //долгих операций с диском, чтобы по прерыванию 32 не вылетело и не испортило стек
    volatile uint8_t *schedule_lock_ptr;

} fs_driver_api_t;


typedef struct{
    uint8_t scancode; //Физический скан-код клавиши (1..127)
    uint8_t type;     //1 - Нажата (Press), 0 - Отпущена (Release)
} kbd_event_t;

#endif
