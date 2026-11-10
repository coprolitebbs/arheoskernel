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

//graphic_layer.c — реализация высокоуровневого слоя графики (Ring 0).
//API описан в drivers/include_drivers/driver_graphic_layer.h.
//
//Что здесь есть:
//  - реестр gr_driver_t с автопереключением активного драйвера по priority;
//  - init_graphic_layer(): если внешних драйверов нет — fb-совместимый
//    fallback поверх штатного fb_driver (legacy LFB из boot_info);
//  - программные реализации всех примитивов через LFB текущего режима:
//    если у драйвера какая-то операция отсутствует, слой подставляет свою;
//  - универсальный консольный API (текст/тайлы/обои/скролл): реализация
//    «солянки» fb_driver_api_t поверх примитивов слоя. После
//    init_graphic_layer() глобальный fb из use_drivers.c указывает на
//    эту таблицу, поэтому всё ядро рисует через слой при любом активном
//    драйвере — отдельных проверок gr_active в вызывающем коде не нужно.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "drivers/include_drivers/driver_graphic_layer.h"
#include "include-kernel/graphic_layer.h"
#include "drivers/include_drivers/drv_format.h"
#include "include-kernel/bootinfo.h"
#include "include-kernel/kernel_heap.h"
#include "include-kernel/vmm.h"
#include "include-kernel/isr.h"
#include "include-kernel/lib.h"
#include "include-kernel/kconsole.h"
#include "include-kernel/kload_screen.h"
#include "include-kernel/locale.h"
#include "include-kernel/font_data.h"
#include "include-kernel/use_drivers.h"
#include "include-kernel/vbe_rm.h"
#include "include-kernel/comdebug.h"

//---------------- реестр драйверов ----------------
#define GR_DRIVERS_MAX 8

static gr_driver_t *gr_registry[GR_DRIVERS_MAX];
static uint32_t gr_driver_count = 0;
static gr_driver_t *gr_active = 0;
static int gr_initialized = 0;

/* Кэш текущего режима и fix-информации активного драйвера.
 * Нужен программным реализациям примитивов (LFB-доступ). */
static gr_mode_info_t gr_cur_mode;
static gr_fix_info_t gr_cur_fix;

//Валидация хранилища обоев (реализация в разделе универсального консольного API)
static void gr_wallpaper_validate(void);

/* ==================== программные реализации ==================== */

/* rgb = 0x00RRGGBB -> значение в формате текущего режима */
static uint32_t gr_scale8(uint32_t v, uint8_t len)
{
    if (len == 0) return 0;
    if (len >= 8) return v & 0xFF;
    return (((v & 0xFF) * ((1u << len) - 1)) + 127) / 255;
}

static uint32_t gr_unscale8(uint32_t v, uint8_t len)
{
    if (len == 0) return 0;
    if (len >= 8) return v & 0xFF;
    uint32_t max = (1u << len) - 1;
    return ((v & max) * 255 + max / 2) / max;
}

static uint32_t gr_pack_color(uint32_t rgb)
{
    if (gr_cur_mode.bpp >= 24) return rgb;   /* 24/32bpp — уже XRGB8888 */
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8)  & 0xFF;
    uint32_t b =  rgb        & 0xFF;
    return (gr_scale8(r, gr_cur_mode.red.length)   << gr_cur_mode.red.offset)
         | (gr_scale8(g, gr_cur_mode.green.length) << gr_cur_mode.green.offset)
         | (gr_scale8(b, gr_cur_mode.blue.length)  << gr_cur_mode.blue.offset);
}

static uint32_t gr_unpack_color(uint32_t raw)
{
    if (gr_cur_mode.bpp >= 24) return raw;
    uint32_t r = gr_unscale8(raw >> gr_cur_mode.red.offset,   gr_cur_mode.red.length);
    uint32_t g = gr_unscale8(raw >> gr_cur_mode.green.offset, gr_cur_mode.green.length);
    uint32_t b = gr_unscale8(raw >> gr_cur_mode.blue.offset,  gr_cur_mode.blue.length);
    return (r << 16) | (g << 8) | b;
}

/* Программный доступ возможен только к настоящему LFB */
static int gr_lfb_ready(void)
{
    if (!gr_active) return 0;
    if (gr_cur_mode.lfb == 0 || gr_cur_mode.lfb == 0xFFFFFFFF) return 0;
    if (gr_cur_mode.pitch == 0) return 0;
    if (gr_cur_mode.bpp != 8 && gr_cur_mode.bpp != 16 &&
        gr_cur_mode.bpp != 24 && gr_cur_mode.bpp != 32) return 0;
    return 1;
}

static int gr_lfb_put_pixel(int x, int y, uint32_t color)
{
    if (!gr_lfb_ready()) return GR_ERR_UNSUPPORTED;
    uint8_t *p = (uint8_t *)gr_cur_mode.lfb
               + (uint32_t)y * gr_cur_mode.pitch
               + (uint32_t)x * (gr_cur_mode.bpp / 8);
    switch (gr_cur_mode.bpp)
    {
    case 32:
        *(volatile uint32_t *)p = color;
        break;
    case 24:
        p[0] = color & 0xFF;
        p[1] = (color >> 8) & 0xFF;
        p[2] = (color >> 16) & 0xFF;
        break;
    case 16:
        *(volatile uint16_t *)p = (uint16_t)gr_pack_color(color);
        break;
    case 8:
        /* палитра: layer не знает соответствие цвет<->индекс, пишем индекс */
        *p = (uint8_t)(color & 0xFF);
        break;
    default:
        return GR_ERR_UNSUPPORTED;
    }
    return GR_OK;
}

static int gr_lfb_get_pixel(int x, int y, uint32_t *out)
{
    if (!gr_lfb_ready() || !out) return GR_ERR_UNSUPPORTED;
    const uint8_t *p = (const uint8_t *)gr_cur_mode.lfb
                     + (uint32_t)y * gr_cur_mode.pitch
                     + (uint32_t)x * (gr_cur_mode.bpp / 8);
    switch (gr_cur_mode.bpp)
    {
    case 32:
        *out = *(const volatile uint32_t *)p;
        break;
    case 24:
        *out = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        break;
    case 16:
        *out = gr_unpack_color(*(const volatile uint16_t *)p);
        break;
    case 8:
        *out = *p;   /* индекс палитры без преобразования */
        break;
    default:
        return GR_ERR_UNSUPPORTED;
    }
    return GR_OK;
}

