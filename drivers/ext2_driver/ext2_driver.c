#include "include/ext2_driver.h"
#include "include/ext2_internal.h"
#include "include/ext2_disk.h"
#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

ext2_volume_t ext2_volumes[EXT2_MAX_VOLUMES];
ext2_volume_t *ext2_volumes_ptr;

ext2_file_t ext2_files[EXT2_MAX_OPEN_FILES];
ext2_file_t *ext2_files_ptr;

uint8_t bg_desc_tables[EXT2_MAX_VOLUMES * 512] __attribute__((aligned(4)));
uint8_t *bg_desc_tables_ptr;

uint8_t ext2_path_buffer[EXT2_PATH_MAX] __attribute__((aligned(4)));
uint8_t *ext2_path_buffer_ptr;

fs_driver_api_t api;
fs_driver_api_t *api_ptr;

//  ----- внутренние функции  -----



//  -----  внешние функции  -----

int ext2_mount(uint32_t drive){
    for(int i = 0; i < EXT2_MAX_VOLUMES; ++i){
        ext2_volume_t *vol = &ext2_volumes_ptr[i];
        if(vol->drive == drive && vol->mounted == 1) return FS_OK;
    }
    int fnd = -1;
    for(int i = 0; i < EXT2_MAX_VOLUMES; ++i){
        ext2_volume_t *vol = &ext2_volumes_ptr[i];
        if(vol->mounted == 0) {
                fnd = i;
                break;
        }
    }
    if(fnd == -1) return FS_TOO_MANY;

    ext2_volume_t *vol = &ext2_volumes_ptr[fnd];
    memset(vol, 0, sizeof(ext2_volume_t));
    vol->drive = drive;

    uint8_t sb_buf[1024]; // Буфер на 1024 байта (2 сектора)
    bool found_fs = false;
    uint32_t detected_start_lba = 0;

    for (uint32_t scan_lba = 0; scan_lba <= 64; scan_lba++) {
        // Суперблок EXT2 всегда смещен ровно на 1024 байта (2 сектора) от начала ФС
        uint32_t target_lba = scan_lba + 2;

        // Читаем 2 сектора суперблока независимой функцией ATA диска
        int r = ext2_disk_read_sectors(target_lba, 2, sb_buf);
        if (r != FS_OK) continue;

        ext2_superblock_t *test_sb = (ext2_superblock_t *)sb_buf;

        // Проверяем магическое число EXT2 (0xEF53)
        if (test_sb->s_magic == EXT2_MAGIC) {
            found_fs = true;
            detected_start_lba = scan_lba;
            // Копируем найденный суперблок в дескриптор тома
            memcpy(&vol->sb, sb_buf, sizeof(ext2_superblock_t));
            break;
        }
    }

    if (!found_fs) {
        return FS_CORRUPTED; // Файловая система EXT2 не найдена на диске
    }

    // Фиксируем динамически вычисленный старт раздела
    vol->partition_start_lba = detected_start_lba;

    // Вычисляем размер логического блока ФС
    vol->block_size = 1024 << vol->sb.s_log_block_size;
    vol->sectors_per_block = vol->block_size / 512;

    // Извлекаем размер структуры инода
    if (vol->sb.s_rev_level == 0) {
        vol->inode_size = 128;
    } else {
        vol->inode_size = vol->sb.s_inode_size;
    }
    // Вычисляем общее количество групп блоков
    vol->groups_count = (vol->sb.s_blocks_count + vol->sb.s_blocks_per_group - 1) / vol->sb.s_blocks_per_group;
    //Считываем таблицу дескрипторов групп блоков (BGDT).
    //При размере блока 1024 байта суперблок занимает Блок 1,
    //а таблица дескрипторов групп начинается ровно с Блока 2 ФС.
    uint32_t bgdt_block = (vol->block_size == 1024) ? 2 : 1;
    uint32_t bgdt_lba = vol->partition_start_lba + (bgdt_block * vol->sectors_per_block);

    uint32_t bgdt_size_bytes = vol->groups_count * sizeof(ext2_bg_desc_t);
    uint32_t bgdt_sectors = (bgdt_size_bytes + 512 - 1) / 512;

    vol->bg_desc_table = bg_desc_tables_ptr + (drive * 512);

    if (bgdt_size_bytes > /*sizeof(vol->bg_desc_table)*/512) {
        return FS_NOT_SUPPORTED;
    }

    // Читаем дескрипторы групп блоков с жесткого диска
    int r = ext2_disk_read_sectors(bgdt_lba, bgdt_sectors, vol->bg_desc_table);
    if (r != FS_OK) return FS_IO_ERROR; /* */

    ext2_bg_desc_t *check_bg = (ext2_bg_desc_t *)vol->bg_desc_table;
    if (check_bg->bg_inode_bitmap == 0) {
        // Контроллер диска вернул нули вместо реальных данных таблицы дескрипторов ФС
        return FS_CORRUPTED;
    }

    vol->mounted = true;
    return FS_OK; // EXT2 успешно примонтирован и полностью изолирован
}


int ext2_unmount(void *volume){
    if (!volume) return FS_IO_ERROR;
    ext2_volume_t *vol = (ext2_volume_t *)volume;
    if (!vol->mounted) return FS_IO_ERROR;
    //Проверяем открытые файлы/каталоги этого тома
    for (int fd = 0; fd < EXT2_MAX_OPEN_FILES; fd++){
        ext2_file_t *file = &ext2_files_ptr[fd];
        if (!file->used) continue;
        if (file->vol == vol) return FS_ACCESS_DENIED;
    }
    vol->mounted = 0;
    return FS_OK;
}


void *ext2_get_volume(uint32_t id){
    if(id >= EXT2_MAX_VOLUMES) return 0;
    for(int i = 0; i < EXT2_MAX_VOLUMES; ++i){
        ext2_volume_t *vol = &ext2_volumes_ptr[i];
        if(vol->drive == id && vol->mounted == 1) return vol;
    }
    return 0;
}

