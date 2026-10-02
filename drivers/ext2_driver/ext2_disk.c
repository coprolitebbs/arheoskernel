#include "include/ext2_disk.h"
#include "include/ext2_internal.h"
#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"
#include "include/ext2_driver.h"


int ext2_disk_read_sectors(uint32_t lba, uint32_t count, void *buf){
    uint16_t *ptr = (uint16_t *)buf;

    //if (vol && vol->schedule_lock_ptr) *vol->schedule_lock_ptr = 1;

    for (int i = 0; i < count; i++) {
        // Выбор диска (Master) и старших 4 бит LBA
        outb(0x1F6, 0xE0 | (((lba + i) >> 24) & 0x0F));

        // Задержка 400нс для старых контроллеров и жесткого KVM-тайминга
        inb(0x1F7); inb(0x1F7); inb(0x1F7); inb(0x1F7);

        outb(0x1F2, 1); // Читаем строго по 1 сектору для стабильности PIO
        outb(0x1F3, (uint8_t)(lba + i));
        outb(0x1F4, (uint8_t)((lba + i) >> 8));
        outb(0x1F5, (uint8_t)((lba + i) >> 16));
        outb(0x1F7, 0x20); // Команда - чтение секторов с повтором

        // Ожидание готовности данных (BSY=0, DRQ=1)
        uint32_t timeout = 500000;
        uint8_t status;
        while (timeout--) {
            status = inb(0x1F7);
            if (!(status & 0x80) && (status & 0x08)) break;
        }

        // Проверяем на таймаут или бит ошибки ERR (бит 0)
        if (timeout == 0 || (status & 0x01)) return FS_IO_ERROR;

        // Обязательный сброс флага направления перед чтением строк портов
        __asm__ __volatile__("cld");

        // Читаем 256 слов (512 байт) в буфер
        insw(0x1F0, ptr + (i * 256), 256);
    }

    //if (vol && vol->schedule_lock_ptr) *vol->schedule_lock_ptr = 0;

    return FS_OK;
}




int ext2_read_block(ext2_volume_t *vol, uint32_t block, void *buf){
    if (!vol || !vol->mounted || block == 0) return FS_IO_ERROR;
    uint32_t lba = vol->partition_start_lba + (block * vol->sectors_per_block);

    int res;
    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 1;
    res = ext2_disk_read_sectors(lba, vol->sectors_per_block, buf);
    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;

    return res;
}


int ext2_read_inode(ext2_volume_t *vol, uint32_t inode_no, ext2_inode_t *out_inode){
    if (!vol || inode_no == 0 || !out_inode) return FS_IO_ERROR;

    uint32_t inodes_per_group = vol->sb.s_inodes_per_group;
    uint32_t group = (inode_no - 1) / inodes_per_group;
    uint32_t index = (inode_no - 1) % inodes_per_group;

    if (group >= vol->groups_count) return FS_IO_ERROR;

    ext2_bg_desc_t *bg_desc = (ext2_bg_desc_t *)(vol->bg_desc_table + (group * 32));
    uint32_t inode_table_start_block = bg_desc->bg_inode_table;

    if (inode_table_start_block == 0) return FS_CORRUPTED;

    uint32_t inode_table_start_lba = vol->partition_start_lba + (inode_table_start_block * vol->sectors_per_block);

    uint32_t sector_offset = index >> 2; // index / 4
    uint32_t offset_in_sector = (index & 3) << 7;  // (index % 4) * 128

    uint32_t target_lba = (uint32_t)inode_table_start_lba + (uint32_t)sector_offset;

    uint8_t *sector_buf = (uint8_t *)api_ptr->malloc_ptr(4096);
    if (!sector_buf) return FS_IO_ERROR;

    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 1;
    int r = ext2_disk_read_sectors(target_lba, 1, sector_buf);
    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;

    if (r != FS_OK) {
        api_ptr->free_ptr(sector_buf);
        return r;
    }

    memcpy(out_inode, sector_buf + offset_in_sector, sizeof(ext2_inode_t));

    api_ptr->free_ptr(sector_buf);
    return FS_OK;
}