static int gr_lfb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    if (!gr_lfb_ready()) return GR_ERR_UNSUPPORTED;
    uint32_t packed = gr_pack_color(color);
    for (int row = 0; row < h; row++)
    {
        uint8_t *line = (uint8_t *)gr_cur_mode.lfb
                      + (uint32_t)(y + row) * gr_cur_mode.pitch
                      + (uint32_t)x * (gr_cur_mode.bpp / 8);
        for (int col = 0; col < w; col++)
        {
            switch (gr_cur_mode.bpp)
            {
            case 32: ((volatile uint32_t *)line)[col] = packed; break;
            case 24:
                line[col * 3 + 0] =  packed        & 0xFF;
                line[col * 3 + 1] = (packed >> 8)  & 0xFF;
                line[col * 3 + 2] = (packed >> 16) & 0xFF;
                break;
            case 16: ((volatile uint16_t *)line)[col] = (uint16_t)packed; break;
            case 8:  line[col] = (uint8_t)(color & 0xFF); break;
            }
        }
    }
    return GR_OK;
}

/* RAM -> FB: пиксели упакованы в формате текущего bpp, строки без padding */
static int gr_lfb_blit(int x, int y, int w, int h, const void *src)
{
    if (!gr_lfb_ready() || !src) return GR_ERR_UNSUPPORTED;
    uint32_t bpp = gr_cur_mode.bpp / 8;
    uint32_t row_bytes = (uint32_t)w * bpp;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < h; i++)
    {
        uint8_t *d = (uint8_t *)gr_cur_mode.lfb
                   + (uint32_t)(y + i) * gr_cur_mode.pitch
                   + (uint32_t)x * bpp;
        memcpy(d, s + (uint32_t)i * row_bytes, row_bytes);
    }
    return GR_OK;
}

/* Копирование прямоугольника внутри LFB с учётом перекрытия */
static int gr_lfb_copy_area(int sx, int sy, int w, int h, int dx, int dy)
{
    if (!gr_lfb_ready()) return GR_ERR_UNSUPPORTED;
    if (sx == dx && sy == dy) return GR_OK;
    uint32_t bpp = gr_cur_mode.bpp / 8;
    uint32_t pitch = gr_cur_mode.pitch;
    uint8_t *base = (uint8_t *)gr_cur_mode.lfb;
    int i;
    if (dy <= sy)
    {
        for (i = 0; i < h; i++)
        {
            uint8_t *d = base + (uint32_t)(dy + i) * pitch + (uint32_t)dx * bpp;
            uint8_t *s = base + (uint32_t)(sy + i) * pitch + (uint32_t)sx * bpp;
            memmove(d, s, (uint32_t)w * bpp);
        }
    }
    else
    {
        for (i = h - 1; i >= 0; i--)
        {
            uint8_t *d = base + (uint32_t)(dy + i) * pitch + (uint32_t)dx * bpp;
            uint8_t *s = base + (uint32_t)(sy + i) * pitch + (uint32_t)sx * bpp;
            memmove(d, s, (uint32_t)w * bpp);
        }
    }
    return GR_OK;
}

/* Обрезка прямоугольника по границам экрана */
static int gr_clip(int *x, int *y, int *w, int *h)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (*w <= 0 || *h <= 0) return GR_ERR_BADARG;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if ((uint32_t)(*x + *w) > gr_cur_mode.xres) *w = (int)gr_cur_mode.xres - *x;
    if ((uint32_t)(*y + *h) > gr_cur_mode.yres) *h = (int)gr_cur_mode.yres - *y;
    if (*w <= 0 || *h <= 0) return GR_ERR_BADARG;
    return GR_OK;
}

/* ==================== кэш текущего режима ==================== */

static int gr_cache_current_mode(void)
{
    if (!gr_active) return GR_ERR_NODEV;

    memset(&gr_cur_mode, 0, sizeof(gr_cur_mode));
    if (gr_active->modes && gr_active->mode_count > gr_active->current_mode)
    {
        gr_cur_mode = gr_active->modes[gr_active->current_mode];
    }
    else if (gr_active->ops && gr_active->ops->get_var)
    {
        int rc = gr_active->ops->get_var(gr_active->priv, &gr_cur_mode);
        if (rc != GR_OK) return rc;
    }
    else if (gr_active->ops && gr_active->ops->get_mode)
    {
        int rc = gr_active->ops->get_mode(gr_active->priv, gr_active->current_mode, &gr_cur_mode);
        if (rc != GR_OK) return rc;
    }
    else
    {
        return GR_ERR_UNSUPPORTED;
    }

    /* fix-информация: из драйвера или синтез из mode */
    memset(&gr_cur_fix, 0, sizeof(gr_cur_fix));
    if (gr_active->ops && gr_active->ops->get_fix)
    {
        gr_active->ops->get_fix(gr_active->priv, &gr_cur_fix);
    }
    else
    {
        gr_cur_fix.smem_start  = gr_cur_mode.lfb;
        gr_cur_fix.smem_len    = gr_cur_mode.pitch * gr_cur_mode.yres;
        gr_cur_fix.type        = GR_TYPE_PACKED_PIXELS;
        gr_cur_fix.visual      = (gr_cur_mode.bpp == 8) ? GR_VISUAL_PSEUDOCOLOR
                                                        : GR_VISUAL_TRUECOLOR;
        gr_cur_fix.line_length = gr_cur_mode.pitch;
    }

    //Обои привязаны к размеру и формату конкретного режима: если режим
    //сменён (другой драйвер, другое разрешение) — сбрасываем несовпадающий
    //буфер, чтобы не вернуть на экран чужие пиксели
    gr_wallpaper_validate();
    return GR_OK;
}

