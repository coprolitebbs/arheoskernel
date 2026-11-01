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

#include <stdint.h>
#include "include-kernel/vfs.h"
#include "include-kernel/lib.h"
#include "include-kernel/bootinfo.h"
#include "include-kernel/kernel_heap.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/syscalls.h"
#include "include-kernel/kconsole.h"
#include "drivers/include_drivers/drv_format.h"
#include "drivers/fat12_driver/include/fat12_internal.h"
#include "drivers/ext2_driver/include/ext2_internal.h"




// Глобальная ядерная таблица открытых файлов
vfs_file_t vfs_file_table[VFS_MAX_OPEN_FILES];

// Глобальный безопасный буфер ядра для нормализации и отсечения префиксов путей VFS
uint8_t vfs_path_buffer[VFS_PATH_MAX] __attribute__((aligned(4)));

uint8_t local_rel_path[VFS_PATH_MAX] __attribute__((aligned(4)));

vfs_mount_t vfs_mount_table[VFS_MAX_MOUNTS];

//Внутренние функции

static void vfs_get_relative_path_safe(const char *path, uint32_t match_len, char *out_buf) {
    if (match_len == 1) {
        strncpy(out_buf, path, VFS_PATH_MAX - 1);
    } else {
        const char *cut_path = path + match_len;
        if (cut_path[0] == '\0') {
            out_buf[0] = '/';
            out_buf[1] = '\0';
        } else {
            strncpy(out_buf, cut_path, VFS_PATH_MAX - 1);
        }
    }
    out_buf[VFS_PATH_MAX - 1] = '\0';
}


//Помощник диспетчера, ищет лучшую точку монтирования для заданного пути
static int vfs_find_mount_slot(const char *path, uint32_t *out_match_len){
    if (!path) return -1;
    int best_slot = -1;
    uint32_t max_match_len = 0;
    bool root_found = false;
    int root_slot_idx = -1;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++){
        if (vfs_mount_table[i].used && vfs_mount_table[i].mount_point != NULL){
            uint32_t mnt_len = strlen(vfs_mount_table[i].mount_point);
            //Запоминаем, если нашли корневую ФС "/"
            if (mnt_len == 1 && vfs_mount_table[i].mount_point[0] == '/'){
                root_found = true;
                root_slot_idx = i;
                continue; //Корень обработаем в самом конце как дефолтный вариант
            }

            //Для вложенных папок (например, "/floppy") делаем строгую проверку префикса
            if (strncmp(path, vfs_mount_table[i].mount_point, mnt_len) == 0){
                // Защита границ токена: после префикса должен быть конец строки либо слэш
                if (path[mnt_len] == '\0' || path[mnt_len] == '/'){
                    if (mnt_len > max_match_len){
                        max_match_len = mnt_len;
                        best_slot = i;
                    }
                }
            }
        }
    }

    //Финальная диспетчеризация: если ни одна подпапка не подошла,
    //но у нас смонтирован корень "/", значит путь относится к корневой ФС
    if (best_slot == -1 && root_found){
        *out_match_len = 1;
        return root_slot_idx; // Возвращаем слот корня (0)
    }

    *out_match_len = max_match_len;
    return best_slot;
}


//Помощник диспетчера: вычисляет относительный путь для конкретного драйвера ФС
static const char* vfs_get_relative_path(const char *path, uint32_t match_len){
    // Полностью очищаем глобальный буфер перед использованием
    memset(vfs_path_buffer, 0, VFS_PATH_MAX);

    if (match_len == 1){
        //Если сработал корень "/", относительный путь равен оригинальному
        strncpy((char *)vfs_path_buffer, path, VFS_PATH_MAX - 1);
    } else {
        //Отсекаем префикс точки монтирования (например, "/floppy")
        const char *cut_path = path + match_len;
        if (cut_path[0] == '\0'){
            vfs_path_buffer[0] = '/';
        } else {
            strncpy((char *)vfs_path_buffer, cut_path, VFS_PATH_MAX - 1);
        }
    }

    vfs_path_buffer[VFS_PATH_MAX - 1] = '\0'; //Гарантируем закрытие строки
    return (const char *)vfs_path_buffer;    //Возвращаем указатель на глобальную память
}


