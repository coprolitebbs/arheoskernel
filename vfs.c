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


vfs_mount_t vfs_mount_table[VFS_MAX_MOUNTS];

// Глобальная ядерная таблица открытых файлов
vfs_file_t vfs_file_table[VFS_MAX_OPEN_FILES];

// Глобальный безопасный буфер ядра для нормализации и отсечения префиксов путей VFS
uint8_t vfs_path_buffer[VFS_PATH_MAX] __attribute__((aligned(4)));

//Внутренние функции

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

    //Ищем свободный глобальный дескриптор в таблице ядра
    int vfs_fd = -1;
    for (int i = 3; i < VFS_MAX_OPEN_FILES; i++){
        if (!vfs_file_table[i].used) { vfs_fd = i; break; }
    }
    if (vfs_fd == -1) return VFS_STATUS_TOO_MANY;

    //Ищем точку монтирования (Longest Match)
    uint32_t match_len = 0;
    int slot = vfs_find_mount_slot(path, &match_len);
    if (slot == -1) return VFS_STATUS_IO_ERROR;

    vfs_mount_t *mnt = &vfs_mount_table[slot];

    //Извлекаем относительный путь через безопасную глобальную память 4КБ
    const char *relative_path = vfs_get_relative_path(path, match_len);

    //Связываем дескрипторы через вызов open конкретного драйвера ФС
    int driver_fd = mnt->driver->open(mnt->volume, relative_path, flags);
    if (driver_fd < 0) return driver_fd;

    //Фиксируем взаимосвязь дескрипторов в глобальной ядерной таблице
    vfs_file_table[vfs_fd].used = true;
    vfs_file_table[vfs_fd].mount = mnt;
    vfs_file_table[vfs_fd].driver_fd = driver_fd;
    vfs_file_table[vfs_fd].flags = flags;

    return vfs_fd;
}


int vfs_close(int vfs_fd){
    if (vfs_fd < 0 || vfs_fd >= VFS_MAX_OPEN_FILES) return VFS_STATUS_INVALID_HANDLE;
    vfs_file_t *file = &vfs_file_table[vfs_fd];
    if (!file->used) return VFS_STATUS_INVALID_HANDLE;

    //Освобождаем дескриптор внутри самого драйвера ФС
    int res = file->mount->driver->close(file->driver_fd);

    //Освобождаем глобальный дескриптор ядра
    if (res == FS_OK){
        file->used = false;
        file->mount = NULL;
        file->driver_fd = -1;
    }
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


int vfs_rmdir(const char *path){
    if (!path || path[0] != '/') return VFS_STATUS_IO_ERROR;

    uint32_t match_len = 0;
    int slot = vfs_find_mount_slot(path, &match_len);
    if (slot == -1) return VFS_STATUS_IO_ERROR;

    vfs_mount_t *mnt = &vfs_mount_table[slot];
    const char *relative_path = vfs_get_relative_path(path, match_len);

    return mnt->driver->rmdir(mnt->volume, relative_path);
}


int vfs_stat(const char *path, fs_stat_t *st){
    int res = VFS_STATUS_IO_ERROR;
    if (!path || path[0] != '/' || !st) return VFS_STATUS_IO_ERROR;

    uint32_t match_len = 0;

    int slot = vfs_find_mount_slot(path, &match_len);
    if (slot == -1) return VFS_STATUS_IO_ERROR;
    //kprintf("her\n");
    vfs_mount_t *mnt = &vfs_mount_table[slot];
    //const char *relative_path = vfs_get_relative_path(path, match_len);
    char local_rel_path[VFS_PATH_MAX];
    memset(local_rel_path, 0, VFS_PATH_MAX);
    const char *rel = vfs_get_relative_path(path, match_len);
    strcpy(local_rel_path, rel);

    //return mnt->driver->stat(mnt->volume, relative_path, st);
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
    res = vfs_mount("/", main_filesystem_fs_api, 0);

    //Резервирование файла 0 под stdin
    vfs_file_table[0].used = true;
    vfs_file_table[0].driver_fd = VFS_DRIVER_SIG; //Маркер устройства ввода
    vfs_file_table[0].flags = FS_OPEN_READ;
    vfs_file_table[0].mount = &vfs_mount_table[0];

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


