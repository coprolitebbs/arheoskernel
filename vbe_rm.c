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
#include "include-kernel/lib.h"

//блоб собирается nasm-ом и попадает в .rodata ядра
extern const uint8_t vbe_rm_blob_start[];
extern const uint8_t vbe_rm_blob_end[];

static int vbe_rm_ready = 0;
static volatile int vbe_rm_busy = 0;

void vbe_rm_init(void)
{
    uint32_t blob_size = (uint32_t)(vbe_rm_blob_end - vbe_rm_blob_start);
    if (blob_size == 0 || blob_size > VBE_RM_BLOB_MAX) return;

    //на момент вызова пейджинг ещё выключен: linear == physical,
    //пишем машинный код прямо на физический 0x4000
    memcpy((void *)VBE_RM_BLOB_ADDR, (const void *)vbe_rm_blob_start, blob_size);
    vbe_rm_ready = 1;
}

int vbe_rm_exec(vbe_rm_regs_t *regs)
{
    if (!vbe_rm_ready || !regs) return -1;

    //однопоточное ядро: банально крутимся, пока предыдущий вызов не дошёл
    //до конца; реентерабельность не нужна, из IRQ вызывать запрещено
    while (vbe_rm_busy) { }
    vbe_rm_busy = 1;

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

    //вход в блоб: CS остаётся flat 0x08, EIP = 0x4000. Блоб сам сохраняет
    //GPR/сегменты/флаги и стек, поэтому клоббер-список — перестраховка
    __asm__ __volatile__(
        "lcall $0x08, $0x4000\n"
        :
        :
        : "memory", "eax", "ebx", "ecx", "edx", "esi", "edi", "cc"
    );

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
