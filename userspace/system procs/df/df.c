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


//df — занятость примонтированных ФС
//Две таблицы: классический df (1K-блоки) и df -h (human-readable)
//
#include "../../include/ustd.h"
#include "../../include/mount_structures.h"

typedef struct
{
    char     fs_name[40];
    char     mount_point[256];
    uint32_t total_1k, used_1k, free_1k;
    uint32_t use_pct;
} df_row_t;

//Хелперы

//Длина строки в видимых UTF-8 символах (кириллица = 2 байта на глиф)
static int u8_visible_len(const char *s)
{
    int n = 0;
    while (*s)
    {
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
        s++;
    }
    return n;
}

//Левое выравнивание (строка + пробелы справа)
static void pad_left(const char *s, int width)
{
    int len = u8_visible_len(s);
    if (s) printf("%s", s);
    for (int i = len; i < width; i++) printf(" ");
}

//Правое выравнивание (пробелы + строка)
static void pad_right_str(const char *s, int width)
{
    int len = u8_visible_len(s);
    for (int i = len; i < width; i++) printf(" ");
    if (s) printf("%s", s);
}

//uint32 -> десятичная строка (без null), возвращает длину
static int u32_to_dec_str(uint32_t v, char *out)
{
    if (v == 0)
    {
        out[0] = '0';
        return 1;
    }
    char tmp[12];
    int t = 0;
    while (v)
    {
        tmp[t++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int j = 0; j < t; j++) out[j] = tmp[t - 1 - j];
    return t;
}

//uint32 -> десятичная C-строка с null
static void u32_to_dec(uint32_t v, char *out /* >= 12 */)
{
    int n = u32_to_dec_str(v, out);
    out[n] = 0;
}


//1K-блоки -> "1.4M" / "976.6M" / "64M" / "1G" / "1.5G" / "1T"
//Одна цифра после точки, округление вверх. Всё на uint32_t + сдвиги

static void fmt_human(uint32_t kib, char *out /* >= 12 */)
{
    if (kib == 0)
    {
        out[0] = '0';
        out[1] = 0;
        return;
    }
    int i;
    if (kib < 1024u)                        /* < 1 MiB -> K */
    {
        i = u32_to_dec_str(kib, out);
        out[i++] = 'K';
        out[i] = 0;
        return;
    }

    uint32_t whole, frac, d;
    char unit;

    if (kib < (1024u * 1024u))              // < 1 GiB -> M
    {
        whole = kib >> 10;
        frac = kib & 0x3FFu;
        d = (frac * 10u + 512u) >> 10;
        unit = 'M';
    }
    else if (kib < (1024u * 1024u * 1024u))    // < 1 TiB -> G
    {
        whole = kib >> 20;
        frac = kib & 0xFFFFFu;
        d = (frac * 10u + 512u) >> 20;
        unit = 'G';
    }
    else                                    // >= 1 TiB -> T
    {
        whole = kib >> 30;
        frac = kib & 0x3FFFFFFFu;
        d = ((frac >> 20) * 10u + 512u) >> 10;
        unit = 'T';
    }
    if (d >= 10)
    {
        d = 0;    // 1.999G -> 2G, не 1.10G
        whole += 1;
    }

    i = u32_to_dec_str(whole, out);
    if (d)
    {
        out[i++] = '.';
        out[i++] = (char)('0' + d);
    }
    out[i++] = unit;
    out[i]   = 0;
}

//Блоки ФС -> 1K-блоки. block_size всегда степень 2 -> сдвиг
static uint32_t blocks_to_1k(uint32_t blocks, uint32_t block_size)
{
    switch (block_size)
    {
    case 512:
        return blocks >> 1;
    case 1024:
        return blocks;
    case 2048:
        return blocks << 1;
    case 4096:
        return blocks << 2;
    case 8192:
        return blocks << 3;
    case 16384:
        return blocks << 4;
    default:
        return 0;
    }
}

//Процент в строку: "0", "5", "50", "100"
static void fmt_pct(uint32_t pct, char *out /* >= 5 */)
{
    if (pct >= 100)
    {
        out[0]='1';
        out[1]='0';
        out[2]='0';
        out[3]=0;
        return;
    }
    if (pct >= 10)
    {
        out[0]=(char)('0'+pct/10);
        out[1]=(char)('0'+pct%10);
        out[2]=0;
        return;
    }
    out[0]=(char)('0'+pct);
    out[1]=0;
}

//"ext2(d0)" в out >= 40
static void make_fs_name(const u_mount_info_t *m, char *out)
{
    int n = 0;
    const char *nm = m->fs_name[0] ? m->fs_name : "fs";
    while (*nm && n < 24) out[n++] = *nm++;
    out[n++] = '(';
    out[n++] = 'd';
    n += u32_to_dec_str(m->drive_id, out + n);
    out[n++] = ')';
    out[n]   = 0;
}

//Построить строку таблицы. Возвращает 1 при успехе
static int build_row(int idx, df_row_t *row)
{
    u_mount_info_t m;
    if (u_get_mount_info(idx, &m) != 0) return 0;

    make_fs_name(&m, row->fs_name);

    int i;
    for (i = 0; i < 255 && m.mount_point[i]; i++) row->mount_point[i] = m.mount_point[i];
    row->mount_point[i] = 0;

    row->total_1k = 0;
    row->used_1k = 0;
    row->free_1k = 0;
    if (m.statfs_supported && m.block_size && m.total_blocks)
    {
        row->total_1k = blocks_to_1k(m.total_blocks, m.block_size);
        row->free_1k  = blocks_to_1k(m.free_blocks,  m.block_size);
        row->used_1k  = (row->total_1k >= row->free_1k)
                        ? (row->total_1k - row->free_1k) : 0;
    }
    if (row->total_1k)
    {
        if (row->used_1k < 42000000u)
            row->use_pct = (row->used_1k * 100u) / row->total_1k;
        else
            row->use_pct = 100;
    }
    else
    {
        row->use_pct = 0;
    }
    return 1;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int count = u_get_mount_count();
    if (count <= 0)
    {
        printf("df: нет смонтированных файловых систем\n");
        return 1;
    }

    //Таблица 1: 1K-блоки
    pad_left("Файловая система", 18);
    pad_right_str("1K-блоков", 10);
    pad_right_str("Исп", 8);
    pad_right_str("Свободно", 11);
    pad_right_str("Исп%", 5);
    printf("  Смонтирована\n");
    printf("-------------------------------------------------------------------\n");

    for (int i = 0; i < count; i++)
    {
        df_row_t r;
        if (!build_row(i, &r)) continue;

        char b1[16], b2[16], b3[16], bp[8];
        u32_to_dec(r.total_1k, b1);
        u32_to_dec(r.used_1k,  b2);
        u32_to_dec(r.free_1k,  b3);
        fmt_pct(r.use_pct, bp);

        pad_left(r.fs_name, 18);
        pad_right_str(b1, 10);
        pad_right_str(b2, 8);
        pad_right_str(b3, 11);
        pad_right_str(bp, 5);
        printf("%%  %s\n", r.mount_point);
    }

    //Таблица 2: а ля df -h
    printf("\n");
    pad_left("Файловая система", 18);
    pad_right_str("Размер", 9);
    pad_right_str("Занято", 9);
    pad_right_str("Свободно", 10);
    pad_right_str("Исп%", 5);
    printf("  Смонтирована\n");
    printf("---------------------------------------------------------------------\n");

    for (int i = 0; i < count; i++)
    {
        df_row_t r;
        if (!build_row(i, &r)) continue;

        char hs[12], hu[12], hf[12], bp[8];
        fmt_human(r.total_1k, hs);
        fmt_human(r.used_1k,  hu);
        fmt_human(r.free_1k,  hf);
        fmt_pct(r.use_pct, bp);

        pad_left(r.fs_name, 18);
        pad_right_str(hs, 9);
        pad_right_str(hu, 9);
        pad_right_str(hf, 10);
        pad_right_str(bp, 5);
        printf("%%  %s\n", r.mount_point);
    }

    return 0;
}
