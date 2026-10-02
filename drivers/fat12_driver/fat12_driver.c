#include "include/fat12_driver.h"
#include "include/fat12_internal.h"
#include "include/fat12_boot.h"
#include "include/fat12_disk.h"
#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/lib.h"


fat12_fs_t fat12_volumes[FAT12_MAX_VOLUMES];
fat12_fs_t *fat12_volumes_ptr;
uint32_t fat12_volume_count;

fat12_file_t fat12_files[FAT12_MAX_OPEN_FILES];
fat12_file_t *fat12_files_ptr;

fs_driver_api_t api;
fs_driver_api_t *api_ptr;

uint8_t fat12_path_buffer[FAT12_PATH_MAX] __attribute__((aligned(4)));
uint8_t *fat12_path_buffer_ptr;


//  ----- внутренние функции  -----


int fat12_read_file_at(fat12_fs_t *vol,uint16_t start_cluster,uint32_t position,void *buffer,uint32_t size){
    if (!vol || !buffer) return FS_IO_ERROR;
    if (!vol->mounted) return FS_IO_ERROR;
    if (size == 0) return 0;
    //Размер кластера в байтах
    uint32_t cluster_size = vol->sectors_per_cluster * vol->bytes_per_sector;
    if (cluster_size == 0) return FS_CORRUPTED;
    //Начинаем с первого кластера файла
    uint16_t cluster = start_cluster;
    if (cluster < 2) return FS_CORRUPTED;
    //Пропускаем кластеры до позиции position
    //Например:
    //position = 0 -> первый кластер
    //position = cluster_size -> второй кластер
    //position = 2 * cluster_size -> третий кластер
    uint32_t cluster_index = position / cluster_size;
    uint32_t offset_in_cluster = position % cluster_size;
    for (uint32_t i = 0; i < cluster_index; i++){
        uint16_t next = fat12_get_next_cluster(vol,cluster);
        //FAT12 EOF
        if (next >= 0xFF8) return FS_CORRUPTED;
        //Зарезервированные / повреждённые значения FAT
        if (next == 0xFF7) return FS_CORRUPTED;
        if (next < 2) return FS_CORRUPTED;
        cluster = next;
    }
    //Теперь cluster содержит кластер, в котором находится position
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t remaining = size;
    //Читаем файл кластер за кластером
    while (remaining > 0){
        //LBA первого сектора текущего кластера: * data_start + (cluster - 2) * sectors_per_cluster
        uint32_t cluster_lba = vol->data_start + ((uint32_t)(cluster - 2) * vol->sectors_per_cluster);
        //Сколько байт осталось в текущем кластере после offset_in_cluster
        uint32_t available = cluster_size - offset_in_cluster;
        uint32_t to_read = remaining;
        if (to_read > available) to_read = available;
        //Читаем нужные сектора
        //Сейчас FAT12_read_sectors() работает с полными секторами^ поэтому определяем:
        //first_sector
        //sector_offset
        uint32_t first_sector = offset_in_cluster / vol->bytes_per_sector;
        uint32_t sector_offset = offset_in_cluster % vol->bytes_per_sector;
        uint32_t sectors_needed = (sector_offset + to_read + vol->bytes_per_sector - 1) / vol->bytes_per_sector;
        //Если чтение начинается/заканчивается не на границе сектора, нужен временный буфер
        //Максимально нам понадобится sectors_per_cluster * 512 байт
        uint8_t sector_buffer[512];
        //Простой случай: читаем целое число секторов прямо в пользовательский buffer
        if (sector_offset == 0 && to_read >= vol->bytes_per_sector){
            uint32_t full_sectors = to_read / vol->bytes_per_sector;
            uint32_t bytes = full_sectors * vol->bytes_per_sector;
            int r = fat12_read_sectors(vol,cluster_lba + first_sector,full_sectors,dst);
            if (r != FS_OK) return r;
            dst += bytes;
            remaining -= bytes;
            //Если полностью закончили текущий кластер — переходим к следующему
            if (remaining == 0) break;
            offset_in_cluster += bytes;
            //Если дошли до конца кластера
            if (offset_in_cluster >= cluster_size){
                offset_in_cluster = 0;
                uint16_t next = fat12_get_next_cluster(vol,cluster);
                if (next >= 0xFF8) return FS_CORRUPTED;
                if (next == 0xFF7) return FS_CORRUPTED;
                if (next < 2) return FS_CORRUPTED;
                cluster = next;
            }
            continue;
        }
        //Нечётное чтение: позиция или размер не выровнены по сектору
        //Читаем один сектор во временный буфер
        int r = fat12_read_sectors(vol,cluster_lba + first_sector,1,sector_buffer);
        if (r != FS_OK) return r;
        uint32_t available_in_sector = vol->bytes_per_sector - sector_offset;
        uint32_t chunk = to_read;
        if (chunk > available_in_sector) chunk = available_in_sector;
        memcpy(dst,sector_buffer + sector_offset,chunk);
        dst += chunk;
        remaining -= chunk;
        offset_in_cluster += chunk;
        //Если закончили кластер — переходим к следующему
        if (offset_in_cluster >= cluster_size){
            offset_in_cluster = 0;
            if (remaining == 0) break;
            uint16_t next = fat12_get_next_cluster(vol,cluster);
            if (next >= 0xFF8) return FS_CORRUPTED;
            if (next == 0xFF7) return FS_CORRUPTED;
            if (next < 2) return FS_CORRUPTED;
            cluster = next;
        }
    }
    //Возвращаем количество прочитанных байт
    return (int)size;
}


static int fat12_make_83(const char *src,char out[11]){
    if (!src || !out) return FS_IO_ERROR;
    //Заполняем всё пробелами
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int pos = 0;
    int ext = 0;
    //Имя файла
    while (*src && *src != '.'){
        if (pos >= 8) return FS_INVALID_PATH;
        char c = *src++;
        //FAT 8.3 используем в верхнем регистре
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        out[pos++] = c;
    }
    //Расширение
    if (*src == '.'){
        src++;
        ext = 1;
        int ext_pos = 0;
        while (*src){
            if (ext_pos >= 3) return FS_INVALID_PATH;
            char c = *src++;
            if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
            out[8 + ext_pos] = c;
            ext_pos++;
        }
    }
    //Имя без расширения тоже допустимо
    (void)ext;
    return FS_OK;
}



