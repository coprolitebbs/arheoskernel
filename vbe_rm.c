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

//vbe_rm.c — сервис вызова BIOS int 10h (VBE) из защищённого режима ядра.
//Машинный блоб тримплина собирается из vbe_rm_blob.asm и линкуется в ядро;
//здесь только копирование блоба в младшую память, подготовка ячеек обмена
//(IDTR, параметры) и C-обвязка lcall.
//Сервис нужен для смены видеорежима на ходу: VBE 4F02 доступен только
//в реальном режиме, а графический слой (gr_ops_t.set_mode) живёт в ядре.

#include "include-kernel/vbe_rm.h"
#include "drivers/include_drivers/drv_format.h"
#include "drivers/include_drivers/driver_graphic_layer.h"
#include "include-kernel/lib.h"
#include "include-kernel/comdebug.h"
#include "include-kernel/ports_io.h"

//блоб собирается nasm-ом и попадает в .rodata ядра
extern const uint8_t vbe_rm_blob_start[];
extern const uint8_t vbe_rm_blob_end[];

//метка целостности лежит в служебной зоне за кодом блоба: код занимает
//0x4000..~0x4260, вход 0x4300, результат 0x4320, ячейки 0x4340..0x4375,
//так что 0x4350 никем не трогается. если младшую память кто-то затер,
//метка исчезнет и vbe_rm_exec откажется прыгать в чужой код
#define VBE_RM_MAGIC_ADDR 0x4350
#define VBE_RM_MAGIC      0x5652454Du

//ячейки обмена IDTR с блобом (vbe_rm_blob.asm): 0x4360 — сохраненный
//PM IDTR ядра, 0x4368 — RM-указатель {limit 0x3FF, base 0} на IVT.
//лежат между магией 0x4350 и INFO-буфером 0x4400, никем не трогаются
#define VBE_RM_PM_IDT_ADDR 0x4360
#define VBE_RM_RM_IDT_ADDR 0x4368

