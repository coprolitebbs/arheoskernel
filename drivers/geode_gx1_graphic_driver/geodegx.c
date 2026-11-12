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

//geodegx.c - драйвер на базе vesa_fb.c, предназначен для работы с графическими ускорителями Geode GX1
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


//---- Регистры графического конвейера Geode GX1 (смещения от GX_BASE) ----
//Geode GX1 Data Book (MediaGX databook), таблица 4-23/4-25:
//GP-регистры лежат в управляющем пространстве GX_BASE+0x8100..0x82FF.
//Сам GX_BASE задается регистром GCR (порт 0x22/0x23, индекс 0xB8):
//биты 1:0 = 01/10/11 -> база 0x40000000/0x80000000/0xC0000000 (00 = выключен),
//биты 3:2 = размер scratchpad (в нем живут BLT-буферы): 00/01/10/11 = 0/2/3/4 КБ.
//Через PCI BAR регистры GP на GX1 НЕ доступны (BAR видеофункции CS5530A
//отдает апертуру framebuffer), поэтому любые схемы "BAR1 = GP" ошибочны.
#define GP_DST_XY          0x8100  //BLT: 31:16=Y, 15:0=X; вектор: старт Y/X
#define GP_WID_HGT         0x8104  //BLT: 31:16=WIDTH, 15:0=HEIGHT; вектор: 29:16=LENGTH, 15:0=INIT_ERR
#define GP_SRC_XY          0x8108  //BLT: 31:16=SRC_X (!), 15:0=SRC_Y; вектор: 31:16=AXIAL_ERR, 15:0=DIAG_ERR
#define GP_SRC_COLOR       0x810C  //цвета mono-расширения источника (цвет заливки НЕ здесь!)
#define GP_PAT_COLOR_A     0x8110  //31:16=PAT_COLOR_0 (цвет solid-примитивов), 15:0=PAT_COLOR_1
#define GP_RASTER_MODE     0x8200  //7:0=ROP (Windows-стиль), 9:8=PM (00=solid), 10/11/12=PT/ST/TB
#define GP_VECTOR_MODE     0x8204  //запись запускает вектор: 0=YMAJ, 1=DMAJ, 2=DMIN, 3=DEST
#define GP_BLT_MODE        0x8208  //запись запускает BLT: 1:0=RS, 4:2=RD, 7:6=SM, 8=реверс Y
#define GP_BLT_STATUS      0x820C  //0=BB, 1=PB, 2=BP (RO); 8=16bpp; 9=строка FB 2048 байт (иначе 1024)

//Биты статуса/конфигурации GP_BLT_STATUS
#define GP_ST_BB           (1u << 0)  //BLT Busy — 0 перед прямым доступом к FB
#define GP_ST_PB           (1u << 1)  //Pipeline Busy
#define GP_ST_BP           (1u << 2)  //BLT Pending — 0 перед загрузкой регистров
#define GP_ST_16BPP        (1u << 8)  //формат пикселя GP = 16bpp (по умолчанию 8bpp)
#define GP_ST_W2048        (1u << 9)  //строка FB = 2048 байт (по умолчанию 1024)

//ROP-коды пайплайна GX1 (входы P/S/D; у solid-примитивов цвет всегда из
//GP_PAT_COLOR_0, у копирования источник — реальный пиксель из FB):
#define GP_ROP_PATCOPY     0xF0   //результат = P — заливка и сплошные линии
#define GP_ROP_SRCCOPY     0xCC   //результат = S — копирование FB->FB

//Биты GP_BLT_MODE для SRCCOPY FB->FB
#define GP_BM_SRC_FB       0x01   //RS=01: источник из FB (через BLT-буфер 0)
#define GP_BM_REV_Y        0x100  //реверс Y — перекрывающиеся копии вниз

// маяк quiet-RM в ячейках блоба vbe_rm_blob.asm (за GDTR-ячейкой 0x4370):
// != 0 -> блок выполняет int 10h с IF=0 и без teletype-маяков на экране.
// Ставится этим драйвером: на GX1-платформе IRQ в RM-окне и teletype
// в графическом режиме (через Virtual VGA/VSA-SMI) — главные подозреваемые
// в зависании смены режима, vesa_fb этот флаг не трогает (для него 0).
#define VBE_RM_QUIET_ADDR  0x4378

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

static gr_mode_info_t geode_cur_mode;
static gr_fix_info_t  geode_fix;
static int            geode_ready = 0;

static description_driver_api_t descr_api;
static gr_ops_t       geode_ops;
static gr_driver_t    geode_driver;


// Базовый адрес управляющего пространства GXm (из GCR, не из PCI BAR)
static volatile uint32_t *gx_base;
// размер scratchpad (BLT-буферов GP) в КБ из GCR[3:2]; 0 = BLT FB->FB недоступен
static uint32_t geode_sp_kb = 0;





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


//Вспомогательная функция для ожидания завершения операции.
//Ждать обязаны все три RO-бита (BB/PB/BP): BP=1 при загрузке регистров
//уничтожает отложенный BLT, BB=1 запрещает прямой доступ к FB.
//Таймаут — от мертвого/недокрученного GP: без него зависший движок
//повесил бы слой намертво
static int geode_wait_blt_idle(void)
{
    uint32_t spins = 0;
    if (!gx_base) return -1;
    while (gx_base[GP_BLT_STATUS / 4] & (GP_ST_BB | GP_ST_PB | GP_ST_BP))
    {
        if (++spins > 20000000u)  //порядка секунды на GX1-233; дальше движок мертв
        {
            vcom_puts("[GEODE] gp engine stuck (BB/PB/BP), timeout\n");
            return -1;
        }
    }
    return 0;
}

