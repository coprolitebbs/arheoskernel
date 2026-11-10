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

//vesa_fb.c — универсальный драйвер VESA VBE по модели simplefb.
//
//Драйвер не программирует железо и не умеет рисовать: он умеет
//  1) прочитать таблицу режимов, собранную stage2 (vbe.inc -> bootinfo);
//  2) отдать её графическому слою через gr_ops_t (get_mode_count/get_mode);
//  3) переключать режимы на ходу через сервис vbe_rm_exec (int 10h 4F02
//     выполняется в реальном режиме тримплином vbe_rm_blob.asm).
//Все примитивы рисования (put_pixel/fill_rect/blit/...) остаются NULL —
//graphic_layer.c подставляет свои программные реализации поверх LFB
//текущего режима. Поэтому драйверу безразличны bpp и pitch конкретного
//режима: слой сам переупаковывает цвета и учитывает длину строки.
//Загружается как обычный ELF-модуль из /adrv (load_additional_drivers,
//DESCR_GRAPHICS_DRIVER), как kbdps2.drv.

#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/lib.h"
//#include "../../include-kernel/vbe_rm.h"
#include "../include_drivers/drv_format.h"
#include "../include_drivers/driver_graphic_layer.h"

static vbe_table_hdr_t *vbe_hdr = 0;
static vbe_mode_rec_t  *vbe_recs = 0;
static uint32_t         vbe_count = 0;

//карта drawable-режимов: слой рисует программно поверх LFB и не умеет
//программировать DAC-палитру, поэтому 8bpp-режимы из списка выбрасываются.
//иначе "-set 800x600" на qemu попадает в первый же 800x600x8 (mode 0x103)
//и экран превращается в цветной мусор: драйвер пишет сырые индексы палитры
static uint32_t vbe_idx_map[VBE_MODES_MAX];
static uint32_t vbe_drawable = 0;

//bitfields режимов, прочитанные из ModeInfoBlock после 4F01 (смещения
//0x1F..0x24). нулевые длины означают "не прочитано — возьми канонический
//дефолт": некоторые BIOS (cirrus/trident в 86box) отдают раскладку,
//отличающуюся от захардкоженной, и пересчет цвета едет
static struct vbe_bitfields
{
    uint8_t bf_r_len, bf_r_off;
    uint8_t bf_g_len, bf_g_off;
    uint8_t bf_b_len, bf_b_off;
} vbe_bits[VBE_MODES_MAX];

static gr_mode_info_t vesa_cur_mode;
static gr_fix_info_t  vesa_fix;
static int            vesa_ready = 0;

static description_driver_api_t descr_api;
static gr_ops_t       vesa_ops;
static gr_driver_t    vesa_driver;


static uint8_t *vbe_rm_info_buf(void)
{
    return (uint8_t *)VBE_RM_INFO_ADDR;
}

//имя режима вида "1024x768x24"
static void vesa_mode_name(uint32_t x, uint32_t y, uint32_t bpp, char *out)
{
    char a[12], b[12], c[12];
    uitoa(x, a, 10);
    uitoa(y, b, 10);
    uitoa(bpp, c, 10);
    strcpy(out, a);
    strcat(out, "x");
    strcat(out, b);
    strcat(out, "x");
    strcat(out, c);
}

