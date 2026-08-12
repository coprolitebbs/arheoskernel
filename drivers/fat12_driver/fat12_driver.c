#include "include/fat12_driver.h"
#include "include/fat12_internal.h"
#include "include/fat12_boot.h"
#include "include/fat12_disk.h"
#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/lib.h"


//fat12_fs_t fat12;
fat12_fs_t fat12_volumes[FAT12_MAX_VOLUMES];
uint32_t fat12_volume_count;

fat12_file_t fat12_files[FAT12_MAX_OPEN_FILES];

fs_driver_api_t api;


//  ----- внутренние функции  -----


int fat12_read_file_at(
    fat12_fs_t *vol,
    uint16_t start_cluster,
    uint32_t position,
    void *buffer,
    uint32_t size
)
{
    if (!vol || !buffer)
        return FS_IO_ERROR;

    if (!vol->mounted)
        return FS_IO_ERROR;

    if (size == 0)
        return 0;

    /*
     * --------------------------------------------------------
     * Размер кластера в байтах.
     * --------------------------------------------------------
     */

    uint32_t cluster_size =
        vol->sectors_per_cluster *
        vol->bytes_per_sector;

    if (cluster_size == 0)
        return FS_CORRUPTED;

    /*
     * --------------------------------------------------------
     * Начинаем с первого кластера файла.
     * --------------------------------------------------------
     */

    uint16_t cluster = start_cluster;

    if (cluster < 2)
        return FS_CORRUPTED;

    /*
     * --------------------------------------------------------
     * Пропускаем кластеры до позиции position.
     *
     * Например:
     *
     * position = 0
     *     → первый кластер
     *
     * position = cluster_size
     *     → второй кластер
     *
     * position = 2 * cluster_size
     *     → третий кластер
     * --------------------------------------------------------
     */

    uint32_t cluster_index =
        position / cluster_size;

    uint32_t offset_in_cluster =
        position % cluster_size;

    for (uint32_t i = 0;
         i < cluster_index;
         i++)
    {
        uint16_t next =
            fat12_get_next_cluster(
                vol,
                cluster
            );

        /*
         * FAT12 EOF.
         */

        if (next >= 0xFF8)
            return FS_CORRUPTED;

        /*
         * Зарезервированные / повреждённые
         * значения FAT.
         */

        if (next == 0xFF7)
            return FS_CORRUPTED;

        if (next < 2)
            return FS_CORRUPTED;

        cluster = next;
    }

    /*
     * --------------------------------------------------------
     * Теперь cluster содержит кластер,
     * в котором находится position.
     * --------------------------------------------------------
     */

    uint8_t *dst =
        (uint8_t *)buffer;

    uint32_t remaining =
        size;

    /*
     * --------------------------------------------------------
     * Читаем файл кластер за кластером.
     * --------------------------------------------------------
     */

    while (remaining > 0)
    {
        /*
         * LBA первого сектора текущего кластера:
         *
         * data_start +
         * (cluster - 2) *
         * sectors_per_cluster
         */

        uint32_t cluster_lba =
            vol->data_start +
            ((uint32_t)(cluster - 2) *
             vol->sectors_per_cluster);

        /*
         * Сколько байт осталось в текущем
         * кластере после offset_in_cluster.
         */

        uint32_t available =
            cluster_size -
            offset_in_cluster;

        uint32_t to_read =
            remaining;

        if (to_read > available)
            to_read = available;

        /*
         * ----------------------------------------------------
         * Читаем нужные сектора.
         * ----------------------------------------------------
         *
         * Сейчас FAT12_read_sectors()
         * работает с полными секторами.
         *
         * Поэтому определяем:
         *
         * first_sector
         * sector_offset
         * ----------------------------------------------------
         */

        uint32_t first_sector =
            offset_in_cluster /
            vol->bytes_per_sector;

        uint32_t sector_offset =
            offset_in_cluster %
            vol->bytes_per_sector;

        uint32_t sectors_needed =
            (sector_offset +
             to_read +
             vol->bytes_per_sector - 1) /
            vol->bytes_per_sector;

        /*
         * Если чтение начинается/заканчивается
         * не на границе сектора, нужен временный
         * буфер.
         *
         * Максимально нам понадобится
         * sectors_per_cluster * 512 байт.
         */

        uint8_t sector_buffer[512];

        /*
         * ----------------------------------------------------
         * Простой случай:
         *
         * читаем целое число секторов
         * прямо в пользовательский buffer.
         * ----------------------------------------------------
         */

        if (sector_offset == 0 &&
            to_read >= vol->bytes_per_sector)
        {
            uint32_t full_sectors =
                to_read /
                vol->bytes_per_sector;

            uint32_t bytes =
                full_sectors *
                vol->bytes_per_sector;

            int r =
                fat12_read_sectors(
                    vol,
                    cluster_lba + first_sector,
                    full_sectors,
                    dst
                );

            if (r != FS_OK)
                return r;

            dst += bytes;
            remaining -= bytes;

            /*
             * Если полностью закончили текущий
             * кластер — переходим к следующему.
             */

            if (remaining == 0)
                break;

            offset_in_cluster += bytes;

            /*
             * Если дошли до конца кластера.
             */

            if (offset_in_cluster >= cluster_size)
            {
                offset_in_cluster = 0;

                uint16_t next =
                    fat12_get_next_cluster(
                        vol,
                        cluster
                    );

                if (next >= 0xFF8)
                    return FS_CORRUPTED;

                if (next == 0xFF7)
                    return FS_CORRUPTED;

                if (next < 2)
                    return FS_CORRUPTED;

                cluster = next;
            }

            continue;
        }

        /*
         * ----------------------------------------------------
         * Нечётное чтение:
         *
         * позиция или размер не выровнены
         * по сектору.
         *
         * Читаем один сектор во временный буфер.
         * ----------------------------------------------------
         */

        int r =
            fat12_read_sectors(
                vol,
                cluster_lba + first_sector,
                1,
                sector_buffer
            );

        if (r != FS_OK)
            return r;

        uint32_t available_in_sector =
            vol->bytes_per_sector -
            sector_offset;

        uint32_t chunk =
            to_read;

        if (chunk > available_in_sector)
            chunk = available_in_sector;

        memcpy(
            dst,
            sector_buffer + sector_offset,
            chunk
        );

        dst += chunk;
        remaining -= chunk;

        offset_in_cluster += chunk;

        /*
         * ----------------------------------------------------
         * Если закончили кластер —
         * переходим к следующему.
         * ----------------------------------------------------
         */

        if (offset_in_cluster >= cluster_size)
        {
            offset_in_cluster = 0;

            if (remaining == 0)
                break;

            uint16_t next =
                fat12_get_next_cluster(
                    vol,
                    cluster
                );

            if (next >= 0xFF8)
                return FS_CORRUPTED;

            if (next == 0xFF7)
                return FS_CORRUPTED;

            if (next < 2)
                return FS_CORRUPTED;

            cluster = next;
        }
    }

    /*
     * Возвращаем количество прочитанных байт.
     */

    return (int)size;
}