/* ==================== fb-совместимый fallback ====================
 * Обёртка над штатным fb_driver (fb_driver_api_t *fb из use_drivers.c).
 * Всё, чего нет в старом API, остаётся NULL: слой подставит заглушки
 * (GR_ERR_UNSUPPORTED) или программные реализации через LFB. */

typedef struct fb_compat_ctx
{
    gr_mode_info_t mode;
    gr_fix_info_t  fix;
    gr_ops_t       ops;
} fb_compat_ctx_t;

static fb_compat_ctx_t fb_compat_ctx;
static gr_driver_t     fb_compat_driver;

//Нативная таблица fb_driver, захваченная ДО перепривязки глобального fb
//на универсальную таблицу слоя. Обёртки fbw_* обязаны звать именно её:
//вызов через глобальный fb после перепривязки дал бы бесконечную
//рекурсию (обёртка -> gr_* -> обёртка -> ...).
static fb_driver_api_t *fb_native = 0;

static int fbw_get_mode_count(void *priv) { (void)priv; return 1; }

static int fbw_get_mode(void *priv, uint32_t index, gr_mode_info_t *out)
{
    (void)priv;
    if (!out) return GR_ERR_BADARG;
    if (index != 0) return GR_ERR_BADMODE;
    *out = fb_compat_ctx.mode;
    return GR_OK;
}

static int fbw_get_var(void *priv, gr_mode_info_t *out)
{
    (void)priv;
    if (!out) return GR_ERR_BADARG;
    *out = fb_compat_ctx.mode;
    return GR_OK;
}

static int fbw_get_fix(void *priv, gr_fix_info_t *out)
{
    (void)priv;
    if (!out) return GR_ERR_BADARG;
    *out = fb_compat_ctx.fix;
    return GR_OK;
}

static int fbw_put_pixel(void *priv, int x, int y, uint32_t color)
{
    (void)priv;
    if (!fb_native || !fb_native->put_pixel) return GR_ERR_NODEV;
    fb_native->put_pixel(x, y, color);
    return GR_OK;
}

static int fbw_fill_rect(void *priv, int x, int y, int w, int h, uint32_t color)
{
    (void)priv;
    if (!fb_native || !fb_native->fill_rect) return GR_ERR_NODEV;
    fb_native->fill_rect(x, y, w, h, color);
    return GR_OK;
}

static int fbw_blit(void *priv, int x, int y, int w, int h, const void *src)
{
    (void)priv;
    if (!fb_native || !fb_native->blit) return GR_ERR_UNSUPPORTED;
    return (fb_native->blit(x, y, w, h, src) == 0) ? GR_OK : GR_ERR_IO;
}

/* set_mode/get_pixel/copy_area/pan/blank/vsync/palette сознательно NULL:
 * режим фиксирован загрузчиком, get_pixel и copy_area выполняются
 * программно слоем поверх того же LFB, остальное — честные заглушки. */

static void fb_compat_setup(void)
{
    memset(&fb_compat_ctx, 0, sizeof(fb_compat_ctx));
    memset(&fb_compat_driver, 0, sizeof(fb_compat_driver));
    if (!boot_info) return;

    gr_mode_info_t *m = &fb_compat_ctx.mode;
    uint32_t bpp = boot_info->bpp;
    if (bpp == 15) bpp = 16;   //так же, как в fb_driver.c

    m->index = 0;
    strcpy(m->name, "boot");
    m->xres = boot_info->width;
    m->yres = boot_info->height;
    m->xres_virtual = m->xres;
    m->yres_virtual = m->yres;
    m->bpp = bpp;
    if (bpp == 16)
    {
        m->red.offset = 11; m->red.length = 5;
        m->green.offset = 5; m->green.length = 6;
        m->blue.offset = 0; m->blue.length = 5;
    }
    else if (bpp >= 24)
    {
        m->red.offset = 16; m->red.length = 8;
        m->green.offset = 8; m->green.length = 8;
        m->blue.offset = 0; m->blue.length = 8;
    }
    m->lfb = boot_info->framebuffer;
    m->pitch = boot_info->pitch;
    m->flags = (boot_info->graphics_flags & GRAPHICS_FLAG_LFB) ? GR_MODE_LFB : 0;

    gr_fix_info_t *f = &fb_compat_ctx.fix;
    strcpy(f->id, "fbdrv");
    f->smem_start = m->lfb;
    f->smem_len = m->pitch * m->yres;
    f->type = GR_TYPE_PACKED_PIXELS;
    f->visual = (bpp == 8) ? GR_VISUAL_PSEUDOCOLOR : GR_VISUAL_TRUECOLOR;
    f->line_length = m->pitch;

    fb_compat_ctx.ops.version = GR_LAYER_ABI_VERSION;
    fb_compat_ctx.ops.get_mode_count = fbw_get_mode_count;
    fb_compat_ctx.ops.get_mode       = fbw_get_mode;
    fb_compat_ctx.ops.get_var        = fbw_get_var;
    fb_compat_ctx.ops.get_fix        = fbw_get_fix;
    fb_compat_ctx.ops.put_pixel      = fbw_put_pixel;
    fb_compat_ctx.ops.fill_rect      = fbw_fill_rect;
    fb_compat_ctx.ops.blit           = fbw_blit;

    fb_compat_driver.version = GR_LAYER_ABI_VERSION;
    strcpy(fb_compat_driver.name, "fbdrv");
    strcpy(fb_compat_driver.longname, "ARHeOS legacy fb_driver (boot LFB)");
    fb_compat_driver.priority = 0;
    fb_compat_driver.modes = &fb_compat_ctx.mode;
    fb_compat_driver.mode_count = 1;
    fb_compat_driver.ops = &fb_compat_ctx.ops;
    fb_compat_driver.priv = &fb_compat_ctx;

    //Захватываем нативную таблицу fb_driver. Сразу кладём её и в
    //native_driver_api: это «люк» в специфичные функции конкретного
    //драйвера, не попавшие в gr_ops_t. Каналом совместимости ядра с
    //fb_driver он не является — для этого служит универсальная таблица
    //слоя (см. gr_universal_api).
    fb_native = fb;
    fb_compat_driver.native_driver_api = (void *)fb_native;
}