//Внешние функции

int vfs_mount(const char *mount_point, fs_driver_api_t *driver, uint32_t drive_id){
    if (!mount_point || !driver) return VFS_STATUS_IO_ERROR;

    //Проверяем, не занята ли уже эта точка монтирования другим разделом
    for (int i = 0; i < VFS_MAX_MOUNTS; i++){
        if (vfs_mount_table[i].used && vfs_mount_table[i].mount_point != NULL){
            if (strcmp(vfs_mount_table[i].mount_point, mount_point) == 0){
                return VFS_STATUS_ALREADY_EXISTS; // Точка монтирования уже занята
            }
        }
    }

    //Ищем свободный слот в таблице монтирования ядра
    int slot = -1;
    for(int i = 0; i < VFS_MAX_MOUNTS; i++){
        if (!vfs_mount_table[i].used){
            slot = i;
            break;
        }
    }
    if (slot == -1) return VFS_STATUS_TOO_MANY;

    //Инициализируем (монтируем) физический диск через переданный драйвер ФС
    int r = driver->mount(drive_id);
    //draw_hex_dword(r, 30, 420);
    //for(;;);

    if (r != FS_OK) return r;

    //Выделяем память из кучи ядра через kmalloc под точную длину строки + терминальный ноль
    uint32_t path_len = strlen(mount_point);
    char *allocated_path = (char *)kmalloc(path_len + 1);
    if (!allocated_path){
        // Откатываем аппаратный mount драйвера, если в ядре закончилась память кучи
        driver->unmount(driver->get_volume(drive_id));
        return VFS_STATUS_IO_ERROR;
    }

    //Копируем строку в выделенный участок кучи
    strcpy(allocated_path, mount_point);

    //Фиксируем точку монтирования в таблице ядра
    vfs_mount_table[slot].used = true;
    vfs_mount_table[slot].driver = driver;
    vfs_mount_table[slot].drive_id = drive_id;
    vfs_mount_table[slot].mount_point = allocated_path; // Сохраняем указатель на кучу

    //Запрашиваем у драйвера указатель на его приватную структуру тома
    vfs_mount_table[slot].volume = driver->get_volume(drive_id);

    return VFS_STATUS_OK;
}


int vfs_unmount(uint32_t drive_id){
    int slot = -1;
    for(int i = 0; i < VFS_MAX_MOUNTS; i++){
        if(vfs_mount_table[i].used && vfs_mount_table[i].drive_id == drive_id) { slot = i; break; }
    }
    if(slot == -1) return VFS_STATUS_IO_ERROR;
    int res = vfs_mount_table[slot].driver->unmount(vfs_mount_table[slot].volume);
    if(res == FS_OK){
            kfree(vfs_mount_table[slot].mount_point);
            vfs_mount_table[slot].mount_point = NULL;
            vfs_mount_table[slot].used = false;
    }

    return res;
}


