#ifndef DRV_FORMAT_H
#define DRV_FORMAT_H

#include <stdint.h>
#include "../../include-kernel/bootinfo.h"

#define DRV_MAGIC "DRV1"

#define DRV_RELOC_ABS32 1
#define DRV_RELOC_PC32  2


//Общий ABI драйверов файловых систем

#define FS_DRIVER_ABI_VERSION 1

//ошибки

#define FS_OK                 0
#define FS_NOT_FOUND         -1
#define FS_ALREADY_EXISTS    -2
#define FS_NO_SPACE          -3
#define FS_INVALID_PATH      -4
#define FS_ACCESS_DENIED     -5
#define FS_IO_ERROR          -6
#define FS_CORRUPTED         -7
#define FS_NOT_SUPPORTED     -8
#define FS_INVALID_HANDLE    -9
#define FS_EOF              -10
#define FS_TOO_MANY         -11
#define FS_DEBUG_CATCH        1

//Флаги открытия

#define FS_OPEN_READ      0x0001
#define FS_OPEN_WRITE     0x0002
#define FS_OPEN_CREATE    0x0004
#define FS_OPEN_APPEND    0x0008
#define FS_OPEN_TRUNCATE  0x0010

//Файловые атрибуты

#define FS_ATTR_REGULAR   0x0000
#define FS_ATTR_DIRECTORY 0x0001
#define FS_ATTR_READONLY  0x0002
#define FS_ATTR_HIDDEN    0x0004
#define FS_ATTR_SYSTEM    0x0008
#define FS_ATTR_ARCHIVE   0x0010

#define KBD_DRIVER_NOT_READY   -2

typedef struct
{
    void (*put_pixel)(int,int,uint32_t);
    void (*fill_rect)(int,int,int,int,uint32_t);
    void (*clear)(uint32_t);
    void (*draw_char)(uint32_t,uint32_t,unsigned char,uint32_t);
    void (*draw_string)(uint32_t,uint32_t,const char*,uint32_t);
    void (*draw_hex)(uint32_t,int,int);

    void* (*malloc_ptr)(uint32_t size);
    void  (*free_ptr)(void *ptr);

    void (*set_fg)(uint32_t color);
    void (*set_bg)(uint32_t color);
    uint32_t (*get_fg)(void);
    uint32_t (*get_bg)(void);
    void (*scroll_up)(uint32_t start_y, uint32_t end_y, uint32_t line_height);

    void (*set_wallpaper)(const void *src_user_buffer, uint32_t size);
    void (*clear_tile)(int x, int y, uint32_t fallback_color);

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
