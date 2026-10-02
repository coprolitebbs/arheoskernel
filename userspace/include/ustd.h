#ifndef USTD_H
#define USTD_H

#include <stddef.h>
#include <stdint.h>

/* open flags */

#define FS_OPEN_READ      0x0001
#define FS_OPEN_WRITE     0x0002
#define FS_OPEN_CREATE    0x0004
#define FS_OPEN_APPEND    0x0008
#define FS_OPEN_TRUNCATE  0x0010

/* file attributes */

#define FS_ATTR_REGULAR   0x0000
#define FS_ATTR_DIRECTORY 0x0001
#define FS_ATTR_READONLY  0x0002
#define FS_ATTR_HIDDEN    0x0004
#define FS_ATTR_SYSTEM    0x0008
#define FS_ATTR_ARCHIVE   0x0010

#define MAX_PROCESS_FDS 16

#define STDIN  0
#define STDOUT 1
#define STDERR 2



//typedef __UINT32_TYPE__ uint32_t;
//typedef __UINT8_TYPE__  uint8_t;
//typedef __SIZE_TYPE__   size_t;

//Копия структуры dirent из вашего ядра для Ring 3
typedef struct
{
    char name[256];
    uint32_t id;
    uint32_t size;
    uint32_t attributes;
    uint32_t inode;
    uint32_t type;
} u_dirent_t;

typedef struct {
    uint32_t size;
    uint32_t attributes;
    uint32_t create_time;
    uint32_t modify_time;
} u_stat_t;


//extern int main(void);
extern int main(int argc, char *argv[]);

//Обертки над системными вызовами ядра
void u_draw_char(uint32_t x, uint32_t y, char c, uint32_t color);
void u_fill_screen(uint32_t color);
void yield(void);
void u_exit(void);
void* u_alloc_page(void);

//Наш самодельный printf
void u_printf(int x, int y, uint32_t color, const char *format, ...);

//Обертки для работы с файлами из Userspace
int open(const char *path, uint32_t flags);
int u_read(int fd, void *buf, uint32_t count);
int u_write(int fd, const void *buf, uint32_t count);
int mkdir(const char *path);
int u_readdir(int fd, u_dirent_t *entry);
int close(int fd);
int unlink(const char *path);
int rmdir(const char *path);
void clear_screen(void);
int u_spawn(const char *path, const char *cmdline);
int u_halt_os(void);
int u_wait_deity(int pid);
int chdir(const char *path);
int getcwd(char *buf, uint32_t max_len);
int stat(const char *path, u_stat_t *st);
int seek(int fd, uint32_t position);
int tell(int fd);
int set_wallpaper(const void *buffer, uint32_t size);
bool fileexists(const char *path);
bool directoryexists(const char *path);

void printf(const char *format, ...);

#endif