int ext2_lookup_in_dir(ext2_volume_t *vol, uint32_t dir_inode_no, const char *name, void *out_ptr, uint32_t *out_entry_id){
    if (!vol || !name || !out_ptr || !out_entry_id) return FS_IO_ERROR;
    if (!vol->mounted) return FS_IO_ERROR;

    // Защита от ведущих слэшей во внутреннем токене
    if (name[0] == '/') name++;
    uint32_t name_len = strlen(name);
    if (name_len == 0) return FS_INVALID_PATH;

    // Шаг 1, читаем инод заданного каталога (вместо жесткого EXT2_ROOT_INO)
    ext2_inode_t dir_inode;
    int r = ext2_read_inode(vol, dir_inode_no, &dir_inode);
    if (r != FS_OK) return r;

    // Проверяем, что этот инод действительно является каталогом (маска 0x4000)
    if ((dir_inode.i_mode & 0x4000) == 0) return FS_INVALID_PATH;

    uint8_t dir_buf[4096] __attribute__((aligned(4)));
    uint8_t *dir_buf_ptr = dir_buf;

    // Шаг 2, сканируем прямые блоки данных каталога
    for (int b = 0; b < 12; b++) {
        uint32_t block_no = dir_inode.i_block[b];
        if (block_no == 0) break; // Списка блоков больше нет

        // Читаем блок данных каталога
        r = ext2_read_block(vol, block_no, dir_buf_ptr);
        if (r != FS_OK) return r;

        // Парсим записи каталога
        uint32_t offset = 0;
        while (offset < vol->block_size) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(dir_buf_ptr + offset);

            // Защита от зацикливания. Какую только бодягу не встретишь в реале, даже, когда индексы нодов могут цикл образовывать
            if (entry->rec_len == 0) break;

            uint32_t real_name_len = entry->name_len & 0xFF;

            // Если инод не равен 0, запись валидна
            if (entry->inode != 0 && real_name_len == name_len) {
                int match = 1;
                for (uint32_t j = 0; j < name_len; j++) {
                    if (entry->name[j] != name[j]) {
                        match = 0;
                        break;
                    }
                }

                if (match) {
                    ext2_inode_t file_inode;
                    r = ext2_read_inode(vol, entry->inode, &file_inode);
                    if (r != FS_OK) return r;

                    memcpy(out_ptr, &file_inode, sizeof(ext2_inode_t));
                    *out_entry_id = entry->inode;
                    return FS_OK;
                }
            }

            // Переходим к следующей записи
            offset += entry->rec_len;
        }
    }

    return FS_NOT_FOUND;
}



int ext2_lookup(void *vol_ptr, const char *name, void *out_ptr, uint32_t *out_entry_id){
    if (!vol_ptr || !name || !out_ptr || !out_entry_id) return FS_IO_ERROR;
    ext2_volume_t *vol = (ext2_volume_t *)vol_ptr;
    // Перенаправляем поиск в универсальную функцию, стартуя с КОРНЯ (EXT2_ROOT_INO)
    return ext2_lookup_in_dir(vol, EXT2_ROOT_INO, name, out_ptr, out_entry_id);
}




int ext2_find_parent_inode(ext2_volume_t *vol, const char *path, uint32_t *out_parent_inode_no, char *out_child_name){
    if (!vol || !path || !out_parent_inode_no || !out_child_name) return FS_IO_ERROR;
    uint32_t current_dir_inode = EXT2_ROOT_INO; // Начинаем всегда с корня ФС

    //Безопасно копируем длинный путь в глобальный буфер драйвера (до 4095 байт)
    strncpy((char *)ext2_path_buffer_ptr, path, EXT2_PATH_MAX - 1);
    ext2_path_buffer_ptr[EXT2_PATH_MAX - 1] = '\0'; // Жесткий терминальный нуль

    // Убираем ведущий слэш для корректной токенизации
    char *p = (char *)ext2_path_buffer_ptr;
    if (*p == '/') p++;

    // Если передан чистый корень "/", то токенизировать нечего
    if (*p == '\0') {
        return FS_INVALID_PATH;
    }

    char *token = strtok(p, "/");
    char *next_token = strtok(NULL, "/");

    if (token == NULL) {
        return FS_INVALID_PATH;
    }

    // Итерируемся по элементам пути, пока не дойдем до последнего имени файла
    while (next_token != NULL) {
        ext2_inode_t dir_inode;
        uint32_t found_inode_no = 0;

        // Ищем подпапку 'token' внутри текущего 'current_dir_inode' каталога
        int r = ext2_lookup_in_dir(vol, current_dir_inode, token, &dir_inode, &found_inode_no);
        if (r != FS_OK) return FS_NOT_FOUND; // Промежуточный каталог отсутствует

        // Убеждаемся, что найденный дескриптор — действительно каталог
        if ((dir_inode.i_mode & 0x4000) == 0) return FS_INVALID_PATH;

        current_dir_inode = found_inode_no; // Спускаемся на уровень ниже
        token = next_token;
        next_token = strtok(NULL, "/");
    }

    //В 'token' теперь лежит чистое имя целевого файла, а в 'current_dir_inode' — инод его папки
    *out_parent_inode_no = current_dir_inode;

    //Имя конкретного файла/папки в EXT2 не может превышать 255 символов,
    //поэтому внешний буфер out_child_name[256] не вылез за границы
    strcpy(out_child_name, token);

    return FS_OK;
}



int ext2_touch(void *vol_ptr, const char *path){
    if (!vol_ptr || !path) return FS_INVALID_PATH;
    ext2_volume_t *vol = (ext2_volume_t *)vol_ptr;
    if (!vol->mounted) return FS_INVALID_HANDLE;

    uint32_t parent_inode_no = EXT2_ROOT_INO;
    char child_name[256];

    const char *slash_check = path;
    if (*slash_check == '/') slash_check++;

    bool has_subdirectories = false;
    while (*slash_check != '\0') {
        if (*slash_check == '/') {
            has_subdirectories = true;
            break;
        }
        slash_check++;
    }

    int r = ext2_find_parent_inode(vol, path, &parent_inode_no, child_name);

    if (r != FS_OK) {
        if (has_subdirectories) {
            return FS_INVALID_PATH;
        } else {
            parent_inode_no = EXT2_ROOT_INO;
            strcpy(child_name, path);
            if (child_name[0] == '/') strcpy(child_name, path + 1);
        }
    }

    uint32_t name_len = strlen(child_name);
    if (name_len == 0 || name_len > 255) return FS_INVALID_PATH;

    ext2_inode_t dummy_inode;
    uint32_t dummy_inode_no = 0;

    if (ext2_lookup_in_dir(vol, parent_inode_no, child_name, &dummy_inode, &dummy_inode_no) == FS_OK) {
        return FS_ALREADY_EXISTS;
    }

    uint32_t new_inode_no = 0;
    r = ext2_alloc_inode(vol, 0, &new_inode_no);
    if (r != FS_OK) return r;

    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(ext2_inode_t));

    new_inode.i_mode = 0x81A4;
    new_inode.i_links_count = 1;
    new_inode.i_size = 0;
    new_inode.i_blocks = 0;
    new_inode.i_ctime = 0x6A8D8447;
    new_inode.i_mtime = 0x6A8D8447;

    r = ext2_write_inode(vol, new_inode_no, &new_inode);
    if (r != FS_OK) return r;

    ext2_inode_t parent_inode;
    r = ext2_read_inode(vol, parent_inode_no, &parent_inode);
    if (r != FS_OK) return r;

    uint32_t block_size = vol->block_size;

    uint8_t *dir_buf = (uint8_t *)api_ptr->malloc_ptr(block_size);
    if (!dir_buf) return FS_IO_ERROR;
    memset(dir_buf, 0, block_size);

    uint32_t parent_phys_block = parent_inode.i_block[0];

    if (parent_phys_block == 0) {
        uint32_t allocated_block = 0;
        int alloc_res = ext2_alloc_block(vol, 0, &allocated_block);
        if (alloc_res != FS_OK) { api_ptr->free_ptr(dir_buf); return alloc_res; }

        parent_phys_block = allocated_block;
        parent_inode.i_block[0] = allocated_block;
        parent_inode.i_blocks += (block_size / 512);

        ext2_dir_entry_t *empty_entry = (ext2_dir_entry_t *)dir_buf;
        empty_entry->inode = 0;
        empty_entry->rec_len = block_size;
        empty_entry->name_len = 0;

        r = ext2_write_block(vol, parent_phys_block, dir_buf);
        if (r != FS_OK) { api_ptr->free_ptr(dir_buf); return r; }

        ext2_write_inode(vol, parent_inode_no, &parent_inode);
    }

    r = ext2_read_block(vol, parent_phys_block, dir_buf);
    if (r != FS_OK) { api_ptr->free_ptr(dir_buf); return r; }

    uint32_t offset = 0;
    ext2_dir_entry_t *last_entry = NULL;

    while (offset < block_size) {
        ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(dir_buf + offset);
        if (entry->rec_len == 0) { api_ptr->free_ptr(dir_buf); return FS_CORRUPTED; }

        if (offset + entry->rec_len >= block_size) {
            last_entry = entry;
            break;
        }
        offset += entry->rec_len;
    }

    if (!last_entry) {
        api_ptr->free_ptr(dir_buf);
        return FS_CORRUPTED;
    }

    uint32_t last_real_name_len = last_entry->name_len & 0xFF;
    uint32_t last_min_rec_len = (8 + last_real_name_len + 3) & ~3;

    uint32_t free_space_in_block = last_entry->rec_len - last_min_rec_len;
    uint32_t new_min_rec_len = (8 + name_len + 3) & ~3;

    if (free_space_in_block < new_min_rec_len) {
        api_ptr->free_ptr(dir_buf);
        return FS_NO_SPACE;
    }

    uint32_t last_old_rec_len = last_entry->rec_len;
    last_entry->rec_len = last_min_rec_len;

    ext2_dir_entry_t *new_entry = (ext2_dir_entry_t *)((uint8_t *)last_entry + last_min_rec_len);
    new_entry->inode = new_inode_no;
    new_entry->rec_len = last_old_rec_len - last_min_rec_len;
    new_entry->name_len = name_len | (1 << 8);

    memcpy(new_entry->name, child_name, name_len);

    r = ext2_write_block(vol, parent_phys_block, dir_buf);

    api_ptr->free_ptr(dir_buf);

    if (r != FS_OK) return r;
    return FS_OK;
}


