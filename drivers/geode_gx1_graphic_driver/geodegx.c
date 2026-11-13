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

//geodegx.c - нативный драйвер графики Geode GX1 (MediaGX GX1 + CS5530),
//развилка vesa_fb.c. Два пути смены режима:
//
//1) Нативный (основной на GX1): драйвер сам программирует Display
//   Controller GX1 (GX_BASE+0x8300), синтезатор DCLK CS5530
//   (GX_BASE+0x10000) и конфигурирует Graphics Pipeline. Никаких
//   обращений к BIOS int 10h: runtime-вызовы VBE на MediaGX-машинах
//   проходят через VSA/SoftVGA (SMM), и на части машин оставляют
//   дисплей в текстовом режиме - черный экран с мигающим курсором
//   после смены разрешения. Нативный путь включается только если
//   CPU - Cyrix/Geode (CPUID "CyrixInstead"), GCR показывает
//   включенную графику и LFB stage2 совпадает с gx_base+0x800000.
//2) Резервный: прежняя цепочка 4F02 -> 4F03 -> 4F01 через реальнорежимный
//   тримплин vbe_rm_blob.asm (как в vesa_fb) - для эмуляторов и машин
//   без Geode, а также для геометрий/глубин вне нативной таблицы.
//
//Источники аппаратных дефайнов (адреса регистров, раскладка бит, порядок
//записей, тайминги PLL):
//  - AMD Geode GX1 Processor Data Book, rev 5.0 (главы 4, 5, 6);
//  - Durango SDK из xf86-video-nsc (src/gfx/rndr_gu1.c, disp_gu1.c,
//    init_gu1.c, vid_5530.c) - эталонная последовательность
//    программирования от производителя, работавшая на серийных машинах.
//  Код драйверов Linux/Xorg не копировался; использованы только факты
//  железа.
// ВАЖНО: таблица 5-24 Data Book для регистров 0x8104/0x8108
//  путает старшую и младшую половины (WIDTH/HEIGHT, SRC_X/SRC_Y,
//  LENGTH/INIT_ERROR, AXIAL/DIAG); раскладка взята по Durango:
//  0x8104: [15:0] WIDTH/VECTOR_LENGTH, [31:16] HEIGHT/INIT_ERROR;
//  0x8108: [15:0] SRC_X/AXIAL_ERROR,  [31:16] SRC_Y/DIAG_ERROR
//  (Durango пишет dword 0x00010004 = "4x1 BLT" в 0x8104).
//
//Особенности GX1, учтенные по Durango:
//  - SP-регистры GP пишутся 16-битными словами (gp_write16);
//  - сплошная заливка: цвет из GP_PAT_COLOR_0 (0x8110), ROP 0xF0, PM=00;
//    известный баг GXm: заливка шире 16 пикселей обязана начинаться на
//    16-пиксельной границе - разбивается на два BLT (gu1_solid_fill);
//  - у GP нет регистра stride: шаг строки движка задается классом
//    ширины в GP_BLT_STATUS (1024/2048 байта, Data Book табл. 5-2);
//    режимы с другим pitch аппаратно не ускоряются (software-слой);
//  - copy_area (чтение source из FB) гоняет данные через BLT-буферы в
//    scratchpad RAM, теги/базы которых программирует BIOS (Data Book
//    5.1.4). Функция написана по gu1_screen_to_screen_blt, но к слою
//    сознательно НЕ подключена до проверки scratchpad на целевой
//    машине. Заливка и вектор BLT-буферов не трогают.
//
//Таблица режимов берется из stage2 (vbe.inc -> bootinfo), отдается слою
//через gr_ops_t. Загружается как обычный ELF-модуль из /adrv
//(load_additional_drivers, DESCR_GRAPHICS_DRIVER), как kbdps2.drv.

#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/lib.h"
#include "../include_drivers/drv_format.h"
#include "../include_drivers/driver_graphic_layer.h"


//конфиг-регистры CPU Cyrix (Data Book 4.3.2.1: порты 0x22/0x23)
#define GX1_CFG_INDEX       0x22
#define GX1_CFG_DATA        0x23
#define GX1_CCR3            0xC3    //доступ к индексам вне C0-CF/FE/FF требует MAPEN
#define GX1_CCR3_MAPEN      0x10
#define GX1_GCR             0xB8    //Graphics Control Register
#define GX1_GCR_GX_MASK     0x03    //биты 1:0 - база GX_BASE (00 = отключено)
#define GX1_DIR0            0xFE    //Device ID 0 (шаг ядра)
#define GX1_DIR1            0xFF    //Device ID 1 (ревизия)

//карта памяти GX_BASE (Data Book, рис. 5-2 и 5.1.2)
//  +0x800000 графика (LFB), +0x8100 Graphics Pipeline (200h),
//  +0x8300 Display Controller (100h), +0x8400 Memory Controller,
//  +0x10000 регистры видео CS5530 (так их адресует Durango:
//  gu1_get_vid_register_base = (GCR&3)<<30 | 0x10000)
#define GX1_FB_BASE_OFF     0x800000
#define GX1_MMIO_SIZE       0x11000 //GP+DC+MC (+0x8500) и блок VID (+0x10000)

//Graphics Pipeline (Data Book табл. 5-23/5-24, Durango gfx_regs.h)
//gx_base[i] - dword по адресу GX_BASE+4*i; gp_write16 - слово по адресу
//GX_BASE+addr. Геометрические SP-регистры - пары 16-битных полей
#define GX1_GP_DST_XY       0x8100  //[15:0] X, [31:16] Y назначения/старта
#define GX1_GP_DST_Y        0x8102
#define GX1_GP_WIDTH        0x8104  //[15:0] WIDTH / VECTOR_LENGTH
#define GX1_GP_VECTOR_LENGTH 0x8104
#define GX1_GP_HEIGHT       0x8106  //[31:16] HEIGHT / INIT_ERROR
#define GX1_GP_INIT_ERROR   0x8106
#define GX1_GP_SRC_XY       0x8108  //[15:0] SRC_X / AXIAL_ERROR
#define GX1_GP_AXIAL_ERROR  0x8108
#define GX1_GP_SRC_Y        0x810A  //[31:16] SRC_Y / DIAG_ERROR
#define GX1_GP_DIAG_ERROR   0x810A
#define GX1_GP_SRC_COLOR_0  0x810C  //цвет 0 моно-источника (заливке не нужен)
#define GX1_GP_PAT_COLOR_0  0x8110  //цвет сплошной заливки/линии (PM=00)
#define GX1_GP_RASTER_MODE  0x8200  //[9:8] PM (00 = solid из PAT_COLOR_0), [7:0] ROP
#define GX1_GP_VECTOR_MODE  0x8204  //запись запускает вектор
#define GX1_GP_BLT_MODE     0x8208  //запись запускает BLT
#define GX1_GP_BLT_STATUS   0x820C  //статус (младший байт) + конфигурация