int vfs_open(const char *path, uint32_t flags){
    if (!path || path[0] != '/') return VFS_STATUS_IO_ERROR;

    if (!(flags & (FS_OPEN_READ | FS_OPEN_WRITE))) return FS_INVALID_PATH;

    int vfs_fd = -1;
    for (int i = 3; i < VFS_MAX_OPEN_FILES; i++) {
        if (!vfs_file_table[i].used) { vfs_fd = i; break; }
    }
    if (vfs_fd == -1) return VFS_STATUS_TOO_MANY;

    uint32_t match_len = 0;
    int slot = vfs_find_mount_slot(path, &match_len);
    if (slot == -1) return VFS_STATUS_IO_ERROR;

    vfs_mount_t *mnt = &vfs_mount_table[slot];

    //char local_rel_path[VFS_PATH_MAX]={0};
    //char *local_rel_path = (char *)kmalloc(VFS_PATH_MAX);
    memset(local_rel_path, 0, VFS_PATH_MAX);
    vfs_get_relative_path_safe(path, match_len, local_rel_path);
    //uint32_t safe_flags = flags;
    //if (safe_flags & 0x0004) {
        //safe_flags &= ~0x0001;
    //}

    //int driver_fd = mnt->driver->open(mnt->volume, local_rel_path, safe_flags);
    //ext2_volume_t *drv = (ext2_volume_t *)mnt->volume;
    //kprintf("---fopen=%x\n",drv->drive);
    int driver_fd = mnt->driver->open(mnt->volume, local_rel_path, flags);
    //kprintf("---after=%x\n",drv->drive);
    //kfree(local_rel_path);
    //if (driver_fd < 0) return driver_fd;
    if (driver_fd < 0) {
        //kprintf("[VFS] open failed: path='%s', err=%d\n", path, driver_fd);
        return driver_fd;
    }

    vfs_file_t *vfs = &vfs_file_table[vfs_fd];
    vfs->flags = flags;
    vfs->used = true;
    vfs->mount = mnt;
    vfs->driver_fd = driver_fd;

    return vfs_fd;
}




int vfs_close(int vfs_fd){
    if (vfs_fd < 0 || vfs_fd >= VFS_MAX_OPEN_FILES) return VFS_STATUS_INVALID_HANDLE;
    vfs_file_t *file = &vfs_file_table[vfs_fd];
    if (!file->used) return VFS_STATUS_INVALID_HANDLE;

    //Освобождаем дескриптор внутри самого драйвера ФС
    int res = file->mount->driver->close(file->driver_fd);

    //Освобождаем глобальный дескриптор ядра
    //if (res == FS_OK){
    file->used = false;
    file->mount = NULL;
    file->driver_fd = -1;
    //}
    return res;
}

int vfs_read(int vfs_fd, void *buffer, uint32_t size){
    if (vfs_fd < 0 || vfs_fd >= VFS_MAX_OPEN_FILES) return VFS_STATUS_INVALID_HANDLE;
    vfs_file_t *file = &vfs_file_table[vfs_fd];
    if (!file->used) return VFS_STATUS_INVALID_HANDLE;
    //Перенаправляем операцию чтения в нужный драйвер по внутреннему дескриптору
    int ret = file->mount->driver->read(file->driver_fd, buffer, size);
    //draw_hex_dword(ret, 120, 450);
    return ret;
}

int vfs_write(int vfs_fd, const void *buffer, uint32_t size){
    if (vfs_fd < 0 || vfs_fd >= VFS_MAX_OPEN_FILES) return VFS_STATUS_INVALID_HANDLE;
    vfs_file_t *file = &vfs_file_table[vfs_fd];
    if (!file->used) return VFS_STATUS_INVALID_HANDLE;
    //Перенаправляем операцию записи
    return file->mount->driver->write(file->driver_fd, buffer, size);
}

int vfs_seek(int vfs_fd, uint32_t position){
    if (vfs_fd < 0 || vfs_fd >= VFS_MAX_OPEN_FILES) return VFS_STATUS_INVALID_HANDLE;
    vfs_file_t *file = &vfs_file_table[vfs_fd];
    if (!file->used) return VFS_STATUS_INVALID_HANDLE;
    return file->mount->driver->seek(file->driver_fd, position);
}

int vfs_tell(int vfs_fd){
    if (vfs_fd < 0 || vfs_fd >= VFS_MAX_OPEN_FILES) return VFS_STATUS_INVALID_HANDLE;
    vfs_file_t *file = &vfs_file_table[vfs_fd];
    if (!file->used) return VFS_STATUS_INVALID_HANDLE;
    return file->mount->driver->tell(file->driver_fd);
}

int vfs_readdir(int vfs_fd, fs_dirent_t *entry){
    if (vfs_fd < 0 || vfs_fd >= VFS_MAX_OPEN_FILES) return VFS_STATUS_INVALID_HANDLE;
    vfs_file_t *file = &vfs_file_table[vfs_fd];
    if (!file->used) return VFS_STATUS_INVALID_HANDLE;
    return file->mount->driver->readdir(file->driver_fd, entry);
}