int ext2_disk_write_sectors(uint32_t lba, uint32_t count, const void *buf){
    const uint16_t *ptr = (const uint16_t *)buf;

    for (int i = 0; i < count; i++) {
        // Выбор диска и передача LBA
        outb(0x1F6, 0xE0 | (((lba + i) >> 24) & 0x0F));

        // Дадим время контроллеру обработать выбор диска (400нс)
        inb(0x1F7); inb(0x1F7); inb(0x1F7); inb(0x1F7);

        // Ждём, пока диск освободится от прошлых операций (очистится BSY)
        uint32_t pre_timeout = 100000;
        while ((inb(0x1F7) & 0x80) && --pre_timeout);

        outb(0x1F2, 1);
        outb(0x1F3, (uint8_t)(lba + i));
        outb(0x1F4, (uint8_t)((lba + i) >> 8));
        outb(0x1F5, (uint8_t)((lba + i) >> 16));
        outb(0x1F7, 0x30); // Команда: Запись секторов

        // Ждем готовности внутреннего буфера диска к приёму данных (DRQ=1)
        uint32_t timeout = 500000;
        uint8_t status;
        while (timeout--) {
            status = inb(0x1F7);
            if (!(status & 0x80) && (status & 0x08)) break;
        }

        if (timeout == 0 || (status & 0x01)) return FS_IO_ERROR;

        __asm__ __volatile__("cld");
        outsw(0x1F0, ptr + (i * 256), 256);

        // Ждём, пока диск физически сбросит сектор на блины
        timeout = 500000;
        while (timeout--) {
            status = inb(0x1F7);
            if (!(status & 0x80)) break;
        }

        if (timeout == 0 || (status & 0x01)) return FS_IO_ERROR;
    }

    return FS_OK;
}




int ext2_write_block(ext2_volume_t *vol, uint32_t block, const void *buf) {
    if (!vol || !vol->mounted || block == 0 || !buf) return FS_IO_ERROR;

    //Если аллокатор пытается писать в системную область группы 0
    if (block == 1 || block == 2) {
        return FS_ACCESS_DENIED; // Возвращаем ошибку доступа вместо разрушения диска
    }

    uint32_t start_lba = (uint32_t)vol->partition_start_lba;
    uint32_t sec_per_blk = (uint32_t)vol->sectors_per_block;

    uint32_t lba = start_lba + (block * sec_per_blk);
    int res;
    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 1;
    res = ext2_disk_write_sectors(lba, sec_per_blk, buf);
    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;
    return res;
}


int ext2_write_inode(ext2_volume_t *vol, uint32_t inode_no, const ext2_inode_t *in_inode) {
    if (!vol || inode_no == 0 || !in_inode) return FS_IO_ERROR;

    uint32_t inodes_per_group = vol->sb.s_inodes_per_group;
    uint32_t group = (inode_no - 1) / inodes_per_group;
    uint32_t index = (inode_no - 1) % inodes_per_group;

    if (group >= vol->groups_count) return FS_IO_ERROR;

    ext2_bg_desc_t *bg_desc = (ext2_bg_desc_t *)(vol->bg_desc_table + (group * 32));
    uint32_t inode_table_start_block = bg_desc->bg_inode_table;

    if (inode_table_start_block == 0) return FS_CORRUPTED;

    uint32_t inode_table_start_lba = vol->partition_start_lba + (inode_table_start_block * vol->sectors_per_block);

    uint32_t sector_offset = index >> 2;
    uint32_t offset_in_sector = (index & 3) << 7;

    uint32_t target_lba = inode_table_start_lba + sector_offset;

    uint8_t sector_buf[512] __attribute__((aligned(4)));
    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 1;
    int r = ext2_disk_read_sectors(target_lba, 1, sector_buf);
    if (api_ptr->schedule_lock_ptr) *api_ptr->schedule_lock_ptr = 0;
    if (r != FS_OK) return r;

    memcpy(sector_buf + offset_in_sector, in_inode, sizeof(ext2_inode_t));

    return ext2_disk_write_sectors(target_lba, 1, sector_buf);
}