//статусные биты GP_BLT_STATUS (только чтение, Data Book табл. 5-24)
#define GP_ST_BUSY          0x0001  //BLT/вектор исполняется (ждать перед LFB)
#define GP_ST_PIPELINE_BUSY 0x0002  //конвейер занят
#define GP_ST_PENDING       0x0004  //BLT ожидает в master-регистрах
//(ждать сброса перед загрузкой регистров)
#define GP_ST_BUSY_MASK     (GP_ST_BUSY | GP_ST_PIPELINE_BUSY | GP_ST_PENDING)

//конфигурационные биты GP_BLT_STATUS (Data Book: W=биты 10:9, M=бит 8)
#define BC_16BPP            0x0100  //пиксельный формат 16bpp (0 = 8bpp)
#define BC_FB_WIDTH_1024    0x0000  //класс шага строки 1024 байта
#define BC_FB_WIDTH_2048    0x0200  //класс шага строки 2048 байта

//биты GP_BLT_MODE (Data Book табл. 5-24; запись запускает BLT)
#define BM_READ_SRC_NONE    0x0000  //источника нет (сплошная заливка)
#define BM_READ_SRC_FB      0x0001  //источник из framebuffer (через BLT-буфер 0)
#define BM_REVERSE_Y        0x0100  //обратный ход по Y (копирование вниз)

//биты GP_VECTOR_MODE (Data Book табл. 5-24; запись запускает вектор)
#define VM_YMAJ             0x0001  //главная ось - Y
#define VM_MAJOR_INC        0x0002  //главная ось возрастает
#define VM_MINOR_INC        0x0004  //минорная ось возрастает

//ROP-коды (Data Book табл. 5-22, кодировка MS/Windows)
#define GX1_ROP_PATCOPY     0xF0    //Output = Pattern (сплошная заливка)
#define GX1_ROP_SRCCOPY     0xCC    //Output = Source

//Display Controller (Data Book табл. 5-27/5-28; Durango gfx_regs.h)
#define GX1_DC_UNLOCK       0x00
#define GX1_DC_UNLOCK_CODE  0x00004758
#define GX1_DC_GENERAL_CFG  0x04
#define GX1_DC_TIMING_CFG   0x08
#define GX1_DC_OUTPUT_CFG   0x0C
#define GX1_DC_FB_ST_OFFSET 0x10
#define GX1_DC_CB_ST_OFFSET 0x14
#define GX1_DC_CURS_ST_OFFSET 0x18
#define GX1_DC_VID_ST_OFFSET 0x20
#define GX1_DC_LINE_DELTA   0x24    //шаг строки в dword'ах
#define GX1_DC_BUF_SIZE     0x28    //длина строки выборки (+биты видео-буфера)
#define GX1_DC_H_TIMING_1   0x30
#define GX1_DC_H_TIMING_2   0x34
#define GX1_DC_H_TIMING_3   0x38
#define GX1_DC_FP_H_TIMING  0x3C
#define GX1_DC_V_TIMING_1   0x40
#define GX1_DC_V_TIMING_2   0x44
#define GX1_DC_V_TIMING_3   0x48
#define GX1_DC_FP_V_TIMING  0x4C

//биты DC_GENERAL_CFG (Data Book табл. 5-29; Durango gfx_regs.h)
#define DC_GCFG_DFLE        0x00000001  //display fifo load enable
#define DC_GCFG_VCLK_DIV    0x00000004  //делитель видеоклока (ядра >266 МГц)
#define DC_GCFG_CMPE        0x00000010  //compression enable
#define DC_GCFG_DECE        0x00000020  //decompression enable
#define DC_GCFG_DCLK_MASK   0x000000C0  //делитель/множитель DCLK
#define DC_GCFG_DCLK_MUL2   0x00000080  //код 2: внутренний DCLK = внешний
#define DC_GCFG_DFCK        0x20000000  //divide flat-panel clock (сбросить)
#define DC_GCFG_DPCK        0x40000000  //divide pixel clock (сбросить)
#define DC_GCFG_DDCK        0x80000000  //divide dot clock (сбросить)
#define DC_GCFG_DFHPSL_POS  8           //приоритет fifo: начало
#define DC_GCFG_DFHPEL_POS  12          //приоритет fifo: конец
//биты DC_TIMING_CFG (Data Book табл. 5-30)
#define DC_TCFG_FPPE        0x00000001
#define DC_TCFG_HSYE        0x00000002
#define DC_TCFG_VSYE        0x00000004
#define DC_TCFG_BLKE        0x00000008
#define DC_TCFG_TGEN        0x00000020  //timing generator enable
//биты DC_OUTPUT_CFG (Data Book табл. 5-31)
#define DC_OCFG_8BPP        0x00000001
#define DC_OCFG_555         0x00000002
#define DC_OCFG_PCKE        0x00000004  //pixel clock both edges (18-бит шина)
#define DC_OCFG_PDEL        0x00001000
#define DC_OCFG_PDEH        0x00002000

//регистры видео CS5530 (Durango gfx_regs.h: база GX_BASE+0x10000)
#define CS5530_DISPLAY_CONFIG 0x0004  //DCfg: выходы CRT/панели, полярность sync
#define CS5530_DOT_CLK_CONFIG 0x0024  //DCLKPLL: делители PLL
#define CS5530_DCFG_CRT_HSYNC_POL 0x00000100  //1 = hsync positive
#define CS5530_DCFG_CRT_VSYNC_POL 0x00000200  //1 = vsync positive

//Регистры графического конвейера Geode GX1 (смещения от GX_BASE)
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
//0x1F..0x24). нулевые длины означают "не прочитано - возьми канонический
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




//Состояние нативной GX1-части
static uint32_t           gx_phys = 0;      //GX_BASE (из GCR)
static volatile uint32_t *gx_mm = 0;        //GX_BASE identity-mapped (uncached)
static uint32_t           gx_core_mhz = 0;  //частота ядра по DIR0/DIR1
static uint32_t           cs_dcfg_saved = 0;//DCfg CS5530 на момент старта
static int                native_ok = 0;    //нативная смена режима доступна
// Базовый адрес управляющего пространства GXm (из GCR, не из PCI BAR)
static volatile uint32_t *gx_base;
// размер scratchpad (BLT-буферов GP) в КБ из GCR[3:2]; 0 = BLT FB->FB недоступен
static uint32_t geode_sp_kb = 0;



//---- низкоуровневые обвязки ----

//локальный COM1-вывод: загрузчик модулей (driver_elf.c) не резолвит
//внешние символы ядра, звать com_puts/com_puthex из драйвера нельзя —
//неразрешенная релокация ушла бы в мусорный адрес. обвязка прямыми
//записями в порт 0x3F8 с ожиданием готовности THR (LSR.5), как в блобе
/*static void vcom_putc(char c)
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
*/
//порты ввода-вывода: локальные обвязки (внешние символы ядра драйверу
//недоступны - см. комментарий к vcom_putc)
static void toutb(uint16_t port, uint8_t v)
{
    __asm__ __volatile__("outb %0, %1" :: "a"(v), "Nd"(port));
}