//чтение GCR (порт 0x22/0x23, индекс 0xB8): биты 1:0 — база GX_BASE,
//биты 3:2 — размер scratchpad (BLT-буферы GP). Только чтение: писать
//в конфиг-регистры CPU из драйвера нельзя (там же ключи блокировки)
static uint8_t geode_gcr_get(void)
{
    uint8_t v;
    __asm__ __volatile__("outb %0, %1" :: "a"((uint8_t)0xB8), "Nd"((uint16_t)0x22));
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"((uint16_t)0x23));
    return v;
}

//упаковка 0x00RRGGBB в формат текущего режима — зеркало gr_pack_color
//графического слоя (примитивам драйвера слой отдает цвет до упаковки)
static uint32_t geode_scale8(uint32_t v, uint8_t len)
{
    if (len == 0) return 0;
    if (len >= 8) return v & 0xFF;
    return (((v & 0xFF) * ((1u << len) - 1)) + 127) / 255;
}

static uint32_t geode_pack_color(uint32_t rgb)
{
    if (geode_cur_mode.bpp >= 24) return rgb;
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8)  & 0xFF;
    uint32_t b =  rgb        & 0xFF;
    return (geode_scale8(r, geode_cur_mode.red.length)   << geode_cur_mode.red.offset)
         | (geode_scale8(g, geode_cur_mode.green.length) << geode_cur_mode.green.offset)
         | (geode_scale8(b, geode_cur_mode.blue.length)  << geode_cur_mode.blue.offset);
}

//GP жив только в 16bpp и только при строке FB 1024/2048 байт: ширина строки
//в GP задается одним битом (GP_BLT_STATUS[9]), произвольного stride нет.
//На GX1 контроллер дисплея держит строку 2048 байт для ВСЕХ 16bpp-режимов
//и 1024 для 8bpp (XY-организация framebuffer, даташит 4.5.8), так что
//16bpp-режимы платы проходят этот фильтр целиком
static int geode_gp_mode_ok(void)
{
    if (!gx_base) return 0;
    if (geode_cur_mode.bpp != 16) return 0;
    if (geode_cur_mode.pitch != 1024 && geode_cur_mode.pitch != 2048) return 0;
    return 1;
}

//конфигурация GP под текущий режим: формат пикселя и ширина строки FB.
//зовется после инициализации и после каждой успешной смены режима —
//VBIOS при 4F02 мог переставить биты 8/9 под свой режим
static void geode_gp_reconfig(void)
{
    if (!gx_base) return;
    uint32_t cfg = 0;
    if (geode_gp_mode_ok())
    {
        cfg |= GP_ST_16BPP;
        if (geode_cur_mode.pitch == 2048) cfg |= GP_ST_W2048;
    }
    gx_base[GP_BLT_STATUS / 4] = cfg;
    vcom_puts("[GEODE] gp reconfig bpp=");
    vcom_puthex(geode_cur_mode.bpp);
    vcom_puts(" pitch=");
    vcom_puthex(geode_cur_mode.pitch);
    vcom_puts(" cfg=");
    vcom_puthex(cfg);
    vcom_puts("\n");
}

//Инициализация аппаратного 2D-движка: подтверждение GX1-платформы по PCI
//и маппинг GP-регистров через GX_BASE из GCR.
//
//ВАЖНО про PCI: сканируем ТОЛЬКО шину 0. Полный проход 256 шин (как было
//раньше) на примитивном хост-бридже GXm уходит в type-1 конфигурационные
//циклы, которые бриджем не транслируются никуда внятного — поведение
//CS5530A на них не гарантируется, а подвешенная шина PCI перед первым
//же BIOS-вызовом — худший из сценариев. Видео на GX1-плате
//(функция CS5530A 0x1078:0x0104) сидит на шине 0, функций 0..7.
//BAR не читаем вовсе: GP-регистры на GX1 всегда за GX_BASE из GCR.
static int geode_init_accel(gr_driver_t *drv)
{
    if (!drv->pci_cfg_read_dword_ptr || !drv->vmm_map_pages_ptr)
        return GR_ERR_UNSUPPORTED;

    //Шина 0, устройства 0..31, функции 0..7: ищем Cyrix/NatSemi/AMD.
    //0x1078 (Cyrix) — host-bridge GXm и функции CS5530A, включая видео
    //0x0104; 0x100B/0x1022 — NatSemi/AMD-бренд Geode. Устройства с
    //vendor 0xFFFF отсутствуют, и функции за отсутствующей тоже
    int found = 0;
    for (uint32_t dev = 0; dev < 32 && !found; dev++)
    {
        for (uint32_t func = 0; func < 8; func++)
        {
            uint32_t id = drv->pci_cfg_read_dword_ptr(0, dev, func, 0x00);
            uint16_t vendor = id & 0xFFFF;
            if (vendor == 0xFFFF) break;      //функций за этой нет
            if (vendor == 0x1078) { found = 1; break; }
            if ((vendor == 0x100B || vendor == 0x1022))
            {
                uint16_t device = id >> 16;
                if (device == 0x0030 || device == 0x0031 || device == 0x0104)
                {
                    found = 1;
                    break;
                }
            }
        }
    }
    if (!found)
    {
        vcom_puts("[GEODE] no GX1-class device on bus 0\n");
        return GR_ERR_NOTFOUND;
    }

    //GX_BASE из GCR: биты 1:0, 00 означает "пространство выключено" —
    //тогда GP нет вовсе (BIOS мог не поднимать графику)
    uint8_t gcr = geode_gcr_get();
    uint32_t gx_phys;
    switch (gcr & 0x3)
    {
    case 1:  gx_phys = 0x40000000u; break;
    case 2:  gx_phys = 0x80000000u; break;
    case 3:  gx_phys = 0xC0000000u; break;
    default:
        vcom_puts("[GEODE] gcr=");
        vcom_puthex(gcr);
        vcom_puts(" - GX_BASE disabled, no GP\n");
        return GR_ERR_NOTFOUND;
    }

    //Scratchpad (GCR[3:2]) — память BLT-буферов GP. Без нее BLT FB->FB
    //некуда буферизовать: копирование уйдет в software, заливка/линии
    //останутся аппаратными (они буферов не требуют)
    static const uint32_t sp_tab[4] = { 0, 2, 3, 4 };
    geode_sp_kb = sp_tab[(gcr >> 2) & 0x3];
    uint32_t sp_kb = geode_sp_kb;

    //Маппим страницу управляющего пространства с GP-регистрами
    //(GX_BASE+0x8000..0x8FFF покрывает 0x8100..0x82FF)
    drv->vmm_map_pages_ptr(gx_phys + 0x8000, 0x1000);

    gx_base = (volatile uint32_t *)(uintptr_t)gx_phys;

    vcom_puts("[GEODE] gx_base=");
    vcom_puthex(gx_phys);
    vcom_puts(" gcr=");
    vcom_puthex(gcr);
    vcom_puts(" scratchpad=");
    vcom_puthex(sp_kb);
    vcom_puts("KB\n");
    return GR_OK;
}



