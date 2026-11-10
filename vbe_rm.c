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
//здесь только копирование блоба в младшую память и C-обвязка lcall.
//Сервис нужен для смены видеорежима на ходу: VBE 4F02 доступен только
//в реальном режиме, а графический слой (gr_ops_t.set_mode) живёт в ядре.

#include "include-kernel/vbe_rm.h"
#include "drivers/include_drivers/drv_format.h"
#include "drivers/include_drivers/driver_graphic_layer.h"
#include "include-kernel/lib.h"
#include "include-kernel/comdebug.h"

//блоб собирается nasm-ом и попадает в .rodata ядра
extern const uint8_t vbe_rm_blob_start[];
extern const uint8_t vbe_rm_blob_end[];

//метка целостности лежит в зарезервированной зоне блоба (0x4000..0x41ff):
//код блоба занимает 0x4000..0x41c4, служебные ячейки 0x41d0..0x41df, так
//что 0x41e0 никем не трогается. если младшую память кто-то затер, метка
//исчезнет и vbe_rm_exec откажется прыгать в чужой код
#define VBE_RM_MAGIC_ADDR 0x41E0
#define VBE_RM_MAGIC      0x5652454Du

static int vbe_rm_ready = 0;
static volatile int vbe_rm_busy = 0;
static uint32_t vbe_rm_blob_size = 0;

void vbe_rm_init(void)
{
    uint32_t blob_size = (uint32_t)(vbe_rm_blob_end - vbe_rm_blob_start);
    if (blob_size == 0 || blob_size > VBE_RM_BLOB_MAX) return;

    //на момент вызова пейджинг ещё выключен: linear == physical,
    //пишем машинный код прямо на физический 0x4000
    memcpy((void *)VBE_RM_BLOB_ADDR, (const void *)vbe_rm_blob_start, blob_size);
    //метка целостности — за концом блоба, сам блоб ее не трогает
    *(volatile uint32_t *)VBE_RM_MAGIC_ADDR = VBE_RM_MAGIC;
    vbe_rm_blob_size = blob_size;
    vbe_rm_ready = 1;
    com_puts("[VBE_RM] blob copied to 0x4000, size=");
    com_puthex(blob_size);
    com_puts("\n");
}

int vbe_rm_exec(vbe_rm_regs_t *regs)
{
    if (!vbe_rm_ready || !regs) return -1;

    //однопоточное ядро: банально крутимся, пока предыдущий вызов не дошёл
    //до конца; реентерабельность не нужна, из IRQ вызывать запрещено
    while (vbe_rm_busy) { }
    vbe_rm_busy = 1;

    //проверка целостности тримплина до прыжка: метка 0x41e0 и весь код
    //блоба обязаны совпадать с тем, что скопировал vbe_rm_init.
    //расхождение — младшую память кто-то затер, lcall ушел бы в чужой
    //код и выглядел бы снаружи как "просто виснет"
    if (*(volatile uint32_t *)VBE_RM_MAGIC_ADDR != VBE_RM_MAGIC ||
        memcmp((const void *)VBE_RM_BLOB_ADDR, vbe_rm_blob_start, vbe_rm_blob_size) != 0)
    {
        com_puts("[VBE_RM] trampoline trashed at 0x4000, refusing to jump\n");
        vbe_rm_busy = 0;
        return -1;
    }

    volatile vbe_rm_regs_t *req = (volatile vbe_rm_regs_t *)VBE_RM_REQ_ADDR;
    volatile vbe_rm_regs_t *res = (volatile vbe_rm_regs_t *)VBE_RM_RES_ADDR;

    //параметры пишем по фиксированному адресу 0x4200 — их читает
    //реальнорежимная часть блоба
    req->ax = regs->ax;
    req->bx = regs->bx;
    req->cx = regs->cx;
    req->dx = regs->dx;
    req->di = regs->di;
    req->si = regs->si;
    req->es = regs->es;

    //трасса вызова: если после этой строки "back" нет — CPU не вернулся
    //из блоба (реальный режим/BIOS); если есть — смотрим на res->ax
    com_puts("[VBE_RM] exec ax=");
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

    //вход в блоб: CS остаётся flat 0x08, EIP = 0x4000. Блоб сам сохраняет
    //GPR/сегменты/флаги и стек, поэтому клоббер-список — перестраховка
    __asm__ __volatile__(
        "lcall $0x08, $0x4000\n"
        :
        :
        : "memory", "eax", "ebx", "ecx", "edx", "esi", "edi", "cc"
    );

    com_puts("[VBE_RM] back, res ax=");
    com_puthex(res->ax);
    com_puts(" bx=");
    com_puthex(res->bx);
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
