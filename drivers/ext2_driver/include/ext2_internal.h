#ifndef EXT2_INTERNAL_H
#define EXT2_INTERNAL_H

#include "../../../include-kernel/bootinfo.h"

#include <stdint.h>
#include <stdbool.h>

#define EXT2_MAX_VOLUMES 10

#define EXT2_MAX_OPEN_FILES 16

#define EXT2_MAGIC 0xEF53

#define EXT2_ROOT_INO 2 //Номер корневого инода всегда равен 2

#define EXT2_FT_DIR  2
#define EXT2_S_IFDIR 0x4000


typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved;
} __attribute__((packed)) ext2_bg_desc_t;



//Структура Суперблока EXT2 (Размер строго 1024 байта)
typedef struct {
    uint32_t s_inodes_count;       /* 0 */
    uint32_t s_blocks_count;       /* 4 */
    uint32_t s_r_blocks_count;     /* 8 */
    uint32_t s_free_blocks_count;  /* 12 */
    uint32_t s_free_inodes_count;  /* 16 */
    uint32_t s_first_data_block;   /* 20 */
    uint32_t s_log_block_size;     /* 24 */
    uint32_t s_log_frag_size;      /* 28 */
    uint32_t s_blocks_per_group;   /* 32 */
    uint32_t s_frags_per_group;    /* 36 */
    uint32_t s_inodes_per_group;   /* 40 */
    uint32_t s_mtime;              /* 44 */
    uint32_t s_wtime;              /* 48 */
    uint16_t s_mnt_count;          /* 52 */
    uint16_t s_max_mnt_count;      /* 54 */
    uint16_t s_magic;              /* 56: 0xEF53 */
    uint16_t s_state;              /* 58 */
    uint16_t s_errors;             /* 60 */
    uint16_t s_minor_rev_level;    /* 62 */
    uint32_t s_lastcheck;          /* 64 */
    uint32_t s_checkinterval;      /* 68 */
    uint32_t s_creator_os;         /* 72 */
    uint32_t s_rev_level;          /* 76: 0 = Rev 0, 1 = Rev 1 */
    uint16_t s_def_resuid;         /* 80 */
    uint16_t s_def_resgid;         /* 82 */

    //Поля для REVISION 1 (Обязательны для корректного смещения s_inode_size)
    uint32_t s_first_ino;          /* 84: Первый немусорный инод */
    uint16_t s_inode_size;         /* 88: РАЗМЕР ИНОДА НА ДИСКЕ (128) */
    uint16_t s_block_group_nr;     /* 90 */
    uint32_t s_feature_compat;     /* 92 */
    uint32_t s_feature_incompat;   /* 96 */
    uint32_t s_feature_ro_compat;  /* 100 */
    uint8_t  s_uuid[16];           /* 104 */
    char     s_volume_name[16];    /* 120 */
    char     s_last_mounted[64];   /* 136 */
    uint32_t s_algo_bitmap;        /* 200 */

    //Заглушка до конца 1024 байт суперблока
    uint8_t  s_padding[820];       /* 204 */
} __attribute__((packed)) ext2_superblock_t;

_Static_assert(sizeof(ext2_superblock_t) == 1024, "bad EXT2 superblock size");


typedef struct {
    uint32_t          drive;
    bool              mounted;
    uint32_t          partition_start_lba;
    uint32_t          block_size;
    uint32_t          sectors_per_block;
    uint32_t          groups_count;
    uint32_t          inode_size;
    ext2_superblock_t sb;
    //uint8_t           bg_desc_table[512] __attribute__((aligned(4)));
    uint8_t*          bg_desc_table;
} ext2_volume_t;


// Структура Инода Linux EXT2 (Размер строго 128 байт) [source: 1.2.8]
typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];   //Массив указателей
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;


_Static_assert(sizeof(ext2_inode_t) == 128, "bad EXT2 inode size");

//Структура записи каталога EXT2 (Совместимая с genext2fs Revision 0/1)
typedef struct {
    uint32_t inode;     /* 0 .. 3 байты: Номер инода */
    uint16_t rec_len;   /* 4 .. 5 байты: Длина записи каталога */
    uint16_t name_len;  /* 6 .. 7 байты: Длина имени файла (В Rev 1 младший байт - длина, старший - тип файла) */
    char     name[];    /* 8+ байты: Имя файла */
} __attribute__((packed)) ext2_dir_entry_t;



typedef struct {
    uint32_t       used;          /* Флаг использования: 1 = занят, 0 = свободен */
    ext2_volume_t *vol;           /* Указатель на том, на котором открыт файл */
    uint32_t       inode_no;      /* Уникальный номер инода файла на диске */
    uint32_t       file_size;     /* Размер файла в байтах (копируется из инода) */
    uint32_t       position;      /* Текущий указатель чтения/записи (смещение в байтах) */
    uint32_t       attributes;    /* Атрибуты/Тип файла (каталог или обычный файл) */
    ext2_inode_t   inode;         /* Полная копия инода файла в памяти для быстрого доступа */
} ext2_file_t;


//extern ext2_volume_t ext2_volumes[EXT2_MAX_VOLUMES];

#endif

