```c
#ifndef DRIVER_API_H
#define DRIVER_API_H

#include <stdint.h>


/*
 * ============================================================
 * Disk I/O
 * ============================================================
 *
 * Работа с физическими секторами.
 *
 * Драйвер файловой системы не обязан знать,
 * каким образом физически происходит чтение.
 *
 * Для FAT12 floppy это в конечном итоге:
 *
 *     BIOS INT 13h
 *
 * ============================================================
 */

typedef int (*disk_read_fn)(
    uint32_t drive,
    uint32_t lba,
    uint32_t count,
    void *buffer
);


typedef int (*disk_write_fn)(
    uint32_t drive,
    uint32_t lba,
    uint32_t count,
    const void *buffer
);


/*
 * ============================================================
 * Kernel services available to drivers
 * ============================================================
 */

typedef struct
{
    disk_read_fn  disk_read;
    disk_write_fn disk_write;

} driver_api_t;


#endif
```