//==================== универсальный консольный API ====================
//Текст, тайлы, обои и скролл — это политика консоли, а не работа железа,
//поэтому они НЕ входят в контракт драйвера (gr_ops_t) и реализуются здесь
//один раз, поверх примитивов слоя (gr_*). Любой драйвер — fb_driver,
//будущие VESA/Geode — получает их бесплатно, как fbcon поверх fbdev в
//Linux. Драйвер не должен уметь рисовать символы и хранить обои.

//Состояние цветов слоя: единый источник для uni_scroll_up и get_fg/get_bg
static uint32_t gr_fg_color = 0x00FFFFFF;
static uint32_t gr_bg_color = 0x00000000;

//Хранилище обоев слоя: полная копия экрана в формате текущего режима.
//Сменой режимов и валидацией размера ведает сам слой, драйверу знать
//об обоях не нужно
static uint8_t  *gr_wallpaper_raw = 0;
static uint32_t  gr_wallpaper_size = 0;
static bool      gr_wallpaper_present = false;

//геометрия и bitfields формата буфера обоев на момент установки: буфер
//рисуется userspace'ом в формате текущего экрана, и при смене режима
//пересобирается под новый формат (масштаб nearest-neighbor + перепаковка
//пикселей), поэтому слой обязан помнить, в каком формате лежат пиксели
static uint32_t gr_wp_w = 0;
static uint32_t gr_wp_h = 0;
static uint32_t gr_wp_bpp = 0;
static uint32_t gr_wp_pitch = 0;
static uint8_t  gr_wp_r_len = 0, gr_wp_r_off = 0;
static uint8_t  gr_wp_g_len = 0, gr_wp_g_off = 0;
static uint8_t  gr_wp_b_len = 0, gr_wp_b_off = 0;

static uint32_t gr_screen_bytes(void)
{
    return gr_cur_mode.pitch * gr_cur_mode.yres;
}

//Сброс/пересчет обоев, не соответствующих текущему режиму (после смены
//режима). Буфер обоев хранится в формате режима на момент установки, поэтому
//при смене разрешения перепаковываем пиксели в новый формат: читаем пиксель
//в старом bpp, распаковываем в 0x00RRGGBB по bitfields источника, пакуем
//в новый bpp и пишем с масштабом nearest-neighbor. Пересчитать нечем
//(формат источника неизвестен, 8bpp, не хватило памяти) — только тогда
//обои сбрасываются, чтобы не вернуть на экран чужие пиксели
static void gr_wp_forget_format(void)
{
    gr_wp_w = 0;
    gr_wp_h = 0;
    gr_wp_bpp = 0;
    gr_wp_pitch = 0;
    gr_wp_r_len = 0; gr_wp_r_off = 0;
    gr_wp_g_len = 0; gr_wp_g_off = 0;
    gr_wp_b_len = 0; gr_wp_b_off = 0;
}

//распаковка пикселя буфера обоев в 0x00RRGGBB по формату источника
static uint32_t gr_wp_read_rgb(const uint8_t *row, uint32_t x)
{
    const uint8_t *p = row + x * (gr_wp_bpp / 8);
    uint32_t raw;
    switch (gr_wp_bpp)
    {
    case 32:
        raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
            | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        break;
    case 24:
        raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        break;
    default:
        raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8);   //15/16 — оба двухбайтовые
        break;
    }
    uint32_t r = gr_unscale8(raw >> gr_wp_r_off, gr_wp_r_len);
    uint32_t g = gr_unscale8(raw >> gr_wp_g_off, gr_wp_g_len);
    uint32_t b = gr_unscale8(raw >> gr_wp_b_off, gr_wp_b_len);
    return (r << 16) | (g << 8) | b;
}

//запись пикселя в новый буфер в формате текущего режима
static void gr_wp_write_px(uint8_t *row, uint32_t x, uint32_t rgb)
{
    uint32_t packed = gr_pack_color(rgb);
    uint8_t *p = row + x * (gr_cur_mode.bpp / 8);
    switch (gr_cur_mode.bpp)
    {
    case 32:
        *(uint32_t *)p = packed;
        break;
    case 24:
        p[0] =  packed        & 0xFF;
        p[1] = (packed >> 8)  & 0xFF;
        p[2] = (packed >> 16) & 0xFF;
        break;
    default:
        *(uint16_t *)p = (uint16_t)packed;   //15/16
        break;
    }
}

//запоминание формата источника обоев — вызывается после установки.
//источник обязан совпадать по размеру с текущим экраном: setbg строит
//буфер из данных SYS_GET_FB_INFO, и те теперь всегда актуальны
static void gr_wp_capture_format(void)
{
    if (gr_wallpaper_size != gr_screen_bytes() || gr_cur_mode.bpp < 16)
    {
        gr_wp_forget_format();
        return;
    }
    gr_wp_w = gr_cur_mode.xres;
    gr_wp_h = gr_cur_mode.yres;
    gr_wp_bpp = gr_cur_mode.bpp;
    gr_wp_pitch = gr_cur_mode.pitch;
    gr_wp_r_len = gr_cur_mode.red.length;   gr_wp_r_off = gr_cur_mode.red.offset;
    gr_wp_g_len = gr_cur_mode.green.length; gr_wp_g_off = gr_cur_mode.green.offset;
    gr_wp_b_len = gr_cur_mode.blue.length;  gr_wp_b_off = gr_cur_mode.blue.offset;
}