//адрес пикселя в LFB (identity-маппинг слоя: VA == физический LFB)
static inline uint8_t *geode_lfb_pixel(int x, int y)
{
    return (uint8_t *)(uintptr_t)geode_cur_mode.lfb
         + (uint32_t)y * geode_cur_mode.pitch
         + (uint32_t)x * (geode_cur_mode.bpp / 8);
}

//программная запись пикселя — fallback примитивов, когда GP недоступен
//(gr_draw_line/gr_fill_rect слоя не подменяют сбойный op на свой LFB-код,
//поэтому драйвер обязан сам довести примитив до экрана в любом режиме)
static void geode_sw_pixel(int x, int y, uint32_t packed)
{
    uint8_t *p = geode_lfb_pixel(x, y);
    switch (geode_cur_mode.bpp)
    {
    case 32: *(volatile uint32_t *)p = packed; break;
    case 24:
        p[0] =  packed        & 0xFF;
        p[1] = (packed >> 8)  & 0xFF;
        p[2] = (packed >> 16) & 0xFF;
        break;
    case 16: *(volatile uint16_t *)p = (uint16_t)packed; break;
    default: *p = (uint8_t)packed; break;
    }
}

//программная заливка (та же геометрия, что у gr_lfb_fill_rect слоя)
static void geode_sw_fill_rect(int x, int y, int w, int h, uint32_t packed)
{
    uint32_t bpp = geode_cur_mode.bpp / 8;
    for (int row = 0; row < h; row++)
    {
        uint8_t *line = (uint8_t *)(uintptr_t)geode_cur_mode.lfb
                      + (uint32_t)(y + row) * geode_cur_mode.pitch
                      + (uint32_t)x * bpp;
        if (bpp == 4)
        {
            uint32_t *l = (uint32_t *)line;
            for (int c = 0; c < w; c++) l[c] = packed;
        }
        else if (bpp == 2)
        {
            uint16_t *l = (uint16_t *)line;
            for (int c = 0; c < w; c++) l[c] = (uint16_t)packed;
        }
        else if (bpp == 3)
        {
            for (int c = 0; c < w; c++)
            {
                uint8_t *p = line + (uint32_t)c * 3;
                p[0] =  packed        & 0xFF;
                p[1] = (packed >> 8)  & 0xFF;
                p[2] = (packed >> 16) & 0xFF;
            }
        }
        else
        {
            for (int c = 0; c < w; c++) line[c] = (uint8_t)packed;
        }
    }
}

//Заливка: GP BLT с источником "none". У solid-примитивов GX1 цвет берется
//из GP_PAT_COLOR_0 (старшее слово GP_PAT_COLOR_A, даташит 4.4.3), ROP=0xF0
//выбирает P. Запуск — запись GP_BLT_MODE (RS=00: источника нет)
static int geode_fill_rect(void *priv, int x, int y, int w, int h, uint32_t color)
{
    (void)priv;
    if (w <= 0 || h <= 0) return GR_ERR_BADARG;

    if (gx_base && geode_gp_mode_ok() && geode_wait_blt_idle() == 0)
    {
        uint32_t c16 = (uint16_t)geode_pack_color(color);
        gx_base[GP_PAT_COLOR_A / 4] = c16 << 16;                     //PAT_COLOR_0
        gx_base[GP_DST_XY / 4]      = ((uint32_t)(uint16_t)y << 16) | (uint16_t)x;
        gx_base[GP_WID_HGT / 4]     = ((uint32_t)(uint16_t)w << 16) | (uint16_t)h;
        gx_base[GP_RASTER_MODE / 4] = GP_ROP_PATCOPY;                //PM=00 solid
        gx_base[GP_BLT_MODE / 4]    = 0x00000000;                    //старт BLT
        return GR_OK;
    }

    geode_sw_fill_rect(x, y, w, h, geode_pack_color(color));
    return GR_OK;
}

