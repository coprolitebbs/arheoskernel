#ifndef DRV_FORMAT_H
#define DRV_FORMAT_H

#include <stdint.h>
#include "../../include-kernel/bootinfo.h"

#define DRV_MAGIC "DRV1"

#define DRV_RELOC_ABS32 1
#define DRV_RELOC_PC32  2

typedef struct
{
    void (*put_pixel)(int,int,uint32_t);
    void (*fill_rect)(int,int,int,int,uint32_t);
    void (*clear)(uint32_t);
    void (*draw_char)(uint32_t,uint32_t,unsigned char,uint32_t);
    void (*draw_string)(uint32_t,uint32_t,const char*,uint32_t);
    void (*draw_hex)(uint32_t,int,int);
} fb_driver_api_t;


/* ============================================================
 * Общий ABI драйверов файловых систем
 * ============================================================ */

#define FS_DRIVER_ABI_VERSION 1

/* ошибки */

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

/* open flags */

#define FS_OPEN_READ      0x0001
#define FS_OPEN_WRITE     0x0002
#define FS_OPEN_CREATE    0x0004
#define FS_OPEN_APPEND    0x0008
#define FS_OPEN_TRUNCATE  0x0010

/* file attributes */

#define FS_ATTR_DIRECTORY 0x0001
#define FS_ATTR_READONLY  0x0002
#define FS_ATTR_HIDDEN    0x0004
#define FS_ATTR_SYSTEM    0x0008
#define FS_ATTR_ARCHIVE   0x0010




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
    int (*mount)(uint32_t drive);
    int (*unmount)(void *volume);
    int (*lookup)(void *vol,const char *name,void *out);
    int (*open)(void *vol,const char *path,uint32_t flags);
    int (*close)(int fd);
    int (*read)(int fd,void *buffer,uint32_t size);
    int (*write)(int fd,const void *buffer,uint32_t size);
    int (*seek)(int fd,uint32_t position);
    int (*tell)(int fd);
    int (*readdir)(int dir,fs_dirent_t *entry);
    int (*stat)(const char *path,fs_stat_t *st);
    void *(*get_volume)(uint32_t id);

    uint32_t (*debug_ret_uint32)();
    uint32_t (*debug_get_fd_addr)(int fd);
    uint32_t (*debug_get_fd_used)(int fd);
    uint32_t (*debug_a)(void);
    uint32_t (*debug_b)(void);
    uint32_t (*debug_c)(void);
    uint32_t (*debug_array)(void);
    uint32_t (*debug_size)(void);
    uint32_t (*debug_addresses)(void);
} fs_driver_api_t;







#endif