static int fat12_root_readdir(fat12_file_t *file,fs_dirent_t *out){
    if (!file || !out) return FS_IO_ERROR;
    fat12_fs_t *vol = file->vol;
    if (!vol || !vol->mounted) return FS_IO_ERROR;
    //Root directory FAT12 состоит из root_size секторов по bytes_per_sector байт
    uint32_t root_bytes = vol->root_size * vol->bytes_per_sector;
    //Конец каталога
    if (file->position >= root_bytes) return FS_EOF;
    uint8_t buffer[512];
    while (file->position < root_bytes){
        uint32_t sector_index = file->position / vol->bytes_per_sector;
        uint32_t entry_offset = file->position % vol->bytes_per_sector;
        uint32_t lba = vol->root_start + sector_index;
        int r = fat12_read_sectors(vol,lba,1,buffer);
        if (r != FS_OK) return r;
        fat12_dirent_t *entry = (fat12_dirent_t *)(buffer + entry_offset);
        //Следующая запись
        file->position += sizeof(fat12_dirent_t);
        uint8_t first = (uint8_t)entry->name[0];
        //0x00 - дальше записей больше нет
        if (first == 0x00) return FS_EOF;
        //Удаленная запись
        if (first == 0xE5) continue;
        //Long File Name
        if ((entry->attr & 0x0F) == 0x0F) continue;
        //Пока просто возвращаем FAT 8.3 имя в обычном виде
        int n = 0;
        for (int i = 0; i < 11 && n < 255; i++){
            if (i == 8){
                //Если есть расширение
                if (entry->name[8] != ' ') out->name[n++] = '.';
            }
            char c = entry->name[i];
            if (c == ' ') continue;
            out->name[n++] = c;
        }
        out->name[n] = '\0';
        out->id = file->position / sizeof(fat12_dirent_t);
        out->size = entry->size;
        out->attributes = 0;
        if (entry->attr & 0x10) out->attributes |= FS_ATTR_DIRECTORY;
        if (entry->attr & 0x01) out->attributes |= FS_ATTR_READONLY;
        if (entry->attr & 0x02) out->attributes |= FS_ATTR_HIDDEN;
        if (entry->attr & 0x04) out->attributes |= FS_ATTR_SYSTEM;
        if (entry->attr & 0x20) out->attributes |= FS_ATTR_ARCHIVE;
        return FS_OK;
    }
    return FS_EOF;
}


// Запись значения в RAM-копию таблицы FAT (Логика обратная fat12_get_next_cluster)
static void fat12_set_cluster_value(fat12_fs_t *vol, uint16_t cluster, uint16_t value){
    uint32_t offset = cluster + (cluster / 2);
    value &= 0x0FFF;
    if (cluster & 1) {
        vol->fat[offset] = (vol->fat[offset] & 0x0F) | ((value & 0x0F) << 4);
        vol->fat[offset + 1] = (value >> 4) & 0xFF;
    } else {
        vol->fat[offset] = value & 0xFF;
        vol->fat[offset + 1] = (vol->fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F);
    }
}

// Принудительный сброс FAT из RAM на дискету (пишем в обе копии FAT для надежности)
static int fat12_flush_fat(fat12_fs_t *vol){
    // Пишем в FAT #1
    int r = fat12_write_sectors(vol, vol->reserved_sectors, vol->sectors_per_fat, vol->fat);
    if (r != FS_OK) return r;
    // Пишем в FAT #2 (если она есть)
    if (vol->fat_count > 1) {
        fat12_write_sectors(vol, vol->reserved_sectors + vol->sectors_per_fat, vol->sectors_per_fat, vol->fat);
    }
    return FS_OK;
}

// Поиск первого свободного кластера (содержит 0x000) и маркировка его как EOF (0xFFF)
static int fat12_allocate_cluster(fat12_fs_t *vol, uint16_t *out_cluster){
    // Кластеры 0 и 1 зарезервированы, ищем со 2-го до конца диска
    uint32_t max_clusters = (vol->sectors_per_fat * vol->bytes_per_sector * 2) / 3;

    for (uint16_t c = 2; c < max_clusters; c++){
        if (fat12_get_next_cluster(vol, c) == 0x000) {
            fat12_set_cluster_value(vol, c, 0xFFF); // Временно помечаем как конец файла
            *out_cluster = c;
            return FS_OK;
        }
    }
    return FS_IO_ERROR; // Диск заполнен (No space left on device)
}

static int fat12_update_dirent(fat12_fs_t *vol, uint16_t parent_cluster, uint32_t entry_id, uint16_t start_cluster, uint32_t file_size){
    uint32_t entries_per_sector = vol->bytes_per_sector / sizeof(fat12_dirent_t);
    uint32_t sector_index = entry_id / entries_per_sector;
    uint32_t entry_offset = (entry_id % entries_per_sector) * sizeof(fat12_dirent_t);

    uint8_t buffer[512] __attribute__((aligned(4)));

    // Вычисляем старт чтения секторов в зависимости от типа родительской папки
    uint32_t scan_start = (parent_cluster == 0) ? vol->root_start : (vol->data_start + (uint32_t)(parent_cluster - 2) * vol->sectors_per_cluster);
    uint32_t lba = scan_start + sector_index;

    int r = fat12_read_sectors(vol, lba, 1, buffer);
    if (r != FS_OK) return r;

    fat12_dirent_t *entry = (fat12_dirent_t *)(buffer + entry_offset);
    entry->cluster_low = start_cluster;
    entry->size = file_size;

    return fat12_write_sectors(vol, lba, 1, buffer);
}