//заполнение gr_mode_info_t из записи таблицы.
//index — drawable-индекс (после фильтра 8bpp), переводится в сырой через vbe_idx_map
static void vesa_fill_mode(uint32_t index, gr_mode_info_t *m)
{
    const vbe_mode_rec_t *r = &vbe_recs[vbe_idx_map[index]];
    memset(m, 0, sizeof(gr_mode_info_t));
    m->index = index;
    vesa_mode_name(r->xres, r->yres, r->bpp, m->name);
    m->xres = r->xres;
    m->yres = r->yres;
    m->xres_virtual = r->xres;
    m->yres_virtual = r->yres;
    m->bpp = r->bpp;

    //канонические bitfields-дефолты: 32/24 — RGB888, 16 — RGB565,
    //8 — палитра (слой сам знает)
    if (m->bpp == 16)
    {
        m->red.offset = 11; m->red.length = 5;
        m->green.offset = 5; m->green.length = 6;
        m->blue.offset = 0; m->blue.length = 5;
    }
    else if (m->bpp >= 24)
    {
        m->red.offset = 16; m->red.length = 8;
        m->green.offset = 8; m->green.length = 8;
        m->blue.offset = 0; m->blue.length = 8;
    }

    //прочитанные из 4F01 раскладки точнее дефолта — перекрываем, если есть
    const struct vbe_bitfields *bf = &vbe_bits[index];
    if (bf->bf_r_len && bf->bf_g_len && bf->bf_b_len)
    {
        m->red.offset = bf->bf_r_off;     m->red.length = bf->bf_r_len;
        m->green.offset = bf->bf_g_off;   m->green.length = bf->bf_g_len;
        m->blue.offset = bf->bf_b_off;    m->blue.length = bf->bf_b_len;
    }

    m->lfb = r->phys_base;
    m->pitch = r->pitch;
    m->flags = GR_MODE_LFB;
}

//---- ops: перечисление и текущий режим ----

static int vs_get_mode_count(void *priv)
{
    (void)priv;
    return vesa_ready ? (int)vbe_drawable : 0;
}

static int vs_get_mode(void *priv, uint32_t index, gr_mode_info_t *out)
{
    (void)priv;
    if (!vesa_ready || !out) return GR_ERR_BADARG;
    if (index >= vbe_drawable) return GR_ERR_BADMODE;
    vesa_fill_mode(index, out);
    return GR_OK;
}

static int vs_get_var(void *priv, gr_mode_info_t *out)
{
    (void)priv;
    if (!vesa_ready || !out) return GR_ERR_NODEV;
    *out = vesa_cur_mode;
    return GR_OK;
}

static int vs_get_fix(void *priv, gr_fix_info_t *out)
{
    (void)priv;
    if (!vesa_ready || !out) return GR_ERR_NODEV;
    *out = vesa_fix;
    return GR_OK;
}

//---- ops: смена режима на ходу через real-mode тримплин ----