int ext2_alloc_block(ext2_volume_t *vol, uint32_t group, uint32_t *out_block_no){
    if (!vol || !out_block_no || group >= vol->groups_count) return FS_IO_ERROR;

    uint8_t *bg_desc_bytes = vol->bg_desc_table + (group * 32);
    uint32_t block_bitmap_no = *(uint32_t *)(bg_desc_bytes + 0);

    if (block_bitmap_no == 0) return FS_CORRUPTED;

    uint8_t *bitmap_buf = (uint8_t *)api_ptr->malloc_ptr(4096);
    if (!bitmap_buf) return FS_IO_ERROR;

    // Делаем до 3 попыток чтения, если контроллер диска заикается
    int retry = 3;
    while (retry--) {
        int r = ext2_read_block(vol, block_bitmap_no, bitmap_buf);
        if (r != FS_OK) continue;

        //В группе 0 системные метаданные должны быть заняты.
        //Первый и второй байты карты группы 0 физически не могут быть чисто нулевыми
        if (group == 0 && bitmap_buf[0] == 0x00 && bitmap_buf[1] == 0x00) {
            //Диск вернул нули (не успел заполнить буфер в PIO) — даем небольшую паузу
            uint32_t delay = 10000;
            while(delay--) { inb(0x1F7); }
            continue;
        }
        break; //Данные похожи на правду, выходим
    }
    // Если после всех попыток диск так и отдает нули, защищаем ФС от краша
    if (group == 0 && bitmap_buf[0] == 0x00) {
        api_ptr->free_ptr(bitmap_buf);
        return FS_IO_ERROR;
    }
    // Ищем первый свободный (нулевой) бит
    uint32_t found_bit = 0xFFFFFFFF;
    for (uint32_t i = 0; i < vol->block_size; i++) {
        if (bitmap_buf[i] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(bitmap_buf[i] & (1 << bit))) {
                    bitmap_buf[i] |= (1 << bit);
                    found_bit = (i * 8) + bit;
                    break;
                }
            }
        }
        if (found_bit != 0xFFFFFFFF) break;
    }

    if (found_bit == 0xFFFFFFFF) {
        api_ptr->free_ptr(bitmap_buf);
        return FS_NO_SPACE;
    }

    //абсолютный номер блока
    uint32_t absolute_block_no = found_bit + vol->sb.s_first_data_block + (group * vol->sb.s_blocks_per_group);

    //Запишем измененную битовую карту блоков обратно на диск
    int r = ext2_write_block(vol, block_bitmap_no, bitmap_buf);

    api_ptr->free_ptr(bitmap_buf);
    if (r != FS_OK) return r;

    // Модифицируем счетчики свободных блоков в дескрипторе группы
    uint16_t *free_blocks_ptr = (uint16_t *)(bg_desc_bytes + 12);
    if (*free_blocks_ptr > 0) {
        (*free_blocks_ptr)--;
    }

    //Сохраняем всю измененную таблицу дескрипторов групп обратно на диск
    uint32_t bgdt_block = (vol->block_size == 1024) ? 2 : 1;
    uint32_t bgdt_lba = vol->partition_start_lba + (bgdt_block * vol->sectors_per_block);
    uint32_t bgdt_size_bytes = vol->groups_count * sizeof(ext2_bg_desc_t);
    uint32_t bgdt_sectors = (bgdt_size_bytes + 512 - 1) / 512;

    r = ext2_disk_write_sectors(bgdt_lba, bgdt_sectors, vol->bg_desc_table);
    if (r != FS_OK) return r;

    // Модифицируем общий счетчик свободных блоков в суперблоке (RAM)
    if (vol->sb.s_free_blocks_count > 0) {
        vol->sb.s_free_blocks_count--;
    }

    // Сбрасываем обновленный суперблок обратно на диск (LBA раздела + 2 сектора)
    uint32_t sb_lba = vol->partition_start_lba + 2;
    uint8_t *sb_save_buf = (uint8_t *)api_ptr->malloc_ptr(4096);
    if (!sb_save_buf) return FS_IO_ERROR;

    r = ext2_disk_read_sectors(sb_lba, 2, sb_save_buf);
    if (r != FS_OK) {
        api_ptr->free_ptr(sb_save_buf);
        return r;
    }

    memcpy(sb_save_buf, &vol->sb, sizeof(ext2_superblock_t));

    r = ext2_disk_write_sectors(sb_lba, 2, sb_save_buf);
    api_ptr->free_ptr(sb_save_buf);
    if (r != FS_OK) return r;

    *out_block_no = absolute_block_no;
    return FS_OK;
}



