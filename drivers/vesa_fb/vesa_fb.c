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
//смена режима делается цепочкой 4F02 -> 4F03 -> 4F01: после успешного 4F02
//железо уже в новом режиме, поэтому дальше ошибок не бывает — геометрия
//берется либо из железа (реальный режим по 4F03 и ModeInfoBlock по 4F01),
//либо из записи stage2-таблицы, проверенной этим же BIOS на буте.
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


//локальный COM1-вывод: загрузчик модулей (driver_elf.c) не резолвит
//внешние символы ядра, звать com_puts/com_puthex из драйвера нельзя —
//неразрешенная релокация ушла бы в мусорный адрес. обвязка прямыми
//записями в порт 0x3F8 с ожиданием готовности THR (LSR.5), как в блобе
static void vcom_putc(char c)
{
    for (;;)
    {
        uint8_t lsr;
        __asm__ __volatile__("inb %1, %0" : "=a"(lsr) : "Nd"((uint16_t)0x3FD));
        if (lsr & 0x20) break;
    }
    __asm__ __volatile__("outb %0, %1" :: "a"(c), "Nd"((uint16_t)0x3F8));
}

static void vcom_puts(const char *s)
{
    while (*s) vcom_putc(*s++);
}

static void vcom_puthex(uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--)
        vcom_putc(hx[(v >> (i * 4)) & 0xF]);
}

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