int vfs_touch(const char *path){
    if (!path || path[0] != '/') return VFS_STATUS_IO_ERROR;

    uint32_t match_len = 0;
    int slot = vfs_find_mount_slot(path, &match_len);
    if (slot == -1) return VFS_STATUS_IO_ERROR;

    vfs_mount_t *mnt = &vfs_mount_table[slot];
    const char *relative_path = vfs_get_relative_path(path, match_len);

    return mnt->driver->touch(mnt->volume, relative_path);
}


int vfs_mkdir(const char *path){
    if (!path || path[0] != '/') return VFS_STATUS_IO_ERROR;

    uint32_t match_len = 0;
    int slot = vfs_find_mount_slot(path, &match_len);
    if (slot == -1) return VFS_STATUS_IO_ERROR;

    vfs_mount_t *mnt = &vfs_mount_table[slot];
    const char *relative_path = vfs_get_relative_path(path, match_len);

    return mnt->driver->mkdir(mnt->volume, relative_path);
}

int vfs_unlink(const char *path){
    if (!path || path[0] != '/') return VFS_STATUS_IO_ERROR;

    uint32_t match_len = 0;
    int slot = vfs_find_mount_slot(path, &match_len);
    if (slot == -1) return VFS_STATUS_IO_ERROR;

    vfs_mount_t *mnt = &vfs_mount_table[slot];
    const char *relative_path = vfs_get_relative_path(path, match_len);

    return mnt->driver->unlink(mnt->volume, relative_path);
}


/*
int vfs_rename(const char *oldpath, const char *newpath){
    if (!oldpath || oldpath[0] != '/' || !newpath || newpath[0] != '/')
        return VFS_STATUS_IO_ERROR;

    if (strcmp(oldpath, newpath) == 0) return FS_OK;

    uint32_t old_ml = 0, new_ml = 0;
    int old_slot = vfs_find_mount_slot(oldpath, &old_ml);
    int new_slot = vfs_find_mount_slot(newpath, &new_ml);
    if (old_slot == -1 || new_slot == -1) return VFS_STATUS_IO_ERROR;
    if (old_slot != new_slot)             return FS_NOT_SUPPORTED;

    vfs_mount_t *mnt = &vfs_mount_table[old_slot];
    char old_rel[VFS_PATH_MAX], new_rel[VFS_PATH_MAX];
    vfs_get_relative_path_safe(oldpath, old_ml, old_rel);
    vfs_get_relative_path_safe(newpath, new_ml, new_rel);

    int sfd = vfs_open(oldpath, FS_OPEN_READ);
    if (sfd < 0) return sfd;
    int dfd = vfs_open(newpath, FS_OPEN_WRITE | FS_OPEN_CREATE | FS_OPEN_TRUNCATE);
    if (dfd < 0){ vfs_close(sfd); return dfd; }

    uint8_t *buf = (uint8_t *)kmalloc(4096);
    if (!buf){ vfs_close(sfd); vfs_close(dfd); return VFS_STATUS_IO_ERROR; }

    int res = FS_OK;
    int r;
    while ((r = vfs_read(sfd, buf, 4096)) > 0){
        if (vfs_write(dfd, buf, r) != r){ res = FS_IO_ERROR; break; }
    }
    if (r < 0) res = FS_IO_ERROR;
    kfree(buf);
    vfs_close(sfd);
    vfs_close(dfd);

    if (res == FS_OK){
        res = vfs_unlink(oldpath);
    }
    return res;
}
*/