int ext2_alloc_inode(ext2_volume_t *vol, uint32_t group, uint32_t *out_inode_no){
    if (!vol || !out_inode_no || group >= vol->groups_count) return FS_IO_ERROR;
    // Вычисляем адрес начала дескриптора нужной группы в кэше ядра
    uint8_t *bg_desc_bytes = vol->bg_desc_table + (group * 32);
    // Извлекаем адреса битовых карт (ручной сборщик полностью защищен от упаковки структур)
    uint32_t inode_bitmap_no = *(uint32_t *)(bg_desc_bytes + 4); // смещение 4 байта
    if (inode_bitmap_no == 0) return FS_CORRUPTED;

    uint8_t *bitmap_buf = (uint8_t *)api_ptr->malloc_ptr(4096);
    if (!bitmap_buf) return FS_IO_ERROR;

    int r = ext2_read_block(vol, inode_bitmap_no, bitmap_buf);
    if (r != FS_OK) {
        api_ptr->free_ptr(bitmap_buf);
        return r;
    }

    // Ищем первый свободный (нулевой) бит
    uint32_t found_bit = 0xFFFFFFFF;
    uint32_t inodes_per_group = vol->sb.s_inodes_per_group;
    uint32_t bytes_to_check = (inodes_per_group + 7) / 8;

    for (uint32_t i = 0; i < bytes_to_check; i++) {
        if (bitmap_buf[i] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(bitmap_buf[i] & (1 << bit))) {
                    uint32_t potential_bit = (i * 8) + bit;
                    if (potential_bit < inodes_per_group) {
                        bitmap_buf[i] |= (1 << bit);
                        found_bit = potential_bit;
                        break;
                    }
                }
            }
        }
        if (found_bit != 0xFFFFFFFF) break;
    }

    if (found_bit == 0xFFFFFFFF) {
        api_ptr->free_ptr(bitmap_buf);
        return FS_NO_SPACE;
    }

    //Абсолютный номер нового инода (индексация в EXT2 начинается строго с 1)
    uint32_t absolute_inode_no = found_bit + 1 + (group * inodes_per_group);

    //Записываем измененную битовую карту инодов обратно на диск
    r = ext2_write_block(vol, inode_bitmap_no, bitmap_buf);
    api_ptr->free_ptr(bitmap_buf);
    if (r != FS_OK) return r;

    // Модифицируем счетчики свободных инодов в дескрипторе группы (смещение 14 байт)
    uint16_t *free_inodes_ptr = (uint16_t *)(bg_desc_bytes + 14);
    if (*free_inodes_ptr > 0) {
        (*free_inodes_ptr)--;
    }

    // Сохраняем всю обновленную таблицу дескрипторов групп на диск
    uint32_t bgdt_block = (vol->block_size == 1024) ? 2 : 1;
    uint32_t bgdt_lba = vol->partition_start_lba + (bgdt_block * vol->sectors_per_block);
    uint32_t bgdt_size_bytes = vol->groups_count * sizeof(ext2_bg_desc_t);
    uint32_t bgdt_sectors = (bgdt_size_bytes + 512 - 1) / 512;

    r = ext2_disk_write_sectors(bgdt_lba, bgdt_sectors, vol->bg_desc_table);
    if (r != FS_OK) return r;

    // Обновляем общий счетчик свободных инодов в суперблоке тома
    if (vol->sb.s_free_inodes_count > 0) {
        vol->sb.s_free_inodes_count--;
    }

    uint32_t sb_lba = vol->partition_start_lba + 2;
    uint8_t *sb_save_buf = (uint8_t *)api_ptr->malloc_ptr(4096);
    if (!sb_save_buf) return FS_IO_ERROR;

    r = ext2_disk_read_sectors(sb_lba, 2, sb_save_buf);
    if (r != FS_OK) {
        api_ptr->free_ptr(sb_save_buf);
        return r;
    }

    memcpy(sb_save_buf, &vol->sb, sizeof(ext2_superblock_t));

    r = ext2_disk_write_sectors(sb_lba, 2, sb_save_buf);
    api_ptr->free_ptr(sb_save_buf);
    if (r != FS_OK) return r;

    *out_inode_no = absolute_inode_no;
    return FS_OK;
}