static void gr_wallpaper_validate(void)
{
    if (!gr_wallpaper_present || !gr_wallpaper_raw) return;
    uint32_t need = gr_screen_bytes();

    //совпадение байтового размера не означает совпадения формата:
    //1024x768x16 и 512x384x32 дают одинаковый размер буфера, но разную
    //раскладку пикселей — blit чужого формата дал бы мусорную палитру.
    //обои остаются без пересчета только при полном совпадении геометрии
    //и формата с текущим режимом
    if (gr_wallpaper_size == need &&
        gr_wp_w == gr_cur_mode.xres && gr_wp_h == gr_cur_mode.yres &&
        gr_wp_bpp == gr_cur_mode.bpp && gr_wp_pitch == gr_cur_mode.pitch)
        return;

    bool can_remap = gr_wp_w != 0 && gr_wp_h != 0 &&
                     gr_wp_bpp >= 16 && gr_cur_mode.bpp >= 16 &&
                     gr_wallpaper_size == gr_wp_pitch * gr_wp_h;
    if (!can_remap)
    {
        kfree(gr_wallpaper_raw);
        gr_wallpaper_raw = 0;
        gr_wallpaper_size = 0;
        gr_wallpaper_present = false;
        gr_wp_forget_format();
        return;
    }

    uint8_t *nbuf = (uint8_t *)kmalloc(need);
    if (!nbuf)
    {
        kfree(gr_wallpaper_raw);
        gr_wallpaper_raw = 0;
        gr_wallpaper_size = 0;
        gr_wallpaper_present = false;
        gr_wp_forget_format();
        return;
    }

    //построчная перепаковка: строка-источник выбирается по пропорции высот,
    //пиксель в строке — по пропорции ширин (nearest-neighbor)
    uint32_t nw = gr_cur_mode.xres;
    uint32_t nh = gr_cur_mode.yres;
    for (uint32_t ny = 0; ny < nh; ny++)
    {
        const uint8_t *srow = gr_wallpaper_raw
                            + (uint32_t)(ny * gr_wp_h / nh) * gr_wp_pitch;
        uint8_t *drow = nbuf + (uint32_t)ny * gr_cur_mode.pitch;
        for (uint32_t nx = 0; nx < nw; nx++)
            gr_wp_write_px(drow, nx, gr_wp_read_rgb(srow, nx * gr_wp_w / nw));
    }

    kfree(gr_wallpaper_raw);
    gr_wallpaper_raw = nbuf;
    gr_wallpaper_size = need;
    gr_wp_w = nw;
    gr_wp_h = nh;
    gr_wp_bpp = gr_cur_mode.bpp;
    gr_wp_pitch = gr_cur_mode.pitch;
    gr_wp_r_len = gr_cur_mode.red.length;   gr_wp_r_off = gr_cur_mode.red.offset;
    gr_wp_g_len = gr_cur_mode.green.length; gr_wp_g_off = gr_cur_mode.green.offset;
    gr_wp_b_len = gr_cur_mode.blue.length;  gr_wp_b_off = gr_cur_mode.blue.offset;
}

//Декодирует один кодпойнт UTF-8, возвращает число потреблённых байт (>=1).
//Некорректные последовательности читаются как одиночный байт, как в fb_driver
static uint32_t gr_utf8_decode(const char *s, int *consumed)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t cp = p[0];
    int n = 1;
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
    {
        cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        n = 2;
    }
    else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    {
        cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6)
           | (uint32_t)(p[2] & 0x3F);
        n = 3;
    }
    else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
             (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
    {
        cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12)
           | ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
        n = 4;
    }
    *consumed = n;
    return cp;
}

//Обёртка для таблицы: активная локаль ядра (тот же источник, что получает fb_driver)
static const void *gr_kernel_active_locale(void)
{
    return (const void *)locale_get_active();
}

//Рисование одного кодпойнта активной локали через примитивы слоя
static void uni_draw_codepoint(uint32_t x, uint32_t y, uint32_t cp, uint32_t color)
{
    const locale_t *loc = locale_get_active();
    if (!loc || !loc->font) return;
    const font_glyph_t *g = font_lookup(loc->font, cp);
    if (!g) return;
    if (x + g->width  > gr_cur_mode.xres) return;
    if (y + g->height > gr_cur_mode.yres) return;

    uint8_t bytes_per_row = (uint8_t)((g->width + 7) / 8);
    for (uint32_t row = 0; row < g->height; row++)
    {
        const uint8_t *src = g->rows + row * bytes_per_row;
        for (uint32_t col = 0; col < g->width; col++)
        {
            if (src[col >> 3] & (0x80 >> (col & 7)))
                gr_put_pixel((int)(x + col), (int)(y + row), color);
        }
    }
}

static void uni_draw_char(uint32_t x, uint32_t y, unsigned char c, uint32_t color)
{
    //То же, что в fb_driver: байт трактуется как кодпойнт активной локали
    uni_draw_codepoint(x, y, (uint32_t)c, color);
}

static void uni_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t color)
{
    if (!str) return;
    while (*str)
    {
        int consumed = 1;
        uint32_t cp = gr_utf8_decode(str, &consumed);
        uni_draw_codepoint(x, y, cp, color);
        x += 8;   //моноширинный шаг, как в fb_driver
        str += consumed;
    }
}

static void uni_draw_hex(uint32_t value, int x, int y)
{
    static const char hexdigits[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++)
    {
        uint32_t v = (value >> ((7 - i) * 4)) & 0xF;
        uni_draw_codepoint((uint32_t)(x + i * 8), (uint32_t)y,
                           (uint32_t)hexdigits[v], 0x00FFFFFF);
    }
}

//«Очистить» с обоями = вернуть обои на весь экран, как в fb_driver.fb_clear
static void uni_clear(uint32_t color)
{
    if (gr_wallpaper_present && gr_wallpaper_raw &&
        gr_wallpaper_size >= gr_screen_bytes())
    {
        //Построчно: буфер обоев хранится с шагом pitch, а gr_blit ждёт
        //упакованные строки без padding
        for (uint32_t row = 0; row < gr_cur_mode.yres; row++)
            gr_blit(0, (int)row, (int)gr_cur_mode.xres, 1,
                    gr_wallpaper_raw + row * gr_cur_mode.pitch);
        return;
    }
    gr_clear(color);
}

static void uni_put_pixel(int x, int y, uint32_t color)
{
    gr_put_pixel(x, y, color);
}

static void uni_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    gr_fill_rect(x, y, w, h, color);
}