int vfs_rename(const char *oldpath, const char *newpath){
    if (!oldpath || oldpath[0] != '/' || !newpath || newpath[0] != '/')
        return VFS_STATUS_IO_ERROR;

    if (strcmp(oldpath, newpath) == 0) return FS_OK;   /* no-op */

    uint32_t old_ml = 0, new_ml = 0;
    int old_slot = vfs_find_mount_slot(oldpath, &old_ml);
    int new_slot = vfs_find_mount_slot(newpath, &new_ml);
    if (old_slot == -1 || new_slot == -1) return VFS_STATUS_IO_ERROR;
    if (old_slot != new_slot)             return FS_NOT_SUPPORTED;

    vfs_mount_t *mnt = &vfs_mount_table[old_slot];
    char old_rel[VFS_PATH_MAX], new_rel[VFS_PATH_MAX];
    vfs_get_relative_path_safe(oldpath, old_ml, old_rel);
    vfs_get_relative_path_safe(newpath, new_ml, new_rel);

    /* 1. Пробуем драйверный fast-path: приёмник ещё не существует */
    if (mnt->driver->rename){
        int r = mnt->driver->rename(mnt->volume, old_rel, new_rel);
        if (r == FS_OK) return FS_OK;
        if (r != FS_NOT_SUPPORTED) return r;
    }

    /* 2. Если приёмник существует — unlink его (это быстро) и повторить rename.
       Иначе копирование через буфер (медленно). */
    fs_stat_t st;
    bool dest_exists = (vfs_stat(newpath, &st) == FS_OK);

    if (dest_exists){
        if (st.attributes & FS_ATTR_DIRECTORY) return FS_NOT_SUPPORTED;
        if (!mnt->driver->rename)              return FS_NOT_SUPPORTED;

        int ur = mnt->driver->unlink(mnt->volume, new_rel);
        if (ur != FS_OK) return ur;            /* откат: old цел, new цел */

        int r = mnt->driver->rename(mnt->volume, old_rel, new_rel);
        if (r == FS_OK) return FS_OK;

        /* приёмник удалён, но rename не прошёл — данные целы, old на месте.
           пользователь может повторить вручную или восстановить из бэкапа */
        return r;
    }

    /* 3. Последний резерв: copy+unlink (driver не умеет / нет rename) */
    int sfd = vfs_open(oldpath, FS_OPEN_READ);
    if (sfd < 0) return sfd;
    int dfd = vfs_open(newpath, FS_OPEN_WRITE | FS_OPEN_CREATE | FS_OPEN_TRUNCATE);
    if (dfd < 0){ vfs_close(sfd); return dfd; }

    uint8_t *buf = (uint8_t *)kmalloc(4096);
    if (!buf){ vfs_close(sfd); vfs_close(dfd); return VFS_STATUS_IO_ERROR; }

    int res = FS_OK;
    int r;
    while ((r = vfs_read(sfd, buf, 4096)) > 0){
        if (vfs_write(dfd, buf, r) != r){ res = FS_IO_ERROR; break; }
    }
    if (r < 0) res = FS_IO_ERROR;
    kfree(buf);
    vfs_close(sfd);
    vfs_close(dfd);

    if (res == FS_OK) res = vfs_unlink(oldpath);
    return res;
}


int vfs_rmdir(const char *path){
    if (!path || path[0] != '/') return VFS_STATUS_IO_ERROR;

    uint32_t match_len = 0;
    int slot = vfs_find_mount_slot(path, &match_len);
    if (slot == -1) return VFS_STATUS_IO_ERROR;

    vfs_mount_t *mnt = &vfs_mount_table[slot];
    const char *relative_path = vfs_get_relative_path(path, match_len);

    return mnt->driver->rmdir(mnt->volume, relative_path);
}


int vfs_stat(const char *path, fs_stat_t *st) {
    if (!path || path[0] != '/' || !st) return VFS_STATUS_IO_ERROR;

    uint32_t match_len = 0;
    int slot = vfs_find_mount_slot(path, &match_len);
    if (slot == -1) return VFS_STATUS_IO_ERROR;

    vfs_mount_t *mnt = &vfs_mount_table[slot];

    //char local_rel_path[VFS_PATH_MAX];
    memset(local_rel_path, 0, VFS_PATH_MAX);
    vfs_get_relative_path_safe(path, match_len, local_rel_path);

    return mnt->driver->stat(mnt->volume, local_rel_path, st);
}




