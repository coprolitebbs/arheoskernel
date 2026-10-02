#ifndef VFS_H
#define VFS_H

#include <stdint.h>
//#include "vfs.h"
#include "../drivers/include_drivers/drv_format.h"
#include "use_drivers.h"

#define VFS_MAX_MOUNTS     64
#define VFS_MAX_OPEN_FILES 32
#define VFS_PATH_MAX 4096

//VFS Statuses
#define VFS_STATUS_OK                   0
#define VFS_STATUS_ALREADY_EXISTS      -2
#define VFS_STATUS_ACCESS_DENIED       -5
#define VFS_STATUS_IO_ERROR            -6
#define VFS_STATUS_CORRUPTED           -7
#define VFS_STATUS_NOT_SUPPORTED       -8
#define VFS_STATUS_INVALID_HANDLE      -9
#define VFS_STATUS_TOO_MANY            -11

#define VFS_DRIVER_SIG                 -100

typedef struct{
    char            *mount_point;//[4096];
    fs_driver_api_t *driver;          // Указатель на структуру API конкретного драйвера
    void            *volume;          // Указатель на структуру физического тома (fat12_fs_t или ext2_volume_t)
    uint32_t         drive_id;        // Номер физического диска (0, 1...)
    bool             used;
} vfs_mount_t;


typedef struct{
    vfs_mount_t *mount;    // Указатель на точку монтирования (какая ФС и какой том)
    int          driver_fd;// Реальный внутренний дескриптор файла внутри драйвера ФС
    uint32_t     flags;    // Флаги доступа (чтение/запись)
    bool         used;
} vfs_file_t;

extern vfs_file_t vfs_file_table[VFS_MAX_OPEN_FILES];

int vfs_mount(const char *mount_point, fs_driver_api_t *driver, uint32_t drive_id);
int vfs_unmount(uint32_t drive_id);
int vfs_open(const char *path, uint32_t flags);
int vfs_close(int vfs_fd);
int vfs_read(int vfs_fd, void *buffer, uint32_t size);
int vfs_write(int vfs_fd, const void *buffer, uint32_t size);
int vfs_seek(int vfs_fd, uint32_t position);
int vfs_tell(int vfs_fd);
int vfs_readdir(int vfs_fd, fs_dirent_t *entry);
int vfs_touch(const char *path);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_stat(const char *path, fs_stat_t *st);
void vfs_resolve_relative_path(const char *user_path, char *out_absolute_path);

int vfs_init_system(void);
int vfs_shutdown_system(void);

#endif