static int uni_blit(int x, int y, int w, int h, const void *src)
{
    return (gr_blit(x, y, w, h, src) == GR_OK) ? 0 : -1;
}

//Обои принимает как есть: приходят из userspace уже в формате экрана
//(setbg декодирует PNG/TGA в raw фреймбуфера)
static void uni_set_wallpaper(const void *src, uint32_t size)
{
    if (!src || size == 0) return;
    if (size > (4 * 1024 * 1024)) return;   //тот же лимит, что в fb_driver
    if (gr_wallpaper_raw)
    {
        kfree(gr_wallpaper_raw);
        gr_wallpaper_raw = 0;
    }
    gr_wallpaper_raw = (uint8_t *)kmalloc(size);
    if (!gr_wallpaper_raw)
    {
        gr_wallpaper_size = 0;
        gr_wallpaper_present = false;
        return;
    }
    memcpy(gr_wallpaper_raw, src, size);
    gr_wallpaper_size = size;
    gr_wallpaper_present = true;
    gr_wp_capture_format();
}

static bool uni_has_wallpaper(void)
{
    return gr_wallpaper_present;
}

//Тайл 8x8, как в fb_driver: с обоями возвращаем пиксели обоев,
//без — заливаем цветом фона консоли
static void uni_clear_tile(int x, int y, uint32_t fallback_color)
{
    if (x < 0 || y < 0 ||
        (uint32_t)x + 8 > gr_cur_mode.xres || (uint32_t)y + 8 > gr_cur_mode.yres)
        return;
    if (gr_wallpaper_present && gr_wallpaper_raw &&
        gr_wallpaper_size >= gr_screen_bytes())
    {
        for (int row = 0; row < 8; row++)
            gr_blit(x, y + row, 8, 1,
                    gr_wallpaper_raw + (uint32_t)(y + row) * gr_cur_mode.pitch
                    + (uint32_t)x * (gr_cur_mode.bpp / 8));
    }
    else
    {
        gr_fill_rect(x, y, 8, 8, fallback_color);
    }
}

//Сдвиг экрана вверх на line_height в диапазоне [start_y, end_y);
//нижняя полоса заливается текущим цветом фона слоя
static void uni_scroll_up(uint32_t start_y, uint32_t end_y, uint32_t line_height)
{
    if (start_y + line_height >= end_y || end_y > gr_cur_mode.yres) return;
    if (start_y >= end_y) return;

    uint32_t move_h = (end_y - line_height) - start_y;
    if (move_h)
        gr_copy_area(0, (int)(start_y + line_height), (int)gr_cur_mode.xres,
                     (int)move_h, 0, (int)start_y);
    gr_fill_rect(0, (int)(end_y - line_height), (int)gr_cur_mode.xres,
                 (int)line_height, gr_bg_color);
}

//Сдвиг прямоугольника по вертикали: dy > 0 — вверх, dy < 0 — вниз;
//освободившаяся полоса заливается цветом fill. Контракт как в fb_driver
static int uni_scroll_rect(int x, int y, int w, int h, int dy, uint32_t fill)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return -1;
    if (x + w > (int)gr_cur_mode.xres || y + h > (int)gr_cur_mode.yres) return -1;
    if (dy == 0) return 0;
    if (dy >= h || dy <= -h)
    {
        gr_fill_rect(x, y, w, h, fill);
        return 0;
    }
    if (dy > 0)
    {
        int move_h = h - dy;
        gr_copy_area(x, y + dy, w, move_h, x, y);
        gr_fill_rect(x, y + move_h, w, dy, fill);
    }
    else
    {
        int ady = -dy;
        int move_h = h - ady;
        gr_copy_area(x, y, w, move_h, x, y + ady);
        gr_fill_rect(x, y, w, ady, fill);
    }
    return 0;
}

static void uni_set_fg(uint32_t color) { gr_fg_color = color; }
static void uni_set_bg(uint32_t color) { gr_bg_color = color; }
static uint32_t uni_get_fg(void) { return gr_fg_color; }
static uint32_t uni_get_bg(void) { return gr_bg_color; }

//Универсальная таблица вида fb_driver_api_t: ЕДИНАЯ точка диспетчеризации
//всего рисования ядра. После init_graphic_layer() глобальный fb из
//use_drivers.c указывает сюда: примитивы уходят в gr_* (нативные операции
//активного драйвера или программные реализации через LFB), текст/тайлы/
//обои/скролл — в универсальные реализации выше. Когда завтра активируется
//VESA или Geode, ядро и эта таблица не меняются — под драйвером меняется
//только gr_active.
static fb_driver_api_t gr_universal_api;

static void gr_universal_api_setup(void)
{
    memset(&gr_universal_api, 0, sizeof(gr_universal_api));

    gr_universal_api.put_pixel      = uni_put_pixel;
    gr_universal_api.fill_rect      = uni_fill_rect;
    gr_universal_api.clear          = uni_clear;
    gr_universal_api.draw_char      = uni_draw_char;
    gr_universal_api.draw_string    = uni_draw_string;
    gr_universal_api.draw_hex       = uni_draw_hex;
    gr_universal_api.draw_codepoint = uni_draw_codepoint;

    gr_universal_api.set_fg = uni_set_fg;
    gr_universal_api.set_bg = uni_set_bg;
    gr_universal_api.get_fg = uni_get_fg;
    gr_universal_api.get_bg = uni_get_bg;

    gr_universal_api.scroll_up    = uni_scroll_up;
    gr_universal_api.scroll_rect  = uni_scroll_rect;
    gr_universal_api.blit         = uni_blit;

    gr_universal_api.set_wallpaper = uni_set_wallpaper;
    gr_universal_api.clear_tile    = uni_clear_tile;
    gr_universal_api.has_wallpaper = uni_has_wallpaper;

    gr_universal_api.malloc_ptr = kmalloc;
    gr_universal_api.free_ptr   = kfree;

    //Шрифтовая подсистема: те же источники, что инжектируются в fb_driver,
    //но слой берёт их напрямую из ядра, без обратных вызовов через драйвер
    gr_universal_api.font = (const uint8_t (*)[8])font_data;
    gr_universal_api.get_active_locale_ptr = gr_kernel_active_locale;
    gr_universal_api.font_lookup_ptr =
        (const void *(*)(const void *, uint32_t))font_lookup;
}