static int fat12_write_file_at(fat12_fs_t *vol, uint16_t *start_cluster, uint32_t position, const void *buffer, uint32_t size){
    if (!vol || !buffer || size == 0) return 0;

    uint32_t cluster_size = vol->sectors_per_cluster * vol->bytes_per_sector;

    // Если у файла еще нет кластеров (новый пустой файл)
    if (*start_cluster < 2){
        uint16_t new_c;
        if (fat12_allocate_cluster(vol, &new_c) != FS_OK) return FS_IO_ERROR;
        *start_cluster = new_c;
    }

    uint16_t cluster = *start_cluster;
    uint32_t cluster_index = position / cluster_size;
    uint32_t offset_in_cluster = position % cluster_size;

    // Шагаем до нужной позиции
    for (uint32_t i = 0; i < cluster_index; i++){
        uint16_t next = fat12_get_next_cluster(vol, cluster);
        if (next >= 0xFF8){ // Достигли конца файла, но позиция записи лежит дальше, расширим файл
            uint16_t new_c;
            if (fat12_allocate_cluster(vol, &new_c) != FS_OK) return FS_IO_ERROR;
            fat12_set_cluster_value(vol, cluster, new_c);
            next = new_c;
        }
        cluster = next;
    }

    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t remaining = size;

    while (remaining > 0){
        uint32_t cluster_lba = vol->data_start + ((uint32_t)(cluster - 2) * vol->sectors_per_cluster);
        uint32_t available = cluster_size - offset_in_cluster;
        uint32_t to_write = remaining;
        if (to_write > available) to_write = available;

        uint32_t first_sector = offset_in_cluster / vol->bytes_per_sector;
        uint32_t sector_offset = offset_in_cluster % vol->bytes_per_sector;

        uint8_t sector_buffer[512];

        // Простой случай- пишем полный сектор/сектора напрямую из буфера
        if (sector_offset == 0 && to_write >= vol->bytes_per_sector) {
            uint32_t full_sectors = to_write / vol->bytes_per_sector;
            uint32_t bytes = full_sectors * vol->bytes_per_sector;

            int r = fat12_write_sectors(vol, cluster_lba + first_sector, full_sectors, src);
            if (r != FS_OK) return r;

            src += bytes;
            remaining -= bytes;
            offset_in_cluster += bytes;
        }
        //Сложный случай (Read-Modify-Write) - пишем часть сектора
        else {
            //Читаем старый сектор, чтобы сохранить неизменяемые байты
            int r = fat12_read_sectors(vol, cluster_lba + first_sector, 1, sector_buffer);
            if (r != FS_OK) return r;

            uint32_t available_in_sector = vol->bytes_per_sector - sector_offset;
            uint32_t chunk = to_write;
            if (chunk > available_in_sector) chunk = available_in_sector;

            // Накладываем новые данные поверх старых
            memcpy(sector_buffer + sector_offset, src, chunk);
            // Пишем сектор обратно
            r = fat12_write_sectors(vol, cluster_lba + first_sector, 1, sector_buffer);
            if (r != FS_OK) return r;

            src += chunk;
            remaining -= chunk;
            offset_in_cluster += chunk;
        }

        // Если заполнили текущий кластер и данные еще остались, выделяем следующий
        if (offset_in_cluster >= cluster_size){
            offset_in_cluster = 0;
            if (remaining == 0) break;

            uint16_t next = fat12_get_next_cluster(vol, cluster);
            if (next >= 0xFF8){
                uint16_t new_c;
                if (fat12_allocate_cluster(vol, &new_c) != FS_OK) return FS_IO_ERROR;
                fat12_set_cluster_value(vol, cluster, new_c);
                next = new_c;
            }
            cluster = next;
        }
    }
    // Сохраняем изменения таблицы FAT на физический диск
    fat12_flush_fat(vol);

    return (int)size;
}

// Функция перевода упакованных даты и времени MS-DOS в Unix Timestamp (секунды с 1970 года)
static uint32_t fat12_dos_to_unix_time(uint16_t dos_date, uint16_t dos_time){
    if (dos_date == 0) return 0; // Время не установлено

    uint32_t second = (dos_time & 0x1F) * 2;
    uint32_t minute = (dos_time >> 5) & 0x3F;
    uint32_t hour   = (dos_time >> 11) & 0x1F;

    uint32_t day    = dos_date & 0x1F;
    uint32_t month  = (dos_date >> 5) & 0x0F;
    uint32_t year   = ((dos_date >> 9) & 0x7F) + 1980; // Год FAT начинается с 1980

    // Простой и быстрый алгоритм подсчета секунд для ядра (без тяжелых библиотек)
    // Массив количества дней с начала года до текущего месяца (для обычного года)
    static const uint16_t days_before_month[] = {
        0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;

    // Считаем количество прошедших лет с 1970 года
    uint32_t total_years = year - 1970;
    // Считаем количество високосных дней с 1970 года (каждый 4-й год)
    // 1972 был первым високосным годом после 1970
    uint32_t leap_days = (total_years + 1) / 4;
    // Считаем общее количество дней
    uint32_t total_days = total_years * 365 + leap_days + days_before_month[month] + (day - 1);
    // Корректировка, если текущий год високосный и мы еще не прошли февраль
    if ((year % 4 == 0) && (month <= 2)){
        total_days--;
    }
    // Переводим всё в секунды
    uint32_t unix_timestamp = (total_days * 86400) + (hour * 3600) + (minute * 60) + second;
    return unix_timestamp;
}


int fat12_lookup_in_dir(fat12_fs_t *vol, uint16_t dir_cluster, const char *name83, fat12_dirent_t *out_entry, uint32_t *out_entry_id){
    if (!vol || !name83 || !out_entry || !out_entry_id) return FS_IO_ERROR;
    if (!vol->mounted) return FS_IO_ERROR;

    uint8_t buffer[512] __attribute__((aligned(4)));
    uint32_t entries_per_sector = vol->bytes_per_sector / sizeof(fat12_dirent_t);
    uint32_t current_entry_index = 0;

    //СЦЕНАРИЙ 1- поиск в корневом каталоге дискеты (у него фиксированные секторы)
    if (dir_cluster == 0){
        for (uint32_t sector = 0; sector < vol->root_size; sector++){
            uint32_t lba = vol->root_start + sector;
            if (fat12_read_sectors(vol, lba, 1, buffer) != FS_OK) return FS_IO_ERROR;

            fat12_dirent_t *entries = (fat12_dirent_t *)buffer;
            for (uint32_t i = 0; i < entries_per_sector; i++){
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00) return FS_NOT_FOUND; // Конец каталога
                if (first == 0xE5 || (entries[i].attr & 0x0F) == 0x0F) {
                    current_entry_index++; continue; // Удален или LFN
                }
                if (memcmp(entries[i].name, name83, 11) == 0){
                    memcpy(out_entry, &entries[i], sizeof(fat12_dirent_t));
                    *out_entry_id = current_entry_index;

                    return FS_OK;
                }
                current_entry_index++;
            }
        }
        return FS_NOT_FOUND;
    }

    //СЦЕНАРИЙ 2 - поиск внутри подкаталога (читаем секторы через цепочку кластеров в FAT)
    uint16_t cluster = dir_cluster;
    uint32_t cluster_size_sectors = vol->sectors_per_cluster;

    while (cluster < 0xFF8){
        if (cluster < 2 || cluster == 0xFF7) return FS_CORRUPTED;
        uint32_t cluster_lba = vol->data_start + ((uint32_t)(cluster - 2) * cluster_size_sectors);

        for (uint32_t sector = 0; sector < cluster_size_sectors; sector++){
            if (fat12_read_sectors(vol, cluster_lba + sector, 1, buffer) != FS_OK) return FS_IO_ERROR;

            fat12_dirent_t *entries = (fat12_dirent_t *)buffer;
            for (uint32_t i = 0; i < entries_per_sector; i++){
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00) return FS_NOT_FOUND;
                if (first == 0xE5 || (entries[i].attr & 0x0F) == 0x0F){
                    current_entry_index++; continue;
                }

                if (memcmp(entries[i].name, name83, 11) == 0){
                    memcpy(out_entry, &entries[i], sizeof(fat12_dirent_t));
                    *out_entry_id = current_entry_index; // Порядковый номер внутри этого подкаталога
                    return FS_OK;
                }
                current_entry_index++;
            }
        }
        // Переходим к следующему кластеру подкаталога по таблице FAT
        cluster = fat12_get_next_cluster(vol, cluster);
    }

    return FS_NOT_FOUND;
}