static uint8_t tinb(uint16_t port)
{
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

//грубая задержка: записи в порт 0x80 (POST) на машинах этого класса
//~1 мкс каждая; задержки ниже - нижние границы, лишние микросекунды безвредны
static void gx1_udelay(uint32_t us)
{
    while (us--)
    {
        toutb(0x80, 0);
        toutb(0x80, 0);
        toutb(0x80, 0);
    }
}

//чтение конфиг-регистра CPU Cyrix (0x22/0x23) c MAPEN, как в Durango
//(gfx_gxm_config_read): индексы вне C0-CF/FE/FF доступны только с MAPEN
static uint8_t gx1_read_conf(uint8_t idx)
{
    uint8_t ccr3, v;
    toutb(GX1_CFG_INDEX, GX1_CCR3);
    ccr3 = tinb(GX1_CFG_DATA);
    toutb(GX1_CFG_INDEX, GX1_CCR3);
    toutb(GX1_CFG_DATA, ccr3 | GX1_CCR3_MAPEN);
    toutb(GX1_CFG_INDEX, idx);
    v = tinb(GX1_CFG_DATA);
    toutb(GX1_CFG_INDEX, GX1_CCR3);
    toutb(GX1_CFG_DATA, ccr3);
    return v;
}

//Display Controller: dword-доступ к GX_BASE+0x8300+reg
static inline void dc_write(uint32_t reg, uint32_t v)
{
    gx_mm[(0x8300 + reg) / 4] = v;
}

static inline uint32_t dc_read(uint32_t reg)
{
    return gx_mm[(0x8300 + reg) / 4];
}

//видеорегистры CS5530: dword-доступ к GX_BASE+0x10000+reg
static inline void vid_write(uint32_t reg, uint32_t v)
{
    gx_mm[(0x10000 + reg) / 4] = v;
}

static inline uint32_t vid_read(uint32_t reg)
{
    return gx_mm[(0x10000 + reg) / 4];
}

//16-битная запись в GP-регистр: SP-регистры GX1 - пары 16-битных ячеек,
//Durango программирует их словами (WRITE_REG16)
static inline void gp_write16(uint32_t addr, uint16_t v)
{
    ((volatile uint16_t *)gx_base)[addr / 2] = v;
}

static inline uint16_t gp_read16(uint32_t addr)
{
    return ((volatile uint16_t *)gx_base)[addr / 2];
}

//CPUID vendor: у GX1 строка "CyrixInstead" (Cyrix-ядро). На других CPU
//порты 0x22/0x23 уходят во внешние циклы - к конфиг-регистрам лезть нельзя
static int gx1_cpu_is_cyrix(void)
{
    uint32_t a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    return b == 0x69727943u /* "Cyri" */
           && c == 0x64616574u /* "tead" */
           && d == 0x736E4978u /* "xIns" */;
}

//частота ядра по DIR0/DIR1 (Data Book 4.3.2; Durango gu1_get_core_freq).
//нужна только для DC_GCFG_VCLK_DIV: на шагах >266 МГц внутренний
//видеоклок обязан делиться (Data Book/Durango: CPU >266 MHz)
static uint32_t gx1_core_freq(void)
{
    uint8_t dir0 = gx1_read_conf(GX1_DIR0) & 0x0F;
    uint8_t dir1 = gx1_read_conf(GX1_DIR1);

    if (dir1 >= 0x50)
    {
        switch (dir0)
        {
        case 0:
        case 2:
            return 133;
        case 5:
            return 166;
        case 3:
            return 200;
        case 6:
            return 233;
        case 7:
            return 266;
        case 4:
            return 300;
        case 1:
            return 333;
        }
    }
    else
    {
        switch (dir0)
        {
        case 0:
        case 2:
            return 133;
        case 7:
            return 166;
        case 1:
        case 3:
            return 200;
        case 4:
        case 6:
            return 233;
        case 5:
            return 266;
        }
    }
    return 0;
}


//Вспомогательная функция для ожидания завершения операции.
//Ждать обязаны все три RO-бита (BB/PB/BP): BP=1 при загрузке регистров
//уничтожает отложенный BLT, BB=1 запрещает прямой доступ к FB.
//Таймаут — от мертвого/недокрученного GP: без него зависший движок повесил бы слой намертво
static int geode_wait_blt_idle(void)
{
    uint32_t spins = 0;
    if (!gx_base) return -1;
    while (gx_base[GP_BLT_STATUS / 4] & (GP_ST_BB | GP_ST_PB | GP_ST_BP))
    {
        if (++spins > 20000000u)  //порядка секунды на GX1-233; дальше движок мертв
        {
            //vcom_puts("[GEODE] gp engine stuck (BB/PB/BP), timeout\n");
            return -1;
        }
    }
    return 0;
}


//ожидание полного простоя GP: BB (BLT busy), PB (pipeline busy),
//BP (BLT pending) - все три обязаны быть сброшены перед загрузкой
//регистров (Data Book табл. 5-24, GP_BLT_STATUS) и перед прямым доступом
//к framebuffer. таймаут грубый; раз в 1024 цикла отдаем квант
static void geode_wait_gp_idle(void)
{
    uint32_t timeout = 1000000;
    while (--timeout)
    {
        if (!(gp_read16(GX1_GP_BLT_STATUS) & GP_ST_BUSY_MASK)) return;
        if ((timeout & 0x3FF) == 0 && geode_driver.yield_ptr)
            geode_driver.yield_ptr();
    }
    //vcom_puts("[GEODE] gp idle timeout\n");
}

//Сконфигурировать GP под текущий режим: пиксельный формат (M) и класс
//шага строки (W). У GX1 нет регистра stride: движок шагает строками
//ровно 1024 или 2048 байта (Data Book табл. 5-2 "Display Resolution Screen Width").
//Если реальный pitch не равен классу, аппаратные операции лягут мимо строк LFB -
//ускорение запрещено (0, слой откатится на программные примитивы). 24/32bpp GP GX1 не умеет совсем
static int geode_gp_config(uint16_t *cfg_out)
{
    uint32_t bpp   = geode_cur_mode.bpp;
    uint32_t pitch = geode_cur_mode.pitch;
    uint16_t cfg;

    if (bpp == 16)      cfg = BC_16BPP;
    else if (bpp == 8)  cfg = 0;
    else return 0;

    if (pitch == 1024)      cfg |= BC_FB_WIDTH_1024;
    else if (pitch == 2048) cfg |= BC_FB_WIDTH_2048;
    else return 0;

    *cfg_out = cfg;
    return 1;
}

//упаковка 0x00RRGGBB в пиксельный формат текущего режима (как gr_pack_color слоя, по bitfields из 4F01/дефолтов)
static uint16_t geode_pack_color(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;
    uint32_t rm = (1u << geode_cur_mode.red.length) - 1;
    uint32_t gm = (1u << geode_cur_mode.green.length) - 1;
    uint32_t bm = (1u << geode_cur_mode.blue.length) - 1;
    return (uint16_t)((((r * rm + 127) / 255) << geode_cur_mode.red.offset)
                      | (((g * gm + 127) / 255) << geode_cur_mode.green.offset)
                      | (((b * bm + 127) / 255) << geode_cur_mode.blue.offset));
}

//Аппаратная сплошная заливка: RS=00 (источника нет), RD=000 (назначение не читается),
//PM=00 (solid, цвет из GP_PAT_COLOR_0), ROP 0xF0.
//Последовательность и баг-воркаунд ширины - точно по Durango
//gu1_solid_fill: BLT шире 16 пикселей обязан начинаться на 16-пиксельной
//границе, поэтому первый кусок рисуется до границы, остаток - вторым BLT
static int geode_fill_rect(void *priv, int x, int y, int w, int h, uint32_t color)
{
    (void)priv;
    if (!gx_base || !native_ok) return GR_ERR_NODEV;

    //Проверка границ (слой уже обрезает, но для прямых вызовов - защита)
    if (x < 0 || y < 0 || w <= 0 || h <= 0)
        return GR_ERR_BADARG;
    if ((uint32_t)x + (uint32_t)w > geode_cur_mode.xres ||
            (uint32_t)y + (uint32_t)h > geode_cur_mode.yres)
        return GR_ERR_BADARG;

    uint16_t cfg;
    if (!geode_gp_config(&cfg)) return GR_ERR_UNSUPPORTED;

    uint16_t col = geode_pack_color(color);

    geode_wait_gp_idle();
    gx_base[GX1_GP_BLT_STATUS / 4] = cfg;   //конфиг bpp/класса ширины
    gp_write16(GX1_GP_DST_XY, (uint16_t)x);
    gp_write16(GX1_GP_DST_Y, (uint16_t)y);
    gp_write16(GX1_GP_HEIGHT, (uint16_t)h);
    gp_write16(GX1_GP_RASTER_MODE, GX1_ROP_PATCOPY);   //ROP 0xF0, PM=00
    gp_write16(GX1_GP_PAT_COLOR_0, col);               //цвет заливки

    if (w <= 16)
    {
        gp_write16(GX1_GP_WIDTH, (uint16_t)w);
        gp_write16(GX1_GP_BLT_MODE, BM_READ_SRC_NONE); //запуск
    }
    else
    {
        //GXm solid-fill bug: первый кусок до 16-пиксельной границы
        uint32_t first = 16 - ((uint32_t)x & 15);
        gp_write16(GX1_GP_WIDTH, (uint16_t)first);
        gp_write16(GX1_GP_BLT_MODE, BM_READ_SRC_NONE);

        geode_wait_gp_idle();
        gp_write16(GX1_GP_DST_XY, (uint16_t)(x + first));
        gp_write16(GX1_GP_DST_Y, (uint16_t)y);
        gp_write16(GX1_GP_WIDTH, (uint16_t)(w - first));
        gp_write16(GX1_GP_BLT_MODE, BM_READ_SRC_NONE);
    }

    return GR_OK;
}

//Аппаратное копирование прямоугольника внутри видеопамяти.
//Последовательность - по Durango gu1_screen_to_screen_blt: перекрытие
//вниз штатно через BM_REVERSE_Y со стартом с нижней строки; перекрытие
//вправо движок не умеет - секции справа налево; ширина секции ограничена
//BLT-буферами (2K scratchpad: 816 байт на буфер, минус выравнивание -
//800px@16bpp / 1600px@8bpp на оба буфера).
//ВНИМАНИЕ: к слою сознательно не подключена - чтение source требует
//настроенных BLT-буферов в scratchpad (их базы/теги программирует BIOS,
//Data Book 5.1.4); включить после проверки на целевой машине
__attribute__((unused))
static int geode_copy_area(void *priv, int sx, int sy, int w, int h, int dx, int dy)
{
    (void)priv;
    if (!gx_base || !native_ok) return GR_ERR_NODEV;

    //Проверка границ
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0 || w <= 0 || h <= 0)
        return GR_ERR_BADARG;
    if ((uint32_t)sx + (uint32_t)w > geode_cur_mode.xres ||
            (uint32_t)sy + (uint32_t)h > geode_cur_mode.yres ||
            (uint32_t)dx + (uint32_t)w > geode_cur_mode.xres ||
            (uint32_t)dy + (uint32_t)h > geode_cur_mode.yres)
        return GR_ERR_BADARG;

    uint16_t cfg;
    if (!geode_gp_config(&cfg)) return GR_ERR_UNSUPPORTED;

    uint16_t mode = BM_READ_SRC_FB;    //SRCCOPY не читает destination
    if (dy > sy)
    {
        mode |= BM_REVERSE_Y;
        sy += h - 1;   //старт с нижней строки при обратном ходе
        dy += h - 1;
    }
    if (dx > sx)
    {
        sx += w;       //отрицательное X: стартуем от правого края,
        dx += w;       //секции идут справа налево
    }

    uint32_t sec_px = (geode_cur_mode.bpp > 8) ? 800 : 1600;

    geode_wait_gp_idle();
    gx_base[GX1_GP_BLT_STATUS / 4] = cfg;
    gp_write16(GX1_GP_RASTER_MODE, GX1_ROP_SRCCOPY);
    gp_write16(GX1_GP_HEIGHT, (uint16_t)h);

    while (w > 0)
    {
        uint32_t sec = ((uint32_t)w > sec_px) ? sec_px : (uint32_t)w;
        geode_wait_gp_idle();
        gp_write16(GX1_GP_SRC_Y, (uint16_t)sy);
        gp_write16(GX1_GP_DST_Y, (uint16_t)dy);
        gp_write16(GX1_GP_WIDTH, (uint16_t)sec);
        if (dx > sx)
        {
            sx -= (int)sec;
            dx -= (int)sec;
            gp_write16(GX1_GP_SRC_XY, (uint16_t)sx);
            gp_write16(GX1_GP_DST_XY, (uint16_t)dx);
        }
        else
        {
            gp_write16(GX1_GP_SRC_XY, (uint16_t)sx);
            gp_write16(GX1_GP_DST_XY, (uint16_t)dx);
            sx += (int)sec;
            dx += (int)sec;
        }
        gp_write16(GX1_GP_BLT_MODE, mode);   //запуск
        w -= (int)sec;
    }

    return GR_OK;
}

