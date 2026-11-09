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
#include <stddef.h>
#include <stdbool.h>
#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/block.h"
#include "../../userspace/include/fileattrs.h"
#include "../../userspace/include/udevices_struct.h"

#define DRV_MAGIC "DRV1"

#define DRV_RELOC_ABS32 1
#define DRV_RELOC_PC32  2

//Общий ABI драйверов файловых систем

#define FS_DRIVER_ABI_VERSION 1

#define KBD_DRIVER_NOT_READY   -2

#define DESCR_FRAMEBUFFER_DRIVER   1
#define DESCR_FILESYSTEM_DRIVER    2
#define DESCR_KEYBOARD_DRIVER      3
#define DESCR_BLOCKDEVICE_DRIVER   4
#define DESCR_USB_DRIVER           5
//Высокоуровневые слои графики/звука (kernel_graphic_layer.h / kernel_sound_layer.h)
#define DESCR_GRAPHICS_DRIVER      6
#define DESCR_SOUND_DRIVER         7


#define BLK_REMOVABLE   0x0001
#define BLK_READONLY    0x0002
#define BLK_PARTITION   0x0004

typedef struct
{
    uint8_t type;
    void *struct_ptr;
} description_driver_api_t;



typedef struct block_driver_api
{
    uint32_t version;     // = 1, для совместимости в будущем
    bool     enabled;
    //I/O
    int (*read)  (uint32_t dev_id, uint64_t lba, uint32_t count, void *buf);
    int (*write) (uint32_t dev_id, uint64_t lba, uint32_t count, const void *buf);
    //Информация об устройстве (для enum, для отладки)
    int (*get_info)(uint32_t dev_id, block_info_t *out);
    //Перечисление всех устройств
    //index = 0, 1, 2, ... ; возвращает -1 (или FS_NOT_FOUND) в конце
    int (*enum_devices)(int index, block_info_t *out);
    //Поиск по имени ("sda1", "usb0p1") — для mount-by-name из userspace
    int (*find_by_name)(const char *name, uint32_t *out_id);
    //Партиции
    int (*get_partition)(uint32_t disk_id, uint32_t part_num, uint32_t *out_id);
    int (*scan_partitions)(uint32_t disk_id);          //MBR/GPT скан, регистрация дочерних
    //Hints
    int (*resolve_boot_drive)(uint32_t bios_drive, uint32_t *out_id);
    //Возвращает id зарегистрированного устройства (0..N-1) или отрицательный FS_* при ошибке
    int (*register_device)(const char *name, uint32_t sector_count, uint32_t flags,
                           int (*read_fn)(void*, uint64_t, uint32_t, void*),
                           int (*write_fn)(void*, uint64_t, uint32_t, const void*),
                           void *priv);

    //Экспортные указатели ядра для внутренних нужд драйвера
    void *(*malloc_ptr)(uint32_t size);
    void  (*free_ptr)(void *ptr);
    void  (*yield_ptr)(void);       //Указатель на yield() ядра для асинхронного сна

} block_driver_api_t;




typedef struct
{
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
    bool (*has_wallpaper)(void);

    int (*scroll_rect)(int x, int y, int w, int h, int dy, uint32_t fill);
    int (*blit)(int x, int y, int w, int h, const void *src);

    const uint8_t (*font)[8];
    //const void *active_locale;
    const void *(*get_active_locale_ptr)(void);
    const void *(*font_lookup_ptr)(const void *font, uint32_t cp);

} fb_driver_api_t;


typedef struct
{
    //Функция-коллбэк, вызывается ядром из обработчика IRQ1
    //Передает сырой скан-код из порта 0x60 в драйвер
    void (*irq_callback)(uint8_t scancode);

    //Функция чтения символа вызывается ядром из системного вызова SYS_READ (stdin)
    //Возвращает транслированный ASCII-символ. Должна блокировать таск (через yield), если буфер пуст
    uint32_t (*get_char_blocked)(void);

    //Функция проверки наличия символов (неблокирующий опрос, аналог kbhit)
    int (*has_chars)(void);

    //Экспортные указатели ядра для внутренних нужд драйвера
    void *(*malloc_ptr)(uint32_t size);
    void  (*free_ptr)(void *ptr);
    void  (*yield_ptr)(void);       //Указатель на yield() ядра для асинхронного сна

    //uint32_t debug_val1;
} kbd_driver_api_t;



typedef struct
{
    uint32_t id;
    uint32_t size;
    uint32_t attributes;
    uint32_t private_data;
} fs_node_t;

typedef struct
{
    char name[256];
    uint32_t id;
    uint32_t size;
    uint32_t attributes;
    uint32_t inode;
    uint32_t type;
} fs_dirent_t;


typedef struct
{
    uint32_t size;
    uint32_t attributes;
    uint32_t create_time;
    uint32_t modify_time;
} fs_stat_t;