//  -----  внешние функции  -----

int fat12_mount(/*boot_info_t *boot*/uint32_t drive){
    if(fat12_volume_count >= FAT12_MAX_VOLUMES) return FS_TOO_MANY;
    fat12_fs_t *vol = &fat12_volumes[fat12_volume_count];
    memset(vol,0,sizeof(fat12_fs_t));
    vol->id = fat12_volume_count;
    vol->drive = drive;
    vol->bytes_per_sector = 512;
    //vol->sectors_per_track = 18;
    //vol->heads = 2;
    int r = fat12_disk_init(vol);
    if (r != FS_OK) return r;
    int rdb = fat12_read_boot_sector(vol);
    if(rdb != FS_OK) return rdb;

    int pbpb = fat12_parse_bpb(vol);
    if(pbpb != FS_OK) return pbpb;


    /*
     * ----------------------------------------------------
     * Загружаем первую FAT в память.
     * ----------------------------------------------------
     */

    uint32_t fat_bytes = vol->sectors_per_fat * vol->bytes_per_sector;
    if (fat_bytes > FAT12_MAX_FAT_SIZE) return FS_NOT_SUPPORTED;
    r = fat12_read_sectors(vol,vol->reserved_sectors,vol->sectors_per_fat,vol->fat);
    if (r != FS_OK) return r;

    vol->mounted = 1;
    fat12_volume_count++;

    return FS_OK;
}


int fat12_unmount(void *volume){
    fat12_fs_t *vol = (fat12_fs_t *)volume;
    vol->mounted = 0;
    return FS_OK;
}