//Аппаратная линия через векторный движок: сплошная (PM=00, цвет из
//GP_PAT_COLOR_0, ROP 0xF0), без чтения destination. Bresenham-члены и
//поправка начальной ошибки - по Durango GX1SubsequentSolidTwoPointLine
static int geode_draw_line(void *priv, int x0, int y0, int x1, int y1, uint32_t color)
{
    (void)priv;
    if (!gx_base || !native_ok) return GR_ERR_NODEV;

    //Проверка границ (линия должна быть в пределах экрана)
    if (x0 < 0 || y0 < 0 || x1 < 0 || y1 < 0 ||
            (uint32_t)x0 >= geode_cur_mode.xres || (uint32_t)y0 >= geode_cur_mode.yres ||
            (uint32_t)x1 >= geode_cur_mode.xres || (uint32_t)y1 >= geode_cur_mode.yres)
        return GR_ERR_BADARG;

    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int y_major = (dy > dx);
    int major = y_major ? dy : dx;
    int minor = y_major ? dx : dy;

    if (major == 0) //точка
        return geode_fill_rect(priv, x0, y0, 1, 1, color);

    uint16_t vm = y_major ? VM_YMAJ : 0;
    if ((y_major ? (y1 >= y0) : (x1 >= x0))) vm |= VM_MAJOR_INC;
    if ((y_major ? (x1 >= x0) : (y1 >= y0))) vm |= VM_MINOR_INC;

    //axial = 2*minor, diag = 2*(minor - major), init = 2*minor - major;
    //при убывании по минорной оси init-- - иначе линия, нарисованная в
    //обе стороны, дает разные пиксели (Durango: initerr-- при !VM_MINOR_INC)
    int axial = 2 * minor;
    int diag = axial - 2 * major;
    int init = axial - major;
    if (!(vm & VM_MINOR_INC)) init--;

    uint16_t cfg;
    if (!geode_gp_config(&cfg)) return GR_ERR_UNSUPPORTED;

    uint16_t col = geode_pack_color(color);

    geode_wait_gp_idle();
    gx_base[GX1_GP_BLT_STATUS / 4] = cfg;
    gp_write16(GX1_GP_PAT_COLOR_0, col);
    gp_write16(GX1_GP_RASTER_MODE, GX1_ROP_PATCOPY);
    gp_write16(GX1_GP_DST_XY, (uint16_t)x0);
    gp_write16(GX1_GP_DST_Y, (uint16_t)y0);
    gp_write16(GX1_GP_VECTOR_LENGTH, (uint16_t)major);
    gp_write16(GX1_GP_INIT_ERROR, (uint16_t)init);
    gp_write16(GX1_GP_AXIAL_ERROR, (uint16_t)axial);
    gp_write16(GX1_GP_DIAG_ERROR, (uint16_t)diag);
    gp_write16(GX1_GP_VECTOR_MODE, vm);   //запись запускает вектор

    return GR_OK;
}