//ключевой принцип: после успешного 4F02 железо уже в новом режиме, и
//возвращать ошибку нельзя — слой остался бы на старой геометрии и рисовал
//бы мусор в чужой LFB (ровно те артефакты и расчетверивание, что наблюдали
//на S3 Trio). поэтому после 4F02 любой сбор геометрии завершается либо
//фактическими данными железа, либо записью stage2-таблицы, которую тот же
//биос проверил при перечислении на буте
static int vs_set_mode(void *priv, uint32_t index)
{
    (void)priv;
    if (!vesa_ready) return GR_ERR_NODEV;
    if (index >= vbe_drawable) return GR_ERR_BADMODE;
    if (!vesa_driver.vbe_rm_exec_ptr) return GR_ERR_UNSUPPORTED;

    vbe_mode_rec_t *rec = &vbe_recs[vbe_idx_map[index]];
    uint16_t clean = (uint16_t)(rec->mode_number & 0x3FFF);

    vcom_puts("[VESA] set idx=");
    vcom_puthex(index);
    vcom_puts(" mode=");
    vcom_puthex(clean);
    vcom_puts("\n");

    //4F02: установка режима с флагом LFB. номер режима обязан быть чистым:
    //биты 14-15 некоторые BIOS считают за неизвестный режим (тот же грабель,
    //что в stage2, см. vbe.inc). бит 15 ("не чистить экран") тоже сбрасываем:
    //случайно выставленный, он оставляет в LFB мусор предыдущего режима
    vbe_rm_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4F02;
    regs.bx = (uint16_t)(clean | 0x4000);
    if (vesa_driver.vbe_rm_exec_ptr(&regs) != 0)
    {
        //отказ BIOS на точную запись — не спешим возвращать ошибку: часть
        //BIOS (seavgabios qemu) перечисляет в 4F00/4F01 режимы, которые сам
        //же отказывается ставить (24bpp на stdvga и т.п.). пробуем прочие
        //drawable-записи той же геометрии WxH; успех переписывает номер
        //режима в исходную запись, дальше 4F03/4F01 читают железо по нему,
        //и get_mode(index) описывает то, что реально на экране. до смены
        //железа экран и кэш слоя остаются согласованными в любом случае
        vcom_puts("[VESA] 4F02 refused, trying alternates\n");
        uint32_t self_raw = vbe_idx_map[index];
        int alt_ok = 0;
        for (uint32_t a = 0; a < vbe_drawable && !alt_ok; a++)
        {
            if (vbe_idx_map[a] == self_raw) continue;
            vbe_mode_rec_t *ar = &vbe_recs[vbe_idx_map[a]];
            if (ar->xres != rec->xres || ar->yres != rec->yres) continue;
            uint16_t aclean = (uint16_t)(ar->mode_number & 0x3FFF);
            if (aclean == clean) continue;
            memset(&regs, 0, sizeof(regs));
            regs.ax = 0x4F02;
            regs.bx = (uint16_t)(aclean | 0x4000);
            if (vesa_driver.vbe_rm_exec_ptr(&regs) != 0) continue;
            vcom_puts("[VESA] alternate mode ");
            vcom_puthex(aclean);
            vcom_puts(" accepted\n");
            rec->mode_number = ar->mode_number;
            clean = aclean;
            alt_ok = 1;
        }
        if (!alt_ok)
        {
            //все альтернативы тоже отвергнуты: железо не тронуто, слой
            //отрисует старый режим как ни в чем не бывало
            vcom_puts("[VESA] 4F02 refused, no alternate accepted\n");
            return GR_ERR_IO;
        }
    }

    //4F03: какой режим BIOS выставил реально. ряд BIOS (S3/Trident) молча
    //подменяет запрошенный режим ближайшим поддержанным — без этой проверки
    //успешный 4F01 мог бы описывать совсем не тот режим, что на экране
    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4F03;
    uint32_t actual = clean;
    if (vesa_driver.vbe_rm_exec_ptr(&regs) == 0)
    {
        actual = (uint32_t)(regs.bx & 0x3FFF);
        if (actual != clean)
        {
            vcom_puts("[VESA] 4F03 substituted ");
            vcom_puthex(clean);
            vcom_puts(" -> ");
            vcom_puthex(actual);
            vcom_puts("\n");
        }
    }
    else
    {
        vcom_puts("[VESA] 4F03 failed, assume requested\n");
    }

    //4F01: ModeInfoBlock активного режима. две попытки: сначала чистый CX
    //(как читает stage2), затем CX с битом 14 — по спеке VBE он запрашивает
    //параметры именно LFB-версии режима (PhysBasePtr отдают не все BIOS
    //на чистый номер)
    uint32_t attr = 0, xres = 0, yres = 0, bpp = 0, pitch = 0, phys = 0;
    uint32_t bf_r_len = 0, bf_r_off = 0;
    uint32_t bf_g_len = 0, bf_g_off = 0;
    uint32_t bf_b_len = 0, bf_b_off = 0;
    int mi_ok = 0;
    for (int attempt = 0; attempt < 2 && !mi_ok; attempt++)
    {
        memset(&regs, 0, sizeof(regs));
        regs.ax = 0x4F01;
        regs.cx = (attempt == 0) ? (uint16_t)actual
                                 : (uint16_t)(actual | 0x4000);
        regs.es = VBE_RM_INFO_ADDR >> 4;
        regs.di = VBE_RM_INFO_ADDR & 0xF;
        if (vesa_driver.vbe_rm_exec_ptr(&regs) != 0) continue;

        const uint8_t *mi = vbe_rm_info_buf();
        attr     = (uint32_t)mi[0x00] | ((uint32_t)mi[0x01] << 8);
        pitch    = (uint32_t)mi[0x10] | ((uint32_t)mi[0x11] << 8);
        xres     = (uint32_t)mi[0x12] | ((uint32_t)mi[0x13] << 8);
        yres     = (uint32_t)mi[0x14] | ((uint32_t)mi[0x15] << 8);
        bpp      = mi[0x19];
        phys     = (uint32_t)mi[0x28] | ((uint32_t)mi[0x29] << 8)
                 | ((uint32_t)mi[0x2A] << 16) | ((uint32_t)mi[0x2B] << 24);
        bf_r_len = mi[0x1F]; bf_r_off = mi[0x20];
        bf_g_len = mi[0x21]; bf_g_off = mi[0x22];
        bf_b_len = mi[0x23]; bf_b_off = mi[0x24];

        vcom_puts("[VESA] 4F01 try");
        vcom_putc((char)('0' + attempt));
        vcom_puts(" attr=");
        vcom_puthex(attr);
        vcom_puts(" mi=");
        vcom_puthex(xres);
        vcom_putc('x');
        vcom_puthex(yres);
        vcom_putc('x');
        vcom_puthex(bpp);
        vcom_puts(" pitch=");
        vcom_puthex(pitch);
        vcom_puts(" phys=");
        vcom_puthex(phys);
        vcom_puts("\n");

        //sanity: нули и неадекватные значения — не доверять, пробуем
        //следующую попытку, затем откат на запись таблицы
        if (pitch == 0 || xres == 0 || yres == 0 || phys == 0) continue;
        if (bpp != 16 && bpp != 24 && bpp != 32) continue;
        //pitch меньше xres*bpp/8 у линейного режима физически невозможен:
        //такой ответ не описывает активный скан-аут, не верить
        if (pitch < xres * ((bpp + 7) / 8)) continue;

        //bitfields валидны, только если поля укладываются в bpp: часть BIOS
        //отдает мусор в MaskSize/FieldPosition для нестандартных режимов
        if (bf_r_len && bf_g_len && bf_b_len)
        {
            if (bf_r_off + bf_r_len > bpp ||
                bf_g_off + bf_g_len > bpp ||
                bf_b_off + bf_b_len > bpp)
            {
                vcom_puts("[VESA] bitfields insane, drop\n");
                bf_r_len = bf_g_len = bf_b_len = 0;
                bf_r_off = bf_g_off = bf_b_off = 0;
            }
        }
        mi_ok = 1;
    }

    if (mi_ok)
    {
        //расхождение с бутовой записью — важный след: либо BIOS подменил
        //режим (выше уже напечатана подмена по 4F03), либо его 4F01
        //описывает не то, что реально едет по шине (известная болезнь
        //части S3: сообщаемый pitch не равен фактическому скан-ауту)
        if (pitch != rec->pitch || xres != rec->xres || yres != rec->yres)
        {
            vcom_puts("[VESA] geometry drift: tbl ");
            vcom_puthex(rec->xres);
            vcom_putc('x');
            vcom_puthex(rec->yres);
            vcom_putc('x');
            vcom_puthex(rec->bpp);
            vcom_puts(" pitch ");
            vcom_puthex(rec->pitch);
            vcom_puts(" -> hw ");
            vcom_puthex(xres);
            vcom_putc('x');
            vcom_puthex(yres);
            vcom_putc('x');
            vcom_puthex(bpp);
            vcom_puts(" pitch ");
            vcom_puthex(pitch);
            vcom_puts("\n");
        }
        //ответ 4F01 принимается только если он описывает реальный режим
        //из таблицы: на CX с битом 14 и после подмен по 4F03 часть BIOS
        //отдает поля чужого или полу-мусорного режима (S3 Trio64 на
        //CX|0x4000 отчитывался вдвое-вчетверо меньшей геометрией). раньше
        //такой ответ перезаписывал запись таблицы, слой рисовал по
        //уехавшей геометрии, и изображение занимало четверть/половину
        //экрана. сверяем xres/yres/bpp со всеми записями: их значения тот
        //же BIOS вернул при перечислении на буте, это нижняя граница доверия
        int known = 0;
        for (uint32_t k = 0; k < vbe_drawable; k++)
        {
            const vbe_mode_rec_t *kr = &vbe_recs[vbe_idx_map[k]];
            if (kr->xres == xres && kr->yres == yres && kr->bpp == bpp)
            {
                known = 1;
                break;
            }
        }
        if (!known)
        {
            vcom_puts("[VESA] 4F01 describes unknown mode, keep stage2 record\n");
            mi_ok = 0;
        }
    }

    if (mi_ok)
    {
        //освежаем запись таблицы фактическими параметрами железа: pitch
        //под виртуальную ширину, подмена режима и смена PhysBasePtr
        //теперь доезжают до слоя и консоли автоматически
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
        else
        {
            //раскладки нет — канонические дефолты слоя (565/888)
            memset(&vbe_bits[index], 0, sizeof(vbe_bits[index]));
        }
    }
    else
    {
        //4F01 сорвался или описал неизвестный режим — остаемся на записи
        //stage2-таблицы: ее параметры тот же биос проверил при перечислении
        //на буте, а раз мы дошли сюда, запрошенный (или альтернативный)
        //номер режима успешно выставился и сейчас. рассинхрона нет,
        //в отличие от возврата ошибки
        vcom_puts("[VESA] 4F01 failed, fallback to stage2 record\n");
        memset(&vbe_bits[index], 0, sizeof(vbe_bits[index]));
    }

    vcom_puts("[VESA] active ");
    vcom_puthex(rec->xres);
    vcom_putc('x');
    vcom_puthex(rec->yres);
    vcom_putc('x');
    vcom_puthex(rec->bpp);
    vcom_puts(" pitch=");
    vcom_puthex(rec->pitch);
    vcom_puts(" phys=");
    vcom_puthex(rec->phys_base);
    vcom_puts("\n");

    //новый LFB в identity-пространство ядра (старые отображения не мешают)
    if (vesa_driver.vmm_map_pages_ptr)
        vesa_driver.vmm_map_pages_ptr(rec->phys_base,
                                      (uint32_t)rec->pitch * rec->yres);

    vesa_fill_mode(index, &vesa_cur_mode);

    memset(&vesa_fix, 0, sizeof(vesa_fix));
    strcpy(vesa_fix.id, "vesa");
    vesa_fix.smem_start = vesa_cur_mode.lfb;
    vesa_fix.smem_len = (uint32_t)vesa_cur_mode.pitch * vesa_cur_mode.yres;
    vesa_fix.type = GR_TYPE_PACKED_PIXELS;
    vesa_fix.visual = (vesa_cur_mode.bpp == 8) ? GR_VISUAL_PSEUDOCOLOR
                                               : GR_VISUAL_TRUECOLOR;
    vesa_fix.line_length = vesa_cur_mode.pitch;

    //дескриптор маяков под новую геометрию: следующий вызов блоба рисует
    //стадии уже в актуальный фреймбуфер
    if (vesa_driver.vbe_rm_screen_trace_ptr)
        vesa_driver.vbe_rm_screen_trace_ptr(vesa_cur_mode.lfb,
                                            vesa_cur_mode.pitch,
                                            (uint8_t)vesa_cur_mode.bpp);
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

    //карта drawable-режимов: 8bpp выпадает, у слоя нет программирования
    //палитры; yres < 400 выпадает тоже — это режимы двойной развертки
    //(320x240 и родня): на части BIOS (Voodoo Banshee) смена в них виснет
    //внутри int 10h, а программный слой поверх LFB двойную развертку
    //все равно не отрисовывает
    vbe_drawable = 0;
    for (uint32_t i = 0; i < vbe_count; i++)
    {
        if (vbe_recs[i].bpp == 8) continue;
        if (vbe_recs[i].yres < 400)
        {
            vcom_puts("[VESA] skip doublescan-suspect mode ");
            vcom_puthex(vbe_recs[i].mode_number & 0x3FFF);
            vcom_puts("\n");
            continue;
        }
        if (vbe_drawable >= VBE_MODES_MAX) break;
        vbe_idx_map[vbe_drawable++] = i;
    }
    if (vbe_drawable == 0) return;

    //дамп drawable-таблицы в COM: сверка имен режимов в grinfo с реальными
    //номерами BIOS делается по этому логу — расхождение видно сразу
    //(в seavgabios, например, 0x115 — это 800x600x24, а не то, что ждёт
    //выбравший его под другое разрешение)
    vcom_puts("[VESA] modes table:\n");
    for (uint32_t i = 0; i < vbe_drawable; i++)
    {
        const vbe_mode_rec_t *t = &vbe_recs[vbe_idx_map[i]];
        vcom_puts("tbl[");
        vcom_puthex(i);
        vcom_puts("] mode ");
        vcom_puthex(t->mode_number & 0x3FFF);
        vcom_puts(" ");
        vcom_puthex(t->xres);
        vcom_putc('x');
        vcom_puthex(t->yres);
        vcom_putc('x');
        vcom_puthex(t->bpp);
        vcom_puts(" pitch ");
        vcom_puthex(t->pitch);
        vcom_puts(" phys ");
        vcom_puthex(t->phys_base);
        vcom_puts("\n");
    }

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