int fat12_lookup(void *vol_ptr, const char *name, void *out_ptr){
    if (!vol_ptr || !name || !out_ptr)
        return FS_IO_ERROR;

    fat12_fs_t *vol = (fat12_fs_t *)vol_ptr;
    fat12_dirent_t *out = (fat12_dirent_t *)out_ptr;

    if (!vol->mounted)
        return FS_IO_ERROR;

    uint8_t buffer[512];

    uint32_t entries_per_sector =
        vol->bytes_per_sector / sizeof(fat12_dirent_t);

    for (uint32_t sector = 0;
         sector < vol->root_size;
         sector++)
    {
        uint32_t lba =
            vol->root_start + sector;

        int r = fat12_read_sectors(
            vol,
            lba,
            1,
            buffer
        );

        if (r != FS_OK)
            return r;

        fat12_dirent_t *entries =
            (fat12_dirent_t *)buffer;

        for (uint32_t i = 0;
             i < entries_per_sector;
             i++)
        {
            fat12_dirent_t *entry =
                &entries[i];

            uint8_t first =
                (uint8_t)entry->name[0];

            /*
             * 0x00:
             * больше записей в каталоге нет.
             */
            if (first == 0x00)
                return FS_NOT_FOUND;

            /*
             * 0xE5:
             * запись удалена.
             */
            if (first == 0xE5)
                continue;

            /*
             * Long File Name.
             */
            if ((entry->attr & 0x0F) == 0x0F)
                continue;

            /*
             * Сравнение FAT 8.3.
             *
             * name должен содержать
             * ровно 11 байт.
             */
            int match = 1;

            for (int j = 0; j < 11; j++)
            {
                if ((uint8_t)entry->name[j] !=
                    (uint8_t)name[j])
                {
                    match = 0;
                    break;
                }
            }

            if (!match)
                continue;

            /*
             * Нашли запись.
             */
            memcpy(
                out,
                entry,
                sizeof(fat12_dirent_t)
            );

            return FS_OK;
        }
    }

    return FS_NOT_FOUND;
}



int fat12_open(void *vol_ptr,const char *name,uint32_t flags){
    (void)flags;
    if (!vol_ptr || !name) return FS_IO_ERROR;

    fat12_fs_t *vol = (fat12_fs_t *)vol_ptr;

    if (!vol->mounted) return FS_IO_ERROR;

    /*
     * Ищем файл
     */

    fat12_dirent_t entry;
    int r = fat12_lookup(vol,name,&entry);
    if (r != FS_OK) return r;

    /*
     * Ищем свободный FD
     */

    for (int fd = 4; fd < FAT12_MAX_OPEN_FILES; fd++){
        //fat12_file_t *file = &fat12_files[fd];

        if (fat12_files[fd].used  == 0){
            fat12_files[fd].used = 1;
            //file->used = 1;
            fat12_files[fd].vol = vol;
            fat12_files[fd].start_cluster = entry.cluster_low;
            fat12_files[fd].file_size = entry.size;
            fat12_files[fd].position = 0;
            return fd;
        }
    }
    return FS_TOO_MANY;
}


int fat12_close(int fd){
    if (fd < 0 || fd >= FAT12_MAX_OPEN_FILES) return FS_IO_ERROR;
    if (!fat12_files[fd].used) return FS_IO_ERROR;
    fat12_files[fd].used = 0;
    return FS_OK;
}


int fat12_read(int fd,void *buffer,uint32_t size){
    return fat12_files[fd].used;

    if (fd < 0 || fd >= FAT12_MAX_OPEN_FILES) return FS_IO_ERROR;
    fat12_file_t *file = &fat12_files[fd];

    if (file->used == false) return FS_IO_ERROR;
    if (!buffer) return FS_IO_ERROR;

    /*
     * EOF
     */

    if (file->position >= file->file_size) return 0;

    /*
     * Не читаем за пределами файла
     */

    uint32_t remaining = file->file_size - file->position;
    if (size > remaining) size = remaining;
    int r = fat12_read_file_at(file->vol,file->start_cluster,file->position,buffer,size);
    if (r < 0) return r;
    file->position += r;
    return r;
}


int fat12_write(int fd,const void *buffer,uint32_t size){
    (void)fd;
    (void)buffer;
    (void)size;

    return FS_NOT_SUPPORTED;
}


int fat12_seek(int fd,uint32_t position){
    if (fd < 0 || fd >= FAT12_MAX_OPEN_FILES) return FS_IO_ERROR;
    fat12_file_t *file = &fat12_files[fd];
    if (!file->used) return FS_IO_ERROR;
    if (position > file->file_size) return FS_IO_ERROR;
    file->position = position;
    return FS_OK;
}


int fat12_tell(int fd){
    if (fd < 0 || fd >= FAT12_MAX_OPEN_FILES) return FS_IO_ERROR;
    if (!fat12_files[fd].used) return FS_IO_ERROR;
    return (int)fat12_files[fd].position;
}


int fat12_readdir(int dir,fs_dirent_t *entry){
    (void)dir;
    (void)entry;

    return FS_NOT_SUPPORTED;
}


int fat12_stat(const char *path,fs_stat_t *st){
    (void)path;
    (void)st;

    return FS_NOT_SUPPORTED;
}


void *fat12_get_volume(uint32_t id)
{
    //if(id >= fat12_volume_count) return 0;
    //if(!fat12_volumes[id].mounted) return 0;
    return &fat12_volumes[id];
}

/*
void *fat12_debug_get_boot_sector(uint32_t id)
{
    if (id >= fat12_volume_count)
        return 0;

    fat12_fs_t *vol = &fat12_volumes[id];

    if (!vol->mounted)
        return 0;

    return vol->boot_sector;
}
*/