int vfs_init_system(void){
    memset(vfs_mount_table, 0, sizeof(vfs_mount_table));

    uint8_t fs_ind = 255;
    uint32_t res = VFS_STATUS_NOT_SUPPORTED;
    switch(boot_info->fs->type){
        case FS_FAT12:
            fs_ind = 0;
            break;
        case FS_EXT2:
            fs_ind = 1;
            break;
    }

    if(fs_ind == 255) return VFS_STATUS_NOT_SUPPORTED;
    if(!fs_apis[fs_ind].api) return VFS_STATUS_INVALID_HANDLE;
    main_filesystem_fs_api = fs_apis[fs_ind].api;

    //монтируем корневую фс
    //res = vfs_mount("/", main_filesystem_fs_api, 0);

    if (blockdevice_driver && blockdevice_driver->enabled &&
        blockdevice_driver->enum_devices){

        //Приоритетная подсказка от BIOS (может быть неверной)
        uint32_t boot_id = 0;
        if (blockdevice_driver->resolve_boot_drive){
            blockdevice_driver->resolve_boot_drive(boot_info->boot_drive, &boot_id);
        }

        block_info_t bi;
        if (blockdevice_driver->get_info(boot_id, &bi) == FS_OK){
            res = vfs_mount("/", main_filesystem_fs_api, boot_id);
            if (res == VFS_STATUS_OK) goto mount_ok;
        }

        //перебрать все устройства
        for (int i = 0; i < 16; i++){
            if (blockdevice_driver->enum_devices(i, &bi) != FS_OK) break;
            if ((uint32_t)i == boot_id) continue;

            res = vfs_mount("/", main_filesystem_fs_api, i);
            if (res == VFS_STATUS_OK){
                //kprintf("[VFS] Root mounted from block id %d\n", i);
                goto mount_ok;
            }
        }
    } else {
        //legacy PIO-путь без block API (старая проверка)
        res = vfs_mount("/", main_filesystem_fs_api, 0);
    }

mount_ok:
    //Резервирование под stdin
    vfs_file_table[0].used = true;
    vfs_file_table[0].driver_fd = VFS_DRIVER_SIG; //Маркер устройства ввода
    vfs_file_table[0].flags = FS_OPEN_READ;
    vfs_file_table[0].mount = &vfs_mount_table[0];
    //kprintf("ok\n");
    return res;
}



int vfs_shutdown_system(void){
    //Проверяем, есть ли в системе открытые файлы (кроме зарезервированного STDIN = 0)
    //Таблица vfs_file_table хранит состояние всех дескрипторов ядра
    for (int i = 1; i < VFS_MAX_OPEN_FILES; i++) {
        if (vfs_file_table[i].used) {
            return HALT_ERR_OPEN_FILES;
        }
    }
    //Размонтируем все не-корневые файловые системы
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_mount_table[i].used && vfs_mount_table[i].mount_point != NULL) {
            //Пропускаем корень, его закроем в самом конце
            if (strcmp(vfs_mount_table[i].mount_point, "/") == 0) {
                continue;
            }
            //Дёргаем unmount конкретного драйвера (например, ext2_unmount проверяет свои внутренние файлы)
            int r = vfs_mount_table[i].driver->unmount(vfs_mount_table[i].volume);
            if (r != FS_OK) {
                return HALT_ERR_VFS_DENIED;
            }
            kfree(vfs_mount_table[i].mount_point);
            vfs_mount_table[i].mount_point = NULL;
            vfs_mount_table[i].used = false;
        }
    }
    //Размонтируем корневой раздел "/"
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_mount_table[i].used && vfs_mount_table[i].mount_point != NULL) {
            if (strcmp(vfs_mount_table[i].mount_point, "/") == 0) {
                int r = vfs_mount_table[i].driver->unmount(vfs_mount_table[i].volume);
                if (r != FS_OK) {
                    return HALT_ERR_VFS_DENIED;
                }

                kfree(vfs_mount_table[i].mount_point);
                vfs_mount_table[i].mount_point = NULL;
                vfs_mount_table[i].used = false;
                break;
            }
        }
    }
    return HALT_SUCCESS;
}