int fat12_find_parent_cluster(fat12_fs_t *vol, const char *path, uint16_t *out_parent_cluster, char out_child_name83[11]){
    if (!vol || !path || !out_parent_cluster || !out_child_name83) return FS_IO_ERROR;
    uint16_t current_dir_cluster = 0; // Стартуем с корня (0)

    //Копируем длинный путь VFS в зону .bss драйвера
    strncpy((char *)fat12_path_buffer_ptr, path, FAT12_PATH_MAX - 1);
    fat12_path_buffer_ptr[FAT12_PATH_MAX - 1] = '\0';

    char *p = (char *)fat12_path_buffer_ptr;
    if (*p == '/') p++;
    if (*p == '\0') return FS_INVALID_PATH;

    char *token = strtok(p, "/");
    char *next_token = strtok(NULL, "/");

    if (token == NULL) return FS_INVALID_PATH;

    while (next_token != NULL){
        char token83[11];
        if (fat12_make_83(token, token83) != FS_OK) return FS_INVALID_PATH;

        fat12_dirent_t dir_entry;
        uint32_t dummy_id;

        // Ищем подпапку на текущем уровне
        int r = fat12_lookup_in_dir(vol, current_dir_cluster, token83, &dir_entry, &dummy_id);
        if (r != FS_OK) return FS_NOT_FOUND;

        // Проверяем, что это действительно папка, а не обычный файл
        if (!(dir_entry.attr & 0x10)) return FS_INVALID_PATH;

        current_dir_cluster = dir_entry.cluster_low; // Спускаемся внутрь
        token = next_token;
        next_token = strtok(NULL, "/");
    }
    // В token осталось финальное имя файла. Конвертируем его в 8.3 наружу в драйвер
    if (fat12_make_83(token, out_child_name83) != FS_OK) return FS_INVALID_PATH;
    *out_parent_cluster = current_dir_cluster;

    return FS_OK;
}


//  -----  внешние функции  -----

int fat12_mount(uint32_t drive){
    for(int i = 0; i < FAT12_MAX_VOLUMES; ++i){
        fat12_fs_t *vol = &fat12_volumes_ptr[i];
        if(vol->drive == drive && vol->mounted == 1) return FS_OK;
    }
    int fnd = -1;
    for(int i = 0; i < FAT12_MAX_VOLUMES; ++i){
        fat12_fs_t *vol = &fat12_volumes_ptr[i];
        if(vol->mounted == 0) {
                fnd = i;
                break;
        }
    }
    if(fnd == -1) return FS_TOO_MANY;
    fat12_fs_t *vol = &fat12_volumes_ptr[fnd];
    memset(vol,0,sizeof(fat12_fs_t));
    vol->id = fat12_volume_count;
    vol->drive = drive;
    vol->bytes_per_sector = 512;

    int r = fat12_disk_init(vol);
    if (r != FS_OK) return r;

    int rdb = fat12_read_boot_sector(vol);

    if(rdb != FS_OK) return rdb;

    int pbpb = fat12_parse_bpb(vol);
    if(pbpb != FS_OK) return pbpb;

    //Загружаем первую FAT в память
    uint32_t fat_bytes = vol->sectors_per_fat * vol->bytes_per_sector;
    if (fat_bytes > FAT12_MAX_FAT_SIZE) return FS_NOT_SUPPORTED;
    r = fat12_read_sectors(vol,vol->reserved_sectors,vol->sectors_per_fat,vol->fat);
    if (r != FS_OK) return r;
    vol->mounted = 1;
    fat12_volume_count++;

    return FS_OK;
}


int fat12_unmount(void *volume){
    if (!volume) return FS_IO_ERROR;
    fat12_fs_t *vol = (fat12_fs_t *)volume;
    if (!vol->mounted) return FS_IO_ERROR;
    //Проверяем открытые файлы/каталоги этого тома
    for (int fd = 0; fd < FAT12_MAX_OPEN_FILES; fd++){
        fat12_file_t *file = &fat12_files_ptr[fd];
        if (!file->used) continue;
        if (file->vol == vol) return FS_ACCESS_DENIED;
    }
    //Открытых объектов этого тома нет, можно размонтировать
    vol->mounted = 0;
    return FS_OK;
}


int fat12_lookup(void *vol_ptr, const char *name, void *out_ptr, uint32_t *out_entry_id){
    if (!vol_ptr || !name || !out_ptr || !out_entry_id) return FS_IO_ERROR;
    fat12_fs_t *vol = (fat12_fs_t *)vol_ptr;

    uint16_t parent_cluster = 0;
    char name83[11];

    int r = fat12_find_parent_cluster(vol, name, &parent_cluster, name83);
    if (r != FS_OK){
        // Если подкаталогов нет, ищем просто в корне
        if (fat12_make_83(name, name83) != FS_OK) return FS_INVALID_PATH;
        parent_cluster = 0;
    }

    return fat12_lookup_in_dir(vol, parent_cluster, name83, (fat12_dirent_t *)out_ptr, out_entry_id);
}