static int vs_set_mode(void *priv, uint32_t index)
{
    (void)priv;
    if (!vesa_ready) return GR_ERR_NODEV;
    if (index >= vbe_drawable) return GR_ERR_BADMODE;
    if (!vesa_driver.vbe_rm_exec_ptr) return GR_ERR_UNSUPPORTED;

    const vbe_mode_rec_t *r = &vbe_recs[vbe_idx_map[index]];

    //4F02: установка режима с флагом LFB. vbe.inc пишет номер в таблицу
    //уже с битом 0x4000, поэтому перед OR сбрасываем оба флаговых бита
    //14-15: случайно выставленный бит 15 BIOS трактует как "не чистить
    //экран", и в LFB остаются пиксели предыдущего режима — мусор на весь
    //кадр до первой полной перерисовки
    vbe_rm_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4F02;
    regs.bx = (uint16_t)((r->mode_number & 0x3FFF) | 0x4000);
    if (vesa_driver.vbe_rm_exec_ptr(&regs) != 0) return GR_ERR_IO;

    //4F01: перечитываем параметры — pitch/bpp/PhysBasePtr могли смениться.
    //номер режима обязан быть чистым: биты 14-15 некоторые BIOS считают
    //за неизвестный режим (тот же грабель, что в stage2, см. vbe.inc)
    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4F01;
    regs.cx = r->mode_number & 0x3FFF;
    regs.es = VBE_RM_INFO_ADDR >> 4;
    regs.di = VBE_RM_INFO_ADDR & 0xF;
    if (vesa_driver.vbe_rm_exec_ptr(&regs) != 0) return GR_ERR_IO;

    const uint8_t *mi = vbe_rm_info_buf();
    uint32_t xres  = (uint32_t)mi[0x12] | ((uint32_t)mi[0x13] << 8);
    uint32_t yres  = (uint32_t)mi[0x14] | ((uint32_t)mi[0x15] << 8);
    uint32_t bpp   = mi[0x19];
    uint32_t pitch = (uint32_t)mi[0x10] | ((uint32_t)mi[0x11] << 8);
    uint32_t phys  = (uint32_t)mi[0x28] | ((uint32_t)mi[0x29] << 8)
                   | ((uint32_t)mi[0x2A] << 16) | ((uint32_t)mi[0x2B] << 24);
    if (phys == 0 || pitch == 0) return GR_ERR_IO;

    //раскладка цветовых масок конкретного BIOS (RedMaskSize/FieldPosition
    //и соседи, смещения 0x1F..0x24); берем только если все три непустые
    uint32_t bf_r_len = mi[0x1F], bf_r_off = mi[0x20];
    uint32_t bf_g_len = mi[0x21], bf_g_off = mi[0x22];
    uint32_t bf_b_len = mi[0x23], bf_b_off = mi[0x24];

    //освежаем запись таблицы актуальными параметрами режима
    vbe_mode_rec_t *rec = &vbe_recs[vbe_idx_map[index]];
    rec->xres = (uint16_t)xres;
    rec->yres = (uint16_t)yres;
    rec->bpp = (uint8_t)bpp;
    rec->pitch = (uint16_t)pitch;
    rec->phys_base = phys;

    if (bf_r_len && bf_g_len && bf_b_len)
    {
        vbe_bits[index].bf_r_len = (uint8_t)bf_r_len;
        vbe_bits[index].bf_r_off = (uint8_t)bf_r_off;
        vbe_bits[index].bf_g_len = (uint8_t)bf_g_len;
        vbe_bits[index].bf_g_off = (uint8_t)bf_g_off;
        vbe_bits[index].bf_b_len = (uint8_t)bf_b_len;
        vbe_bits[index].bf_b_off = (uint8_t)bf_b_off;
    }

    //новый LFB в identity-пространство ядра (старые отображения не мешают)
    if (vesa_driver.vmm_map_pages_ptr)
        vesa_driver.vmm_map_pages_ptr(phys, pitch * yres);

    vesa_fill_mode(index, &vesa_cur_mode);

    memset(&vesa_fix, 0, sizeof(vesa_fix));
    strcpy(vesa_fix.id, "vesa");
    vesa_fix.smem_start = vesa_cur_mode.lfb;
    vesa_fix.smem_len = pitch * yres;
    vesa_fix.type = GR_TYPE_PACKED_PIXELS;
    vesa_fix.visual = (vesa_cur_mode.bpp == 8) ? GR_VISUAL_PSEUDOCOLOR
                                               : GR_VISUAL_TRUECOLOR;
    vesa_fix.line_length = pitch;
    return GR_OK;
}

//---- регистрация ----