//==================== реестр драйверов ====================

static void gr_inject_runtime(gr_driver_t *drv)
{
    if (!drv->malloc_ptr) drv->malloc_ptr = kmalloc;
    if (!drv->free_ptr)   drv->free_ptr   = kfree;
    if (!drv->yield_ptr)  drv->yield_ptr  = sys_yield;
    if (!drv->vmm_map_pages_ptr)          drv->vmm_map_pages_ptr          = vmm_map_pages;
    if (!drv->vmm_map_pages_uncached_ptr) drv->vmm_map_pages_uncached_ptr = vmm_map_pages_uncached;
    if (!drv->printf_ptr) drv->printf_ptr = kprintf;
    //Сервис вызова BIOS int 10h через real-mode тримплин: нужен драйверам
    //со сменой видеорежима на ходу (vesa_fb -> 4F02); остальным не мешает
    if (!drv->vbe_rm_exec_ptr) drv->vbe_rm_exec_ptr = vbe_rm_exec;
}

int graphic_layer_register_driver(gr_driver_t *drv)
{
    if (!drv || !drv->ops) return GR_ERR_BADARG;
    if (drv->version != GR_LAYER_ABI_VERSION)
    {
        if (drv->printf_ptr)
            drv->printf_ptr("GFX: driver '%s' rejected, ABI %d != %d\n",
                            drv->name, (int)drv->version, GR_LAYER_ABI_VERSION);
        return GR_ERR_BADARG;
    }
    for (uint32_t i = 0; i < gr_driver_count; i++)
        if (gr_registry[i] == drv) return GR_ERR_BUSY;
    if (gr_driver_count >= GR_DRIVERS_MAX) return GR_ERR_NOMEM;

    gr_inject_runtime(drv);
    gr_registry[gr_driver_count++] = drv;

    //Автопереключение: нет активного или новый не менее приоритетный
    if (!gr_active || drv->priority >= gr_active->priority)
    {
        gr_active = drv;
        gr_cache_current_mode();
    }
    return GR_OK;
}

int graphic_layer_unregister_driver(const char *name)
{
    for (uint32_t i = 0; i < gr_driver_count; i++)
    {
        if (strcmp(gr_registry[i]->name, name) == 0)
        {
            int was_active = (gr_registry[i] == gr_active);
            for (uint32_t j = i; j + 1 < gr_driver_count; j++)
                gr_registry[j] = gr_registry[j + 1];
            gr_driver_count--;
            if (was_active)
            {
                gr_active = 0;
                gr_driver_t *best = 0;
                for (uint32_t k = 0; k < gr_driver_count; k++)
                    if (!best || gr_registry[k]->priority > best->priority)
                        best = gr_registry[k];
                if (best) { gr_active = best; gr_cache_current_mode(); }
            }
            return GR_OK;
        }
    }
    return GR_ERR_NOTFOUND;
}

int graphic_layer_activate_driver(const char *name)
{
    for (uint32_t i = 0; i < gr_driver_count; i++)
    {
        if (strcmp(gr_registry[i]->name, name) == 0)
        {
            gr_active = gr_registry[i];
            return gr_cache_current_mode();
        }
    }
    return GR_ERR_NOTFOUND;
}

gr_driver_t *graphic_layer_get_active(void) { return gr_active; }
uint32_t graphic_layer_get_driver_count(void) { return gr_driver_count; }

/* ==================== инициализация слоя ==================== */

void init_graphic_layer(void)
{
    if (gr_initialized) return;
    gr_initialized = 1;

    //Внешние драйверы могли зарегистрироваться раньше нас.
    gr_driver_t *best = 0;
    for (uint32_t i = 0; i < gr_driver_count; i++)
        if (!best || gr_registry[i]->priority > best->priority)
            best = gr_registry[i];

    if (best)
    {
        gr_active = best;
        gr_cache_current_mode();
        load_screen_add_string("GFX layer: active '%s'", best->name);
    }
    else
    {
        //Никакого иного драйвера не загружено — подключаем штатный fb_driver.
        //native_driver_api с нативной таблицей fb_driver выставляется внутри
        //fb_compat_setup() до перепривязки fb.
        fb_compat_setup();
        graphic_layer_register_driver(&fb_compat_driver);
        if (boot_info)
            load_screen_add_string("GFX layer: fbdrv fallback %dx%dx%d",
                                   (int)boot_info->width, (int)boot_info->height,
                                   (int)boot_info->bpp);
        else
            load_screen_add_string("GFX layer: fbdrv fallback (no bootinfo)");
    }

    //Единая точка диспетчеризации: перенаправляем глобальный fb на
    //универсальную таблицу слоя. Все вызовы fb->... в ядре с этого
    //момента идут через слой при любом активном драйвере, проверки
    //gr_active в вызывающем коде не нужны. Нативная таблица fb_driver
    //уже захвачена в fb_native и доступна через native_driver_api.
    gr_universal_api_setup();
    fb = &gr_universal_api;
}

/* ==================== API режимов ==================== */

int gr_get_mode_count(void)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (gr_active->modes) return (int)gr_active->mode_count;
    if (gr_active->ops && gr_active->ops->get_mode_count)
        return gr_active->ops->get_mode_count(gr_active->priv);
    return 0;
}

int gr_enum_mode(uint32_t index, gr_mode_info_t *out)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (!out) return GR_ERR_BADARG;
    if (gr_active->modes)
    {
        if (index >= gr_active->mode_count) return GR_ERR_BADMODE;
        *out = gr_active->modes[index];
        return GR_OK;
    }
    if (gr_active->ops && gr_active->ops->get_mode)
        return gr_active->ops->get_mode(gr_active->priv, index, out);
    return GR_ERR_UNSUPPORTED;
}

