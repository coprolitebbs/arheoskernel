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

#include "../include/uimage.h"
#include "../include/ustd.h"
#include "../../include-kernel/lib.h"
#include "../include/umemory.h"
#include "../include/syscall.h"


static uint16_t u_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}


int get_fb_info(uint32_t *pitch, uint32_t *bpp, uint32_t *width, uint32_t *height){
    uint32_t out_pitch, out_bpp, out_width, out_height;

    __asm__ __volatile__(
        "int $0x80"
        : "=a"(out_pitch), "=b"(out_bpp), "=c"(out_width), "=d"(out_height)
        : "a"(SYS_GET_FB_INFO)
        : "memory"
    );

    if (pitch)  *pitch  = out_pitch;
    if (bpp)    *bpp    = out_bpp;
    if (width)  *width  = out_width;
    if (height) *height = out_height;
    return 0;
}



uint8_t* load_tga_to_buffer(const char *path, uint32_t target_w, uint32_t target_h, uint32_t target_bpp, uint32_t target_pitch, uint32_t mode, img_loading_struct_t *loading_struct){
    int fd = open(path, FS_OPEN_READ);
    if (fd < 0) return NULL;

    tga_header_t hdr;
    if (u_read(fd, &hdr, sizeof(tga_header_t)) != sizeof(tga_header_t)) {
        close(fd);
        return NULL;
    }

    if (hdr.image_type != 2 || (hdr.bits_per_pixel != 24 && hdr.bits_per_pixel != 32)) {
        close(fd);
        return NULL;
    }

    uint32_t src_w = hdr.width;
    uint32_t src_h = hdr.height;
    uint32_t src_bytes_pp = hdr.bits_per_pixel / 8;
    uint32_t src_size_bytes = src_w * src_h * src_bytes_pp;

    uint8_t *raw_pixels = (uint8_t *)malloc(src_size_bytes);
    if (!raw_pixels) { close(fd); return NULL; }

    if (hdr.id_length > 0) {
        uint8_t dummy;
        for (int i = 0; i < hdr.id_length; i++) u_read(fd, &dummy, 1);
    }

    if(loading_struct != nullptr) loading_struct->start_loading_phase(0,src_size_bytes);
    uint32_t bytes_read = 0;
    while (bytes_read < src_size_bytes) {
        uint32_t chunk = src_size_bytes - bytes_read;
        if (chunk > 32768) chunk = 32768;
        int r = u_read(fd, raw_pixels + bytes_read, chunk);
        if (r <= 0) { break;}
        bytes_read += r;
        if(loading_struct != nullptr) loading_struct->loading_page_step(bytes_read);
        //yield();
        //printf("here\n");
    }
    close(fd);
    if(loading_struct != nullptr) loading_struct->stop_loading_phase();

    if (bytes_read < src_size_bytes) {
        free(raw_pixels);
        return NULL;
    }


    uint32_t dest_size_bytes = target_h * target_pitch;
    uint8_t *dest_canvas = (uint8_t *)malloc(dest_size_bytes);
    if (!dest_canvas) {
        //free_pages(raw_pixels, src_size_bytes);
        free(raw_pixels);
        return NULL;
    }
    memset(dest_canvas, 0, dest_size_bytes);

    uint32_t target_bytes_pp = target_bpp / 8;

    for (uint32_t y = 0; y < target_h; y++) {
        for (uint32_t x = 0; x < target_w; x++) {
            uint32_t src_x = 0;
            uint32_t src_y = 0;

            if (mode == 2) {
                //Режим WP_MODE_STRETCH (Растягивание)
                //Целочисленный Nearest Neighbor алгоритм без использования float/FPU
                src_x = (x * src_w) / target_w;
                src_y = (y * src_h) / target_h;
                //Защита от выхода за границы округления
                if (src_x >= src_w) src_x = src_w - 1;
                if (src_y >= src_h) src_y = src_h - 1;
            }
            else if (mode == 1) {
                //Режим WP_MODE_TILE (Замощение / Плитка)
                src_x = x % src_w;
                src_y = y % src_h;
            }
            else {
                //Режим WP_MODE_CENTER (Обычный вывод 1-к-1 с черными краями)
                if (x >= src_w || y >= src_h) {
                    continue;
                }
                src_x = x;
                src_y = y;
            }

            //Учитываем дескриптор направления строк TGA (бит 5)
            uint32_t actual_src_y = src_y;
            if (!(hdr.image_descriptor & 0x20)) {
                actual_src_y = (src_h - 1) - src_y;
            }

            uint32_t src_pixel_offset = (actual_src_y * src_w + src_x) * src_bytes_pp;

            uint8_t b = raw_pixels[src_pixel_offset + 0];
            uint8_t g = raw_pixels[src_pixel_offset + 1];
            uint8_t r = raw_pixels[src_pixel_offset + 2];
            uint8_t a = (src_bytes_pp == 4) ? raw_pixels[src_pixel_offset + 3] : 0xFF;

            uint8_t *dst_ptr = dest_canvas + (y * target_pitch) + (x * target_bytes_pp);

            if (target_bpp == 32) {
                dst_ptr[0] = b;
                dst_ptr[1] = g;
                dst_ptr[2] = r;
                dst_ptr[3] = a;
            }
            else if (target_bpp == 24) {
                dst_ptr[0] = b;
                dst_ptr[1] = g;
                dst_ptr[2] = r;
            }
            else if (target_bpp == 16) {
                *(uint16_t*)dst_ptr = u_rgb565(r, g, b);
            }
            else if (target_bpp == 8) {
                dst_ptr[0] = (uint8_t)((r + g + b) / 3);
            }
        }
    }

    free(raw_pixels);
    return dest_canvas;
}