static int geode_wait_engine_idle(void *priv)
{
    (void)priv;
    if (gx_base) geode_wait_gp_idle();
    return GR_OK;
}

//нативная смена режима: DC GX1 + PLL CS5530

//инициализация нативной части GX1: CPUID-гейт, GCR, сверка LFB,
//маппинг GX_BASE (регистры доступа работают через GX_BASE+0x10000)
static int geode_init_native(gr_driver_t *drv)
{
    (void)drv;

    //1. процессор обязан быть Cyrix/Geode
    if (!gx1_cpu_is_cyrix())
    {
        //vcom_puts("[GEODE] native: not a Cyrix/Geode CPU, VBE path\n");
        return GR_ERR_NOTFOUND;
    }

    //2. база GX_BASE из GCR (биты 1:0); 00 = графика отключена
    uint8_t gcr = gx1_read_conf(GX1_GCR);
    if ((gcr & GX1_GCR_GX_MASK) == 0)
    {
        //vcom_puts("[GEODE] native: GCR graphics disabled\n");
        return GR_ERR_NOTFOUND;
    }
    gx_phys = (uint32_t)(gcr & GX1_GCR_GX_MASK) << 30;

    //3. сверка с LFB stage2: графика GX1 всегда по gx_base+0x800000
    //(Data Book 5.1.3). расхождение - вычисленная база не настоящая
    if (vbe_drawable == 0 || !vbe_recs) return GR_ERR_NOTFOUND;
    uint32_t lfb = vbe_recs[vbe_idx_map[0]].phys_base;
    if (lfb != gx_phys + GX1_FB_BASE_OFF)
    {
        /*vcom_puts("[GEODE] native: LFB mismatch stage2 ");
        vcom_puthex(lfb);
        vcom_puts(" vs gx_base+0x800000 ");
        vcom_puthex(gx_phys + GX1_FB_BASE_OFF);
        vcom_puts(", VBE path\n");*/
        return GR_ERR_IO;
    }

    //4. маппинг uncached: GP/DC (+0x8100/+0x8300) и блок VID (+0x10000)
    if (!geode_driver.vmm_map_pages_uncached_ptr)
        return GR_ERR_UNSUPPORTED;
    geode_driver.vmm_map_pages_uncached_ptr(gx_phys, GX1_MMIO_SIZE);
    gx_mm = (volatile uint32_t *)(uintptr_t)gx_phys;
    gx_base = gx_mm;

    //5. пробное чтение статуса GP: 0xFFFFFFFF значит мимо
    uint32_t st = gx_mm[GX1_GP_BLT_STATUS / 4];
    if (st == 0xFFFFFFFFu)
    {
        //vcom_puts("[GEODE] native: GP status reads 0xFFFFFFFF\n");
        return GR_ERR_IO;
    }

    //6. частота ядра по DIR0/DIR1 (для VCLK_DIV на шагах >266 МГц)
    gx_core_mhz = gx1_core_freq();

    //7. снимок DCfg CS5530: включенные выходы (CRT/панель, задержки,
    //skew) остаются как выставил BIOS на буте - трогаем только полярность
    cs_dcfg_saved = vid_read(CS5530_DISPLAY_CONFIG);

    /*vcom_puts("[GEODE] native: gx_base ");
    vcom_puthex(gx_phys);
    vcom_puts(" gp_status=");
    vcom_puthex(st);
    vcom_puts(" core=");
    vcom_puthex(gx_core_mhz);
    vcom_puts(" dcfg=");
    vcom_puthex(cs_dcfg_saved);
    vcom_puts("\n");*/

    //8. дамп состояния DC на буте в COM - по нему отлаживаются тайминги
    //конкретной машины (панель/CRT) без осциллографа
    static const uint32_t dump_regs[] = { GX1_DC_GENERAL_CFG,
                                          GX1_DC_TIMING_CFG, GX1_DC_OUTPUT_CFG, GX1_DC_FB_ST_OFFSET,
                                          GX1_DC_LINE_DELTA, GX1_DC_BUF_SIZE, GX1_DC_H_TIMING_1,
                                          GX1_DC_H_TIMING_2, GX1_DC_H_TIMING_3, GX1_DC_V_TIMING_1,
                                          GX1_DC_V_TIMING_2, GX1_DC_V_TIMING_3
                                        };
    /*for (uint32_t i = 0; i < sizeof(dump_regs) / sizeof(dump_regs[0]); i++)
    {
        vcom_puts("[GEODE] dc[");
        vcom_puthex(dump_regs[i]);
        vcom_puts("]=");
        vcom_puthex(dc_read(dump_regs[i]));
        vcom_puts("\n");
    }
    vcom_puts("[GEODE] dclk=");
    vcom_puthex(vid_read(CS5530_DOT_CLK_CONFIG));
    vcom_puts("\n");*/

    return GR_OK;
}