int fat12_touch(void *vol_ptr, const char *path){
    if (!vol_ptr || !path) return FS_IO_ERROR;
    fat12_fs_t *vol = (fat12_fs_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    uint16_t parent_cluster = 0;
    char name83[11];

    //Находим кластер родительской папки и имя файла в формате 8.3
    int r = fat12_find_parent_cluster(vol, path, &parent_cluster, name83);
    if (r != FS_OK){
        // Если подкаталогов нет в пути, значит создаем в корне
        if (fat12_make_83(path, name83) != FS_OK) return FS_INVALID_PATH;
        parent_cluster = 0;
    }

    //Проверяем, существует ли уже файл с таким именем СТРОГО в этой папке
    fat12_dirent_t dummy_entry;
    uint32_t dummy_id;
    if (fat12_lookup_in_dir(vol, parent_cluster, name83, &dummy_entry, &dummy_id) == FS_OK){
        return FS_ALREADY_EXISTS;
    }

    //Вычисляем границы секторов для поиска свободного слота dirent
    uint32_t scan_start = (parent_cluster == 0) ? vol->root_start : (vol->data_start + (uint32_t)(parent_cluster - 2) * vol->sectors_per_cluster);
    uint32_t scan_sectors = (parent_cluster == 0) ? vol->root_size : vol->sectors_per_cluster;

    uint8_t buffer[512] __attribute__((aligned(4)));
    uint32_t entries_per_sector = vol->bytes_per_sector / sizeof(fat12_dirent_t);

    for (uint32_t sector = 0; sector < scan_sectors; sector++){
        uint32_t lba = scan_start + sector;

        r = fat12_read_sectors(vol, lba, 1, buffer);
        if (r != FS_OK) return r;

        fat12_dirent_t *entries = (fat12_dirent_t *)buffer;

        for (uint32_t i = 0; i < entries_per_sector; i++){
            uint8_t first_byte = (uint8_t)entries[i].name[0];
            // Нашли свободный или удаленный слот (0x00 или 0xE5)
            if (first_byte == 0x00 || first_byte == 0xE5){
                memset(&entries[i], 0, sizeof(fat12_dirent_t));
                memcpy(entries[i].name, name83, 11);
                entries[i].attr = 0x20; // Флаг - Архивный файл (Regular File)
                entries[i].cluster_low = 0; // Для пустого файла кластер = 0
                entries[i].size = 0;
                entries[i].modify_date = 0x5C21; // Условная DOS дата
                entries[i].modify_time = 0x0000;
                // Записываем измененный сектор каталога обратно на диск
                return fat12_write_sectors(vol, lba, 1, buffer);
            }
        }
    }

    return FS_NO_SPACE; // Свободных слотов в этой директории больше нет
}



int fat12_open(void *vol_ptr, const char *name, uint32_t flags){
    if (!vol_ptr || !name) return FS_IO_ERROR;
    fat12_fs_t *vol = (fat12_fs_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    //Особый случай: открытие корня "/"
    if (strcmp(name, "/") == 0) {
        for (int fd = 0; fd < FAT12_MAX_OPEN_FILES; fd++) {
            if (fat12_files_ptr[fd].used == 0) {
                fat12_files_ptr[fd].used = 1;
                fat12_files_ptr[fd].vol = vol;
                fat12_files_ptr[fd].start_cluster = 0;
                fat12_files_ptr[fd].file_size = 0;
                fat12_files_ptr[fd].position = 0;
                fat12_files_ptr[fd].attributes = FS_ATTR_DIRECTORY;
                fat12_files_ptr[fd].parent_dir_cluster = 0;
                return fd;
            }
        }
        return FS_TOO_MANY;
    }

    //Поиск файла в иерархии вложенных папок
    uint16_t parent_cluster = 0;
    char name83[11];

    int r = fat12_find_parent_cluster(vol, name, &parent_cluster, name83);
    if (r != FS_OK){
        if (fat12_make_83(name, name83) != FS_OK) return FS_INVALID_PATH;
        parent_cluster = 0;
    }

    fat12_dirent_t entry;
    uint32_t entry_id = 0;
    r = fat12_lookup_in_dir(vol, parent_cluster, name83, &entry, &entry_id);

    if (r == FS_NOT_FOUND && (flags & FS_OPEN_CREATE)){
        //Вызываем вашу отлаженную функцию fat12_touch для создания пустого файла
        int touch_res = fat12_touch(vol, name);
        if (touch_res != FS_OK) return touch_res; //Возвращаем ошибку, если диск переполнен

        //Делаем повторный lookup, теперь файл гарантированно существует на дискете
        r = fat12_lookup_in_dir(vol, parent_cluster, name83, &entry, &entry_id);
    }

    if (r != FS_OK) return r; // Файл не найден

    //Ищем свободное место в таблице дескрипторов файлов
    for (int fd = 0; fd < FAT12_MAX_OPEN_FILES; fd++){
        if (fat12_files_ptr[fd].used == 0) {
            fat12_files_ptr[fd].used = 1;
            fat12_files_ptr[fd].vol = vol;
            fat12_files_ptr[fd].start_cluster = entry.cluster_low;
            fat12_files_ptr[fd].file_size = entry.size;
            fat12_files_ptr[fd].position = 0;

            // Конвертируем DOS-атрибуты в системные флаги ядра
            uint32_t core_attributes = 0;
            if (entry.attr & 0x10) core_attributes |= FS_ATTR_DIRECTORY;
            if (entry.attr & 0x01) core_attributes |= FS_ATTR_READONLY;
            fat12_files_ptr[fd].attributes = core_attributes;

            fat12_files_ptr[fd].entry_id = entry_id;

            // Фиксируем инод родителя, чтобы функция записи знала куда сбрасывать dirent
            fat12_files_ptr[fd].parent_dir_cluster = parent_cluster;

            return fd;
        }
    }
    return FS_TOO_MANY;
}





int fat12_close(int fd){
    if (fd < 0 || fd >= FAT12_MAX_OPEN_FILES) return FS_IO_ERROR;
    if (!fat12_files_ptr[fd].used) return FS_IO_ERROR;
    fat12_files_ptr[fd].used = 0;
    return FS_OK;
}


int fat12_read(int fd,void *buffer,uint32_t size){
    if (fd < 0 || fd >= FAT12_MAX_OPEN_FILES) return FS_IO_ERROR;
    fat12_file_t *file = &fat12_files_ptr[fd];

    if (file->used == false) return FS_IO_ERROR;
    if (!buffer) return FS_IO_ERROR;
    //EOF
    if (file->position >= file->file_size) return 0;
    //Не читаем за пределами файла
    uint32_t remaining = file->file_size - file->position;
    if (size > remaining) size = remaining;
    int r = fat12_read_file_at(file->vol,file->start_cluster,file->position,buffer,size);
    if (r < 0) return r;
    file->position += r;
    return r;
}



int fat12_write(int fd, const void *buffer, uint32_t size){
    if (fd < 0 || fd >= FAT12_MAX_OPEN_FILES) return FS_IO_ERROR;

    fat12_file_t *file = &fat12_files_ptr[fd];
    if (file->used == false) return FS_IO_ERROR;
    if (!buffer || size == 0) return 0;

    if (file->attributes & FS_ATTR_DIRECTORY) return FS_ACCESS_DENIED;

    uint16_t start_cluster = file->start_cluster;

    int r = fat12_write_file_at(file->vol, &start_cluster, file->position, buffer, size);
    if (r < 0) return r;

    file->position += r;
    if (file->position > file->file_size){
        file->file_size = file->position;
    }

    file->start_cluster = start_cluster;

    //Передаем file->parent_dir_cluster для честного сохранения метаданных в подпапке
    fat12_update_dirent(file->vol, file->parent_dir_cluster, file->entry_id, file->start_cluster, file->file_size);

    return r;
}


int fat12_seek(int fd,uint32_t position){
    if (fd < 0 || fd >= FAT12_MAX_OPEN_FILES) return FS_IO_ERROR;
    fat12_file_t *file = &fat12_files_ptr[fd];
    if (!file->used) return FS_IO_ERROR;
    if (position > file->file_size) return FS_IO_ERROR;
    file->position = position;
    return FS_OK;
}


int fat12_tell(int fd){
    if (fd < 0 || fd >= FAT12_MAX_OPEN_FILES) return FS_IO_ERROR;
    fat12_file_t *file = &fat12_files_ptr[fd];
    if (!file->used) return FS_IO_ERROR;
    return (int)file->position;
}


int fat12_readdir(int dir, fs_dirent_t *entry){
    if (dir < 0 || dir >= FAT12_MAX_OPEN_FILES || !entry) return FS_IO_ERROR;

    fat12_file_t *file = &fat12_files_ptr[dir];
    if (!file->used) return FS_IO_ERROR;
    // Проверяем, что это действительно каталог
    if (!(file->attributes & FS_ATTR_DIRECTORY) && !(file->attributes & 0x10)) {
        return FS_ACCESS_DENIED;
    }

    fat12_fs_t *vol = file->vol;
    uint32_t entry_size = sizeof(fat12_dirent_t); // 32 байта

    //Сценарийц 1 - Читаем КОРНЕВОЙ каталог дискеты (start_cluster == 0)
    if (file->start_cluster == 0) {
        uint32_t root_bytes = vol->root_size * vol->bytes_per_sector;
        if (file->position >= root_bytes) return FS_NOT_FOUND;

        uint8_t sector_buf[512] __attribute__((aligned(4)));

        while (file->position < root_bytes){
            uint32_t sector_index = file->position / vol->bytes_per_sector;
            uint32_t entry_offset = file->position % vol->bytes_per_sector;
            uint32_t lba = vol->root_start + sector_index;
            if (fat12_read_sectors(vol, lba, 1, sector_buf) != FS_OK) return FS_IO_ERROR;
            fat12_dirent_t *fat_entry = (fat12_dirent_t *)(sector_buf + entry_offset);

            // Сдвигаем позицию файла на следующую запись (на 32 байта вперед)
            file->position += entry_size;

            uint8_t first = (uint8_t)fat_entry->name[0];
            if (first == 0x00) return FS_NOT_FOUND; // Записей больше нет
            if (first == 0xE5 || (fat_entry->attr & 0x0F) == 0x0F) continue; // Удален или LFN

            // Декодируем имя FAT 8.3 в entry->name
            int n = 0;
            for (int i = 0; i < 11; i++){
                if (i == 8 && fat_entry->name[8] != ' ') entry->name[n++] = '.';
                if (fat_entry->name[i] == ' ') continue;
                entry->name[n++] = fat_entry->name[i];
            }
            entry->name[n] = '\0';

            entry->inode = file->position / entry_size; // Имитируем номер инода для VFS
            entry->size = fat_entry->size;
            entry->type = (fat_entry->attr & 0x10) ? FS_ATTR_DIRECTORY : 0;

            return FS_OK;
        }
        return FS_NOT_FOUND;
    }

    //Сценарий 2 - Читаем подкаталог области данных (start_cluster >= 2)
    //Подкаталог в FAT12 может расти бесконечно, поэтому читаем его кластер за кластером
    uint32_t cluster_size = vol->sectors_per_cluster * vol->bytes_per_sector;
    uint8_t block_buf[512] __attribute__((aligned(4)));

    //Подкаталоги не имеют фиксированного размера в байтах, читаем пока не встретим 0x00
    while (true) {
        uint32_t cluster_index = file->position / cluster_size;
        uint32_t offset_in_cluster = file->position % cluster_size;

        //Ищем физический кластер, в котором сейчас находится указатель file->position
        uint16_t cluster = file->start_cluster;
        bool cluster_valid = true;

        for (uint32_t i = 0; i < cluster_index; i++){
            uint16_t next = fat12_get_next_cluster(vol, cluster);
            if (next >= 0xFF8 || next < 2 || next == 0xFF7){
                cluster_valid = false;
                break;
            }
            cluster = next;
        }

        if (!cluster_valid) return FS_NOT_FOUND; // Цепочка кластеров папки закончилась

        uint32_t cluster_lba = vol->data_start + ((uint32_t)(cluster - 2) * vol->sectors_per_cluster);
        uint32_t sector_in_cluster = offset_in_cluster / vol->bytes_per_sector;
        uint32_t entry_offset_in_sector = offset_in_cluster % vol->bytes_per_sector;

        if (fat12_read_sectors(vol, cluster_lba + sector_in_cluster, 1, block_buf) != FS_OK){
            return FS_IO_ERROR;
        }

        fat12_dirent_t *fat_entry = (fat12_dirent_t *)(block_buf + entry_offset_in_sector);

        // Шагаем по позиции вперед
        file->position += entry_size;

        uint8_t first = (uint8_t)fat_entry->name[0];
        if (first == 0x00) return FS_NOT_FOUND; // Конец записей в подкаталоге
        if (first == 0xE5 || (fat_entry->attr & 0x0F) == 0x0F) continue; // Пропускаем удаленные/LFN

        // Декодируем 8.3 имя
        int n = 0;
        for (int i = 0; i < 11; i++){
            if (i == 8 && fat_entry->name[8] != ' ') entry->name[n++] = '.';
            if (fat_entry->name[i] == ' ') continue;
            entry->name[n++] = fat_entry->name[i];
        }
        entry->name[n] = '\0';

        entry->inode = file->position / entry_size;
        entry->size = fat_entry->size;
        entry->type = (fat_entry->attr & 0x10) ? FS_ATTR_DIRECTORY : 0;

        return FS_OK;
    }

    return FS_NOT_FOUND;
}


int fat12_stat(void *vol_ptr, const char *path, fs_stat_t *st){
    if (!vol_ptr || !path || !st) return FS_IO_ERROR;
    fat12_fs_t *vol = (fat12_fs_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    //Особый случай - запрос информации о корневом каталоге "/"
    if (path[0] == '/' && path[1] == '\0'){
        st->size = 0; // Для каталогов в FAT размер традиционно 0
        st->attributes = FS_ATTR_DIRECTORY;
        st->create_time = 0;
        st->modify_time = 0;
        return FS_OK;
    }
    //Разбор пути
    uint16_t parent_cluster = 0;
    char name83[11];

    int r = fat12_find_parent_cluster(vol, path, &parent_cluster, name83);
    if (r != FS_OK){
        // Если подкаталогов нет в пути, значит элемент лежит в корне
        if (fat12_make_83(path, name83) != FS_OK) return FS_INVALID_PATH;
        parent_cluster = 0;
    }

    fat12_dirent_t entry;
    uint32_t entry_id = 0;
    //Ищем целевой элемент строго внутри найденного родительского кластера
    r = fat12_lookup_in_dir(vol, parent_cluster, name83, &entry, &entry_id);
    if (r != FS_OK) return r;

    //Заполняем структуру ответа для VFS ядра
    st->size = entry.size;
    st->attributes = 0;

    // Конвертируем DOS-атрибуты в системные флаги VFS
    if (entry.attr & 0x01) st->attributes |= FS_ATTR_READONLY;
    if (entry.attr & 0x02) st->attributes |= FS_ATTR_HIDDEN;
    if (entry.attr & 0x04) st->attributes |= FS_ATTR_SYSTEM;
    if (entry.attr & 0x10) st->attributes |= FS_ATTR_DIRECTORY;
    if (entry.attr & 0x20) st->attributes |= FS_ATTR_ARCHIVE;

    // Декодируем DOS-время в Unix Timestamp с помощью вашей функции
    st->create_time = fat12_dos_to_unix_time(entry.create_date, entry.create_time);
    st->modify_time = fat12_dos_to_unix_time(entry.modify_date, entry.modify_time);

    return FS_OK;
}


void *fat12_get_volume(uint32_t id){
    if(id >= FAT12_MAX_VOLUMES) return 0;
    for(int i = 0; i < FAT12_MAX_VOLUMES; ++i){
        fat12_fs_t *vol = &fat12_volumes_ptr[i];
        if(vol->drive == id && vol->mounted == 1) return &fat12_volumes_ptr[id];
    }

    return FS_OK;
}


int fat12_mkdir(void *vol_ptr, const char *path){
    if (!vol_ptr || !path) return FS_IO_ERROR;
    fat12_fs_t *vol = (fat12_fs_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    uint16_t parent_cluster = 0;
    char name83[11];

    int r = fat12_find_parent_cluster(vol, path, &parent_cluster, name83);
    if (r != FS_OK){
        if (fat12_make_83(path, name83) != FS_OK) return FS_INVALID_PATH;
        parent_cluster = 0;
    }

    // Проверяем, что такого имени еще нет
    fat12_dirent_t dummy; uint32_t dummy_id;
    if (fat12_lookup_in_dir(vol, parent_cluster, name83, &dummy, &dummy_id) == FS_OK){
        return FS_ALREADY_EXISTS;
    }

    // Выделяем новый кластер под данные создаваемой папки
    uint16_t new_cluster = 0;
    if (fat12_allocate_cluster(vol, &new_cluster) != FS_OK) return FS_NO_SPACE;

    // Инициализируем содержимое нового кластера системными точками . и ..
    uint8_t local_buf[512];
    memset(local_buf, 0, 512);
    uint32_t entries_per_sector = vol->bytes_per_sector / sizeof(fat12_dirent_t);

    fat12_dirent_t *dot = (fat12_dirent_t *)&local_buf[0];
    memset(dot->name, ' ', 11); dot->name[0] = '.';
    dot->attr = 0x10; // Subdirectory
    dot->cluster_low = new_cluster;

    fat12_dirent_t *dotdot = (fat12_dirent_t *)&local_buf[sizeof(fat12_dirent_t)];
    memset(dotdot->name, ' ', 11); dotdot->name[0] = '.'; dotdot->name[1] = '.';
    dotdot->attr = 0x10;
    dotdot->cluster_low = parent_cluster; // Указывает на родителя

    // Физически пишем этот сектор в область данных нового кластера
    uint32_t new_cluster_lba = vol->data_start + ((uint32_t)(new_cluster - 2) * vol->sectors_per_cluster);
    if (fat12_write_sectors(vol, new_cluster_lba, 1, local_buf) != FS_OK) return FS_IO_ERROR;

    // Теперь вставляем запись о новой папке в родительский каталог
    uint32_t scan_start = (parent_cluster == 0) ? vol->root_start : (vol->data_start + (uint32_t)(parent_cluster - 2) * vol->sectors_per_cluster);
    uint32_t scan_sectors = (parent_cluster == 0) ? vol->root_size : vol->sectors_per_cluster;

    // В условии цикла теперь проверяется правильная переменная 's'
    for (uint32_t s = 0; s < scan_sectors; s++) {
        if (fat12_read_sectors(vol, scan_start + s, 1, local_buf) != FS_OK) return FS_IO_ERROR;
        fat12_dirent_t *entries = (fat12_dirent_t *)local_buf;

        for (uint32_t i = 0; i < entries_per_sector; i++) {
            uint8_t first = entries[i].name[0];
            if (first == 0x00 || first == 0xE5) {
                // Нашли пустой слот!
                memset(&entries[i], 0, sizeof(fat12_dirent_t));
                memcpy(entries[i].name, name83, 11);
                entries[i].attr = 0x10; // КАТАЛОГ
                entries[i].cluster_low = new_cluster;
                entries[i].size = 0;

                // Сбрасываем обновленный сектор родителя на диск
                return fat12_write_sectors(vol, scan_start + s, 1, local_buf);
            }
        }
    }

    return FS_NO_SPACE;
}


int fat12_unlink(void *vol_ptr, const char *path){
    if (!vol_ptr || !path) return FS_IO_ERROR;
    fat12_fs_t *vol = (fat12_fs_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    uint16_t parent_cluster = 0;
    char name83[11];

    int r = fat12_find_parent_cluster(vol, path, &parent_cluster, name83);
    if (r != FS_OK) {
        if (fat12_make_83(path, name83) != FS_OK) return FS_INVALID_PATH;
        parent_cluster = 0;
    }

    fat12_dirent_t entry;
    uint32_t entry_id = 0;
    // Находим файл
    if (fat12_lookup_in_dir(vol, parent_cluster, name83, &entry, &entry_id) != FS_OK) {
        return FS_NOT_FOUND;
    }

    // Не разрешаем удалять папки через unlink
    if (entry.attr & 0x10) return FS_ACCESS_DENIED;

    // Освобождаем цепочку кластеров файла в таблице FAT
    uint16_t cluster = entry.cluster_low;
    while (cluster >= 2 && cluster < 0xFF8) {
        uint16_t next = fat12_get_next_cluster(vol, cluster);
        fat12_set_cluster_value(vol, cluster, 0x000); //Помечаем кластер как свободный
        cluster = next;
    }
    fat12_flush_fat(vol); // Сбрасываем изменения таблицы FAT на дискету

    //Помечаем запись в родительском каталоге как удаленную (0xE5)
    uint32_t entries_per_sector = vol->bytes_per_sector / sizeof(fat12_dirent_t);
    uint32_t sector_index = entry_id / entries_per_sector;
    uint32_t entry_offset = (entry_id % entries_per_sector) * sizeof(fat12_dirent_t);

    uint8_t local_buf[512];
    uint32_t scan_start = (parent_cluster == 0) ? vol->root_start : (vol->data_start + (parent_cluster - 2) * vol->sectors_per_cluster);
    uint32_t target_lba = scan_start + sector_index;

    if (fat12_read_sectors(vol, target_lba, 1, local_buf) != FS_OK) return FS_IO_ERROR;

    fat12_dirent_t *entries = (fat12_dirent_t *)(local_buf + entry_offset);
    entries->name[0] = 0xE5; //Маркер DOS-удаления файла

    return fat12_write_sectors(vol, target_lba, 1, local_buf);
}


int fat12_rmdir(void *vol_ptr, const char *path){
    if (!vol_ptr || !path) return FS_IO_ERROR;
    fat12_fs_t *vol = (fat12_fs_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    uint16_t parent_cluster = 0;
    char name83[11];

    //Находим кластер родительской папки и имя удаляемой папки в формате 8.3
    int r = fat12_find_parent_cluster(vol, path, &parent_cluster, name83);
    if (r != FS_OK) {
        if (fat12_make_83(path, name83) != FS_OK) return FS_INVALID_PATH;
        parent_cluster = 0;
    }
    //Находим запись удаляемой папки внутри родителя
    fat12_dirent_t entry;
    uint32_t entry_id = 0;
    r = fat12_lookup_in_dir(vol, parent_cluster, name83, &entry, &entry_id);
    if (r != FS_OK) return FS_NOT_FOUND;

    //Тут защита, rmdir предназначен исключительно для каталогов
    if (!(entry.attr & 0x10)) return FS_ACCESS_DENIED;

    uint16_t target_cluster = entry.cluster_low;
    if (target_cluster < 2) return FS_CORRUPTED; // Корень удалять нельзя

    uint32_t cluster_size_sectors = vol->sectors_per_cluster;
    uint32_t entries_per_sector = vol->bytes_per_sector / sizeof(fat12_dirent_t);

    // Теперь это честный 512-байтовый массив на стеке
    uint8_t buffer[512] __attribute__((aligned(4)));

    uint16_t scan_cluster = target_cluster;
    while (scan_cluster >= 2 && scan_cluster < 0xFF8){
        uint32_t cluster_lba = vol->data_start + ((uint32_t)(scan_cluster - 2) * cluster_size_sectors);

        for (uint32_t s = 0; s < cluster_size_sectors; s++){
            if (fat12_read_sectors(vol, cluster_lba + s, 1, buffer) != FS_OK) return FS_IO_ERROR;
            fat12_dirent_t *sub_entries = (fat12_dirent_t *)buffer;

            for (uint32_t i = 0; i < entries_per_sector; i++){
                uint8_t first = (uint8_t)sub_entries[i].name[0];

                if (first == 0x00) break; // Дальше записей в секторе нет
                if (first == 0xE5 || (sub_entries[i].attr & 0x0F) == 0x0F) continue; // Удален или LFN
                // В FAT12 пустая папка содержит только записи "." и "..".
                // Если мы встретили имя, которое не начинается с точки — каталог не пуст
                if (sub_entries[i].name[0] != '.') {
                    return FS_ACCESS_DENIED; // Ошибка: каталог содержит файлы, удаление запрещено
                }
            }
        }
        scan_cluster = fat12_get_next_cluster(vol, scan_cluster);
    }

    //Освободим цепочку кластеров удаляемой папки в FAT
    uint16_t free_cluster = target_cluster;
    while (free_cluster >= 2 && free_cluster < 0xFF8) {
        uint16_t next = fat12_get_next_cluster(vol, free_cluster);
        fat12_set_cluster_value(vol, free_cluster, 0x000); // Обнуляем кластер в FAT
        free_cluster = next;
    }
    fat12_flush_fat(vol); // Сбрасываем обновленную FAT на дискету

    //Маркируем запись о папке в родительском каталоге как удаленную (0xE5)
    uint32_t scan_start = (parent_cluster == 0) ? vol->root_start : (vol->data_start + (uint32_t)(parent_cluster - 2) * vol->sectors_per_cluster);
    uint32_t sector_index = entry_id / entries_per_sector;
    uint32_t entry_offset = (entry_id % entries_per_sector) * sizeof(fat12_dirent_t);
    uint32_t target_lba = scan_start + sector_index;

    if (fat12_read_sectors(vol, target_lba, 1, buffer) != FS_OK) return FS_IO_ERROR;

    fat12_dirent_t *parent_entries = (fat12_dirent_t *)(buffer + entry_offset);
    parent_entries->name[0] = 0xE5; // Выставляем маркер DOS-удаления

    return fat12_write_sectors(vol, target_lba, 1, buffer);
}








void fs_init(struct boot_info *boot){
    //fat12_mount(boot);
    //for(int i = 0 ; i < FAT12_MAX_OPEN_FILES; i++){
        //fat12_files[i].used = 0;
    //}
}


void driver_main(struct boot_info *boot){
    fs_init(boot);

    api_ptr = &api;
    //fs_driver_api_t *a = &api;

    api_ptr->mount = fat12_mount;
    api_ptr->unmount = fat12_unmount;
    api_ptr->lookup = fat12_lookup;
    api_ptr->open = fat12_open;
    api_ptr->touch = fat12_touch;
    api_ptr->close = fat12_close;
    api_ptr->write = fat12_write;
    api_ptr->seek = fat12_seek;
    api_ptr->tell = fat12_tell;
    api_ptr->readdir = fat12_readdir;
    api_ptr->mkdir = fat12_mkdir;
    api_ptr->rmdir = fat12_rmdir;
    api_ptr->stat = fat12_stat;
    api_ptr->get_volume = fat12_get_volume;
    api_ptr->read = fat12_read;
    api_ptr->unlink = fat12_unlink;
    //api_ptr->set_floppy_irq_fired_ptr = set_floppy_irq_fired_ptr;

    fat12_files_ptr = fat12_files;
    for(int i = 0 ; i < FAT12_MAX_OPEN_FILES; i++){
        fat12_files_ptr[i].used = 0;
    }

    fat12_volumes_ptr = fat12_volumes;
    for(int i = 0 ; i < FAT12_MAX_VOLUMES; i++){
        fat12_volumes_ptr[i].mounted = 0;
    }

    fat12_path_buffer_ptr = fat12_path_buffer;
}


fs_driver_api_t *driver_get_api(void){
    return &api;
}