/* =====================================================================
 *  PNG-декодер (RFC 2083) + собственный inflate (RFC 1950/1951).
 *
 *  Поддерживается:
 *   - цветовые типы 0 (gray), 2 (RGB), 3 (палитра), 4 (gray+A), 6 (RGBA);
 *   - глубины 1/2/4/8/16 бит (все допустимые комбинации);
 *   - фильтры строк 0..4 (None/Sub/Up/Average/Paeth);
 *   - Adam7-интерлейс;
 *   - прозрачность tRNS (палитра, gray, truecolor);
 *   - 16-битные изображения (старший байт, как в netpbm);
 *   - проверка CRC чанков и Adler32 каждого zlib-потока;
 *   - IDAT из нескольких склеенных zlib-потоков (Adam7 от libpng/PIL).
 *
 *  Внешний zlib не нужен, всё есть: inflate написан с нуля по RFC 1951.
 *
 *  Формат результата: байты B,G,R,A на пиксель (как raw TGA32),
 *  строки сверху-вниз. Дальше load_png_to_buffer() конвертирует
 *  в целевой формат точно так же, как load_tga_to_buffer().
 * ===================================================================== */

#define UI_PNG_MAX_WIDTH     8192u
#define UI_PNG_MAX_HEIGHT    8192u
#define UI_PNG_MAX_PIXELS    (16u * 1024u * 1024u)   //предел на w*h
#define UI_PNG_MAX_FILE      (64u * 1024u * 1024u)   //предел на размер файла

//последняя ошибка (для отладки через printf)
static char ui_png_err[80] = "no error";

const char *uimage_png_last_error(void) {
    return ui_png_err;
}

static void ui_png_fail(const char *msg) {
    uint32_t i = 0;
    while (msg[i] != 0 && i < sizeof(ui_png_err) - 1) {
        ui_png_err[i] = msg[i];
        i++;
    }
    ui_png_err[i] = 0;
}

//big-endian читалки
static uint32_t ui_png_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t ui_png_be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