//программирование синтезатора DCLK CS5530. значения - из штатной таблицы
//Durango (CS5530_PLLtable в vid_5530.c), последовательность - точно по
//gfx_set_clock_frequency: загрузка делителей, reset+bypass, пауза,
//сброс reset, сброс bypass
static void cs5530_set_dclk(uint32_t pll)
{
    volatile uint32_t *r = &gx_mm[(0x10000 + CS5530_DOT_CLK_CONFIG) / 4];
    *r = pll;                    //новые делители, PLL off-line
    *r = pll | 0x80000100;       //Reset (бит 31) + Bypass (бит 8)
    gx1_udelay(1000);            //PLL стабилизируется
    *r = pll & 0x7FFFFFFF;       //Reset low
    *r = pll & 0x7FFFFEFF;       //Bypass low - PLL на новую частоту
}

//стандартные тайминги VESA DMT для нативной смены режима. dclk_khz -
//частота пиксельного клока; pll - значение DCLKPLL из таблицы Durango
//(vid_5530.c CS5530_PLLtable); hfp/hsync/hbp и vfp/vsync/vbp - фронты
//VESA DMT; pol: бит0 = hsync positive, бит1 = vsync positive
struct gx1_timing
{
    uint32_t xres, yres;
    uint32_t dclk_khz;
    uint32_t pll;
    uint32_t hfp, hsync, hbp;
    uint32_t vfp, vsync, vbp;
    uint8_t  pol;
};

static const struct gx1_timing gx1_modes[] =
{
    // pll (Durango)   hfp hsync hbp   vfp vsync vbp  pol
    {  640,  480, 25175, 0x31C45801,  16,  96,  48, 10,  2, 33, 0x0 }, // 60Гц
    {  640,  480, 31500, 0x33915801,  24,  40, 128,  9,  3, 28, 0x0 }, // 73Гц
    {  800,  600, 36000, 0x31EC4801,  24,  72, 128,  1,  2, 22, 0x3 }, // 56Гц
    {  800,  600, 40000, 0x33088801,  40, 128,  88,  1,  4, 23, 0x3 }, // 60Гц
    {  800,  600, 50000, 0x23088801,  56,  64, 120, 37,  6, 23, 0x3 }, // 72Гц
    { 1024,  768, 65000, 0x37911801,  24, 136, 160,  3,  6, 29, 0x0 }, // 60Гц
    { 1024,  768, 75000, 0x37E22801,  24, 136, 144,  3,  6, 29, 0x0 }, // 70Гц
    { 1024,  768, 78750, 0x27915801,  16,  96, 176,  1,  3, 28, 0x3 }, // 75Гц
    { 1280, 1024,108000, 0x2710C805,  48, 112, 248,  1,  3, 38, 0x3 }, // 60Гц
};

static const struct gx1_timing *gx1_find_timing(uint32_t x, uint32_t y)
{
    for (uint32_t i = 0; i < sizeof(gx1_modes) / sizeof(gx1_modes[0]); i++)
        if (gx1_modes[i].xres == x && gx1_modes[i].yres == y)
            return &gx1_modes[i];
    return 0;
}