//Копирование FB->FB через GP BLT (RS=01: источник из FB через BLT-буфер).
//Строка источника целиком буферизуется в scratchpad, поэтому ширина одной
//операции ограничена памятью BLT-буфера: режем на вертикальные полосы
//(если полоса одна — это один BLT, поведение слоя не меняется).
//При копировании вниз (dy > sy) включаем реверс Y и стартуем с нижней
//строки — иначе BLT затирает еще не прочитанные строки источника
static int geode_copy_area(void *priv, int sx, int sy, int w, int h, int dx, int dy)
{
    (void)priv;
    if (w <= 0 || h <= 0) return GR_ERR_BADARG;

    if (gx_base && geode_gp_mode_ok() && geode_sp_kb >= 2)
    {
        //строка источника должна влезть в BLT-буфер: консервативно
        //считаем, что под буфер уходит половина scratchpad
        uint32_t bytes_pp = geode_cur_mode.bpp / 8;
        uint32_t chunk = (geode_sp_kb * 1024u / 2u) / bytes_pp;
        if (chunk == 0) chunk = 1;

        for (int off = 0; off < w; off += (int)chunk)
        {
            int cw = w - off;
            if (cw > (int)chunk) cw = (int)chunk;

            if (geode_wait_blt_idle() != 0) break;

            if (dy > sy)
            {
                //реверс Y: старт — ЛЕВЫЙ-НИЖНИЙ угол полосы (X инкрементный,
                //Y декрементный реверс-битом), иначе BLT затирает источник
                gx_base[GP_SRC_XY / 4] = ((uint32_t)(uint16_t)(sx + off) << 16)
                                       |  (uint16_t)(sy + h - 1);
                gx_base[GP_DST_XY / 4] = ((uint32_t)(uint16_t)(dx + off) << 16)
                                       |  (uint16_t)(dy + h - 1);
                gx_base[GP_WID_HGT / 4]     = ((uint32_t)(uint16_t)cw << 16) | (uint16_t)h;
                gx_base[GP_RASTER_MODE / 4] = GP_ROP_SRCCOPY;
                gx_base[GP_BLT_MODE / 4]    = GP_BM_SRC_FB | GP_BM_REV_Y;
            }
            else
            {
                gx_base[GP_SRC_XY / 4] = ((uint32_t)(uint16_t)(sx + off) << 16)
                                       |  (uint16_t)sy;
                gx_base[GP_DST_XY / 4] = ((uint32_t)(uint16_t)(dx + off) << 16)
                                       |  (uint16_t)dy;
                gx_base[GP_WID_HGT / 4]     = ((uint32_t)(uint16_t)cw << 16) | (uint16_t)h;
                gx_base[GP_RASTER_MODE / 4] = GP_ROP_SRCCOPY;
                gx_base[GP_BLT_MODE / 4]    = GP_BM_SRC_FB;
            }
        }
        return GR_OK;
    }

    //software: memmove построчно с направлением против затирания
    uint32_t bpp = geode_cur_mode.bpp / 8;
    uint32_t row_bytes = (uint32_t)w * bpp;
    if (dy <= sy)
    {
        for (int i = 0; i < h; i++)
        {
            uint8_t *d = (uint8_t *)(uintptr_t)geode_cur_mode.lfb
                       + (uint32_t)(dy + i) * geode_cur_mode.pitch + (uint32_t)dx * bpp;
            uint8_t *s = (uint8_t *)(uintptr_t)geode_cur_mode.lfb
                       + (uint32_t)(sy + i) * geode_cur_mode.pitch + (uint32_t)sx * bpp;
            __builtin_memmove(d, s, row_bytes);
        }
    }
    else
    {
        for (int i = h - 1; i >= 0; i--)
        {
            uint8_t *d = (uint8_t *)(uintptr_t)geode_cur_mode.lfb
                       + (uint32_t)(dy + i) * geode_cur_mode.pitch + (uint32_t)dx * bpp;
            uint8_t *s = (uint8_t *)(uintptr_t)geode_cur_mode.lfb
                       + (uint32_t)(sy + i) * geode_cur_mode.pitch + (uint32_t)sx * bpp;
            __builtin_memmove(d, s, row_bytes);
        }
    }
    return GR_OK;
}