void vfs_resolve_relative_path(const char *user_path, char *out_absolute_path){
    //Начисто зануляем результирующий буфер (размером 4096 байт)
    memset(out_absolute_path, 0, VFS_PATH_MAX);
    //Временный рабочий буфер кучи ядра для первичного склеивания путей
    char *scratch_buf = (char *)kmalloc(VFS_PATH_MAX);
    if (!scratch_buf){
        out_absolute_path[0] = '/';
        out_absolute_path[1] = '\0';
        return;
    }
    memset(scratch_buf, 0, VFS_PATH_MAX);

    //Первичная сборка сырой строки (CWD + user_path)
    if (user_path[0] == '/'){
        //Путь уже абсолютный, просто копируем в scratch_buf
        for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path[i] != '\0'; i++){
            scratch_buf[i] = user_path[i];
        }
    } else {
        //Путь относительный — приклеиваем CWD текущего процесса
        int idx = 0;
        for (int i = 0; current_task->cwd[i] != '\0' && idx < (VFS_PATH_MAX - 2); i++){
            scratch_buf[idx++] = current_task->cwd[i];
        }
        if (idx > 1 && scratch_buf[idx - 1] != '/'){
            scratch_buf[idx++] = '/';
        }
        for (int i = 0; user_path[i] != '\0' && idx < (VFS_PATH_MAX - 1); i++){
            scratch_buf[idx++] = user_path[i];
        }
        scratch_buf[idx] = '\0';
    }

    //Посимвольная POSIX-канонизация без strtok и без висячих указателей
    int src_idx = 0;
    int dst_idx = 0;
    out_absolute_path[dst_idx++] = '/'; //Начинаем всегда строго с корня ФС

    while (scratch_buf[src_idx] != '\0' && dst_idx < (VFS_PATH_MAX - 1)){
        //Пропускаем дублирующиеся слэши (например, "bin//sekhmet")
        if (scratch_buf[src_idx] == '/') {
            src_idx++;
            continue;
        }
        //Вычленяем следующий изолированный компонент пути во временный буфер токена
        char token_name[256];
        int token_len = 0;
        while (scratch_buf[src_idx] != '/' && scratch_buf[src_idx] != '\0' && token_len < 255){
            token_name[token_len++] = scratch_buf[src_idx++];
        }
        token_name[token_len] = '\0';
        //Анализируем выделенный компонент по значению
        if (strcmp(token_name, ".") == 0 || token_len == 0){
            //Одиночная точка "." — текущий каталог, просто пропускаем
            continue;
        }

        if (strcmp(token_name, "..") == 0){
            //Две точки ".." — откатываемся на один каталог назад в out_absolute_path
            if (dst_idx > 1) {
                dst_idx--; //Шагаем назад за закрывающий слэш предыдущей папки
                while (dst_idx > 0 && out_absolute_path[dst_idx] != '/'){
                    dst_idx--;
                }
                if (dst_idx == 0){
                    dst_idx = 1; //Удерживаем корень ФС "/", выше него подняться нельзя
                }
            }
            continue;
        }

        //Обычное имя папки/файла — дописываем в результирующий буфер out_absolute_path
        if (dst_idx > 1 && out_absolute_path[dst_idx - 1] != '/'){
            out_absolute_path[dst_idx++] = '/';
        }

        for (int i = 0; token_name[i] != '\0' && dst_idx < (VFS_PATH_MAX - 1); i++){
            out_absolute_path[dst_idx++] = token_name[i];
        }
    }
    //Закрываем результирующую строку нулем
    out_absolute_path[dst_idx] = '\0';
    //Страховка: если в процессе удаления точек путь полностью обнулился, оставляем "/"
    if (dst_idx == 0){
        out_absolute_path[0] = '/';
        out_absolute_path[1] = '\0';
    }
    kfree(scratch_buf);
}