//нативная смена режима: прямое программирование DC GX1 и PLL CS5530.
//порядок и формулы - точно по Durango gfx_set_specified_mode:
//unlock -> timing off -> fifo off -> DCLK -> геометрия -> H/V тайминги ->
//OCFG -> TCFG -> GCFG -> полярность sync -> relock -> конфиг GP.
//pitch задается классом ширины (Data Book табл. 5-2): 16bpp 640/800/1024
//дают 2048 байта - только при таком классе работает GP-движок.
//1280x1024 (строка 2560 байт) программиуется с фактическим шагом - GP
//там не ускоряет (нет класса), рисует программный слой
static int geode_native_set_mode(vbe_mode_rec_t *rec)
{
    const struct gx1_timing *t = gx1_find_timing(rec->xres, rec->yres);
    if (!t)
    {
        /*vcom_puts("[GEODE] native: no VESA timing for ");
        vcom_puthex(rec->xres);
        vcom_putc('x');
        vcom_puthex(rec->yres);
        vcom_puts(", VBE path\n");*/
        return -1;
    }
    if (rec->bpp != 16)
    {
        //vcom_puts("[GEODE] native: bpp!=16, VBE path\n");
        return -1;
    }

    uint32_t size = (uint32_t)rec->xres * 2;          //байта на строку
    //класс шага строки (Data Book табл. 5-2, не-Pyramid: максимум 2048);
    //строки длиннее 2048 байт (1280x1024x16 = 2560) несут фактический шаг:
    //DC переваривает любой stride, а GP в таких режимах не ускоряет
    uint32_t pitch = (size <= 1024) ? 1024
                     : ((size <= 2048) ? 2048 : size);

    //разблокировка критичных регистров DC
    dc_write(GX1_DC_UNLOCK, GX1_DC_UNLOCK_CODE);

    //гасим экран и останавливаем timing generator
    uint32_t tcfg = dc_read(GX1_DC_TIMING_CFG);
    tcfg &= ~(DC_TCFG_BLKE | DC_TCFG_TGEN);
    dc_write(GX1_DC_TIMING_CFG, tcfg);
    gx1_udelay(1000); //незавершенные запросы к памяти до отключения fifo

    //отключаем fifo load и компрессию на время смены
    uint32_t gcfg = dc_read(GX1_DC_GENERAL_CFG);
    gcfg &= ~(DC_GCFG_DFLE | DC_GCFG_CMPE | DC_GCFG_DECE);
    dc_write(GX1_DC_GENERAL_CFG, gcfg);

    //чистим все делители клока, как gfx_set_specified_mode
    gcfg &= ~(DC_GCFG_DDCK | DC_GCFG_DPCK | DC_GCFG_DFCK);
    gcfg &= ~DC_GCFG_DCLK_MASK;
    dc_write(GX1_DC_GENERAL_CFG, gcfg);

    //новая частота пиксельного клока
    cs5530_set_dclk(t->pll);

    //DCLK mul = 2: внутренний dot clock равен внешнему пиксельному;
    //на шагах >266 МГц дополнительно делится видеоклок (VCLK_DIV)
    gcfg |= DC_GCFG_DCLK_MUL2;
    if (gx_core_mhz > 266) gcfg |= DC_GCFG_VCLK_DIV;
    dc_write(GX1_DC_GENERAL_CFG, gcfg);
    gx1_udelay(1000); //ADL lock: клок должен устоять до записи таймингов

    //организация framebuffer: старт с нуля, служебные offsets в ноль,
    //шаг строки класса, длина строки выборки фактическая
    dc_write(GX1_DC_FB_ST_OFFSET, 0);
    dc_write(GX1_DC_CB_ST_OFFSET, 0);
    dc_write(GX1_DC_CURS_ST_OFFSET, 0);
    dc_write(GX1_DC_VID_ST_OFFSET, 0);
    uint32_t vid_buf = dc_read(GX1_DC_BUF_SIZE) & 0x3FFF0000;
    dc_write(GX1_DC_LINE_DELTA, pitch >> 2);
    dc_write(GX1_DC_BUF_SIZE, ((size >> 3) + 2) | vid_buf);

    //горизонталь: активная | полный цикл; blank start/end; sync start/end
    uint32_t hact = rec->xres;
    uint32_t hbs = hact;               //blank start = конец active
    uint32_t hss = hbs + t->hfp;       //sync start
    uint32_t hse = hss + t->hsync;     //sync end
    uint32_t hbe = hse + t->hbp;       //blank end = htotal
    dc_write(GX1_DC_H_TIMING_1, (hact - 1) | ((hbe - 1) << 16));
    dc_write(GX1_DC_H_TIMING_2, (hbs - 1) | ((hbe - 1) << 16));
    uint32_t hsync_val = (hss - 1) | ((hse - 1) << 16);
    dc_write(GX1_DC_H_TIMING_3, hsync_val);
    dc_write(GX1_DC_FP_H_TIMING, hsync_val);

    //вертикаль
    uint32_t vact = rec->yres;
    uint32_t vbs = vact;
    uint32_t vss = vbs + t->vfp;
    uint32_t vse = vss + t->vsync;
    uint32_t vbe = vse + t->vbp;
    dc_write(GX1_DC_V_TIMING_1, (vact - 1) | ((vbe - 1) << 16));
    dc_write(GX1_DC_V_TIMING_2, (vbs - 1) | ((vbe - 1) << 16));
    dc_write(GX1_DC_V_TIMING_3, (vss - 1) | ((vse - 1) << 16));
    dc_write(GX1_DC_FP_V_TIMING, (vss - 2) | ((vse - 2) << 16));

    //выход: 16bpp 565, panel data enable, пиксель на обоих фронтах
    //(ровно как ocfg в gfx_set_specified_mode для 16bpp)
    uint32_t ocfg = DC_OCFG_PCKE | DC_OCFG_PDEL | DC_OCFG_PDEH;
    dc_write(GX1_DC_OUTPUT_CFG, ocfg);

    //включаем timing generator, sync, blank; задержка; общий конфиг
    tcfg = DC_TCFG_FPPE | DC_TCFG_HSYE | DC_TCFG_VSYE | DC_TCFG_BLKE | DC_TCFG_TGEN;
    dc_write(GX1_DC_TIMING_CFG, tcfg);
    gx1_udelay(1000);

    //general cfg: fifo priority 6/5 (как Durango), fifo load on,
    //DCLK mul 2, VCLK_DIV для быстрых шагов; курсор/видео/компрессия off
    gcfg = (6 << DC_GCFG_DFHPEL_POS) | (5 << DC_GCFG_DFHPSL_POS)
           | DC_GCFG_DCLK_MUL2 | DC_GCFG_DFLE;
    if (gx_core_mhz > 266) gcfg |= DC_GCFG_VCLK_DIV;
    dc_write(GX1_DC_GENERAL_CFG, gcfg);

    //DCfg CS5530: полярность sync по VESA нового режима (как
    //gfx_set_display_control); включенные выходы остаются от BIOS
    uint32_t dcfg = cs_dcfg_saved;
    dcfg &= ~(CS5530_DCFG_CRT_HSYNC_POL | CS5530_DCFG_CRT_VSYNC_POL);
    if (t->pol & 1) dcfg |= CS5530_DCFG_CRT_HSYNC_POL;
    if (t->pol & 2) dcfg |= CS5530_DCFG_CRT_VSYNC_POL;
    vid_write(CS5530_DISPLAY_CONFIG, dcfg);

    //обратно под замок
    dc_write(GX1_DC_UNLOCK, 0);

    //конфигурация GP под новый режим (как финальный WRITE_REG16
    //GP_BLIT_STATUS в gfx_set_specified_mode); класс - от НОВОГО pitch:
    //geode_cur_mode обновится только в geode_finish_mode
    if (pitch == 1024 || pitch == 2048)
    {
        uint16_t cfg = BC_16BPP
                       | ((pitch == 2048) ? BC_FB_WIDTH_2048 : BC_FB_WIDTH_1024);
        gp_write16(GX1_GP_BLT_STATUS, cfg);
    }

    rec->pitch = (uint16_t)pitch;   //слой и LFB-маппинг живут с новым шагом

    /*vcom_puts("[GEODE] native mode set ");
    vcom_puthex(rec->xres);
    vcom_putc('x');
    vcom_puthex(rec->yres);
    vcom_putc('x');
    vcom_puthex(rec->bpp);
    vcom_puts(" pitch=");
    vcom_puthex(pitch);
    vcom_puts(" ok\n");*/
    return 0;
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
        m->red.offset = 11;
        m->red.length = 5;
        m->green.offset = 5;
        m->green.length = 6;
        m->blue.offset = 0;
        m->blue.length = 5;
    }
    else if (m->bpp >= 24)
    {
        m->red.offset = 16;
        m->red.length = 8;
        m->green.offset = 8;
        m->green.length = 8;
        m->blue.offset = 0;
        m->blue.length = 8;
    }

    //прочитанные из 4F01 раскладки точнее дефолта — перекрываем, если есть
    const struct vbe_bitfields *bf = &vbe_bits[index];
    if (bf->bf_r_len && bf->bf_g_len && bf->bf_b_len)
    {
        m->red.offset = bf->bf_r_off;
        m->red.length = bf->bf_r_len;
        m->green.offset = bf->bf_g_off;
        m->green.length = bf->bf_g_len;
        m->blue.offset = bf->bf_b_off;
        m->blue.length = bf->bf_b_len;
    }

    m->lfb = r->phys_base;
    m->pitch = r->pitch;
    m->flags = GR_MODE_LFB;
}

//ops: перечисление и текущий режим

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

//общий хвост смены режима (нативный и VBE): ремап LFB под новую геометрию,
//кэш слоя, fix-инфо, маяк геометрии в COM
static void geode_finish_mode(uint32_t index, vbe_mode_rec_t *rec)
{
    //новый LFB в identity-пространство ядра (старые отображения не мешают)
    if (geode_driver.vmm_map_pages_ptr)
        geode_driver.vmm_map_pages_ptr(rec->phys_base,
                                       (uint32_t)rec->pitch * rec->yres);

    geode_fill_mode(index, &geode_cur_mode);

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
}

//ops: смена режима на ходу