typedef struct
{
    uint32_t block_size;    //размер блока/кластера в байтах
    uint32_t total_blocks;  //всего блоков
    uint32_t free_blocks;   //свободных блоков
    uint32_t total_inodes;  //всего inode (0 если не применимо)
    uint32_t free_inodes;   //свободных inode (0 если не применимо)
} fs_statfs_t;

typedef struct
{
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
    int (*rename)(void *vol, const char *oldpath, const char *newpath);
    void *(*get_volume)(uint32_t id);

    volatile uint8_t *floppy_irq_fired_ptr;  //irq6 flag
    //void *dma_buffer;

    void (*relink_drive_id)(uint32_t index, uint32_t id);

    void *(*malloc_ptr)(uint32_t size);
    void  (*free_ptr)(void *ptr);

    //Указатель на запрет вызова shedule() из обработчика прерываний - нужно для
    //долгих операций с диском, чтобы по прерыванию 32 не вылетело и не испортило стек
    volatile uint8_t *schedule_lock_ptr;
    //Владение kernel_schedule_lock (см. isr.c / systemcall_spawn_process)
    //Драйвер не должен безусловно писать 1/0 в чужой флаг: systemcall_spawn_process
    //держит блокировку всё время спавна (защита CR3-окон). Обнуление её из драйвера
    //открывает окно вытеснения внутри спавна -> Page Fault в чужом адресном пространстве.
    //Драйвер становится владельцем только если флаг был 0 и снимает его только если
    //владелец — он сам.  we = KSL_ACQUIRE(p); ...операция...; KSL_RELEASE(p, we);
    #define KSL_ACQUIRE(p) __extension__ ({ volatile uint8_t *_ksl_p = (p); \
            uint8_t _ksl_we = 0; \
            if (_ksl_p && *_ksl_p == 0) { *_ksl_p = 1; _ksl_we = 1; } \
            _ksl_we; })
    #define KSL_RELEASE(p, we) do { volatile uint8_t *_ksl_p = (p); \
            if (_ksl_p && (we)) *_ksl_p = 0; } while (0)

    //Доступен ли block device interface в драйвере
    bool block_device_driver_enabled;
    const struct block_driver_api *block;   //NULL пока блочный драйвер не загружен

    //uint32_t debug1;
    uint32_t __ext2_dbg_hit_lba;

    const char *fs_name;                              //"ext2", "fat12"
    int (*statfs)(void *vol, fs_statfs_t *statfs); //NULL если не поддерживается

} fs_driver_api_t;


typedef struct
{
    //Оставлено для работы в USB-legacy, первое найденное устройство
    uint32_t vid;
    uint32_t pid;
    uint32_t dev_class;
    uint32_t sector_count;
    uint32_t sector_size;
    uint32_t block_id;
    uint32_t port;

    //Полный список найденных устройств: Драйвер заполняет, ядро читает после usb_start()
    //и кладёт каждую запись в usb_registry
    uint32_t device_count;
    usb_dev_record_t devices[USB_REGISTRY_MAX];

    uint32_t last_error;

    volatile uint8_t *schedule_lock_ptr;

    const struct block_driver_api *block; //ссылка на структуру блочного драйвера


    int (*usb_start)(void);

    uint32_t debug1;
    uint32_t debug2;
    uint32_t debug3;
    uint32_t debug4;
    uint32_t debug5;
    uint32_t debug6;
    uint32_t debug7;
    uint32_t debug8;
    uint32_t debug9;

    void (*vmm_map_pages_ptr)(uint32_t phys, uint32_t size);
    void (*vmm_map_pages_uncached_ptr)(uint32_t phys, uint32_t size);
    //DMA-совместимая память: физически непрерывные страницы
    //Возвращает виртуальный адрес (для доступа из драйвера) и записывает физический в *phys_out
    //(для TD/QH/буферов, куда контроллер обращается по физическому адресу)
    //На x86 identity-mapped регион: virt == phys, но драйвер не должен на это полагаться — только через этот API
    void *(*dma_alloc_ptr)(uint32_t size, uint32_t *phys_out);
    void  (*dma_free_ptr)(void *virt, uint32_t size);

    //Экспортные указатели ядра для внутренних нужд драйвера
    void *(*malloc_ptr)(uint32_t size);
    void  (*free_ptr)(void *ptr);
    void  (*yield_ptr)(void);       //Указатель на yield() ядра для асинхронного сна

    uint32_t (*get_ticks)(void);

    void (*load_screen_add_string)(const char *fmt, ...);
    void (*fake_load_screen_add_string)(const char *fmt, ...);


} usb_stack_driver_api_t;


typedef struct
{
    uint8_t scancode; //Физический скан-код клавиши (1..127)
    uint8_t type;     //1 - Нажата (Press), 0 - Отпущена (Release)
} kbd_event_t;




#endif