int gr_set_mode(uint32_t index)
{
    if (!gr_active) return GR_ERR_NODEV;
    //трасса смены режима: видно, дошел ли вызов до драйвера и с каким кодом
    //вышел; тишина после этой строки означает зависание внутри set_mode
    com_puts("[GR] set_mode idx=");
    com_puthex(index);
    if (gr_active->ops && gr_active->ops->set_mode)
    {
        int rc = gr_active->ops->set_mode(gr_active->priv, index);
        com_puts(" rc=");
        com_puthex((uint32_t)rc);
        com_puts("\n");
        if (rc != GR_OK) return rc;
    }
    else
    {
        //драйвер не умеет менять режим — разрешаем только текущий
        com_puts(" no set_mode op\n");
        if (index != gr_active->current_mode) return GR_ERR_UNSUPPORTED;
    }
    gr_active->current_mode = index;
    return gr_cache_current_mode();
}

int gr_get_current_mode(gr_mode_info_t *out)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (!out) return GR_ERR_BADARG;
    *out = gr_cur_mode;
    return GR_OK;
}

int gr_get_fix_info(gr_fix_info_t *out)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (!out) return GR_ERR_BADARG;
    *out = gr_cur_fix;
    return GR_OK;
}

/* ==================== API примитивов ==================== */

int gr_put_pixel(int x, int y, uint32_t color)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (x < 0 || y < 0 ||
        (uint32_t)x >= gr_cur_mode.xres || (uint32_t)y >= gr_cur_mode.yres)
        return GR_ERR_BADARG;
    if (gr_active->ops && gr_active->ops->put_pixel)
        return gr_active->ops->put_pixel(gr_active->priv, x, y, color);
    return gr_lfb_put_pixel(x, y, color);
}

int gr_get_pixel(int x, int y, uint32_t *out)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (x < 0 || y < 0 ||
        (uint32_t)x >= gr_cur_mode.xres || (uint32_t)y >= gr_cur_mode.yres)
        return GR_ERR_BADARG;
    if (gr_active->ops && gr_active->ops->get_pixel)
        return gr_active->ops->get_pixel(gr_active->priv, x, y, out);
    return gr_lfb_get_pixel(x, y, out);
}

int gr_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    int rc = gr_clip(&x, &y, &w, &h);
    if (rc != GR_OK) return rc;
    if (gr_active->ops && gr_active->ops->fill_rect)
        return gr_active->ops->fill_rect(gr_active->priv, x, y, w, h, color);
    return gr_lfb_fill_rect(x, y, w, h, color);
}

int gr_clear(uint32_t color)
{
    if (!gr_active) return GR_ERR_NODEV;
    return gr_fill_rect(0, 0, (int)gr_cur_mode.xres, (int)gr_cur_mode.yres, color);
}

int gr_hline(int x, int y, int w, uint32_t color)
{
    return gr_fill_rect(x, y, w, 1, color);
}

int gr_vline(int x, int y, int h, uint32_t color)
{
    return gr_fill_rect(x, y, 1, h, color);
}

int gr_draw_rect(int x, int y, int w, int h, uint32_t color)
{
    int rc = gr_clip(&x, &y, &w, &h);
    if (rc != GR_OK) return rc;
    gr_hline(x, y, w, color);
    gr_hline(x, y + h - 1, w, color);
    gr_vline(x, y, h, color);
    gr_vline(x + w - 1, y, h, color);
    return GR_OK;
}

int gr_draw_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    if (!gr_active) return GR_ERR_NODEV;
    /* Брезенхэм; отсечением по экрану не заморачиваемся — put_pixel сам отбросит */
    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x1 >= x0) ? 1 : -1;
    int sy = (y1 >= y0) ? 1 : -1;
    int err = dx - dy;
    for (;;)
    {
        gr_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
    return GR_OK;
}

int gr_blit(int x, int y, int w, int h, const void *src)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (w <= 0 || h <= 0) return GR_ERR_BADARG;
    if (x < 0 || y < 0 ||
        (uint32_t)(x + w) > gr_cur_mode.xres || (uint32_t)(y + h) > gr_cur_mode.yres)
        return GR_ERR_BADARG;
    if (gr_active->ops && gr_active->ops->blit)
        return gr_active->ops->blit(gr_active->priv, x, y, w, h, src);
    return gr_lfb_blit(x, y, w, h, src);
}

int gr_copy_area(int sx, int sy, int w, int h, int dx, int dy)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (w <= 0 || h <= 0) return GR_ERR_BADARG;
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0 ||
        (uint32_t)(sx + w) > gr_cur_mode.xres || (uint32_t)(sy + h) > gr_cur_mode.yres ||
        (uint32_t)(dx + w) > gr_cur_mode.xres || (uint32_t)(dy + h) > gr_cur_mode.yres)
        return GR_ERR_BADARG;
    if (gr_active->ops && gr_active->ops->copy_area)
        return gr_active->ops->copy_area(gr_active->priv, sx, sy, w, h, dx, dy);
    return gr_lfb_copy_area(sx, sy, w, h, dx, dy);
}

/* ==================== управление выводом ==================== */

int gr_pan_display(uint32_t yoffset)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (!gr_active->ops || !gr_active->ops->pan_display) return GR_ERR_UNSUPPORTED;
    int rc = gr_active->ops->pan_display(gr_active->priv, yoffset);
    if (rc == GR_OK) gr_cache_current_mode();
    return rc;
}

int gr_blank(int blank)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (!gr_active->ops || !gr_active->ops->blank) return GR_ERR_UNSUPPORTED;
    return gr_active->ops->blank(gr_active->priv, blank);
}

int gr_wait_vsync(void)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (!gr_active->ops || !gr_active->ops->wait_vsync) return GR_ERR_UNSUPPORTED;
    return gr_active->ops->wait_vsync(gr_active->priv);
}

int gr_set_palette(uint32_t start, uint32_t count, const uint8_t *rgb)
{
    if (!gr_active) return GR_ERR_NODEV;
    if (!gr_active->ops || !gr_active->ops->set_palette) return GR_ERR_UNSUPPORTED;
    if (!rgb && count) return GR_ERR_BADARG;
    return gr_active->ops->set_palette(gr_active->priv, start, count, rgb);
}