//упакованный формат IDTR для sidt/lidt: 2 байта лимита + 4 байта базы
struct idtr_cell
{
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static int vbe_rm_ready = 0;
static volatile int vbe_rm_busy = 0;
static uint32_t vbe_rm_blob_size = 0;

//on-screen маяки стадий блоб печатает сам — BIOS teletype '0'..'3'
//(scrmk в vbe_rm_blob.asm), ячейки дескриптора ему не нужны. прямой
//рисунок в LFB из реального режима невозможен (лимит сегмента 64 КБ
//дает #GP — именно это ломало смену режима в fix4), поэтому экранные
//маркеры делаются только штатным вызовом BIOS. vbe_rm_screen_trace
//осталась как COM-след геометрии: драйвер сообщает lfb/pitch/bpp
//при регистрации и после каждой успешной смены режима

//задать геометрию маяков. зовется графическим драйвером при регистрации
//и после каждой успешной смены режима — COM-след актуальной геометрии;
//сами экранные маркеры блоб печатает teletype и геометрии не требует
int vbe_rm_screen_trace(uint32_t lfb, uint32_t pitch, uint8_t bpp)
{
    com_puts("[VBE_RM] screen trace lfb=");
    com_puthex(lfb);
    com_puts(" pitch=");
    com_puthex(pitch);
    com_puts(" bpp=");
    com_puthex(bpp);
    com_puts("\n");
    return 0;
}

//развернуть блоб в младшей памяти: эталонная копия из .rodata + метка
//целостности за кодом. зовется из vbe_rm_init на старте и из vbe_rm_exec
//для саморемонта, если младшую память кто-то затер между вызовами или
//прямо во время окна RM
static void vbe_rm_deploy(void)
{
    //первый вызов идет до включения пейджинга (linear == physical);
    //повторные — из exec с пейджингом, но младшая память identity-mapped,
    //запись по VA 0x4000 и там попадает на физический 0x4000
    memcpy((void *)VBE_RM_BLOB_ADDR, (const void *)vbe_rm_blob_start, vbe_rm_blob_size);
    //метка целостности — за концом блоба, сам блоб ее не трогает
    *(volatile uint32_t *)VBE_RM_MAGIC_ADDR = VBE_RM_MAGIC;
}

void vbe_rm_init(void)
{
    uint32_t blob_size = (uint32_t)(vbe_rm_blob_end - vbe_rm_blob_start);
    if (blob_size == 0 || blob_size > VBE_RM_BLOB_MAX) return;

    vbe_rm_blob_size = blob_size;
    vbe_rm_deploy();
    vbe_rm_ready = 1;
    com_puts("[VBE_RM] blob copied to 0x4000, size=");
    com_puthex(blob_size);
    com_puts("\n");
}

int vbe_rm_exec(vbe_rm_regs_t *regs)
{
    if (!vbe_rm_ready || !regs) return -1;

    //однопоточное ядро: банально крутимся, пока предыдущий вызов не дошёл
    //до конца; реентерабельность не нужна, из IRQ вызывать запрещено.
    //если это сообщение появилось больше одного раза — вызов случился
    //из контекста, где предыдущий exec ещё не вернулся (реентрантность)
    while (vbe_rm_busy)
    {
        com_puts("[VBE_RM] busy-wait, previous exec not returned\n");
    }
    vbe_rm_busy = 1;

    //проверка целостности тримплина до прыжка: метка 0x4350 и весь код
    //блоба обязаны совпадать с тем, что скопировал vbe_rm_init.
    //расхождение — младшую память кто-то затер: раньше это был вечный
    //отказ всех последующих смен режима (внешне — "4F02 refused"), теперь
    //блоб развертывается заново из эталонной копии в .rodata и вызов
    //продолжается; если затирающий активен, следующий exec снова
    //отремонтирует и напечатает след
    if (*(volatile uint32_t *)VBE_RM_MAGIC_ADDR != VBE_RM_MAGIC ||
        memcmp((const void *)VBE_RM_BLOB_ADDR, vbe_rm_blob_start, vbe_rm_blob_size) != 0)
    {
        com_puts("[VBE_RM] trampoline trashed at 0x4000, redeploying\n");
        vbe_rm_deploy();
        if (*(volatile uint32_t *)VBE_RM_MAGIC_ADDR != VBE_RM_MAGIC ||
            memcmp((const void *)VBE_RM_BLOB_ADDR, vbe_rm_blob_start, vbe_rm_blob_size) != 0)
        {
            com_puts("[VBE_RM] redeploy verify failed, refusing to jump\n");
            vbe_rm_busy = 0;
            return -2;
        }
    }

    //вектор int 10h обязан указывать на BIOS: сегмент 0x0000 в IVT означает,
    //что вектор затёрт — int 10h в реальном режиме улетит на 0000:0000 и
    //процессор поедет исполнять нули IVT (внешне — вечный спин без смены
    //режима). в норме сегмент >= 0xC000 (VGA BIOS в верхнем ROM)
    uint32_t ivt10 = *(volatile uint32_t *)0x40;
    if (ivt10 == 0)
    {
        com_puts("[VBE_RM] IVT int10 vector is 0000:0000, refusing BIOS call\n");
        vbe_rm_busy = 0;
        return -3;
    }

    volatile vbe_rm_regs_t *req = (volatile vbe_rm_regs_t *)VBE_RM_REQ_ADDR;
    volatile vbe_rm_regs_t *res = (volatile vbe_rm_regs_t *)VBE_RM_RES_ADDR;

    //параметры пишем по фиксированному адресу 0x4300 — их читает
    //реальнорежимная часть блоба
    req->ax = regs->ax;
    req->bx = regs->bx;
    req->cx = regs->cx;
    req->dx = regs->dx;
    req->di = regs->di;
    req->si = regs->si;
    req->es = regs->es;

    //трасса вызова: если после этой строки "back" нет — CPU не вернулся
    //из блоба (реальный режим/BIOS); если есть — смотрим на res->ax.
    //блоб параллельно пишет маяки-символы в COM1: '1'..'b' — см.
    //vbe_rm_blob.asm; последний видимый маяк — точная стадия смерти
    com_puts("[VBE_RM] exec ivt10=");
    com_puthex(ivt10 >> 16);
    com_puts(":");
    com_puthex(ivt10 & 0xFFFF);
    com_puts(" ax=");
    com_puthex(req->ax);
    com_puts(" bx=");
    com_puthex(req->bx);
    com_puts(" cx=");
    com_puthex(req->cx);
    com_puts(" dx=");
    com_puthex(req->dx);
    com_puts(" di=");
    com_puthex(req->di);
    com_puts(" si=");
    com_puthex(req->si);
    com_puts(" es=");
    com_puthex(req->es);
    com_puts("\n");

    //IDTR обязан быть переключен блобом на IVT: в реальном режиме на 286+
    //векторы диспетчируются через IDT-механизм, и без переключения int 10h
    //попадал в PM-стаб ядра (шлюз 0x10 IDT -> isr16, селектор 0x08): в RM
    //селектор 0x08 дает базу 0x80, 32-битный стаб исполнялся как 16-битный
    //мусор с физического 0x798B, CPU бесконечно ловил #UD и не возвращался
    //из BIOS — снаружи висение после маяка '6'. сохраняем PM IDTR ядра в
    //ячейку 0x4360 и кладем RM-указатель {limit 0x3FF, base 0} в 0x4368
    struct idtr_cell pm_idt;
    __asm__ __volatile__("sidt %0" : "=m"(pm_idt));
    *(volatile struct idtr_cell *)VBE_RM_PM_IDT_ADDR = pm_idt;
    struct idtr_cell rm_idt = { 0x3FF, 0x00000000u };
    *(volatile struct idtr_cell *)VBE_RM_RM_IDT_ADDR = rm_idt;

    //вход в блоб: CS остаётся flat 0x08, EIP = 0x4000. Блоб сам сохраняет
    //GPR/сегменты/флаги/стек, переключает IDTR на IVT и PIC на легаси-базу,
    //поэтому клоббер-список — перестраховка
    __asm__ __volatile__("lcall $0x08, $0x4000\n" : : : "memory", "eax", "ebx", "ecx", "edx", "esi", "edi", "cc");

    //дренаж OBF 8042: скан-код, пришедший в RM-окно смены режима (IRQ1 был
    //под маской), остается в выходном буфере непрочитанным, и контроллер
    //не выдает новые байты, пока буфер полон — клавиатура умирает до
    //перезагрузки (отпускание Enter, запустившего смену режима, попадает
    //в окно почти всегда). читаем 0x60, пока статус 0x64 показывает OBF
    //(бит 0); байты aux-канала (бит 5 статуса) тоже вычитываем — OBF
    //чистится только чтением, но в счетчик клавиатурных байтов они не идут
    uint8_t drained_kbd = 0;
    for (uint32_t i = 0; i < 64; i++)
    {
        uint8_t st = inb(0x64);
        if (!(st & 0x01)) break;
        uint8_t d = inb(0x60);
        (void)d;
        if (!(st & 0x20)) drained_kbd++;
    }
    if (drained_kbd)
    {
        com_puts("[VBE_RM] drained stuck kbd bytes: ");
        com_puthex(drained_kbd);
        com_puts("\n");
    }

    //контроль затирания во время окна RM: блоб и метка обязаны остаться
    //эталонными и после возврата. расхождение — кто-то пишет в младшую
    //память параллельно с BIOS-вызовом; развернем заново и оставим след,
    //иначе следующий exec узнает о порче только по своей предпроверке
    if (*(volatile uint32_t *)VBE_RM_MAGIC_ADDR != VBE_RM_MAGIC ||
        memcmp((const void *)VBE_RM_BLOB_ADDR, vbe_rm_blob_start, vbe_rm_blob_size) != 0)
    {
        com_puts("[VBE_RM] trampoline trashed during RM window, redeploying\n");
        vbe_rm_deploy();
    }

    com_puts("[VBE_RM] back, res ax=");
    com_puthex(res->ax);
    com_puts(" bx=");
    com_puthex(res->bx);
    com_puts(" cx=");
    com_puthex(res->cx);
    com_puts(" dx=");
    com_puthex(res->dx);
    com_puts(" di=");
    com_puthex(res->di);
    com_puts(" si=");
    com_puthex(res->si);
    com_puts(" es=");
    com_puthex(res->es);
    com_puts("\n");

    regs->ax = res->ax;
    regs->bx = res->bx;
    regs->cx = res->cx;
    regs->dx = res->dx;
    regs->di = res->di;
    regs->si = res->si;
    regs->es = res->es;

    vbe_rm_busy = 0;
    return (regs->ax == 0x004F) ? 0 : -1;
}

uint8_t *vbe_rm_info_buf(void)
{
    return (uint8_t *)VBE_RM_INFO_ADDR;
}