void driver_main(struct boot_info *boot)
{
    descr_api.type = 0;
    descr_api.struct_ptr = 0;
    vesa_ready = 0;
    vbe_hdr = 0;
    vbe_recs = 0;
    vbe_count = 0;

    if (!boot) return;

    //без LFB (банковский режим) наш путь не работает: рисовать нечем
    if (!(boot->graphics_flags & GRAPHICS_FLAG_LFB)) return;

    //таблица режимов, собранная stage2
    if (!boot->vbe_modes || boot->vbe_mode_count == 0) return;
    vbe_hdr = vbe_table_hdr(boot->vbe_modes);
    if (vbe_hdr->magic != VBE_TABLE_MAGIC) return;

    vbe_recs = vbe_table_recs(boot->vbe_modes);
    vbe_count = boot->vbe_mode_count;
    if (vbe_count > VBE_MODES_MAX) vbe_count = VBE_MODES_MAX;
    if (vbe_hdr->count < vbe_count) vbe_count = vbe_hdr->count;
    if (vbe_count == 0) return;

    //карта drawable-режимов: 8bpp выпадает, у слоя нет программирования палитры
    vbe_drawable = 0;
    for (uint32_t i = 0; i < vbe_count; i++)
    {
        if (vbe_recs[i].bpp == 8) continue;
        if (vbe_drawable >= VBE_MODES_MAX) break;
        vbe_idx_map[vbe_drawable++] = i;
    }
    if (vbe_drawable == 0) return;

    //формы bitfields сбрасываем: модуль мог грузиться повторно, старые
    //значения от прошлого режима недействительны
    for (uint32_t i = 0; i < vbe_drawable; i++)
    {
        vbe_bits[i].bf_r_len = 0; vbe_bits[i].bf_r_off = 0;
        vbe_bits[i].bf_g_len = 0; vbe_bits[i].bf_g_off = 0;
        vbe_bits[i].bf_b_len = 0; vbe_bits[i].bf_b_off = 0;
    }

    //текущий режим: сначала индекс из bootinfo (он в индексах сырой таблицы,
    //переводим в drawable), иначе ищем по параметрам
    uint32_t cur = 0xFFFFFFFF;
    if (boot->vbe_current < vbe_count)
    {
        for (uint32_t i = 0; i < vbe_drawable; i++)
        {
            if (vbe_idx_map[i] == boot->vbe_current)
            {
                cur = i;
                break;
            }
        }
    }
    else
    {
        for (uint32_t i = 0; i < vbe_drawable; i++)
        {
            const vbe_mode_rec_t *rec = &vbe_recs[vbe_idx_map[i]];
            uint32_t b = boot->bpp;
            if (b == 15) b = 16;            //так же, как в fb_driver.c
            if (rec->xres == boot->width &&
                rec->yres == boot->height &&
                rec->bpp == b)
            {
                cur = i;
                break;
            }
        }
    }
    //boot-режим не попал в drawable-список (например, он был 8bpp) —
    //берем первый drawable-режим, лишь бы у него был настоящий LFB
    if (cur == 0xFFFFFFFF && vbe_drawable > 0) cur = 0;
    if (cur == 0xFFFFFFFF || vbe_recs[vbe_idx_map[cur]].phys_base == 0) return;

    vesa_fill_mode(cur, &vesa_cur_mode);

    memset(&vesa_fix, 0, sizeof(vesa_fix));
    strcpy(vesa_fix.id, "vesa");
    vesa_fix.smem_start = vesa_cur_mode.lfb;
    vesa_fix.smem_len = vesa_cur_mode.pitch * vesa_cur_mode.yres;
    vesa_fix.type = GR_TYPE_PACKED_PIXELS;
    vesa_fix.visual = (vesa_cur_mode.bpp == 8) ? GR_VISUAL_PSEUDOCOLOR
                                               : GR_VISUAL_TRUECOLOR;
    vesa_fix.line_length = vesa_cur_mode.pitch;

    memset(&vesa_ops, 0, sizeof(vesa_ops));
    vesa_ops.version = GR_LAYER_ABI_VERSION;
    vesa_ops.get_mode_count = vs_get_mode_count;
    vesa_ops.get_mode = vs_get_mode;
    vesa_ops.get_var = vs_get_var;
    vesa_ops.get_fix = vs_get_fix;
    vesa_ops.set_mode = vs_set_mode;
    //примитивы рисования сознательно NULL: их делает слой поверх LFB

    memset(&vesa_driver, 0, sizeof(vesa_driver));
    vesa_driver.version = GR_LAYER_ABI_VERSION;
    strcpy(vesa_driver.name, "vesa");
    strcpy(vesa_driver.longname, "VESA VBE linear framebuffer (simplefb model)");
    vesa_driver.priority = 10;          //выше fbdrv-fallback (у него 0)
    vesa_driver.modes = 0;              //режимы отдаём динамически через ops
    vesa_driver.mode_count = vbe_drawable;
    vesa_driver.ops = &vesa_ops;
    vesa_driver.priv = 0;
    vesa_driver.current_mode = cur;

    descr_api.type = DESCR_GRAPHICS_DRIVER;
    descr_api.struct_ptr = &vesa_driver;
    vesa_ready = 1;
}

description_driver_api_t *driver_get_api(void)
{
    return &descr_api;
}