/*
int fat12_read_file(void *vol_ptr,const char *name,void *buffer,uint32_t size){
    if (!vol_ptr || !name || !buffer) return FS_IO_ERROR;

    fat12_fs_t *vol = (fat12_fs_t *)vol_ptr;
    if (!vol->mounted) return FS_IO_ERROR;

    if (size == 0) return FS_OK;

    fat12_dirent_t entry;

    int r = fat12_lookup(vol,name,&entry);
    if (r != FS_OK) return r;

    uint32_t file_size = entry.size;

    uint32_t bytes_to_read = size;

    if (bytes_to_read > file_size) bytes_to_read = file_size;

    if (bytes_to_read == 0) return FS_OK;

    uint16_t cluster = entry.cluster_low;

    if (cluster < 2) return FS_CORRUPTED;

    uint32_t cluster_size = vol->sectors_per_cluster * vol->bytes_per_sector;
    if (cluster_size == 0) return FS_CORRUPTED;

    uint8_t cluster_buffer[4096];

    if (cluster_size > sizeof(cluster_buffer)) return FS_NOT_SUPPORTED;

    uint8_t *dst = (uint8_t *)buffer;

    uint32_t remaining = bytes_to_read;

    while (remaining > 0){

        if (cluster < 2) return FS_CORRUPTED;

        if (cluster >= 0x0FF8) return FS_CORRUPTED;

        uint32_t lba = vol->data_start + ((uint32_t)(cluster - 2) * vol->sectors_per_cluster);

        r = fat12_read_sectors(vol,lba,vol->sectors_per_cluster,cluster_buffer);
        if (r != FS_OK) return r;

        uint32_t copy_size = remaining;
        if (copy_size > cluster_size) copy_size = cluster_size;
        memcpy(dst,cluster_buffer,copy_size);
        dst += copy_size;
        remaining -= copy_size;

        if (remaining == 0) break;

        uint16_t next = fat12_get_next_cluster(vol,cluster);

        if (next >= 0x0FF8) return FS_CORRUPTED;

        if (next < 2) return FS_CORRUPTED;

        cluster = next;
    }

    return FS_OK;
}
*/

uint32_t debug_ret_uint32(){
    return (uint32_t)&fat12_files[4];
}

uint32_t debug_get_fd_addr(int fd)
{
    if (fd < 0 || fd >= FAT12_MAX_OPEN_FILES)
        return 0;

    return (uint32_t)&fat12_files[fd];
}

uint32_t debug_get_fd_used(int fd)
{
    if (fd < 0 || fd >= FAT12_MAX_OPEN_FILES)
        return 0;

    return fat12_files[fd].used;
}

uint32_t debug_a(void)
{
    return (uint32_t)&fat12_files[0];
}

uint32_t debug_b(void)
{
    return (uint32_t)&fat12_files[1];
}

uint32_t debug_c(void)
{
    return (uint32_t)&fat12_files[4];
}

uint32_t debug_array(void)
{
    return (uint32_t)fat12_files;
}

uint32_t debug_size(void)
{
    return sizeof(fat12_file_t);
}

uint32_t debug_addresses(void)
{
    uint32_t a0 = (uint32_t)&fat12_files[0];
    uint32_t a1 = (uint32_t)&fat12_files[1];
    uint32_t a4 = (uint32_t)&fat12_files[4];

    /*
     * Возвращаем разницу между [4] и [0].
     */
    return a4 - a0;
}






void fs_init(struct boot_info *boot){
    //fat12_mount(boot);
    for(int i = 0 ; i < FAT12_MAX_OPEN_FILES; i++){
        fat12_files[i].used = 0;
    }
}


void driver_main(struct boot_info *boot){

    fs_init(boot);

    fs_driver_api_t *a = &api;


    a->mount = fat12_mount;
    a->unmount = fat12_unmount;
    a->lookup = fat12_lookup;
    a->open = fat12_open;
    a->close = fat12_close;
    a->write = fat12_write;
    a->seek = fat12_seek;
    a->tell = fat12_tell;
    a->readdir = fat12_readdir;
    a->stat = fat12_stat;
    a->get_volume = fat12_get_volume;
    a->read = fat12_read;

    a->debug_ret_uint32 = debug_ret_uint32;
    a->debug_get_fd_addr = debug_get_fd_addr;
    a->debug_get_fd_used = debug_get_fd_used;
    a->debug_a = debug_a;
    a->debug_b = debug_b;
    a->debug_c = debug_c;
    a->debug_array = debug_array;
    a->debug_size = debug_size;
    a->debug_addresses = debug_addresses;
    //a->read_file = fat12_read_file;
    //a->debug_get_boot_sector = fat12_debug_get_boot_sector;

}


fs_driver_api_t *driver_get_api(void){
    return &api;
}