//ключевой принцип: после успешной смены железа возвращать ошибку нельзя —
//слой остался бы на старой геометрии и рисовал бы мусор в чужой LFB.
//поэтому любая успешная смена завершается обновлением кэша слоя (geode_finish_mode)
static int vs_set_mode(void *priv, uint32_t index)
{
    (void)priv;
    if (!geode_ready) return GR_ERR_NODEV;
    if (index >= vbe_drawable) return GR_ERR_BADMODE;

    vbe_mode_rec_t *rec = &vbe_recs[vbe_idx_map[index]];

    //нативный путь GX1: прямое программирование DC+PLL без BIOS.
    //На MediaGX-машинах runtime int 10h идет через VSA/SoftVGA (SMM) и на
    //части машин оставляет дисплей в текстовом режиме - черный экран
    //с курсором. трамплин ему не нужен
    if (native_ok && geode_native_set_mode(rec) == 0)
    {
        geode_finish_mode(index, rec);
        return GR_OK;
    }

    if (!geode_driver.vbe_rm_exec_ptr) return GR_ERR_UNSUPPORTED;

    uint16_t clean = (uint16_t)(rec->mode_number & 0x3FFF);

    /*vcom_puts("[GEODE set idx=");
    vcom_puthex(index);
    vcom_puts(" mode=");
    vcom_puthex(clean);
    vcom_puts("\n");*/

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
        //vcom_puts("[GEODE] 4F02 exec failed, retry once\n");
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
            //vcom_puts("[GEODE] 4F02 refused, trying alternates\n");
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
                //vcom_puts("[GEODE] alternate mode ");
                //vcom_puthex(aclean);
                //vcom_puts(" accepted\n");
                rec->mode_number = ar->mode_number;
                clean = aclean;
                alt_ok = 1;
            }
            if (!alt_ok)
            {
                //все альтернативы тоже отвергнуты: железо не тронуто, слой
                //отрисует старый режим как ни в чем не бывало
                //vcom_puts("[GEODE] 4F02 refused, no alternate accepted\n");
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
        /*if (actual != clean)
        {
            vcom_puts("[GEODE] 4F03 substituted ");
            vcom_puthex(clean);
            vcom_puts(" -> ");
            vcom_puthex(actual);
            vcom_puts("\n");
        }*/
    }
    else
    {
        //vcom_puts("[GEODE] 4F03 failed, assume requested\n");
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
                    /*vcom_puts("[GEODE] 4F03 changed after re-set ");
                    vcom_puthex(actual);
                    vcom_puts(" -> ");
                    vcom_puthex(recheck);
                    vcom_puts("\n");*/
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
        bf_r_len = mi[0x1F];
        bf_r_off = mi[0x20];
        bf_g_len = mi[0x21];
        bf_g_off = mi[0x22];
        bf_b_len = mi[0x23];
        bf_b_off = mi[0x24];

        /*vcom_puts("[GEODE] 4F01 try");
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
        vcom_puts("\n");*/

        //Нули и неадекватные значения — не доверять, пробуем
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
                //vcom_puts("[GEODE] bitfields insane, drop\n");
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
            /*vcom_puts("[GEODE] geometry drift: tbl ");
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
            vcom_puts("\n");*/
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
            //vcom_puts("[GEODE] 4F01 mismatches reference mode, keep stage2 record\n");
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
            /*if (pitch != pitch_ref)
            {
                vcom_puts("[GEODE] pitch implausible or 2x drifted, clamp ");
                vcom_puthex(pitch);
                vcom_puts(" -> ");
                vcom_puthex(pitch_ref);
                vcom_puts("\n");
            }*/
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
        //vcom_puts("[GEODE] 4F01 failed, fallback to stage2 record\n");
        memset(&vbe_bits[index], 0, sizeof(vbe_bits[index]));
    }

    /*vcom_puts("[GEODE] active ");
    vcom_puthex(rec->xres);
    vcom_putc('x');
    vcom_puthex(rec->yres);
    vcom_putc('x');
    vcom_puthex(rec->bpp);
    vcom_puts(" pitch=");
    vcom_puthex(rec->pitch);
    vcom_puts(" phys=");
    vcom_puthex(rec->phys_base);
    vcom_puts("\n");*/

    geode_finish_mode(index, rec);
    return GR_OK;
}

//регистрация

void driver_main(struct boot_info *boot)
{
    descr_api.type = 0;
    descr_api.struct_ptr = 0;
    geode_ready = 0;
    native_ok = 0;
    gx_phys = 0;
    gx_mm = 0;
    gx_base = 0;
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
            //vcom_puts("[GEODE] skip doublescan-suspect mode ");
            //vcom_puthex(vbe_recs[i].mode_number & 0x3FFF);
            //vcom_puts("\n");
            continue;
        }
        //санитизация длины строки: pitch меньше xres*байт_на_пиксель у
        //линейного режима физически невозможен — это ложь BIOS при
        //перечислении (болезнь S3: сообщаемый pitch не равен
        //фактическому скан-ауту). рисунок по такой записи разваливается
        //на мусорные пикселы и повторы строк; канонический минимум —
        //единственная безопасная оценка
        uint32_t min_pitch = (uint32_t)vbe_recs[i].xres *
                             ((vbe_recs[i].bpp + 7) / 8);
        if (vbe_recs[i].pitch < min_pitch)
        {
            /*vcom_puts("[GEODE] stage2 pitch implausible, clamp ");
            vcom_puthex(vbe_recs[i].pitch);
            vcom_puts(" -> ");
            vcom_puthex(min_pitch);
            vcom_puts("\n");*/
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
    //vcom_puts("[GEODE] modes table:\n");
    for (uint32_t i = 0; i < vbe_drawable; i++)
    {
        const vbe_mode_rec_t *t = &vbe_recs[vbe_idx_map[i]];
        /*vcom_puts("tbl[");
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
        vcom_puts("\n");*/
    }

    //формы bitfields сбрасываем: модуль мог грузиться повторно, старые
    //значения от прошлого режима недействительны
    for (uint32_t i = 0; i < vbe_drawable; i++)
    {
        vbe_bits[i].bf_r_len = 0;
        vbe_bits[i].bf_r_off = 0;
        vbe_bits[i].bf_g_len = 0;
        vbe_bits[i].bf_g_off = 0;
        vbe_bits[i].bf_b_len = 0;
        vbe_bits[i].bf_b_off = 0;
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
    //примитивы рисования вешаются ниже только при успешной нативной
    //инициализации: без GX_BASE они вернули бы ошибку, а слой не обязан
    //откатываться на software при неудачном аппаратном вызове —
    //по факту линии/заливка просто пропадали бы с экрана

    memset(&geode_driver, 0, sizeof(geode_driver));
    geode_driver.version = GR_LAYER_ABI_VERSION;
    strcpy(geode_driver.name, "geodegx");
    strcpy(geode_driver.longname, "GEODE GX1 graphic driver");
    geode_driver.priority = 10;          //выше fbdrv-fallback (у него 0)
    geode_driver.modes = 0;              //режимы отдаём динамически через ops
    geode_driver.mode_count = vbe_drawable;
    geode_driver.ops = &geode_ops;
    geode_driver.priv = 0;
    geode_driver.current_mode = cur;

    descr_api.type = DESCR_GRAPHICS_DRIVER;
    descr_api.struct_ptr = &geode_driver;

    //нативная инициализация GX1 (CPUID -> GCR -> GX_BASE). Аппаратные
    //примитивы подключаются только при успехе. copy_area сознательно
    //не подключается (чтение source требует BLT-буферов scratchpad,
    //см. комментарий к geode_copy_area); заливка и вектор их не трогают
    if (geode_init_native(&geode_driver) == GR_OK)
    {
        native_ok = 1;
        geode_ops.fill_rect = geode_fill_rect;
        geode_ops.draw_line = geode_draw_line;
        //vcom_puts("[GEODE] native GX1 path enabled, GP fill/line on\n");
    }
    else
    {
        native_ok = 0;
        //vcom_puts("[GEODE] native path unavailable, VBE mode set only\n");
    }

    geode_ready = 1;
}

description_driver_api_t *driver_get_api(void)
{
    return &descr_api;
}