//CRC32 (полином 0xEDB88320, как в PNG)
static uint32_t ui_png_crc32(const uint8_t *buf, uint32_t len) {
    static uint32_t table[256];
    static int      table_ready = 0;
    uint32_t c, i;
    int k;

    if (!table_ready) {
        for (i = 0; i < 256; i++) {
            c = i;
            for (k = 0; k < 8; k++)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        table_ready = 1;
    }

    c = 0xFFFFFFFFu;
    for (i = 0; i < len; i++)
        c = table[(c ^ buf[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

//Adler32 (zlib)
static uint32_t ui_png_adler32(const uint8_t *buf, uint32_t len) {
    uint32_t a = 1, b = 0;

    while (len > 0) {
        uint32_t t = (len > 5552u) ? 5552u : len;
        len -= t;
        while (t--) { a += *buf++; b += a; }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

//побитовый читатель (LSB-first, как в deflate)
typedef struct {
    const uint8_t *data;
    uint32_t       size;
    uint32_t       pos;
    uint32_t       bitbuf;
    int            bitcnt;
} ui_png_bitrd_t;

//возвращает беззнаковое значение need<=16 бит или -1 (данные кончились)
static int ui_png_bits(ui_png_bitrd_t *br, int need) {
    int val;

    while (br->bitcnt < need) {
        if (br->pos >= br->size) return -1;
        br->bitbuf |= (uint32_t)br->data[br->pos++] << br->bitcnt;
        br->bitcnt += 8;
    }
    val = (int)(br->bitbuf & ((1u << need) - 1u));
    br->bitbuf >>= need;
    br->bitcnt  -= need;
    return val;
}

//каноническое дерево Хаффмана
typedef struct {
    uint16_t count[16];    //число кодов каждой длины
    uint16_t symbol[288];  //символы в порядке (длина, номер)
} ui_png_huff_t;

//0 - ок, 1 - пустое дерево (нет кодов), -1 - переполнение кодов
static int ui_png_huff_build(ui_png_huff_t *h, const uint8_t *lengths, int n) {
    int      i, left;
    uint16_t offs[16];

    for (i = 0; i < 16; i++) h->count[i] = 0;
    for (i = 0; i < n;  i++) h->count[lengths[i]]++;
    if (h->count[0] == (uint16_t)n) return 1;

    left = 1;
    for (i = 1; i < 16; i++) {
        left <<= 1;
        left -= (int)h->count[i];
        if (left < 0) return -1;
    }

    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (uint16_t)i;

    return 0;
}

//возвращает символ или -1/-2 (ошибка потока)
static int ui_png_huff_decode(ui_png_bitrd_t *br, const ui_png_huff_t *h) {
    int code = 0, first = 0, index = 0, len, bit, cnt;

    for (len = 1; len <= 15; len++) {
        bit = ui_png_bits(br, 1);
        if (bit < 0) return -1;
        code |= bit;

        cnt = (int)h->count[len];
        if (code - first < cnt) return (int)h->symbol[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code  <<= 1;
    }
    return -2;
}

//таблицы длин/дистанций deflate (RFC 1951, раздел 3.2.5)
static const uint16_t ui_png_lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t ui_png_lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t ui_png_dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const uint8_t ui_png_dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
/* порядок длин кода длин (RFC 1951, 3.2.7) */
static const uint8_t ui_png_clcidx[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

//Статические буферы декомпрессора
//Реентерабельность не нужна: в ArheOS процесс однопоточный, а у каждого процесса своя память
static ui_png_huff_t ui_png_lit, ui_png_dist, ui_png_clh;
static uint8_t       ui_png_lens[288 + 32];


//inflate: распаковывает deflate-потоки IDAT в out[0..outsize)
//Возвращает 0 при успехе (out заполнен ровно полностью), иначе -N
//ls (может быть NULL) получает прогресс через loading_page_step(outpos)

//распаковка одного deflate-потока (RFC 1951)
//Состояние бита-читателя в *brp (zlib-заголовок уже пропущен)
//Пишет в out с позиции *outpos_io и обновляет её.
//0 — поток дошёл до final-блока, <0 — ошибка.
//Состояние brp сохраняется: после потока там лежит его Adler32
//(и, возможно, следующий поток — см. ui_png_inflate_multi)
static int ui_png_inflate_stream(ui_png_bitrd_t *brp,
                                 uint8_t *out, uint32_t outsize,
                                 uint32_t *outpos_io,
                                 img_loading_struct_t *ls) {
    ui_png_bitrd_t br = *brp;   //рабочая копия состояния читателя
    uint32_t outpos    = *outpos_io;
    uint32_t next_step = outpos + 32768;
    int final, err = 0;

    do {
        int type;

        final = ui_png_bits(&br, 1);
        type  = ui_png_bits(&br, 2);
        if (final < 0 || type < 0) { err = -1; break; }   //данные кончились

        if (type == 0) {
            //сохранённый блок (stored)
            int lenv, nlen, drop;

            drop = br.bitcnt & 7; //выравнивание на байт
            br.bitbuf >>= drop;
            br.bitcnt  -= drop;

            lenv = ui_png_bits(&br, 16);
            nlen = ui_png_bits(&br, 16);
            if (lenv < 0 || nlen < 0) { err = -1; break; }
            if ((((uint32_t)lenv ^ (uint32_t)nlen) & 0xFFFFu) != 0xFFFFu) { err = -2; break; }

            //блок Хаффмана (фиксированный или динамический)
            if (type == 1) {
                //фиксированные коды (RFC 1951, 3.2.6)
                uint8_t fl[288];
                int i;
                for (i = 0;   i < 144; i++) fl[i] = 8;
                for (;       i < 256; i++) fl[i] = 9;
                for (;       i < 280; i++) fl[i] = 7;
                for (;       i < 288; i++) fl[i] = 8;
                if (ui_png_huff_build(&ui_png_lit, fl, 288) < 0) { err = -2; break; }
                for (i = 0; i < 30; i++) fl[i] = 5;
                if (ui_png_huff_build(&ui_png_dist, fl, 30) < 0) { err = -2; break; }
            } else {
                //динамические коды (RFC 1951, 3.2.7)
                int nlen, ndist, ncode, i, n;

                nlen  = ui_png_bits(&br, 5);
                ndist = ui_png_bits(&br, 5);
                ncode = ui_png_bits(&br, 4);
                if (nlen < 0 || ndist < 0 || ncode < 0) { err = -1; break; }
                nlen += 257; ndist += 1; ncode += 4;
                if (nlen > 286 || ndist > 30) { err = -6; break; }

                {
                    uint8_t clens[19];
                    memset(clens, 0, sizeof(clens));
                    for (i = 0; i < ncode; i++) {
                        int v = ui_png_bits(&br, 3);
                        if (v < 0) { err = -1; break; }
                        clens[ui_png_clcidx[i]] = (uint8_t)v;
                    }
                    if (err) break;
                    if (ui_png_huff_build(&ui_png_clh, clens, 19) < 0) { err = -6; break; }
                }

                memset(ui_png_lens, 0, sizeof(ui_png_lens));
                i = 0;
                n = nlen + ndist;
                while (i < n) {
                    int sym = ui_png_huff_decode(&br, &ui_png_clh);
                    int rep, val = 0;

                    if (sym < 0) { err = -1; break; }
                    if (sym < 16) {
                        ui_png_lens[i++] = (uint8_t)sym;
                        continue;
                    }
                    if (sym == 16) {
                        if (i == 0) { err = -6; break; }
                        val = ui_png_lens[i - 1];
                        rep = ui_png_bits(&br, 2);
                        if (rep >= 0) rep += 3;
                    } else if (sym == 17) {
                        rep = ui_png_bits(&br, 3);
                        if (rep >= 0) rep += 3;
                    } else {
                        rep = ui_png_bits(&br, 7);
                        if (rep >= 0) rep += 11;
                    }
                    if (rep < 0) { err = -1; break; }
                    if (i + rep > n) { err = -6; break; }
                    while (rep--) ui_png_lens[i++] = (uint8_t)val;
                }
                if (err) break;

                if (ui_png_huff_build(&ui_png_lit, ui_png_lens, nlen) < 0) { err = -6; break; }
                if (ui_png_huff_build(&ui_png_dist, ui_png_lens + nlen, ndist) < 0) { err = -6; break; }
                if (ui_png_lit.count[0] == (uint16_t)nlen) { err = -6; break; }  //пустое дерево литералов
            }

            //декодирование блока
            for (;;) {
                int sym = ui_png_huff_decode(&br, &ui_png_lit);

                if (sym < 0) { err = -4; break; }
                if (sym < 256) {
                    if (outpos >= outsize) { err = -5; break; }
                    out[outpos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;                                    //конец блока
                } else {
                    int eb, lenv, dsym, d;

                    sym -= 257;
                    if (sym >= 29) { err = -6; break; }       //286/287 запрещены
                    eb = ui_png_bits(&br, ui_png_lext[sym]);
                    if (eb < 0) { err = -1; break; }
                    lenv = (int)ui_png_lbase[sym] + eb;

                    dsym = ui_png_huff_decode(&br, &ui_png_dist);
                    if (dsym < 0) { err = -4; break; }
                    if (dsym >= 30) { err = -6; break; }      //30/31 запрещены
                    eb = ui_png_bits(&br, ui_png_dext[dsym]);
                    if (eb < 0) { err = -1; break; }
                    d = (int)ui_png_dbase[dsym] + eb;

                    if ((uint32_t)d > outpos) { err = -8; break; }        //ссылка назад слишком далеко
                    if (lenv > (int)(outsize - outpos)) { err = -5; break; }

                    while (lenv--) {
                        out[outpos] = out[outpos - d];
                        outpos++;
                    }
                }
            }
            if (err) break;
        }
        else {
            err = -3; break;   //type == 3 запрещён
        }

        if (ls && outpos >= next_step) {
            ls->loading_page_step(outpos);
            next_step = outpos + 32768;
        }
    } while (!final);

    *brp       = br;      //возвращаем состояние (позиция Adler32)
    *outpos_io = outpos;
    if (ls && !err) ls->loading_page_step(outpos);
    return err;
}

//распаковка данных IDAT (RFC 1950)
//IDAT может содержать несколько склеенных zlib-потоков —
//при Adam7-интерлейсе каждый проход сжимается отдельным потоком
//(так пишут libpng, PIL, GIMP). Поэтому после каждого потока
//читаем его Adler32 и, если изображение распаковано не целиком,
//продолжаем следующим потоком.
//Коды ошибок (для switch в ui_png_decode):
// -1  данные кончились           -2  битый stored-блок
// -3  запрещённый тип блока      -5  выход за размер выхода
// -6  битые коды Хаффмана        -8  ссылка назад слишком далеко
// -9  поток оборван              -11 это не deflate
// -12 битый заголовок zlib       -13 zlib-словарь не поддерживается
// -14 Adler32 не совпал
static int ui_png_inflate_multi(const uint8_t *zdata, uint32_t zsize,
                                uint8_t *out, uint32_t outsize,
                                img_loading_struct_t *ls) {
    ui_png_bitrd_t br;
    uint32_t outpos = 0;

    br.data   = zdata;
    br.size   = zsize;
    br.pos    = 0;
    br.bitbuf = 0;
    br.bitcnt = 0;

    while (outpos < outsize) {
        uint32_t stream_start = outpos;
        uint32_t cmf, flg;
        int rc;

        //zlib-заголовок потока (RFC 1950): CMF FLG
        if (br.size - br.pos < 6) return -9;
        cmf = zdata[br.pos];
        flg = zdata[br.pos + 1];
        if ((cmf & 0x0Fu) != 0x08u)           return -11;
        if ((((cmf << 8) | flg) % 31u) != 0u) return -12;
        if (flg & 0x20u)                      return -13;
        br.pos += 2;

        //сам deflate-поток
        rc = ui_png_inflate_stream(&br, out, outsize, &outpos, ls);
        if (rc != 0) return rc;

        //выравнивание до границы байта после final-блока
        br.bitbuf = 0;
        br.bitcnt = 0;

        //Adler32 этого потока (4 байта, big-endian)
        if (br.size - br.pos < 4) return -9;
        if (ui_png_adler32(out + stream_start, outpos - stream_start) !=
            ui_png_be32(zdata + br.pos)) {
            return -14;
        }
        br.pos += 4;
    }

    return 0;   //всё распаковано; возможный хвост игнорируем
}

//предиктор Paeth (RFC 2083, фильтр 4)
static int ui_png_paeth(int a, int b, int c) {
    int p  = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc)             return b;
    return c;
}

//разворачивает одну строку на месте; возвращает -1 при кривом типе фильтра
static int ui_png_unfilter_row(uint8_t *row, const uint8_t *prev,
                               uint32_t rb, uint32_t fbpp, uint32_t ft) {
    uint32_t i;

    switch (ft) {
    case 0:
        break;
    case 1: //Sub
        for (i = fbpp; i < rb; i++)
            row[i] = (uint8_t)(row[i] + row[i - fbpp]);
        break;
    case 2: //Up
        if (prev)
            for (i = 0; i < rb; i++)
                row[i] = (uint8_t)(row[i] + prev[i]);
        break;
    case 3: //Average
        for (i = 0; i < rb; i++) {
            int a = (i >= fbpp) ? row[i - fbpp] : 0;
            int b = prev ? prev[i] : 0;
            row[i] = (uint8_t)(row[i] + ((a + b) >> 1));
        }
        break;
    case 4: //Paeth
        for (i = 0; i < rb; i++) {
            int a = (i >= fbpp) ? row[i - fbpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= fbpp) ? prev[i - fbpp] : 0;
            row[i] = (uint8_t)(row[i] + ui_png_paeth(a, b, c));
        }
        break;
    default:
        return -1;
    }
    return 0;
}

//геометрия проходов Adam7
static const uint8_t ui_png_ix0[7] = {0, 4, 0, 2, 0, 1, 0};
static const uint8_t ui_png_iy0[7] = {0, 0, 4, 0, 2, 0, 1};
static const uint8_t ui_png_idx[7] = {8, 8, 4, 4, 2, 2, 1};
static const uint8_t ui_png_idy[7] = {8, 8, 8, 4, 4, 2, 2};

//масштабирование сэмпла в 8 бит (бит-репликация для 1/2/4)
static uint8_t ui_png_scale(uint32_t v, uint32_t depth) {
    switch (depth) {
    case 1:  return (uint8_t)(v ? 255u : 0u);
    case 2:  return (uint8_t)(v * 85u);
    case 4:  return (uint8_t)(v * 17u);
    case 16: return (uint8_t)(v >> 8);   //старший байт
    default: return (uint8_t)v;          //8
    }
}

//сэмпл глубины 1/2/4: x-й пиксель строки (MSB-first упаковка)
static uint32_t ui_png_bit_sample(const uint8_t *row, uint32_t x, uint32_t depth) {
    uint32_t bitpos = x * depth;
    uint8_t  byte   = row[bitpos >> 3];
    uint32_t shift  = 8u - depth - (bitpos & 7u);
    return ((uint32_t)byte >> shift) & ((1u << depth) - 1u);
}

static uint8_t ui_png_pal_r[256], ui_png_pal_g[256], ui_png_pal_b[256], ui_png_pal_a[256];


//Декодирует PNG из памяти в буфер B,G,R,A (4 байта на пиксель,
//строки сверху-вниз). Возвращает буфер (malloc) или NULL
//ls — необязательные колбэки прогресса (как в TGA)
static uint8_t *ui_png_decode(const uint8_t *data, uint32_t size,
                              uint32_t *out_w, uint32_t *out_h,
                              img_loading_struct_t *ls) {
    static const uint8_t depth_ok[7] = { 0x1F, 0, 0x18, 0x0F, 0x18, 0, 0x18 };
    static const uint8_t chan_tab[7] = { 1,    0, 3,    1,    2,    0, 4    };

    uint32_t pos, w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    uint32_t channels = 0, bitpx = 0, fbpp = 0;
    int  seen_ihdr = 0, seen_idat = 0, idat_done = 0;
    uint32_t idat_total = 0, idat_pos0 = 0;

    //метаданные прозрачности
    int      has_trns = 0, trns_count = 0;
    uint32_t trns_gray = 0, trns_r = 0, trns_g = 0, trns_b = 0;
    uint8_t  trns_a[256];
    uint32_t plte_count = 0;
    uint8_t  plte[768];

    uint8_t *zdata = NULL, *raw = NULL, *canvas = NULL;
    uint32_t total_raw = 0, npass, p;

    if (size < 8) { ui_png_fail("PNG: файл слишком мал"); return NULL; }
    {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (memcmp(data, sig, 8) != 0) { ui_png_fail("PNG: неверная сигнатура"); return NULL; }
    }

    memset(trns_a, 255, sizeof(trns_a));

    //проход 1: разбор чанков
    pos = 8;
    while (pos + 12 <= size) {
        uint32_t clen = ui_png_be32(data + pos);
        const uint8_t *t = data + pos + 4;
        const uint8_t *d = data + pos + 8;

        if (clen > size - pos - 12) { ui_png_fail("PNG: повреждённый чанк"); return NULL; }

        if (memcmp(t, "IHDR", 4) == 0) {
            if (seen_ihdr) { ui_png_fail("PNG: повторный IHDR"); return NULL; }
            if (pos != 8)  { ui_png_fail("PNG: IHDR не первый чанк"); return NULL; }
            if (clen != 13) { ui_png_fail("PNG: кривой IHDR"); return NULL; }
            if (ui_png_crc32(t, clen + 4) != ui_png_be32(d + clen)) {
                ui_png_fail("PNG: CRC IHDR"); return NULL;
            }

            w         = ui_png_be32(d + 0);
            h         = ui_png_be32(d + 4);
            depth     = d[8];
            ctype     = d[9];
            interlace = d[12];

            if (d[10] != 0 || d[11] != 0) { ui_png_fail("PNG: неизвестный метод сжатия"); return NULL; }
            if (ctype > 6 || depth_ok[ctype] == 0) { ui_png_fail("PNG: неизвестный тип цвета"); return NULL; }
            {
                //биты маски depth_ok: 0->1bpp, 1->2bpp, 2->4bpp, 3->8bpp, 4->16bpp
                uint32_t dbit;
                switch (depth) {
                case 1:  dbit = 0; break;
                case 2:  dbit = 1; break;
                case 4:  dbit = 2; break;
                case 8:  dbit = 3; break;
                case 16: dbit = 4; break;
                default: dbit = 5; break;
                }
                if (dbit > 4 || !(depth_ok[ctype] & (1u << dbit))) {
                    ui_png_fail("PNG: недопустимая глубина"); return NULL;
                }
            }
            if (w == 0 || h == 0 || w > UI_PNG_MAX_WIDTH || h > UI_PNG_MAX_HEIGHT) {
                ui_png_fail("PNG: слишком большие размеры"); return NULL;
            }
            if (w * h > UI_PNG_MAX_PIXELS) { ui_png_fail("PNG: слишком много пикселей"); return NULL; }
            if (interlace > 1) { ui_png_fail("PNG: кривой флаг интерлейса"); return NULL; }

            channels = chan_tab[ctype];
            bitpx    = channels * depth;
            fbpp     = (bitpx + 7) / 8;
            seen_ihdr = 1;
        }
        else if (memcmp(t, "PLTE", 4) == 0) {
            if (!seen_ihdr) { ui_png_fail("PNG: PLTE до IHDR"); return NULL; }
            if (seen_idat)  { ui_png_fail("PNG: PLTE после IDAT"); return NULL; }
            if ((clen % 3) != 0 || clen == 0 || clen > 768) {
                ui_png_fail("PNG: кривая палитра"); return NULL;
            }
            if (ui_png_crc32(t, clen + 4) != ui_png_be32(d + clen)) {
                ui_png_fail("PNG: CRC PLTE"); return NULL;
            }
            memcpy(plte, d, clen);
            plte_count = clen / 3;
        }
        else if (memcmp(t, "tRNS", 4) == 0) {
            if (!seen_ihdr) { ui_png_fail("PNG: tRNS до IHDR"); return NULL; }
            if (seen_idat)  { ui_png_fail("PNG: tRNS после IDAT"); return NULL; }
            if (ui_png_crc32(t, clen + 4) != ui_png_be32(d + clen)) {
                ui_png_fail("PNG: CRC tRNS"); return NULL;
            }
            if (ctype == 3) {
                if (plte_count == 0) { ui_png_fail("PNG: tRNS до PLTE"); return NULL; }
                trns_count = (clen < plte_count) ? (int)clen : (int)plte_count;
                memcpy(trns_a, d, trns_count);
                has_trns = 1;
            } else if (ctype == 0) {
                if (clen < 2) { ui_png_fail("PNG: кривой tRNS"); return NULL; }
                trns_gray = ui_png_be16(d);
                has_trns = 1;
            } else if (ctype == 2) {
                if (clen < 6) { ui_png_fail("PNG: кривой tRNS"); return NULL; }
                trns_r = ui_png_be16(d);
                trns_g = ui_png_be16(d + 2);
                trns_b = ui_png_be16(d + 4);
                has_trns = 1;
            }
            //для типов 4 и 6 tRNS запрещён — игнорируем
        }
        else if (memcmp(t, "IDAT", 4) == 0) {
            if (!seen_ihdr)  { ui_png_fail("PNG: IDAT до IHDR"); return NULL; }
            if (idat_done)   { ui_png_fail("PNG: IDAT не подряд"); return NULL; }
            if (idat_total == 0) idat_pos0 = pos;
            if (ui_png_crc32(t, clen + 4) != ui_png_be32(d + clen)) {
                ui_png_fail("PNG: CRC IDAT"); return NULL;
            }
            idat_total += clen;
            seen_idat = 1;
        }
        else if (memcmp(t, "IEND", 4) == 0) {
            if (ui_png_crc32(t, clen + 4) != ui_png_be32(d + clen)) {
                ui_png_fail("PNG: CRC IEND"); return NULL;
            }
            break;
        }
        else {
            //прочие (вспомогательные) чанки пропускаем
            if (seen_idat) idat_done = 1;
        }

        pos += 12 + clen;
    }

    if (!seen_ihdr) { ui_png_fail("PNG: нет IHDR"); return NULL; }
    if (!seen_idat) { ui_png_fail("PNG: нет данных изображения"); return NULL; }
    if (ctype == 3 && plte_count == 0) { ui_png_fail("PNG: нет палитры"); return NULL; }
    //IEND не обязателен: обрезанный хвост поймает Adler32

    //геометрия
    {
        uint32_t pw[7], ph[7], rowb[7];
        uint32_t x0, y0, dx, dy;

        npass = interlace ? 7u : 1u;
        for (p = 0; p < npass; p++) {
            if (interlace) {
                x0 = ui_png_ix0[p]; y0 = ui_png_iy0[p];
                dx = ui_png_idx[p]; dy = ui_png_idy[p];
            } else {
                x0 = 0; y0 = 0; dx = 1; dy = 1;
            }
            pw[p] = (w > x0) ? (w - x0 + dx - 1) / dx : 0;
            ph[p] = (h > y0) ? (h - y0 + dy - 1) / dy : 0;
            //пустой проход (нет пикселей) не даёт ни байта в поток —
            //ни строк, ни фильтр-байтов (так пишут libpng/PIL/GIMP)
            if (pw[p] == 0 || ph[p] == 0) { pw[p] = 0; ph[p] = 0; }
            rowb[p] = (pw[p] * bitpx + 7) / 8;
            total_raw += ph[p] * (rowb[p] + 1);
        }
    }

    //проход 2: склейка IDAT
    zdata = (uint8_t *)malloc(idat_total);
    if (!zdata) { ui_png_fail("PNG: нет памяти под IDAT"); return NULL; }
    {
        uint32_t zp = 0;
        pos = idat_pos0;
        while (pos + 12 <= size) {
            uint32_t clen = ui_png_be32(data + pos);
            if (memcmp(data + pos + 4, "IDAT", 4) != 0) break;
            memcpy(zdata + zp, data + pos + 8, clen);
            zp += clen;
            pos += 12 + clen;
        }
    }

    //распаковка (один или несколько zlib-потоков)
    if (idat_total < 6) {
        free(zdata);
        ui_png_fail("PNG: короткий zlib-поток");
        return NULL;
    }

    raw = (uint8_t *)malloc(total_raw);
    if (!raw) {
        free(zdata);
        ui_png_fail("PNG: нет памяти под данные");
        return NULL;
    }

    if (ls) ls->start_loading_phase(0, total_raw);
    {
        int rc = ui_png_inflate_multi(zdata, idat_total, raw, total_raw, ls);
        if (ls) ls->stop_loading_phase();
        if (rc != 0) {
            free(raw); free(zdata);
            switch (rc) {
            case -5:  ui_png_fail("PNG: распаковка превысила размер"); break;
            case -2:  ui_png_fail("PNG: битый stored-блок"); break;
            case -3:  ui_png_fail("PNG: запрещённый тип блока"); break;
            case -6:  ui_png_fail("PNG: битые коды Хаффмана"); break;
            case -8:  ui_png_fail("PNG: ссылка за пределы данных"); break;
            case -9:  ui_png_fail("PNG: поток оборван"); break;
            case -11: ui_png_fail("PNG: это не deflate"); break;
            case -12: ui_png_fail("PNG: битый заголовок zlib"); break;
            case -13: ui_png_fail("PNG: zlib-словарь не поддерживается"); break;
            case -14: ui_png_fail("PNG: контрольная сумма Adler32"); break;
            default:  ui_png_fail("PNG: данные оборваны"); break;
            }
            return NULL;
        }
    }

    free(zdata);
    zdata = NULL;

    //палитра + прозрачность
    if (ctype == 3) {
        uint32_t i;
        for (i = 0; i < plte_count; i++) {
            ui_png_pal_r[i] = plte[i * 3 + 0];
            ui_png_pal_g[i] = plte[i * 3 + 1];
            ui_png_pal_b[i] = plte[i * 3 + 2];
            ui_png_pal_a[i] = (has_trns && i < (uint32_t)trns_count) ? trns_a[i] : 255;
        }
    }

    //холст
    canvas = (uint8_t *)malloc(w * h * 4);
    if (!canvas) {
        free(raw);
        ui_png_fail("PNG: нет памяти под изображение");
        return NULL;
    }
    memset(canvas, 0, w * h * 4);

    //разворачиваем фильтры и раскладываем пиксели
    {
        uint32_t pw[7], ph[7], rowb[7], poff[7];
        uint32_t x0, y0, dx, dy;
        uint32_t mask = (depth == 16) ? 0xFFFFu : ((1u << depth) - 1u);

        for (p = 0; p < npass; p++) {
            if (interlace) {
                x0 = ui_png_ix0[p]; y0 = ui_png_iy0[p];
                dx = ui_png_idx[p]; dy = ui_png_idy[p];
            } else {
                x0 = 0; y0 = 0; dx = 1; dy = 1;
            }
            pw[p] = (w > x0) ? (w - x0 + dx - 1) / dx : 0;
            ph[p] = (h > y0) ? (h - y0 + dy - 1) / dy : 0;
            //пустой проход не даёт байтов — см. расчёт total_raw
            if (pw[p] == 0 || ph[p] == 0) { pw[p] = 0; ph[p] = 0; }
            rowb[p] = (pw[p] * bitpx + 7) / 8;
            poff[p] = (p == 0) ? 0 : poff[p - 1] + ph[p - 1] * (rowb[p - 1] + 1);
        }

        for (p = 0; p < npass; p++) {
            uint8_t *base;
            uint32_t y;

            //геометрия прохода — берём ЗДЕСЬ, а не из прошлого цикла:
            //скаляры x0/y0/dx/dy должны соответствовать текущему p
            if (interlace) {
                x0 = ui_png_ix0[p]; y0 = ui_png_iy0[p];
                dx = ui_png_idx[p]; dy = ui_png_idy[p];
            } else {
                x0 = 0; y0 = 0; dx = 1; dy = 1;
            }

            if (pw[p] == 0 || ph[p] == 0) continue;
            base = raw + poff[p];

            for (y = 0; y < ph[p]; y++) {
                uint8_t *row = base + y * (rowb[p] + 1);
                int rc = ui_png_unfilter_row(row + 1, y ? (row - (rowb[p] + 1) + 1) : NULL,
                                             rowb[p], fbpp, row[0]);
                if (rc != 0) {
                    free(raw); free(canvas);
                    ui_png_fail("PNG: неизвестный фильтр строки");
                    return NULL;
                }
            }

            for (y = 0; y < ph[p]; y++) {
                const uint8_t *row = base + y * (rowb[p] + 1) + 1;
                uint8_t *drow = canvas + (y0 + y * dy) * w * 4;
                uint32_t x;

                for (x = 0; x < pw[p]; x++) {
                    uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
                    uint8_t r8, g8, b8, a8;
                    uint8_t *dp;

                    if (depth == 16) {
                        const uint8_t *q = row + x * channels * 2;
                        s0 = ui_png_be16(q);
                        if (channels > 1) s1 = ui_png_be16(q + 2);
                        if (channels > 2) s2 = ui_png_be16(q + 4);
                        if (channels > 3) s3 = ui_png_be16(q + 6);
                    } else if (depth == 8) {
                        const uint8_t *q = row + x * channels;
                        s0 = q[0];
                        if (channels > 1) s1 = q[1];
                        if (channels > 2) s2 = q[2];
                        if (channels > 3) s3 = q[3];
                    } else {
                        s0 = ui_png_bit_sample(row, x, depth);
                    }

                    switch (ctype) {
                    case 0: { //grayscale
                        r8 = g8 = b8 = ui_png_scale(s0, depth);
                        a8 = 255;
                        if (has_trns && ((s0 & mask) == (trns_gray & mask))) a8 = 0;
                        break;
                    }
                    case 2: { //truecolor
                        r8 = ui_png_scale(s0, depth);
                        g8 = ui_png_scale(s1, depth);
                        b8 = ui_png_scale(s2, depth);
                        a8 = 255;
                        if (has_trns &&
                            (s0 & mask) == (trns_r & mask) &&
                            (s1 & mask) == (trns_g & mask) &&
                            (s2 & mask) == (trns_b & mask)) a8 = 0;
                        break;
                    }
                    case 3: { //палитра
                        if (s0 < plte_count) {
                            r8 = ui_png_pal_r[s0];
                            g8 = ui_png_pal_g[s0];
                            b8 = ui_png_pal_b[s0];
                            a8 = ui_png_pal_a[s0];
                        } else {
                            //индекс вне палитры — трактуем как непрозрачный чёрный
                            r8 = g8 = b8 = 0;
                            a8 = 255;
                        }
                        break;
                    }
                    case 4: { //gray + alpha
                        r8 = g8 = b8 = ui_png_scale(s0, depth);
                        a8 = ui_png_scale(s1, depth);
                        break;
                    }
                    default: { //6: RGBA
                        r8 = ui_png_scale(s0, depth);
                        g8 = ui_png_scale(s1, depth);
                        b8 = ui_png_scale(s2, depth);
                        a8 = ui_png_scale(s3, depth);
                        break;
                    }
                    }

                    dp = drow + (x0 + x * dx) * 4;
                    dp[0] = b8;
                    dp[1] = g8;
                    dp[2] = r8;
                    dp[3] = a8;
                }
            }
        }
    }

    free(raw);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    ui_png_fail("ok");
    return canvas;
}



//Загрузка PNG из файла — аналог load_tga_to_buffer():
//те же режимы (WP_MODE_CENTER/TILE/STRETCH), те же целевые форматы
//(32/24/16/8 бит), те же колбэки прогресса
//Возвращает canvas (malloc) или NULL (см. uimage_png_last_error())

uint8_t* load_png_to_buffer(const char *path, uint32_t target_w, uint32_t target_h, uint32_t target_bpp, uint32_t target_pitch, uint32_t mode, img_loading_struct_t *loading_struct){
    int fd = open(path, FS_OPEN_READ);
    if (fd < 0) return NULL;

    //читаем файл целиком в память (размер заранее не знаем)
    uint32_t cap = 65536, len = 0;
    uint8_t *fdata = (uint8_t *)malloc(cap);
    if (!fdata) { close(fd); return NULL; }

    for (;;) {
        if (len + 32768 > cap) {
            uint32_t ncap = cap * 2;
            uint8_t *nd = (uint8_t *)malloc(ncap);
            if (!nd) { free(fdata); close(fd); return NULL; }
            memcpy(nd, fdata, len);
            free(fdata);
            fdata = nd;
            cap = ncap;
        }
        int r = u_read(fd, fdata + len, 32768);
        if (r <= 0) break;
        len += (uint32_t)r;
        if (len > UI_PNG_MAX_FILE) { free(fdata); close(fd); return NULL; }
    }
    close(fd);

    if (len < 8) { free(fdata); return NULL; }

    uint32_t src_w = 0, src_h = 0;
    uint8_t *src_pixels = ui_png_decode(fdata, len, &src_w, &src_h, loading_struct);
    free(fdata);
    if (!src_pixels) return NULL;

    //конверсия в целевой формат — как в load_tga_to_buffer()
    uint32_t dest_size_bytes = target_h * target_pitch;
    uint8_t *dest_canvas = (uint8_t *)malloc(dest_size_bytes);
    if (!dest_canvas) {
        free(src_pixels);
        return NULL;
    }
    memset(dest_canvas, 0, dest_size_bytes);

    uint32_t target_bytes_pp = target_bpp / 8;

    for (uint32_t y = 0; y < target_h; y++) {
        for (uint32_t x = 0; x < target_w; x++) {
            uint32_t src_x = 0;
            uint32_t src_y = 0;

            if (mode == 2) {
                //WP_MODE_STRETCH — целочисленный nearest neighbor
                src_x = (x * src_w) / target_w;
                src_y = (y * src_h) / target_h;
                if (src_x >= src_w) src_x = src_w - 1;
                if (src_y >= src_h) src_y = src_h - 1;
            }
            else if (mode == 1) {
                //WP_MODE_TILE — замощение
                src_x = x % src_w;
                src_y = y % src_h;
            }
            else {
                //WP_MODE_CENTER — 1:1, края чёрные
                if (x >= src_w || y >= src_h) {
                    continue;
                }
                src_x = x;
                src_y = y;
            }

            //PNG всегда хранится сверху-вниз, переворот не нужен
            uint32_t src_pixel_offset = (src_y * src_w + src_x) * 4;

            uint8_t b = src_pixels[src_pixel_offset + 0];
            uint8_t g = src_pixels[src_pixel_offset + 1];
            uint8_t r = src_pixels[src_pixel_offset + 2];
            uint8_t a = src_pixels[src_pixel_offset + 3];

            uint8_t *dst_ptr = dest_canvas + (y * target_pitch) + (x * target_bytes_pp);

            if (target_bpp == 32) {
                dst_ptr[0] = b;
                dst_ptr[1] = g;
                dst_ptr[2] = r;
                dst_ptr[3] = a;
            }
            else if (target_bpp == 24) {
                dst_ptr[0] = b;
                dst_ptr[1] = g;
                dst_ptr[2] = r;
            }
            else if (target_bpp == 16) {
                *(uint16_t*)dst_ptr = u_rgb565(r, g, b);
            }
            else if (target_bpp == 8) {
                dst_ptr[0] = (uint8_t)((r + g + b) / 3);
            }
        }
    }

    free(src_pixels);
    return dest_canvas;
}


/* =====================================================================
 *  Универсальный загрузчик load_image_to_buffer().
 *
 *  Формат выбирается по расширению имени файла (регистр не важен):
 *   - ".png" -> load_png_to_buffer();
 *   - ".tga" -> load_tga_to_buffer();
 *   - расширения нет / оно незнакомое -> содержимое опознаётся по
 *     первым 8 байтам (PNG-сигнатура), иначе файл читается как TGA
 *     (load_tga_to_buffer() сам вернёт NULL на невалидных данных)
 *
 *  Параметры, режимы и результат — те же, что у
 *  load_tga_to_buffer() / load_png_to_buffer()
 * ===================================================================== */

//сравнение расширения без учёта регистра ("png" == "PNG" == "Png")
static int ui_ext_eq(const char *s, const char *ext) {
    if (*s == '\0') return 0;
    while (*ext) {
        char a = *s;
        char b = *ext;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
        s++;
        ext++;
    }
    return (*s == '\0');
}

//true, если файл начинается с PNG-сигнатуры 89 50 4E 47 0D 0A 1A 0A
static int ui_file_is_png(const char *path) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint8_t head[8];
    int fd, r, i;

    fd = open(path, FS_OPEN_READ);
    if (fd < 0) return 0;
    r = u_read(fd, head, 8);
    close(fd);
    if (r < 8) return 0;

    for (i = 0; i < 8; i++)
        if (head[i] != sig[i]) return 0;
    return 1;
}

uint8_t* load_image_to_buffer(const char *path, uint32_t target_w, uint32_t target_h, uint32_t target_bpp, uint32_t target_pitch, uint32_t mode, img_loading_struct_t *loading_struct){
    const char *ext = NULL;
    const char *p;

    if (!path) return NULL;

    //расширение = последняя точка после последнего '/'
    for (p = path; *p; p++) {
        if (*p == '/') ext = NULL;
        else if (*p == '.') ext = p;
    }

    if (ext != NULL && ext[1] != '\0') {
        if (ui_ext_eq(ext + 1, "png"))
            return load_png_to_buffer(path, target_w, target_h, target_bpp, target_pitch, mode, loading_struct);
        if (ui_ext_eq(ext + 1, "tga"))
            return load_tga_to_buffer(path, target_w, target_h, target_bpp, target_pitch, mode, loading_struct);
    }

    //расширения нет или оно незнакомое — опознаём по содержимому
    if (ui_file_is_png(path))
        return load_png_to_buffer(path, target_w, target_h, target_bpp, target_pitch, mode, loading_struct);

    //всё прочее отдаём TGA-загрузчику
    return load_tga_to_buffer(path, target_w, target_h, target_bpp, target_pitch, mode, loading_struct);
}