//Линии. H/V — через заливку; диагонали — векторным движком GP (Bresenham
//в железе): старт в GP_DST_XY, длина/начальная ошибка в GP_WID_HGT,
//осевая/диагональная ошибки в GP_SRC_XY, запуск записью GP_VECTOR_MODE.
//Если GP недоступен — программный Брезенхэм прямо в LFB (слой при наличии
//op->draw_line на свой код не откатывается, обязаны уметь сами)
static int geode_draw_line(void *priv, int x0, int y0, int x1, int y1, uint32_t color)
{
    (void)priv;

    //горизонтальная — заливка высотой 1, вертикальная — шириной 1
    if (y0 == y1)
    {
        int x = (x0 < x1) ? x0 : x1;
        int w = (x0 < x1) ? (x1 - x0 + 1) : (x0 - x1 + 1);
        return geode_fill_rect(priv, x, y0, w, 1, color);
    }
    if (x0 == x1)
    {
        int y = (y0 < y1) ? y0 : y1;
        int h = (y0 < y1) ? (y1 - y0 + 1) : (y0 - y1 + 1);
        return geode_fill_rect(priv, x0, y, 1, h, color);
    }

    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    uint32_t packed = geode_pack_color(color);

    if (gx_base && geode_gp_mode_ok() && geode_wait_blt_idle() == 0)
    {
        //векторный движок GP: классический Брезенхэм с дроблением шага
        //на "осевой" (шаг по мажорной оси) и "диагональный" (по обеим).
        //начальная ошибка 2*minor-major, осевая 2*minor,
        //диагональная 2*(minor-major); мажорная ось определяет LENGTH.
        //конец линии включительно: LENGTH = major+1 (нулевой LENGTH не
        //рисует ни пиксела). если на железе пикселов окажется на один
        //больше/меньше — правится здесь, в одном месте
        int major, minor;
        uint32_t vmode = 0;
        if (dx >= dy)            //X-мажорная: YMAJ=0, мажорная ось — X
        {
            major = dx; minor = dy;
            if (x1 >= x0) vmode |= 0x2;    //DMAJ: шаг по X вперед
            if (y1 >= y0) vmode |= 0x4;    //DMIN: шаг по Y вперед
        }
        else                     //Y-мажорная: YMAJ=1, мажорная ось — Y
        {
            major = dy; minor = dx;
            vmode |= 0x1;                  //YMAJ
            if (y1 >= y0) vmode |= 0x2;    //DMAJ: шаг по Y вперед
            if (x1 >= x0) vmode |= 0x4;    //DMIN: шаг по X вперед
        }

        uint32_t c16 = (uint16_t)geode_pack_color(color);
        gx_base[GP_PAT_COLOR_A / 4] = c16 << 16;      //цвет solid-примитива
        gx_base[GP_DST_XY / 4]      = ((uint32_t)(uint16_t)y0 << 16) | (uint16_t)x0;
        //LENGTH в битах 29:16 (биты 31:30 в векторном режиме зарезервированы)
        gx_base[GP_WID_HGT / 4]     = ((uint32_t)(major + 1) << 16)
                                    | (uint32_t)(uint16_t)(2 * minor - major);
        gx_base[GP_SRC_XY / 4]      = ((uint32_t)(uint16_t)(2 * minor) << 16)
                                    | (uint16_t)(uint32_t)(2 * (minor - major));
        gx_base[GP_RASTER_MODE / 4] = GP_ROP_PATCOPY;
        gx_base[GP_VECTOR_MODE / 4] = vmode;          //запуск вектора
        return GR_OK;
    }

    //программный Брезенхэм по LFB
    int sx = (x1 >= x0) ? 1 : -1;
    int sy = (y1 >= y0) ? 1 : -1;
    int err = dx - dy;
    for (;;)
    {
        geode_sw_pixel(x0, y0, packed);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
    return GR_OK;
}


static int geode_wait_engine_idle(void *priv)
{
    (void)priv;
    if (gx_base) geode_wait_blt_idle();
    return GR_OK;
}



static uint8_t *vbe_rm_info_buf(void)
{
    return (uint8_t *)VBE_RM_INFO_ADDR;
}

//имя режима вида "1024x768x24"
static void geode_mode_name(uint32_t x, uint32_t y, uint32_t bpp, char *out)
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
static void geode_fill_mode(uint32_t index, gr_mode_info_t *m)
{
    const vbe_mode_rec_t *r = &vbe_recs[vbe_idx_map[index]];
    memset(m, 0, sizeof(gr_mode_info_t));
    m->index = index;
    geode_mode_name(r->xres, r->yres, r->bpp, m->name);
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
    return geode_ready ? (int)vbe_drawable : 0;
}

static int vs_get_mode(void *priv, uint32_t index, gr_mode_info_t *out)
{
    (void)priv;
    if (!geode_ready || !out) return GR_ERR_BADARG;
    if (index >= vbe_drawable) return GR_ERR_BADMODE;
    geode_fill_mode(index, out);
    return GR_OK;
}

static int vs_get_var(void *priv, gr_mode_info_t *out)
{
    (void)priv;
    if (!geode_ready || !out) return GR_ERR_NODEV;
    *out = geode_cur_mode;
    return GR_OK;
}

static int vs_get_fix(void *priv, gr_fix_info_t *out)
{
    (void)priv;
    if (!geode_ready || !out) return GR_ERR_NODEV;
    *out = geode_fix;
    return GR_OK;
}

//---- ops: смена режима на ходу через real-mode тримплин ----

//ключевой принцип: после успешного 4F02 железо уже в новом режиме, и
//возвращать ошибку нельзя — слой остался бы на старой геометрии и рисовал
//бы мусор в чужой LFB . поэтому после 4F02 любой сбор геометрии завершается либо
//фактическими данными железа, либо записью stage2-таблицы, которую тот же
//биос проверил при перечислении на буте
static int vs_set_mode(void *priv, uint32_t index)
{
    (void)priv;
    if (!geode_ready) return GR_ERR_NODEV;
    if (index >= vbe_drawable) return GR_ERR_BADMODE;
    if (!geode_driver.vbe_rm_exec_ptr) return GR_ERR_UNSUPPORTED;

    vbe_mode_rec_t *rec = &vbe_recs[vbe_idx_map[index]];
    uint16_t clean = (uint16_t)(rec->mode_number & 0x3FFF);

    vcom_puts("[GEODE set idx=");
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
    if (geode_driver.vbe_rm_exec_ptr(&regs) != 0)
    {
        //разовый сбой окна RM (железо в окне, саморемонт тримплина в
        //vbe_rm_exec) не обязан означать отказ BIOS: 4F02 идемпотентен,
        //повтор безопасен — железо либо уже в запрошенном режиме, либо
        //не тронуто
        vcom_puts("[GEODE] 4F02 exec failed, retry once\n");
        memset(&regs, 0, sizeof(regs));
        regs.ax = 0x4F02;
        regs.bx = (uint16_t)(clean | 0x4000);
        if (geode_driver.vbe_rm_exec_ptr(&regs) != 0)
        {
            //отказ BIOS на точную запись — не спешим возвращать ошибку: часть
            //BIOS (seavgabios qemu) перечисляет в 4F00/4F01 режимы, которые сам
            //же отказывается ставить (24bpp на stdvga и т.п.). пробуем прочие
            //drawable-записи той же геометрии WxH; успех переписывает номер
            //режима в исходную запись, дальше 4F03/4F01 читают железо по нему,
            //и get_mode(index) описывает то, что реально на экране. до смены
            //железа экран и кэш слоя остаются согласованными в любом случае
            vcom_puts("[GEODE] 4F02 refused, trying alternates\n");
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
                if (geode_driver.vbe_rm_exec_ptr(&regs) != 0) continue;
                vcom_puts("[GEODE] alternate mode ");
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
                vcom_puts("[GEODE] 4F02 refused, no alternate accepted\n");
                return GR_ERR_IO;
            }
        }
    }

    //4F03: какой режим BIOS выставил реально. ряд BIOS (S3/Trident) молча
    //подменяет запрошенный режим ближайшим поддержанным — без этой проверки
    //успешный 4F01 мог бы описывать совсем не тот режим, что на экране
    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4F03;
    uint32_t actual = clean;
    if (geode_driver.vbe_rm_exec_ptr(&regs) == 0)
    {
        actual = (uint32_t)(regs.bx & 0x3FFF);
        if (actual != clean)
        {
            vcom_puts("[GEODE] 4F03 substituted ");
            vcom_puthex(clean);
            vcom_puts(" -> ");
            vcom_puthex(actual);
            vcom_puts("\n");
        }
    }
    else
    {
        vcom_puts("[GEODE] 4F03 failed, assume requested\n");
    }

    //подмена режима фиксируется в записи: следующая смена этого индекса
    //пойдет сразу на рабочий номер BIOS, без повторной подмены
    if (actual != clean)
        rec->mode_number = (uint16_t)(actual | 0x4000);

    //4F01: ModeInfoBlock активного режима. три попытки: сначала чистый CX
    //(как читает stage2), затем CX с битом 14 — по спеке VBE он запрашивает
    //параметры именно LFB-версии режима (PhysBasePtr отдают не все BIOS
    //на чистый номер); третья — после повторного 4F02 в тот же режим:
    //S3 Trio64 сразу после смены отчитывается в 4F01 геометрией чужого
    //режима (вдвое-вчетверо меньшей или соседней из списка) — его
    //внутреннее состояние описания режима рассинхронизировано сменой;
    //идемпотентный повторный 4F02 пересинхронизирует VBIOS, и следующий
    //4F01 читает уже активный скан-аут
    uint32_t attr = 0, xres = 0, yres = 0, bpp = 0, pitch = 0, phys = 0;
    uint32_t bf_r_len = 0, bf_r_off = 0;
    uint32_t bf_g_len = 0, bf_g_off = 0;
    uint32_t bf_b_len = 0, bf_b_off = 0;
    int mi_ok = 0;
    for (int attempt = 0; attempt < 3 && !mi_ok; attempt++)
    {
        if (attempt == 2)
        {
            //повторная установка того же режима идемпотентна: железо либо
            //уже в нем, либо вернется в него же; ошибки окна RM (саморемонт
            //тримплина в vbe_rm_exec) не означают отказ BIOS — прерываем
            //цикл и остаемся на записи stage2
            memset(&regs, 0, sizeof(regs));
            regs.ax = 0x4F02;
            regs.bx = (uint16_t)(clean | 0x4000);
            if (geode_driver.vbe_rm_exec_ptr(&regs) != 0) break;
            memset(&regs, 0, sizeof(regs));
            regs.ax = 0x4F03;
            if (geode_driver.vbe_rm_exec_ptr(&regs) == 0)
            {
                uint32_t recheck = (uint32_t)(regs.bx & 0x3FFF);
                if (recheck != actual)
                {
                    //после пересета BIOS назвал другой активный режим:
                    //обновляем точку опоры, иначе и третий 4F01 ляжет мимо
                    vcom_puts("[GEODE] 4F03 changed after re-set ");
                    vcom_puthex(actual);
                    vcom_puts(" -> ");
                    vcom_puthex(recheck);
                    vcom_puts("\n");
                    actual = recheck;
                    rec->mode_number = (uint16_t)(actual | 0x4000);
                }
            }
        }
        memset(&regs, 0, sizeof(regs));
        regs.ax = 0x4F01;
        regs.cx = (uint16_t)((attempt & 1) ? (actual | 0x4000) : actual);
        regs.es = VBE_RM_INFO_ADDR >> 4;
        regs.di = VBE_RM_INFO_ADDR & 0xF;
        if (geode_driver.vbe_rm_exec_ptr(&regs) != 0) continue;

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

        vcom_puts("[GEODE] 4F01 try");
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
                vcom_puts("[GEODE] bitfields insane, drop\n");
                bf_r_len = bf_g_len = bf_b_len = 0;
                bf_r_off = bf_g_off = bf_b_off = 0;
            }
        }
        mi_ok = 1;
    }

    //опорная запись для сверки ответа 4F01: режим, который BIOS назвал
    //активным (4F03). заполняется после 4F01, здесь только заготовка
    vbe_mode_rec_t *ref = rec;

    if (mi_ok)
    {
        //режим, который BIOS назвал активным (4F03), ищется в таблице
        //по номеру; если таких записей несколько (после перезаписи номеров
        //альтернативами), приоритет у записи с тем же bpp, что в ответе
        //4F01. нет такой записи — опора на запрошенную запись
        for (uint32_t pass = 0; pass < 2 && ref == rec; pass++)
        {
            for (uint32_t k = 0; k < vbe_drawable; k++)
            {
                vbe_mode_rec_t *kr = &vbe_recs[vbe_idx_map[k]];
                if ((uint16_t)(kr->mode_number & 0x3FFF) != (uint16_t)actual)
                    continue;
                if (pass == 0 && (uint32_t)kr->bpp != bpp) continue;
                ref = kr;
                break;
            }
        }
        //расхождение с опорной записью — важный след: либо BIOS подменил
        //режим (выше уже напечатана подмена по 4F03), либо его 4F01
        //описывает не то, что реально едет по шине (известная болезнь
        //части S3: сообщаемый pitch не равен фактическому скан-ауту)
        if (pitch != ref->pitch || xres != ref->xres || yres != ref->yres)
        {
            vcom_puts("[GEODE] geometry drift: tbl ");
            vcom_puthex(ref->xres);
            vcom_putc('x');
            vcom_puthex(ref->yres);
            vcom_putc('x');
            vcom_puthex(ref->bpp);
            vcom_puts(" pitch ");
            vcom_puthex(ref->pitch);
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
        //ответ 4F01 принимается только при точном совпадении xres/yres/bpp
        //с опорной записью — той геометрией, что этот же BIOS отдал при
        //перечислении на буте для этого же номера режима. проверка
        //"известный среди всех записей" пропускала ответ геометрией чужого,
        //но известного режима (S3 Trio64 после смены отчитывался то
        //вдвое-вчетверо меньшей геометрией, то геометрией соседнего режима
        //из списка): запись портилась, слой рисовал по уехавшей геометрии —
        //удвоение/учетверение экрана при рабочей консоли
        if (xres != ref->xres || yres != ref->yres || bpp != ref->bpp)
        {
            vcom_puts("[GEODE] 4F01 mismatches reference mode, keep stage2 record\n");
            mi_ok = 0;
        }
    }

    if (mi_ok)
    {
        //разметка pitch: у линейного режима pitch лежит в диапазоне
        //[xres*байт_на_пиксель, 2*xres*байт] — BIOS округляет строку вверх,
        //но не более чем вдвое. значения вне диапазона и ровно двукратный
        //дрейф от опорной записи (тот же режим, тот же BIOS на буте дал
        //другое) — ложь про активный скан-аут; такой pitch сводится к
        //опорному, иначе слой рисует по уехавшей длине строки — удвоение
        uint32_t bytes = xres * ((bpp + 7) / 8);
        uint32_t pitch_ref = (ref->pitch >= bytes) ? ref->pitch : bytes;
        if (pitch < bytes || pitch > bytes * 2 ||
            pitch == pitch_ref * 2 || pitch_ref == pitch * 2)
        {
            if (pitch != pitch_ref)
            {
                vcom_puts("[GEODE] pitch implausible or 2x drifted, clamp ");
                vcom_puthex(pitch);
                vcom_puts(" -> ");
                vcom_puthex(pitch_ref);
                vcom_puts("\n");
            }
            pitch = pitch_ref;
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
        //4F01 сорвался или разошелся с опорной записью — остаемся на записи
        //stage2-таблицы: ее параметры тот же биос проверил при перечислении
        //на буте, а раз мы дошли сюда, запрошенный (или альтернативный)
        //номер режима успешно выставился и сейчас. рассинхрона нет,
        //в отличие от возврата ошибки
        vcom_puts("[GEODE] 4F01 failed, fallback to stage2 record\n");
        memset(&vbe_bits[index], 0, sizeof(vbe_bits[index]));
    }

    vcom_puts("[GEODE] active ");
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
    if (geode_driver.vmm_map_pages_ptr)
        geode_driver.vmm_map_pages_ptr(rec->phys_base,
                                      (uint32_t)rec->pitch * rec->yres);

    geode_fill_mode(index, &geode_cur_mode);

    //GP обязан перечитать формат пикселя/ширину строки FB под новый режим:
    //VBIOS при 4F02 переставлял биты 8/9 GP_BLT_STATUS под свою геометрию
    geode_gp_reconfig();

    memset(&geode_fix, 0, sizeof(geode_fix));
    strcpy(geode_fix.id, "geode");
    geode_fix.smem_start = geode_cur_mode.lfb;
    geode_fix.smem_len = (uint32_t)geode_cur_mode.pitch * geode_cur_mode.yres;
    geode_fix.type = GR_TYPE_PACKED_PIXELS;
    geode_fix.visual = (geode_cur_mode.bpp == 8) ? GR_VISUAL_PSEUDOCOLOR
                                               : GR_VISUAL_TRUECOLOR;
    geode_fix.line_length = geode_cur_mode.pitch;

    //дескриптор маяков под новую геометрию: следующий вызов блоба рисует
    //стадии уже в актуальный фреймбуфер
    if (geode_driver.vbe_rm_screen_trace_ptr)
        geode_driver.vbe_rm_screen_trace_ptr(geode_cur_mode.lfb,
                                            geode_cur_mode.pitch,
                                            (uint8_t)geode_cur_mode.bpp);
    return GR_OK;
}

//---- регистрация ----

void driver_main(struct boot_info *boot)
{
    descr_api.type = 0;
    descr_api.struct_ptr = 0;
    geode_ready = 0;
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
    //все равно не отрисовывает. записи с нулевой/некорректной геометрией
    //(S3 Trio64 при перечислении отдает и такие) выпадают тоже: рисовать
    //по ним нельзя, а 4F01 после смены на них не спасает — BIOS сам
    //подменяет режим и геометрия все равно уезжает
    vbe_drawable = 0;
    for (uint32_t i = 0; i < vbe_count; i++)
    {
        if (vbe_recs[i].bpp == 8) continue;
        if (vbe_recs[i].bpp != 16 && vbe_recs[i].bpp != 24 &&
            vbe_recs[i].bpp != 32) continue;
        if (vbe_recs[i].xres == 0 || vbe_recs[i].yres == 0 ||
            vbe_recs[i].phys_base == 0) continue;
        if (vbe_recs[i].yres < 400)
        {
            vcom_puts("[GEODE] skip doublescan-suspect mode ");
            vcom_puthex(vbe_recs[i].mode_number & 0x3FFF);
            vcom_puts("\n");
            continue;
        }
        //санитизация длины строки: pitch меньше xres*байт_на_пиксель у
        //линейного режима физически невозможен — это ложь BIOS при
        //перечислении (известная болезнь S3: сообщаемый pitch не равен
        //фактическому скан-ауту). рисунок по такой записи разваливается
        //на мусорные пикселы и повторы строк; канонический минимум —
        //единственная безопасная оценка
        uint32_t min_pitch = (uint32_t)vbe_recs[i].xres *
                             ((vbe_recs[i].bpp + 7) / 8);
        if (vbe_recs[i].pitch < min_pitch)
        {
            vcom_puts("[GEODE] stage2 pitch implausible, clamp ");
            vcom_puthex(vbe_recs[i].pitch);
            vcom_puts(" -> ");
            vcom_puthex(min_pitch);
            vcom_puts("\n");
            vbe_recs[i].pitch = (uint16_t)min_pitch;
        }
        if (vbe_drawable >= VBE_MODES_MAX) break;
        vbe_idx_map[vbe_drawable++] = i;
    }
    if (vbe_drawable == 0) return;

    //дамп drawable-таблицы в COM: сверка имен режимов в grinfo с реальными
    //номерами BIOS делается по этому логу — расхождение видно сразу
    //(в seavgabios, например, 0x115 — это 800x600x24, а не то, что ждёт
    //выбравший его под другое разрешение)
    vcom_puts("[GEODE] modes table:\n");
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

    geode_fill_mode(cur, &geode_cur_mode);

    memset(&geode_fix, 0, sizeof(geode_fix));
    strcpy(geode_fix.id, "geode");
    geode_fix.smem_start = geode_cur_mode.lfb;
    geode_fix.smem_len = geode_cur_mode.pitch * geode_cur_mode.yres;
    geode_fix.type = GR_TYPE_PACKED_PIXELS;
    geode_fix.visual = (geode_cur_mode.bpp == 8) ? GR_VISUAL_PSEUDOCOLOR
                                               : GR_VISUAL_TRUECOLOR;
    geode_fix.line_length = geode_cur_mode.pitch;

    memset(&geode_ops, 0, sizeof(geode_ops));
    geode_ops.version = GR_LAYER_ABI_VERSION;
    geode_ops.get_mode_count = vs_get_mode_count;
    geode_ops.get_mode = vs_get_mode;
    geode_ops.get_var = vs_get_var;
    geode_ops.get_fix = vs_get_fix;
    geode_ops.set_mode = vs_set_mode;
    geode_ops.wait_engine_idle = geode_wait_engine_idle;
    geode_ops.draw_line = geode_draw_line;
    //примитивы рисования сознательно NULL: их делает слой поверх LFB

    memset(&geode_driver, 0, sizeof(geode_driver));
    geode_driver.version = GR_LAYER_ABI_VERSION;
    strcpy(geode_driver.name, "geode_gx");
    strcpy(geode_driver.longname, "GEODE GX1 graphic driver");
    geode_driver.priority = 10;          //выше fbdrv-fallback (у него 0)
    geode_driver.modes = 0;              //режимы отдаём динамически через ops
    geode_driver.mode_count = vbe_drawable;
    geode_driver.ops = &geode_ops;
    geode_driver.priv = 0;
    geode_driver.current_mode = cur;

    descr_api.type = DESCR_GRAPHICS_DRIVER;
    descr_api.struct_ptr = &geode_driver;


    //Примитивы регистрируются ВСЕГДА: реализация сама выбирает между GP
    //и LFB-программным путём на каждом вызове. Если ops->fill_rect оставить
    //NULL при рабочем set_mode — слой рисует своим lfb-кодом, а при
    //НЕ-NULL, но сбойном op (как было раньше) слой НЕ откатывается на
    //свой код и примитив просто пропадает — gr_fill_rect возвращает ошибку
    geode_ops.fill_rect  = geode_fill_rect;
    geode_ops.copy_area  = geode_copy_area;
    geode_ops.draw_line  = geode_draw_line;

    //Пытаемся инициализировать аппаратный 2D-движок (только лог GP):
    //если GP не найден — примитивы работают программно поверх LFB
    if (geode_init_accel(&geode_driver) == GR_OK)
    {
        geode_gp_reconfig();
        vcom_puts("[GEODE] GP engine mapped\n");
    }
    else
    {
        vcom_puts("[GEODE] GP unavailable, primitives run on LFB\n");
    }

    //quiet-RM для блоба vbe_rm_blob.asm: все BIOS-вызовы этого драйвера
    //(4F02/4F03/4F01) идут с IF=0 и без teletype-маяков. На GX1-плате IRQ
    //в RM-окне (тик 100Гц/клава/RTC сквозь легаси-PIC) и teletype в
    //графическом режиме (через Virtual VGA -> VSA -> SMI) — основные
    //подозреваемые в зависании смены разрешения; vesa_fb флаг не трогает
    //и на CF-18 работает по-старому (IF=1). Если на конкретной плате VBIOS
    //без тиков вешается в 4F02 — вернуть 0 здесь же, одной строкой
    *(volatile uint8_t *)VBE_RM_QUIET_ADDR = 1;


    geode_ready = 1;
}

description_driver_api_t *driver_get_api(void)
{
    return &descr_api;
}