int ext2_open(void *vol_ptr, const char *name, uint32_t flags){
    if (!vol_ptr || !name) return FS_IO_ERROR;
    ext2_volume_t *vol = (ext2_volume_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    //Особый случай: открытие корневого каталога "/"
    if (name[0] == '/' && name[1] == '\0') {
        for (int fd = 0; fd < EXT2_MAX_OPEN_FILES; fd++) {
            if (ext2_files_ptr[fd].used == 0) {
                ext2_files_ptr[fd].used = 1;
                ext2_files_ptr[fd].vol = vol;
                ext2_files_ptr[fd].inode_no = EXT2_ROOT_INO;
                ext2_files_ptr[fd].position = 0;
                ext2_files_ptr[fd].attributes = 0x4000; // Маска каталога

                int r = ext2_read_inode(vol, EXT2_ROOT_INO, &ext2_files_ptr[fd].inode);
                if (r != FS_OK) {
                    ext2_files_ptr[fd].used = 0;
                    return r;
                }

                ext2_files_ptr[fd].file_size = ext2_files_ptr[fd].inode.i_size;
                return fd;
            }
        }
        return FS_TOO_MANY;
    }

    //Разбор пути и поиск файла в любом каталоге
    uint32_t parent_inode_no = EXT2_ROOT_INO;
    char child_name[256];

    // Проверяем, есть ли вообще слэш в пути (кроме ведущего)
    const char *slash_check = name;
    if (*slash_check == '/') slash_check++;

    bool has_subdirectories = false;
    while (*slash_check != '\0') {
        if (*slash_check == '/') {
            has_subdirectories = true;
            break;
        }
        slash_check++;
    }

    //Вызываем парсер пути, чтобы найти инод родительской папки
    int r = ext2_find_parent_inode(vol, name, &parent_inode_no, child_name);
    if (r != FS_OK) {
        if (has_subdirectories) {
            //Если промежуточный каталог из пути не найден — возвращаем ошибку
            return FS_NOT_FOUND;
        } else {
            //Слэшей в пути не было (просто имя), значит файл должен быть в корне
            parent_inode_no = EXT2_ROOT_INO;
            strcpy(child_name, name);
            if (child_name[0] == '/') strcpy(child_name, name + 1);
        }
    }

    ext2_inode_t found_inode;
    uint32_t found_inode_no = 0;

    //Ищем файл строго ВНУТРИ найденного родительского каталога
    r = ext2_lookup_in_dir(vol, parent_inode_no, child_name, &found_inode, &found_inode_no);

    if (r == FS_NOT_FOUND && (flags & FS_OPEN_CREATE)) {
        int touch_res = ext2_touch(vol, name);
        if (touch_res != FS_OK) return touch_res;

        r = ext2_lookup_in_dir(vol, parent_inode_no, child_name, &found_inode, &found_inode_no);
    }

    if (r != FS_OK) return r; // Возвращаем код ошибки (например, FS_NOT_FOUND), если файл не существует

    //Ищем свободное место в глобальной таблице открытых файлов EXT2
    for (int fd = 0; fd < EXT2_MAX_OPEN_FILES; fd++) {
        if (ext2_files_ptr[fd].used == 0) {
            ext2_files_ptr[fd].used = 1;
            ext2_files_ptr[fd].vol = vol;
            ext2_files_ptr[fd].inode_no = found_inode_no;
            ext2_files_ptr[fd].file_size = found_inode.i_size;
            ext2_files_ptr[fd].position = 0; //Указатель смещения сбрасываем в 0

            //Сохраняем маску атрибутов (i_mode содержит тип файла, например 0x8000 или 0x4000)
            ext2_files_ptr[fd].attributes = found_inode.i_mode;

            //Копируем полную структуру инода для последующих функций чтения/записи
            memcpy(&ext2_files_ptr[fd].inode, &found_inode, sizeof(ext2_inode_t));

            return fd;
        }
    }

    return FS_TOO_MANY;
}



int ext2_close(int fd) {
    if (fd < 0 || fd >= EXT2_MAX_OPEN_FILES) return FS_IO_ERROR;
    if (ext2_files_ptr[fd].used == 0) return FS_IO_ERROR;

    ext2_files_ptr[fd].used = 0;
    return FS_OK;
}


int ext2_seek(int fd, uint32_t position) {
    if (fd < 0 || fd >= EXT2_MAX_OPEN_FILES) return FS_IO_ERROR;
    ext2_file_t *file = &ext2_files_ptr[fd];
    if (file->used == 0) return FS_IO_ERROR;

    // Запрещаем выходить за границы размера файла
    if (position > file->file_size) {
        file->position = file->file_size;
    } else {
        file->position = position;
    }

    return FS_OK;
}


int ext2_tell(int fd){
    if (fd < 0 || fd >= EXT2_MAX_OPEN_FILES) return FS_IO_ERROR;
    ext2_file_t *file = &ext2_files_ptr[fd];
    if (file->used == 0) return FS_IO_ERROR;

    return (int)file->position;
}


int ext2_readdir(int dir, fs_dirent_t *entry){
    if (dir < 0 || dir >= EXT2_MAX_OPEN_FILES || !entry) return FS_IO_ERROR;

    ext2_file_t *file = &ext2_files_ptr[dir];
    if (file->used == 0) return FS_IO_ERROR;
    if ((file->attributes & 0x4000) == 0) return FS_ACCESS_DENIED;

    uint32_t block_size = file->vol->block_size;

    uint8_t *dir_buf = (uint8_t *)api_ptr->malloc_ptr(block_size);
    if (!dir_buf) return FS_IO_ERROR;

    while (file->position < file->file_size) {
        uint32_t file_block_index = file->position / block_size;
        uint32_t offset_in_block = file->position % block_size;

        if (file_block_index >= 12) {
            api_ptr->free_ptr(dir_buf);
            return FS_NOT_FOUND;
        }
        uint32_t phys_block = file->inode.i_block[file_block_index];
        if (phys_block == 0) {
            file->position = (file_block_index + 1) * block_size;
            continue;
        }
        // Вычитываем свежий блок каталога с жесткого диска в кучу ядра
        int r = ext2_read_block(file->vol, phys_block, dir_buf);
        if (r != FS_OK) {
            api_ptr->free_ptr(dir_buf);
            return r;
        }
        // Накладываем структуру на текущее смещение внутри вычитанного буфера
        ext2_dir_entry_t *ext2_entry = (ext2_dir_entry_t *)(dir_buf + offset_in_block);

        // Защита от зацикливания (битый образ ФС)
        if (ext2_entry->rec_len == 0) {
            api_ptr->free_ptr(dir_buf);
            return FS_CORRUPTED;
        }
        uint32_t name_start_offset = offset_in_block + 8;
        uint32_t real_name_len = ext2_entry->name_len & 0xFF;
        if (real_name_len > 255) real_name_len = 255;

        // Сразу шагаем по позиции файла вперед на размер всей записи
        file->position += ext2_entry->rec_len;

        // Если инод равен 0 — файл удален в ФС, шагаем дальше по циклу
        if (ext2_entry->inode == 0) {
            continue;
        }

        // Заполняем структуру ответа для VFS ядра
        entry->inode = ext2_entry->inode;

        // Копируем имя из жестко вычисленного смещения плоского массива dir_buf наружу в ядро
        memcpy(entry->name, &dir_buf[name_start_offset], real_name_len);
        entry->name[real_name_len] = '\0'; /* Закрываем строку нулем */

        entry->type = 0; // Обычный файл по умолчанию

        // Проверяем байты прямо из плоского массива dir_buf
        if (real_name_len == 1 && dir_buf[name_start_offset] == '.') {
            entry->name[0] = '.';
            entry->name[1] = '\0';
            entry->type = FS_ATTR_DIRECTORY;
        }
        else if (real_name_len == 2 && dir_buf[name_start_offset] == '.' && dir_buf[name_start_offset + 1] == '.') {
            entry->name[0] = '.';
            entry->name[1] = '.';
            entry->name[2] = '\0';
            entry->type = FS_ATTR_DIRECTORY;
        }
        else {
            uint8_t ext2_type = (ext2_entry->name_len >> 8) & 0xFF;
            if (ext2_type == 2) {
                entry->type = FS_ATTR_DIRECTORY;
            } else {
                ext2_inode_t temp_inode;
                if (ext2_read_inode(file->vol, ext2_entry->inode, &temp_inode) == FS_OK) {
                    if ((temp_inode.i_mode & 0xF000) == 0x4000) {
                        entry->type = FS_ATTR_DIRECTORY;
                    }
                }
            }
        }

        api_ptr->free_ptr(dir_buf);
        return FS_OK;
    }

    api_ptr->free_ptr(dir_buf);
    return FS_NOT_FOUND; // Каталог полностью прочитан, файлов больше нет
}


int ext2_stat(void *vol_ptr, const char *path, fs_stat_t *st){
    if (!vol_ptr || !path || !st) return FS_IO_ERROR;

    ext2_volume_t *vol = (ext2_volume_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    //Особый случай - запрос информации о корневом каталоге "/"
    if (path[0] == '/' && path[1] == '\0') {
        ext2_inode_t root_inode;
        int r = ext2_read_inode(vol, EXT2_ROOT_INO, &root_inode);
        if (r != FS_OK) return r;

        st->size = root_inode.i_size;
        st->attributes = FS_ATTR_DIRECTORY;
        st->create_time = root_inode.i_ctime;
        st->modify_time = root_inode.i_mtime;
        return FS_OK;
    }

    //Разбор пути для нахождения вложенного элемента
    uint32_t parent_inode_no = EXT2_ROOT_INO;
    char child_name[256];

    //Проверяем, есть ли вообще слэш в пути (кроме ведущего)
    const char *slash_check = path;
    if (*slash_check == '/') slash_check++;

    bool has_subdirectories = false;
    while (*slash_check != '\0') {
        if (*slash_check == '/') {
            has_subdirectories = true;
            break;
        }
        slash_check++;
    }

    //Вызываем парсер пути, чтобы найти инод родительской папки
    int r = ext2_find_parent_inode(vol, path, &parent_inode_no, child_name);
    if (r != FS_OK) {
        if (has_subdirectories) {
            //Если промежуточный каталог из пути не найден — возвращаем ошибку
            return FS_NOT_FOUND;
        } else {
            //Слэшей в пути не было (просто имя), значит файл должен быть в корне
            parent_inode_no = EXT2_ROOT_INO;
            strcpy(child_name, path);
            if (child_name[0] == '/') strcpy(child_name, path + 1);
        }
    }

    ext2_inode_t inode;
    uint32_t inode_no = 0;

    //Ищем целевой элемент строго ВНУТРИ найденного родительского каталога
    r = ext2_lookup_in_dir(vol, parent_inode_no, child_name, &inode, &inode_no);
    if (r != FS_OK) return r;

    //Заполняем размер файла/каталога наружу в VFS ядра
    st->size = inode.i_size;

    //Декодируем Unix-атрибуты/маски EXT2 (i_mode) в маски вашей VFS
    st->attributes = 0;

    // Проверяем биты типа файла в i_mode
    if ((inode.i_mode & 0xF000) == 0x4000) {
        st->attributes |= FS_ATTR_DIRECTORY; /* Папка (S_IFDIR) */
    }

    //Проверяем флаги прав на чтение (DOS-аналог Read-Only — это отсутствие прав на запись владельцу)
    if (!(inode.i_mode & 0x0080)) {
        st->attributes |= FS_ATTR_READONLY;
    }

    //Копируем 32-битные Unix Timestamp временные метки напрямую наружу
    st->create_time = inode.i_ctime; // Время создания (или изменения инода)
    st->modify_time = inode.i_mtime; // Время последней модификации данных

    return FS_OK;
}


int ext2_read(int fd, void *buffer, uint32_t size){
    if (fd < 0 || fd >= EXT2_MAX_OPEN_FILES) return FS_IO_ERROR;
    ext2_file_t *file = &ext2_files_ptr[fd];
    if (file->used == 0 || !buffer || size == 0) return 0;

    // Защита от чтения за пределами файла
    if (file->position >= file->file_size) return 0;
    if (file->position + size > file->file_size) {
        size = file->file_size - file->position;
    }

    uint8_t *dst = (uint8_t *)buffer;
    uint32_t bytes_read = 0;
    uint32_t remaining = size;
    uint32_t block_size = file->vol->block_size;

    uint8_t *block_buf = (uint8_t *)api_ptr->malloc_ptr(block_size);
    uint32_t *indirect_buf = (uint32_t *)api_ptr->malloc_ptr(block_size);

    if (!block_buf || !indirect_buf) {
        if (block_buf) api_ptr->free_ptr(block_buf);
        if (indirect_buf) api_ptr->free_ptr(indirect_buf);
        return 0;
    }

    uint32_t ptrs = block_size / 4; //Количество указателей в одном блоке (например, 256)

    while (remaining > 0) {
        uint32_t file_block_index = file->position / block_size;
        uint32_t offset_in_block = file->position % block_size;

        uint32_t available_in_block = block_size - offset_in_block;
        uint32_t to_copy = remaining;
        if (remaining > available_in_block) to_copy = available_in_block;

        uint32_t phys_block = 0;

        // Прямая адресация (0 .. 11)
        if (file_block_index < 12) {
            phys_block = file->inode.i_block[file_block_index];
        }
        // Одинарная косвенная адресация
        else if (file_block_index < 12 + ptrs) {
            uint32_t table1 = file->inode.i_block[12];
            if (table1 != 0) {
                if (ext2_read_block(file->vol, table1, (uint8_t *)indirect_buf) == FS_OK) {
                    phys_block = indirect_buf[file_block_index - 12];
                }
            }
        }
        // Двойная косвенная адресация
        else if (file_block_index < 12 + ptrs + (ptrs * ptrs)) {
            uint32_t table2 = file->inode.i_block[13];
            if (table2 != 0) {
                uint32_t rel_idx = file_block_index - 12 - ptrs;
                uint32_t i1 = rel_idx / ptrs;
                uint32_t i2 = rel_idx % ptrs;

                if (ext2_read_block(file->vol, table2, (uint8_t *)indirect_buf) == FS_OK) {
                    uint32_t table1 = indirect_buf[i1];
                    if (table1 != 0 && ext2_read_block(file->vol, table1, (uint8_t *)indirect_buf) == FS_OK) {
                        phys_block = indirect_buf[i2];
                    }
                }
            }
        }
        // Тройная косвенная адресация
        else {
            uint32_t table3 = file->inode.i_block[14];
            if (table3 != 0) {
                uint32_t rel_idx = file_block_index - 12 - ptrs - (ptrs * ptrs);
                uint32_t i1 = rel_idx / (ptrs * ptrs);
                uint32_t i2 = (rel_idx % (ptrs * ptrs)) / ptrs;
                uint32_t i3 = rel_idx % ptrs;

                if (ext2_read_block(file->vol, table3, (uint8_t *)indirect_buf) == FS_OK) {
                    uint32_t table2 = indirect_buf[i1];
                    if (table2 != 0 && ext2_read_block(file->vol, table2, (uint8_t *)indirect_buf) == FS_OK) {
                        uint32_t table1 = indirect_buf[i2];
                        if (table1 != 0 && ext2_read_block(file->vol, table1, (uint8_t *)indirect_buf) == FS_OK) {
                            phys_block = indirect_buf[i3];
                        }
                    }
                }
            }
        }

        // Чтение блока данных
        if (phys_block == 0) {
            memset(dst + bytes_read, 0, to_copy); // Разреженный кусок файла
        } else {
            int r = ext2_read_block(file->vol, phys_block, block_buf);
            if (r != FS_OK) {
                api_ptr->free_ptr(block_buf);
                api_ptr->free_ptr(indirect_buf);
                return r;
            }
            memcpy(dst + bytes_read, block_buf + offset_in_block, to_copy);
        }

        file->position += to_copy;
        bytes_read += to_copy;
        remaining -= to_copy;
    }

    api_ptr->free_ptr(block_buf);
    api_ptr->free_ptr(indirect_buf);

    return (int)bytes_read;
}

int ext2_write(int fd, const void *buffer, uint32_t size){
    if (fd < 0 || fd >= EXT2_MAX_OPEN_FILES) return FS_TOO_MANY;
    ext2_file_t *file = &ext2_files_ptr[fd];
    if (file->used == 0 || !buffer || size == 0) return 0;

    if ((file->attributes & 0xF000) == 0x4000) return FS_ACCESS_DENIED;

    uint32_t block_size = file->vol->block_size;
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t bytes_written = 0;
    uint32_t remaining = size;

    uint8_t *block_buf = (uint8_t *)api_ptr->malloc_ptr(block_size);
    uint32_t *indirect_buf = (uint32_t *)api_ptr->malloc_ptr(block_size);

    if (!block_buf || !indirect_buf) {
        if (block_buf) api_ptr->free_ptr(block_buf);
        if (indirect_buf) api_ptr->free_ptr(indirect_buf);
        return FS_IO_ERROR;
    }

    uint32_t ptrs = block_size / 4;
    uint32_t sectors_per_block = block_size / 512;

    while (remaining > 0) {
        uint32_t file_block_index = file->position / block_size;
        uint32_t offset_in_block = file->position % block_size;

        uint32_t phys_block = 0;
        bool is_new_block = false;

        //Запись прямых блоков (0 .. 11)
        if (file_block_index < 12) {
            phys_block = file->inode.i_block[file_block_index];
            if (phys_block == 0) {
                uint32_t new_block = 0;
                if (ext2_alloc_block(file->vol, 0, &new_block) != FS_OK) break;
                file->inode.i_block[file_block_index] = new_block;
                phys_block = new_block;
                file->inode.i_blocks += sectors_per_block;
                is_new_block = true;
            }
        }
        // Запись с одинарной косвенностью
        else if (file_block_index < 12 + ptrs) {
            uint32_t table1 = file->inode.i_block[12];
            if (table1 == 0) {
                if (ext2_alloc_block(file->vol, 0, &table1) != FS_OK) break;
                file->inode.i_block[12] = table1;
                file->inode.i_blocks += sectors_per_block;
                memset((uint8_t *)indirect_buf, 0, block_size);
                ext2_write_block(file->vol, table1, (uint8_t *)indirect_buf);
            }

            if (ext2_read_block(file->vol, table1, (uint8_t *)indirect_buf) != FS_OK) break;
            uint32_t idx = file_block_index - 12;
            phys_block = indirect_buf[idx];

            if (phys_block == 0) {
                if (ext2_alloc_block(file->vol, 0, &phys_block) != FS_OK) break;
                indirect_buf[idx] = phys_block;
                file->inode.i_blocks += sectors_per_block;
                is_new_block = true;
                ext2_write_block(file->vol, table1, (uint8_t *)indirect_buf);
            }
        }
        // Запись с двойной косвенностью
        else if (file_block_index < 12 + ptrs + (ptrs * ptrs)) {
            uint32_t table2 = file->inode.i_block[13];
            if (table2 == 0) {
                if (ext2_alloc_block(file->vol, 0, &table2) != FS_OK) break;
                file->inode.i_block[13] = table2;
                file->inode.i_blocks += sectors_per_block;
                memset((uint8_t *)indirect_buf, 0, block_size);
                ext2_write_block(file->vol, table2, (uint8_t *)indirect_buf);
            }

            uint32_t rel_idx = file_block_index - 12 - ptrs;
            uint32_t i1 = rel_idx / ptrs;
            uint32_t i2 = rel_idx % ptrs;

            if (ext2_read_block(file->vol, table2, (uint8_t *)indirect_buf) != FS_OK) break;
            uint32_t table1 = indirect_buf[i1];

            if (table1 == 0) {
                if (ext2_alloc_block(file->vol, 0, &table1) != FS_OK) break;
                indirect_buf[i1] = table1;
                ext2_write_block(file->vol, table2, (uint8_t *)indirect_buf);

                uint32_t *temp_buf = (uint32_t *)api_ptr->malloc_ptr(block_size);
                if (temp_buf) {
                    memset(temp_buf, 0, block_size);
                    ext2_write_block(file->vol, table1, (uint8_t *)temp_buf);
                    api_ptr->free_ptr(temp_buf);
                }
            }

            if (ext2_read_block(file->vol, table1, (uint8_t *)indirect_buf) != FS_OK) break;
            phys_block = indirect_buf[i2];

            if (phys_block == 0) {
                if (ext2_alloc_block(file->vol, 0, &phys_block) != FS_OK) break;
                indirect_buf[i2] = phys_block;
                file->inode.i_blocks += sectors_per_block;
                is_new_block = true;
                ext2_write_block(file->vol, table1, (uint8_t *)indirect_buf);
            }
        }

        //Запись данных в физический блок (Read-Modify-Write)
        uint32_t available_in_block = block_size - offset_in_block;
        uint32_t to_write = remaining;
        if (remaining > available_in_block) to_write = available_in_block;

        if (offset_in_block == 0 && to_write == block_size) {
            memcpy(block_buf, src + bytes_written, block_size);
            if (ext2_write_block(file->vol, phys_block, block_buf) != FS_OK) break;
        }
        else if (is_new_block) {
            memset(block_buf, 0, block_size);
            memcpy(block_buf + offset_in_block, src + bytes_written, to_write);
            if (ext2_write_block(file->vol, phys_block, block_buf) != FS_OK) break;
        }
        else {
            if (ext2_read_block(file->vol, phys_block, block_buf) != FS_OK) break;
            memcpy(block_buf + offset_in_block, src + bytes_written, to_write);
            if (ext2_write_block(file->vol, phys_block, block_buf) != FS_OK) break;
        }

        file->position += to_write;
        bytes_written += to_write;
        remaining -= to_write;

        if (file->position > file->inode.i_size) {
            file->inode.i_size = file->position;
            file->file_size = file->position;
        }
    }

    ext2_write_inode(file->vol, file->inode_no, &file->inode);

    api_ptr->free_ptr(block_buf);
    api_ptr->free_ptr(indirect_buf);

    return (int)bytes_written;
}




int ext2_mkdir(void *vol_ptr, const char *path){
    if (!vol_ptr || !path) return FS_IO_ERROR;
    ext2_volume_t *vol = (ext2_volume_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    uint32_t parent_inode_no = EXT2_ROOT_INO;
    char child_name[256];

    const char *slash_check = path;
    if (*slash_check == '/') slash_check++;

    bool has_subdirectories = false;
    while (*slash_check != '\0') {
        if (*slash_check == '/') {
            has_subdirectories = true;
            break;
        }
        slash_check++;
    }

    int r = ext2_find_parent_inode(vol, path, &parent_inode_no, child_name);
    if (r != FS_OK) {
        if (has_subdirectories) return FS_INVALID_PATH;
        else {
            parent_inode_no = EXT2_ROOT_INO;
            strcpy(child_name, path);
            if (child_name[0] == '/') strcpy(child_name, path + 1);
        }
    }

    ext2_inode_t parent_inode;
    r = ext2_read_inode(vol, parent_inode_no, &parent_inode);
    if (r != FS_OK) return r;

    ext2_inode_t dummy_inode;
    uint32_t dummy_inode_no;
    if (ext2_lookup_in_dir(vol, parent_inode_no, child_name, &dummy_inode, &dummy_inode_no) == FS_OK) {
        return -2;
    }

    uint32_t new_inode_no = 0;
    r = ext2_alloc_inode(vol, 0, &new_inode_no);
    if (r != FS_OK) return r;

    uint32_t new_block_no = 0;
    r = ext2_alloc_block(vol, 0, &new_block_no);
    if (r != FS_OK) return r;

    uint8_t *block_buf = (uint8_t *)api_ptr->malloc_ptr(vol->block_size);
    if (!block_buf) return FS_IO_ERROR;
    memset(block_buf, 0, vol->block_size);

    uint32_t offset = 0;

    //Запись 1: "."
    ext2_dir_entry_t *dot = (ext2_dir_entry_t *)(block_buf + offset);
    dot->inode = new_inode_no;
    dot->name_len = 1;
    dot->rec_len = 12;
    dot->name[0] = '.';

    offset += dot->rec_len;

    //Запись 2: ".."
    ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)(block_buf + offset);
    dotdot->inode = parent_inode_no;
    dotdot->name_len = 2;
    dotdot->rec_len = vol->block_size - offset;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    r = ext2_write_block(vol, new_block_no, block_buf);
    if (r != FS_OK) { api_ptr->free_ptr(block_buf); return r; }

    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(ext2_inode_t));
    new_inode.i_mode = EXT2_S_IFDIR | 0755;
    new_inode.i_size = vol->block_size;
    new_inode.i_links_count = 2;
    new_inode.i_blocks = vol->sectors_per_block;
    new_inode.i_block[0] = new_block_no;

    r = ext2_write_inode(vol, new_inode_no, &new_inode);
    if (r != FS_OK) { api_ptr->free_ptr(block_buf); return r; }

    uint32_t parent_block = parent_inode.i_block[0];
    r = ext2_read_block(vol, parent_block, block_buf);
    if (r != FS_OK) { api_ptr->free_ptr(block_buf); return r; }

    uint32_t current_offset = 0;
    ext2_dir_entry_t *entry = NULL;

    while (current_offset < vol->block_size) {
        entry = (ext2_dir_entry_t *)(block_buf + current_offset);
        uint32_t real_len = (8 + entry->name_len + 3) & ~3;

        if (current_offset + entry->rec_len >= vol->block_size) {
            uint32_t old_rec_len = entry->rec_len;
            entry->rec_len = real_len;
            current_offset += real_len;

            ext2_dir_entry_t *new_entry = (ext2_dir_entry_t *)(block_buf + current_offset);
            new_entry->inode = new_inode_no;
            new_entry->name_len = strlen(child_name);
            new_entry->rec_len = old_rec_len - real_len;
            memcpy(new_entry->name, child_name, new_entry->name_len);
            break;
        }
        current_offset += entry->rec_len;
    }

    r = ext2_write_block(vol, parent_block, block_buf);
    api_ptr->free_ptr(block_buf);
    if (r != FS_OK) return r;

    parent_inode.i_links_count++;
    r = ext2_write_inode(vol, parent_inode_no, &parent_inode);

    return r;
}


int ext2_unlink(void *vol_ptr, const char *path){
    if (!vol_ptr || !path) return FS_IO_ERROR;
    ext2_volume_t *vol = (ext2_volume_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    uint32_t parent_inode_no = EXT2_ROOT_INO;
    char child_name[256];

    const char *slash_check = path;
    if (*slash_check == '/') slash_check++;

    bool has_subdirectories = false;
    while (*slash_check != '\0') {
        if (*slash_check == '/') {
            has_subdirectories = true;
            break;
        }
        slash_check++;
    }

    int r = ext2_find_parent_inode(vol, path, &parent_inode_no, child_name);
    if (r != FS_OK) {
        if (has_subdirectories) {
            return FS_NOT_FOUND;
        } else {
            parent_inode_no = EXT2_ROOT_INO;
            strcpy(child_name, path);
            if (child_name[0] == '/') strcpy(child_name, path + 1);
        }
    }

    ext2_inode_t file_inode;
    uint32_t file_inode_no = 0;
    r = ext2_lookup_in_dir(vol, parent_inode_no, child_name, &file_inode, &file_inode_no);
    if (r != FS_OK) return FS_NOT_FOUND;

    if ((file_inode.i_mode & 0xF000) == 0x4000) return FS_ACCESS_DENIED;

    uint8_t *bitmap_buf = (uint8_t *)api_ptr->malloc_ptr(4096);
    if (!bitmap_buf) return FS_IO_ERROR;
    memset(bitmap_buf, 0, 4096);

    uint8_t *bg_desc_bytes = vol->bg_desc_table + (0 * 32);
    uint32_t block_bitmap_no = *(uint32_t *)(bg_desc_bytes + 0);

    if (block_bitmap_no != 0) {
        r = ext2_read_block(vol, block_bitmap_no, bitmap_buf);
        if (r == FS_OK) {
            uint32_t freed_blocks_count = 0;

            for (int b = 0; b < 12; b++) {
                uint32_t phys_block = file_inode.i_block[b];
                if (phys_block == 0) continue;

                uint32_t block_idx = phys_block - vol->sb.s_first_data_block;
                uint32_t byte_idx = block_idx / 8;
                uint32_t bit_idx = block_idx % 8;

                if (bitmap_buf[byte_idx] & (1 << bit_idx)) {
                    bitmap_buf[byte_idx] &= ~(1 << bit_idx);
                    freed_blocks_count++;
                }
            }

            if (freed_blocks_count > 0) {
                ext2_write_block(vol, block_bitmap_no, bitmap_buf);
                uint16_t *free_blocks_ptr = (uint16_t *)(bg_desc_bytes + 12);
                *free_blocks_ptr += freed_blocks_count;
                vol->sb.s_free_blocks_count += freed_blocks_count;
            }
        }
    }

    uint32_t inode_bitmap_no = *(uint32_t *)(bg_desc_bytes + 4);
    if (inode_bitmap_no != 0) {
        r = ext2_read_block(vol, inode_bitmap_no, bitmap_buf);
        if (r == FS_OK) {
            uint32_t inode_idx = file_inode_no - 1;
            uint32_t byte_idx = inode_idx / 8;
            uint32_t bit_idx = inode_idx % 8;

            if (bitmap_buf[byte_idx] & (1 << bit_idx)) {
                bitmap_buf[byte_idx] &= ~(1 << bit_idx);
                ext2_write_block(vol, inode_bitmap_no, bitmap_buf);
                uint16_t *free_inodes_ptr = (uint16_t *)(bg_desc_bytes + 14);
                (*free_inodes_ptr)++;
                vol->sb.s_free_inodes_count++;
            }
        }
    }

    ext2_inode_t zero_inode;
    memset(&zero_inode, 0, sizeof(ext2_inode_t));
    ext2_write_inode(vol, file_inode_no, &zero_inode);

    ext2_inode_t parent_inode;
    r = ext2_read_inode(vol, parent_inode_no, &parent_inode);
    if (r != FS_OK) { api_ptr->free_ptr(bitmap_buf); return r; }

    uint32_t parent_block = parent_inode.i_block[0];
    r = ext2_read_block(vol, parent_block, bitmap_buf);
    if (r != FS_OK) { api_ptr->free_ptr(bitmap_buf); return r; }

    uint32_t current_offset = 0;
    ext2_dir_entry_t *entry = NULL;
    ext2_dir_entry_t *prev_entry = NULL;
    bool record_deleted = false;

    while (current_offset < vol->block_size) {
        entry = (ext2_dir_entry_t *)(bitmap_buf + current_offset);
        if (entry->rec_len == 0) break;

        if (entry->inode == file_inode_no) {
            if (prev_entry != NULL) {
                prev_entry->rec_len += entry->rec_len;
                record_deleted = true;
                break;
            } else {
                entry->inode = 0;
                record_deleted = true;
                break;
            }
        }

        prev_entry = entry;
        current_offset += entry->rec_len;
    }

    if (record_deleted) {
        ext2_write_block(vol, parent_block, bitmap_buf);
    }

    // Возвращаем занятую страницу битовой карты куче ядра перед финализацией секторов
    api_ptr->free_ptr(bitmap_buf);

    uint32_t bgdt_block = (vol->block_size == 1024) ? 2 : 1;
    uint32_t bgdt_lba = vol->partition_start_lba + (bgdt_block * vol->sectors_per_block);
    uint32_t bgdt_size_bytes = vol->groups_count * sizeof(ext2_bg_desc_t);
    uint32_t bgdt_sectors = (bgdt_size_bytes + 512 - 1) / 512;
    ext2_disk_write_sectors(bgdt_lba, bgdt_sectors, vol->bg_desc_table);

    uint32_t sb_lba = vol->partition_start_lba + 2;
    uint8_t *sb_save_buf = (uint8_t *)api_ptr->malloc_ptr(4096);
    if (sb_save_buf) {
        r = ext2_disk_read_sectors(sb_lba, 2, sb_save_buf);
        if (r == FS_OK) {
            memcpy(sb_save_buf, &vol->sb, sizeof(ext2_superblock_t));
            ext2_disk_write_sectors(sb_lba, 2, sb_save_buf);
        }
        api_ptr->free_ptr(sb_save_buf);
    }

    return FS_OK;
}


int ext2_rmdir(void *vol_ptr, const char *path) {
    if (!vol_ptr || !path) return FS_IO_ERROR;
    ext2_volume_t *vol = (ext2_volume_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    uint32_t parent_inode_no = EXT2_ROOT_INO;
    char child_name[256];

    const char *slash_check = path;
    if (*slash_check == '/') slash_check++;

    bool has_subdirectories = false;
    while (*slash_check != '\0') {
        if (*slash_check == '/') {
            has_subdirectories = true;
            break;
        }
        slash_check++;
    }

    int r = ext2_find_parent_inode(vol, path, &parent_inode_no, child_name);
    if (r != FS_OK) {
        if (has_subdirectories) {
            return FS_NOT_FOUND;
        } else {
            parent_inode_no = EXT2_ROOT_INO;
            strcpy(child_name, path);
            if (child_name[0] == '/') strcpy(child_name, path + 1);
        }
    }

    ext2_inode_t dir_inode;
    uint32_t dir_inode_no = 0;
    r = ext2_lookup_in_dir(vol, parent_inode_no, child_name, &dir_inode, &dir_inode_no);
    if (r != FS_OK) return FS_NOT_FOUND;

    if ((dir_inode.i_mode & 0xF000) != 0x4000) return FS_ACCESS_DENIED;

    uint8_t *block_buf = (uint8_t *)api_ptr->malloc_ptr(4096);
    if (!block_buf) return FS_IO_ERROR;
    memset(block_buf, 0, 4096);

    uint32_t dir_phys_block = dir_inode.i_block[0];

    if (dir_phys_block != 0) {
        r = ext2_read_block(vol, dir_phys_block, block_buf);
        if (r != FS_OK) { api_ptr->free_ptr(block_buf); return r; }

        uint32_t offset = 0;
        while (offset < vol->block_size) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(block_buf + offset);
            if (entry->rec_len == 0) break;

            if (entry->inode != 0) {
                uint32_t len = entry->name_len & 0xFF;
                uint8_t *name_ptr = ((uint8_t *)entry) + 8;

                if (!(len == 1 && name_ptr[0] == '.') &&
                    !(len == 2 && name_ptr[0] == '.' && name_ptr[1] == '.'))
                {
                    api_ptr->free_ptr(block_buf);
                    return FS_ACCESS_DENIED;
                }
            }
            offset += entry->rec_len;
        }
    }

    uint8_t *bg_desc_bytes = vol->bg_desc_table + (0 * 32);
    uint32_t block_bitmap_no = *(uint32_t *)(bg_desc_bytes + 0);

    if (dir_phys_block != 0 && block_bitmap_no != 0) {
        r = ext2_read_block(vol, block_bitmap_no, block_buf);
        if (r == FS_OK) {
            uint32_t block_idx = dir_phys_block - vol->sb.s_first_data_block;
            uint32_t byte_idx = block_idx / 8;
            uint32_t bit_idx = block_idx % 8;

            if (block_buf[byte_idx] & (1 << bit_idx)) {
                block_buf[byte_idx] &= ~(1 << bit_idx);
                ext2_write_block(vol, block_bitmap_no, block_buf);

                uint16_t *free_blocks_ptr = (uint16_t *)(bg_desc_bytes + 12);
                (*free_blocks_ptr)++;
                vol->sb.s_free_blocks_count++;
            }
        }
    }

    uint32_t inode_bitmap_no = *(uint32_t *)(bg_desc_bytes + 4);
    if (inode_bitmap_no != 0) {
        r = ext2_read_block(vol, inode_bitmap_no, block_buf);
        if (r == FS_OK) {
            uint32_t inode_idx = dir_inode_no - 1;
            uint32_t byte_idx = inode_idx / 8;
            uint32_t bit_idx = inode_idx % 8;

            if (block_buf[byte_idx] & (1 << bit_idx)) {
                block_buf[byte_idx] &= ~(1 << bit_idx);
                ext2_write_block(vol, inode_bitmap_no, block_buf);

                uint16_t *free_inodes_ptr = (uint16_t *)(bg_desc_bytes + 14);
                (*free_inodes_ptr)++;
                vol->sb.s_free_inodes_count++;
            }
        }
    }

    ext2_inode_t zero_inode;
    memset(&zero_inode, 0, sizeof(ext2_inode_t));
    ext2_write_inode(vol, dir_inode_no, &zero_inode);

    ext2_inode_t parent_inode;
    r = ext2_read_inode(vol, parent_inode_no, &parent_inode);
    if (r != FS_OK) { api_ptr->free_ptr(block_buf); return r; }

    uint32_t parent_block = parent_inode.i_block[0];
    r = ext2_read_block(vol, parent_block, block_buf);
    if (r != FS_OK) { api_ptr->free_ptr(block_buf); return r; }

    uint32_t current_offset = 0;
    ext2_dir_entry_t *entry = NULL;
    ext2_dir_entry_t *prev_entry = NULL;
    bool record_deleted = false;

    while (current_offset < vol->block_size) {
        entry = (ext2_dir_entry_t *)(block_buf + current_offset);
        if (entry->rec_len == 0) break;

        if (entry->inode == dir_inode_no) {
            if (prev_entry != NULL) {
                prev_entry->rec_len += entry->rec_len;
                record_deleted = true;
                break;
            } else {
                entry->inode = 0;
                record_deleted = true;
                break;
            }
        }
        prev_entry = entry;
        current_offset += entry->rec_len;
    }

    if (record_deleted) {
        ext2_write_block(vol, parent_block, block_buf);
    }

    api_ptr->free_ptr(block_buf);

    if (parent_inode.i_links_count > 2) {
        parent_inode.i_links_count--;
        ext2_write_inode(vol, parent_inode_no, &parent_inode);
    }

    uint32_t bgdt_block = (vol->block_size == 1024) ? 2 : 1;
    uint32_t bgdt_lba = vol->partition_start_lba + (bgdt_block * vol->sectors_per_block);
    uint32_t bgdt_size_bytes = vol->groups_count * sizeof(ext2_bg_desc_t);
    uint32_t bgdt_sectors = (bgdt_size_bytes + 512 - 1) / 512;
    ext2_disk_write_sectors(bgdt_lba, bgdt_sectors, vol->bg_desc_table);

    uint32_t sb_lba = vol->partition_start_lba + 2;
    uint8_t *sb_save_buf = (uint8_t *)api_ptr->malloc_ptr(4096);
    if (sb_save_buf) {
        r = ext2_disk_read_sectors(sb_lba, 2, sb_save_buf);
        if (r == FS_OK) {
            memcpy(sb_save_buf, &vol->sb, sizeof(ext2_superblock_t));
            ext2_disk_write_sectors(sb_lba, 2, sb_save_buf);
        }
        api_ptr->free_ptr(sb_save_buf);
    }

    return FS_OK;
}






void fs_init(struct boot_info *boot){

}


void driver_main(struct boot_info *boot){

    fs_init(boot);

    api_ptr = &api;


    ext2_volumes_ptr = ext2_volumes;
    for(int i = 0 ; i < EXT2_MAX_VOLUMES; i++){
        ext2_volumes_ptr[i].mounted = 0;
    }
    ext2_files_ptr = ext2_files;
    for(int i = 0 ; i < EXT2_MAX_OPEN_FILES; i++){
        ext2_files_ptr[i].used = 0;
    }

    bg_desc_tables_ptr = bg_desc_tables;
    ext2_path_buffer_ptr = ext2_path_buffer;

    api_ptr->mount = ext2_mount;
    api_ptr->unmount = ext2_unmount;
    api_ptr->get_volume = ext2_get_volume;
    api_ptr->lookup = ext2_lookup;
    api_ptr->open = ext2_open;
    api_ptr->close = ext2_close;
    api_ptr->seek = ext2_seek;
    api_ptr->tell = ext2_tell;
    api_ptr->readdir = ext2_readdir;
    api_ptr->stat = ext2_stat;
    api_ptr->read = ext2_read;
    api_ptr->write = ext2_write;
    api_ptr->touch = ext2_touch;
    api_ptr->mkdir = ext2_mkdir;
    api_ptr->rmdir = ext2_rmdir;
    api_ptr->unlink = ext2_unlink;

}


fs_driver_api_t *driver_get_api(void){
    return &api;
}


