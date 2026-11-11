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

#include "include-kernel/kernel.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/comdebug.h"
#include "include-kernel/isr.h"
#include "include-kernel/syscalls.h"
#include "include-kernel/bootinfo.h"
#include "include-kernel/pmm.h"
#include "include-kernel/vmm.h"
#include "include-kernel/vfs.h"
#include "include-kernel/task.h"
#include "include-kernel/kernel_heap.h"
#include "include-kernel/use_drivers.h"
#include "include-kernel/kconsole.h"
#include "include-kernel/ports_io.h"
#include "include-kernel/kbd_translate.h"
#include "include-kernel/locale.h"
#include "include-kernel/font_vga.h"
#include "userspace/include/syscall.h"
#include "include-kernel/usb_registry.h"
#include "userspace/include/udevices_struct.h"
#include "userspace/include/mount_structures.h"
#include "include-kernel/graphic_layer.h"
#include "userspace/include/ugraph.h"
#include "include-kernel/hardware.h"

// Состояние модификаторов клавиатуры в ядре
bool shift_pressed = false;

//таблица векторов системных вызовов
syscall_vectors_table_t syscalls_table[MAX_SYSCALLS]= {0};

void syscall_handler(struct regs *r)
{

    if (r->eax < MAX_SYSCALLS && syscalls_table[r->eax].func)
    {
        syscalls_table[r->eax].func(r);
    }
    else
    {
        r->eax = -1;//Неизвестный системный вызов
    }

}

uint32_t tty_get_copied_bytes(void)
{
    return current_tty ? current_tty->copied_bytes : 0;
}




void systemcall_fill_screen(struct regs *r)
{
    kernel_fill_screen(r->ebx);
}

void systemcall_exit(struct regs *r)
{
    current_task->state = TASK_TERMINATED;
    current_task->esp = 0;
    schedule(r);
    for(;;);
}

void systemcall_yield(struct regs *r)
{
    r->eax = 0;
    schedule(r);
}

void systemcall_draw_string(struct regs *r)
{
    uint32_t x = r->ebx;
    uint32_t y = r->ecx;
    const char *user_str = (const char *)r->edx;
    uint32_t color = r->esi;
    if (!user_str) return;
    //Проверяем валидность указателя в пространстве пользователя.
    //Строка должна лежать в границах Userspace кучи/стека (выше 0x40000000)
    if ((uint32_t)user_str < 0x40000000)
    {
        r->eax = -1;
        return;
    }
    //Вызываем UTF-8 обёртку отрисовки строк через ядро, которая безопасно перенаправит указатель в драйвер
    //kernel_draw_string(x, y, user_str, color);
    char buf[256];
    int i = 0;
    while (i < 255 && user_str[i])
    {
        buf[i] = user_str[i];
        i++;
    }
    buf[i] = 0;
    kernel_draw_string(x, y, buf, color);
    r->eax = 0;
}

//текущая геометрия экрана из кэша графического слоя. boot_info описывает
//только режим, выставленный загрузчиком: после смены разрешения на ходу
//(SYS_GRAPH_INFO SET_MODE) он уже не соответствует действительности, и
//рисование по его числам уезжает в чужие строки LFB — артефакты,
//дублирование и триплирование картинки по горизонтали
static void gr_sys_screen_dims(uint32_t *w, uint32_t *h)
{
    gr_mode_info_t m;
    if (gr_get_current_mode(&m) == GR_OK && m.xres != 0 && m.yres != 0)
    {
        *w = m.xres;
        *h = m.yres;
        return;
    }
    *w = boot_info ? boot_info->width : 0;
    *h = boot_info ? boot_info->height : 0;
}

void systemcall_draw_char(struct regs *r)
{
    uint32_t scr_w = 0, scr_h = 0;
    gr_sys_screen_dims(&scr_w, &scr_h);
    if(r->ebx >= scr_w || r->ecx >= scr_h) return;
    if(fb && fb->draw_codepoint)
    {
        fb->draw_codepoint(r->ebx, r->ecx, r->edx, r->esi);
    }

}

void systemcall_get_fb_info(struct regs *r)
{
    //геометрия обязана идти от активного режима графического слоя: setbg,
    //sekhmet и arfetch собирают свой рендер из этих чисел, и после смены
    //режима boot_info отдавал бы старые width/height/bpp/pitch — обои и
    //графика рисовались в формате уже несуществующего режима
    gr_mode_info_t m;
    if (gr_get_current_mode(&m) == GR_OK &&
        m.xres != 0 && m.yres != 0 && m.bpp != 0 && m.pitch != 0)
    {
        r->eax = m.pitch;
        r->ebx = m.bpp;
        r->ecx = m.xres;
        r->edx = m.yres;
        return;
    }
    r->eax = boot_info->pitch;
    r->ebx = boot_info->bpp;
    r->ecx = boot_info->width;
    r->edx = boot_info->height;
}


void systemcall_alloc_page(struct regs *r)
{
    //Верхняя граница кучи Ring 3 = нижняя граница user stack (task.h)
    if (current_task->heap_next >= USER_STACK_VA_FLOOR)
    {
        r->eax = 0;
        return;
    }
    uint32_t virt = current_task->heap_next;
    uint32_t phys = (uint32_t)pmm_alloc_page();
    if (phys == 0)
    {
        r->eax = 0;
        return;
    }
    //Блокируем планировщик прерываний таймера на время транзакции маппинга
    kernel_schedule_lock = 1;
    //Включаем каталог страниц текущего процесса в CR3, чтобы таблицы были видимы
    vmm_switch_directory(current_task->page_dir_phys);
    uint32_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    //Зануление физической страницы ОЗУ
    //memset((void*)phys, 0, PAGE_SIZE);
    //Мапим страницу
    vmm_map_page(current_task->page_dir, virt, phys, flags);
    if (vmm_get_phys_addr(current_task->page_dir, virt) == 0)
    {
        //маппинг провалился — не пишем memset по незамапленному VA
        pmm_free_page((void*)phys);
        kernel_schedule_lock = 0;
        r->eax = 0;
        return;
    }
    memset((void*)virt, 0, PAGE_SIZE);

    if(!vm_page_add(current_task, virt, phys, flags))
    {
        vmm_unmap_page(current_task->page_dir, virt);
        pmm_free_page((void*)phys);
        vmm_switch_directory(current_task->page_dir_phys);
        kernel_schedule_lock = 0;
        r->eax = 0;
        return;
    }
    //Сброс TLB-кэша процессора (вместо тяжелого invlpg)
    uint32_t flush_cr3;
    __asm__ __volatile__("mov %%cr3, %0\n\t" "mov %0, %%cr3" : "=r"(flush_cr3) :: "memory");

    kernel_schedule_lock = 0;

    current_task->heap_next += PAGE_SIZE;
    r->eax = virt; //Отдаем виртуальный адрес кучи в процесс Ring 3
}

//ebx = виртуальный адрес освобождаемой страницы (virt)
void systemcall_free_page(struct regs *r)
{
    uint32_t virt = r->ebx;
    //Куча Ring 3 заканчивается там же, где начинается user stack (task.h)
    if ((virt & 0xFFF) != 0 || virt < 0x50000000 || virt >= USER_STACK_VA_FLOOR)
    {
        r->eax = -1;
        return;
    }
    kernel_schedule_lock = 1;
    uint32_t pd_index = virt >> 22;
    uint32_t pde = (uint32_t)current_task->page_dir[pd_index];
    if (!(pde & PAGE_PRESENT) || (pde & 0xFFFFF000) >= 0xC0000000)
    {
        kernel_schedule_lock = 0;
        r->eax = -1;
        return;
    }
    uint32_t phys = vmm_get_phys_addr(current_task->page_dir, virt);
    if (phys == 0)
    {
        kernel_schedule_lock = 0;
        r->eax = -1;
        return;
    }
    if (!vm_page_remove(current_task, virt))
    {
        kernel_schedule_lock = 0;
        r->eax = -1;
        return;
    }
    //Размапим первую страницу, вернем phys в PMM
    vmm_unmap_page(current_task->page_dir, virt);
    pmm_free_page((void*)(phys & 0xFFFFF000));
    kernel_schedule_lock = 0;
    r->eax = 0;
}

//SYS_OPEN (ebx = const char *path, ecx = flags_o)
void systemcall_file_open(struct regs *r)
{
    const char *user_path = (const char *)r->ebx;
    uint32_t flags_o = r->ecx;
    if (!user_path)
    {
        r->eax = -1;
        return;
    }
    //Ищем свободный локальный слот внутри текущей задачи (начиная со слота 3)
    int local_fd = -1;
    for (int i = 3; i < MAX_PROCESS_FDS; i++)
    {
        if (current_task->fds[i] == -1)
        {
            local_fd = i;
            break;
        }
    }
    if (local_fd == -1)
    {
        r->eax = -1;
        return;
    }
    //Выделяем память в куче под временный буфер пути Ring 3
    char *kernel_path = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path)
    {
        r->eax = -1;
        return;
    }
    memset(kernel_path, 0, VFS_PATH_MAX);
    //Маршалим строку из пространства пользователя в кучу ядра
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path[i] != '\0'; i++)
    {
        kernel_path[i] = user_path[i];
    }
    //Выделяем память под итоговый абсолютный каноничный путь
    char *absolute_path = (char *)kmalloc(VFS_PATH_MAX);
    if (!absolute_path)
    {
        kfree(kernel_path);
        r->eax = -1;
        return;
    }
    vfs_resolve_relative_path(kernel_path, absolute_path);
    kfree(kernel_path);
    //Открываем файл на уровне глобальной VFS ядра по честному абсолютному пути
    int global_vfs_fd;

    global_vfs_fd = vfs_open(absolute_path, flags_o);

    kfree(absolute_path);
    if (global_vfs_fd < 0)
    {
        r->eax = global_vfs_fd;
        return;
    }
    //Связываем дескрипторы и возвращаем локальный хэндл в Userspace Ring 3
    current_task->fds[local_fd] = global_vfs_fd;
    r->eax = local_fd;
}

//SYS_CLOSE (ebx = int file_d)
void systemcall_file_close(struct regs *r)
{
    int file_d = (int)r->ebx;
    if (file_d < 0 || file_d >= MAX_PROCESS_FDS)
    {
        r->eax = -1;
        return;
    }
    int global_fd = current_task->fds[file_d];
    if (global_fd < 0)
    {
        r->eax = -1;
        return;
    }

    //Считаем, есть ли в других слотах процесса ссылка на тот же global_fd
    int refs = 0;
    for (int i = 0; i < MAX_PROCESS_FDS; i++)
    {
        if (i != file_d && current_task->fds[i] == global_fd) refs++;
    }

    current_task->fds[file_d] = -1;

    //Реально закрываем только когда ссылок больше нет и это не std-поток
    if (refs == 0 && global_fd > 2)
    {
        r->eax = vfs_close(global_fd);
    }
    else
    {
        r->eax = FS_OK;
    }
}

//SYS_READDIR (ebx = file_d, ecx = fs_dirent_t*)
void systemcall_readdir(struct regs *r)
{
    int file_d = (int)r->ebx;
    fs_dirent_t *user_entry = (fs_dirent_t *)r->ecx;
    if (!user_entry || file_d < 0 || file_d >= MAX_PROCESS_FDS)
    {
        r->eax = -1;
        return;
    }
    int global_fd = current_task->fds[file_d];
    if (global_fd < 0)
    {
        r->eax = -1;
        return;
    }

    fs_dirent_t kernel_entry;
    memset(&kernel_entry, 0, sizeof(fs_dirent_t));

    int vfs_res = vfs_readdir(global_fd, &kernel_entry);
    if (vfs_res != FS_OK)
    {
        r->eax = vfs_res;
        return;
    }
    user_entry->inode = kernel_entry.inode;
    user_entry->type = kernel_entry.type;
    user_entry->attributes = kernel_entry.attributes;
    user_entry->size       = kernel_entry.size;
    user_entry->id         = kernel_entry.id;

    int i = 0;
    for (i = 0; i < 255; i++)
    {
        user_entry->name[i] = kernel_entry.name[i];
        if (kernel_entry.name[i] == '\0')
        {
            break;
        }
    }
    user_entry->name[i] = '\0';
    r->eax = FS_OK;
}

//================ Канонический редактор строки ядра (cooked STDIN) ================
//SYS_READ(STDIN) в cooked-режиме сам набирает строку до Enter: печатает эхо,
//правит её (Backspace/DEL, стрелки, HOME/END), отменяет по Ctrl+C и мигает
//курсором. Приложение получает готовую UTF-8 строку, завершённую '\n'
//(пользовательский scanf/u_read этого даже не замечают — они читают байты).

//Число Unicode-кодпойнтов в первых bytes байтах UTF-8 строки
static uint32_t cooked_cp_count(const char *buf, uint32_t bytes)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < bytes; i++)
    {
        if ((((uint8_t)buf[i]) & 0xC0) != 0x80) n++; //не continuation
    }
    return n;
}

//UTF-8 кодирование кодпойнта, возвращает число записанных байтов (1..4)
static int cooked_utf8_encode(uint32_t cp, uint8_t *out)
{
    int n = 0;
    if (cp < 0x80)
    {
        out[n++] = (uint8_t)cp;
    }
    else if (cp < 0x800)
    {
        out[n++] = (uint8_t)(0xC0 | (cp >> 6));
        out[n++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out[n++] = (uint8_t)(0xE0 | (cp >> 12));
        out[n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    else
    {
        out[n++] = (uint8_t)(0xF0 | (cp >> 18));
        out[n++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        out[n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    return n;
}

//Начало кодпойнта, ПРЕДШЕСТВУЮЩЕГО байту pos (pos всегда на границе кодпойнтов)
static uint32_t cooked_prev_cp(const char *buf, uint32_t pos)
{
    if (pos == 0) return 0;
    uint32_t i = pos - 1;
    while (i > 0 && ((((uint8_t)buf[i]) & 0xC0) == 0x80)) i--;
    return i;
}

//Индекс ЗА кодпойнтом, начинающимся в pos (не дальше len)
static uint32_t cooked_next_cp(const char *buf, uint32_t pos, uint32_t len)
{
    uint32_t i = pos + 1;
    while (i < len && ((((uint8_t)buf[i]) & 0xC0) == 0x80)) i++;
    return i;
}

//Первая свободная ячейка за последним символом строки — по фактическому
//текстовому курсору ядра (он после печати всегда стоит ровно там, включая
//автоперенос на новую строку и скролл)
static void cooked_track_end_from_cursor(tty_t *t)
{
    t->kernel_end_row = (int32_t)(t->cursor_y / t->cell_h);
    t->kernel_end_col = (int32_t)(t->cursor_x / t->cell_w);
}

//Ячейка экрана для байта pos внутри набираемой строки.
//Считаем ОТ КОНЦА строки (kernel_end_row/col актуальны после каждого
//изменения), поэтому переносы и скроллы позицию не сбивают
static void cooked_calc_cursor_cell(const tty_t *t, uint32_t pos, int32_t *out_row, int32_t *out_col)
{
    int cols = (int)t->console_cols;
    if (cols <= 0) cols = 1;

    uint32_t start_col = t->input_start_x / t->cell_w;
    uint32_t total_cells = start_col + cooked_cp_count(t->line_buffer, t->copied_bytes);
    uint32_t pos_cells   = start_col + cooked_cp_count(t->line_buffer, pos);

    int32_t back = (int32_t)total_cells - (int32_t)pos_cells; //ячеек от pos до конца
    int32_t c = t->kernel_end_col - back;
    int32_t r = t->kernel_end_row;
    while (c < 0)
    {
        c += cols;
        r--;
    }
    *out_row = r;
    *out_col = c;
}

//Служебное: дописать десятичное число в буфер
static int cooked_emit_uint(char *out, int n, uint32_t v)
{
    char tmp[8];
    int t = 0;
    if (v == 0)
    {
        out[n++] = '0';
        return n;
    }
    while (v > 0 && t < (int)sizeof(tmp))
    {
        tmp[t++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (t > 0)
    {
        out[n++] = tmp[--t];
    }
    return n;
}

//CUP-последовательность \x1B[row+1;col+1H прямо в STDOUT ядра
//(координаты 1-based, как требует ANSI; парсер ядра сам клампит за экран)
static void cooked_emit_cup(int32_t row, int32_t col)
{
    char seq[24];
    int n = 0;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    seq[n++] = '\x1B';
    seq[n++] = '[';
    n = cooked_emit_uint(seq, n, (uint32_t)(row + 1));
    seq[n++] = ';';
    n = cooked_emit_uint(seq, n, (uint32_t)(col + 1));
    seq[n++] = 'H';
    for (int i = 0; i < n; i++)
    {
        kernel_stdout_write_char(seq[i], console_fgcolor);
    }
}

//Перерисовка хвоста строки: CUP к ячейке (row,col), печать line_buffer[from..),
//фиксация нового конца строки и EL-очистка остатка ряда (вместо пробела —
//чтобы не уехать за нижнюю границу экрана и не вызвать лишний скролл).
//При отключенном эхе (TTY_ECHO снят) печать не производится вовсе
static void cooked_repaint_from(tty_t *t, uint32_t from, int32_t row, int32_t col, bool echo)
{
    if (!echo) return;
    cooked_emit_cup(row, col);
    for (uint32_t i = from; i < t->copied_bytes; i++)
    {
        kernel_stdout_write_char(t->line_buffer[i], console_fgcolor);
    }
    //Конец строки — где текстовый курсор сразу после печати хвоста
    cooked_track_end_from_cursor(t);
    //Стираем бывший хвост старой отрисовки до края ряда (без скролла)
    kernel_stdout_write_char('\x1B', console_fgcolor);
    kernel_stdout_write_char('[',  console_fgcolor);
    kernel_stdout_write_char('K',  console_fgcolor);
    //Текстовый курсор обратно в позицию ввода
    cooked_emit_cup(row, col);
}

//Пересчитать и показать курсор набора в ячейке байта pos
static void cooked_sync_cursor(tty_t *t, uint32_t pos, bool echo)
{
    int32_t row, col;
    cooked_calc_cursor_cell(t, pos, &row, &col);
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    t->kernel_cursor_row = row;
    t->kernel_cursor_col = col;
    if (echo) kconsole_line_cursor_draw_at((uint32_t)col, (uint32_t)row, true);
}

//Перестановка курсора БЕЗ перерисовки текста (стрелки, HOME/END):
//CUP + глиф курсора, ни одна ячейка текста не трогается
static void cooked_move_cursor(tty_t *t, bool echo)
{
    int32_t row, col;
    cooked_calc_cursor_cell(t, t->line_cursor_pos, &row, &col);
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    t->kernel_cursor_row = row;
    t->kernel_cursor_col = col;
    if (echo)
    {
        cooked_emit_cup(row, col);
        kconsole_line_cursor_draw_at((uint32_t)col, (uint32_t)row, true);
    }
}

//Отдаёт приложению кусок готовой строки (UTF-8 байты + завершающий '\n').
//Возвращает число отданных байт или -1, если строка ещё не готова
static int cooked_deliver_line(tty_t *t, uint8_t *user_buf, uint32_t count)
{
    if (!t->line_ready) return -1;

    uint32_t total = t->copied_bytes;                    //без '\n'
    uint32_t have  = total + 1 - t->line_sent_bytes;     //+1 за '\n'
    if (have == 0 || count == 0) return -1;

    uint32_t to_copy = (count < have) ? count : have;
    for (uint32_t i = 0; i < to_copy; i++)
    {
        uint32_t idx = t->line_sent_bytes + i;
        user_buf[i] = (idx < total) ? (uint8_t)t->line_buffer[idx] : (uint8_t)'\n';
    }
    t->line_sent_bytes += to_copy;

    if (t->line_sent_bytes >= total + 1)
    {
        //Строка исчерпана: чистим состояние, следующая — с чистого листа
        t->line_ready        = false;
        t->copied_bytes      = 0;
        t->line_cursor_pos   = 0;
        t->line_sent_bytes   = 0;
        t->kernel_line_active = false;
    }
    return (int)to_copy;
}

void systemcall_file_read(struct regs *r)
{
    int file_d = (int)r->ebx;
    void *user_buf = (void *)r->ecx;
    uint32_t count = r->edx;

    if (!user_buf || count == 0)
    {
        r->eax = -1;
        return;
    }

    int global_fd = current_task->fds[file_d];
    if (global_fd < 0)
    {
        r->eax = -1;
        return;
    }

    if (global_fd == 0)
    {
        if (!kbd_driver_api)
        {
            r->eax = -1;
            return;
        }

        //RAW-режим
        if (current_task->tty_flags & TTY_KBD_RAW)
        {
            if (kbd_driver_api->has_chars())
            {
                uint32_t packed = kbd_driver_api->get_char_blocked();
                if (count >= 4)
                {
                    *(uint32_t*)user_buf = packed;
                    r->eax = 4;
                }
                else if (count >= 1)
                {
                    *(uint8_t*)user_buf  = (uint8_t)(packed & 0xFF);
                    r->eax = 1;
                }
                else
                {
                    r->eax = 0;
                }
            }
            else
            {
                r->eax = 0;
            }
            return;
        }
        //=== COOKED: канонический редактор строки ядра ===
        tty_t *tty = current_tty;
        if (!tty)
        {
            r->eax = -1;
            return;
        }

        bool echo = (current_task->tty_flags & TTY_ECHO) != 0;

        //1) Уже готовая строка — отдаём её байты (возможно, по частям)
        int ready = cooked_deliver_line(tty, (uint8_t *)user_buf, count);
        if (ready >= 0)
        {
            r->eax = ready;
            return;
        }

        //2) Старт новой строки: якорь — текущая позиция текстового курсора ядра
        if (!tty->kernel_line_active && tty->copied_bytes == 0)
        {
            tty->input_start_x      = tty->cursor_x;
            tty->input_start_y      = tty->cursor_y / tty->cell_h;
            tty->copied_bytes       = 0;
            tty->line_cursor_pos    = 0;
            tty->line_sent_bytes    = 0;
            tty->kernel_ctrl_down   = false;
            tty->kernel_e0_prefix   = false;
            //Конец строки сейчас = сам якорь. Без этой синхронизации позиция
            //курсора считается от устаревшего kernel_end (при первом чтении это
            //0,0, при повторном — конец ПРЕДЫДУЩЕЙ строки), и курсор рисуется
            //не там, где закончился вывод. После первого же нажатия он "отлетал"
            //к правильному месту — теперь стоит там с самого начала
            cooked_track_end_from_cursor(tty);
            //Мигание и глиф — только при включенном эхе (ввод без эха — невидимый)
            tty->kernel_line_active = echo;
            tty->cursor_visible     = true;
            tty->last_blink_tick    = tick_count;
            kconsole_reset_utf8_state();
            //Курсор виден сразу, до первого нажатия
            cooked_sync_cursor(tty, 0, echo);
        }

        //3) Разбор накопившихся событий клавиатуры
        while (kbd_driver_api->has_chars())
        {
            uint32_t packed = kbd_driver_api->get_char_blocked();
            uint8_t sc   = (uint8_t)(packed & 0xFF);
            uint8_t type = (uint8_t)((packed >> 8) & 0xFF);

            //Префикс расширенных клавиш (стрелки, HOME/END, DEL)
            if (sc == 0xE0)
            {
                tty->kernel_e0_prefix = true;
                continue;
            }
            bool e0 = tty->kernel_e0_prefix;
            tty->kernel_e0_prefix = false;

            //Ctrl отслеживаем только как модификатор Ctrl+C
            if (sc == 0x1D)
            {
                tty->kernel_ctrl_down = (type == 1);
                continue;
            }
            //Отпускания клавиш редактору не интересны
            if (type == 0) continue;

            //--- Ctrl+C: отмена ввода строки ---
            if (tty->kernel_ctrl_down && sc == 0x2E)
            {
                if (echo)
                {
                    kernel_stdout_write_codepoint('^', console_fgcolor);
                    kernel_stdout_write_codepoint('C', console_fgcolor);
                    kernel_stdout_write_codepoint('\n', console_fgcolor);
                }
                kconsole_line_cursor_hide();
                tty->copied_bytes       = 0;
                tty->line_cursor_pos    = 0;
                tty->line_sent_bytes    = 0;
                tty->line_ready         = false;
                tty->kernel_line_active = false;
                r->eax = -1; //чтение прервано: scanf/u_read увидят ошибку
                return;
            }

            uint32_t pos = tty->line_cursor_pos;
            uint32_t len = tty->copied_bytes;

            //--- Enter: строка готова ---
            if (sc == 0x1C)
            {
                if (echo) kernel_stdout_write_codepoint('\n', console_fgcolor);
                if (len < LINE_BUFFER_SIZE) tty->line_buffer[len] = '\0';
                tty->line_ready         = true;
                tty->line_sent_bytes    = 0;
                tty->kernel_line_active = false; //гасим мигание
                kconsole_line_cursor_hide();
                break;
            }

            //--- Расширенные клавиши: стрелки, HOME/END, DEL ---
            if (e0)
            {
                if (sc == 0x4B) //стрелка влево — на кодпойнт влево
                {
                    if (pos > 0)
                    {
                        tty->line_cursor_pos = cooked_prev_cp(tty->line_buffer, pos);
                        cooked_move_cursor(tty, echo);
                    }
                    continue;
                }
                if (sc == 0x4D) //стрелка вправо — на кодпойнт вправо
                {
                    if (pos < len)
                    {
                        tty->line_cursor_pos = cooked_next_cp(tty->line_buffer, pos, len);
                        cooked_move_cursor(tty, echo);
                    }
                    continue;
                }
                if (sc == 0x47) //HOME — начало строки
                {
                    if (pos != 0)
                    {
                        tty->line_cursor_pos = 0;
                        cooked_move_cursor(tty, echo);
                    }
                    continue;
                }
                if (sc == 0x4F) //END — конец строки
                {
                    if (pos != len)
                    {
                        tty->line_cursor_pos = len;
                        cooked_move_cursor(tty, echo);
                    }
                    continue;
                }
                if (sc == 0x53) //DEL — удалить кодпойнт ПОД курсором
                {
                    if (pos < len)
                    {
                        uint32_t nxt = cooked_next_cp(tty->line_buffer, pos, len);
                        int32_t row, col;
                        //Ячейку pos считаем ДО модификации (конец строки ещё старый)
                        cooked_calc_cursor_cell(tty, pos, &row, &col);
                        for (uint32_t i = nxt; i <= len; i++)
                        {
                            tty->line_buffer[pos + (i - nxt)] = tty->line_buffer[i];
                        }
                        tty->copied_bytes = len - (nxt - pos);
                        cooked_repaint_from(tty, pos, row, col, echo);
                        cooked_sync_cursor(tty, pos, echo);
                    }
                    continue;
                }
                //Стрелки вверх/вниз и прочие расширенные — игнорируем
                continue;
            }

            uint32_t cp = kbd_translate_event(packed);
            if (cp == 0) continue;

            //--- Backspace: удалить кодпойнт ПЕРЕД курсором ---
            if (sc == 0x0E || cp == '\b')
            {
                if (pos > 0)
                {
                    uint32_t prev = cooked_prev_cp(tty->line_buffer, pos);
                    int32_t row, col;
                    //Ячейку prev считаем ДО модификации (конец строки ещё старый)
                    cooked_calc_cursor_cell(tty, prev, &row, &col);
                    for (uint32_t i = pos; i <= len; i++)
                    {
                        tty->line_buffer[prev + (i - pos)] = tty->line_buffer[i];
                    }
                    tty->copied_bytes    = len - (pos - prev);
                    tty->line_cursor_pos = prev;
                    cooked_repaint_from(tty, prev, row, col, echo);
                    cooked_sync_cursor(tty, prev, echo);
                }
                continue;
            }

            //--- Печатные символы: вставка кодпойнта в позицию курсора ---
            if (cp >= 0x20 && cp != 0x7F)
            {
                uint8_t utf8[4];
                int step = cooked_utf8_encode(cp, utf8);
                if (step > 0 && len + (uint32_t)step < LINE_BUFFER_SIZE - 1)
                {
                    if (pos < len)
                    {
                        //Вставка в середину: сдвиг хвоста вправо и перерисовка
                        int32_t row, col;
                        //Ячейку pos считаем ДО модификации
                        cooked_calc_cursor_cell(tty, pos, &row, &col);
                        for (int32_t i = (int32_t)len; i >= (int32_t)pos; i--)
                        {
                            tty->line_buffer[i + step] = tty->line_buffer[i];
                        }
                        for (int k = 0; k < step; k++)
                        {
                            tty->line_buffer[pos + k] = (char)utf8[k];
                        }
                        tty->copied_bytes    = len + (uint32_t)step;
                        tty->line_cursor_pos = pos + (uint32_t)step;
                        cooked_repaint_from(tty, pos, row, col, echo);
                        cooked_sync_cursor(tty, pos + (uint32_t)step, echo);
                    }
                    else
                    {
                        //Дописывание в конец: сначала буфер, потом эхо
                        for (int k = 0; k < step; k++)
                        {
                            tty->line_buffer[len + k] = (char)utf8[k];
                        }
                        tty->copied_bytes    = len + (uint32_t)step;
                        tty->line_cursor_pos = len + (uint32_t)step;
                        if (echo)
                        {
                            //Текстовый курсор ядра мог остаться где угодно
                            //(после вставки в середину строки или стрелок) —
                            //сначала ставим его в первую свободную ячейку за
                            //строкой и только затем печатаем символ. Иначе
                            //символ ложится поверх середины строки
                            cooked_emit_cup(tty->kernel_end_row, tty->kernel_end_col);
                            kernel_stdout_write_codepoint(cp, console_fgcolor);
                            //Конец строки — где курсор сразу после печати
                            cooked_track_end_from_cursor(tty);
                        }
                        cooked_sync_cursor(tty, tty->line_cursor_pos, echo);
                    }
                }
                continue;
            }
            //Прочие управляющие коды игнорируем (например '\t': он занимает
            //4 ячейки, но один кодпойнт — сломал бы расчёт позиций)
        }

        //4) Если во время разбора нажали Enter — сразу отдаём начало строки
        int ready_after = cooked_deliver_line(tty, (uint8_t *)user_buf, count);
        if (ready_after >= 0)
        {
            r->eax = ready_after;
            return;
        }

        r->eax = 0; //строка ещё не готова — приложение подождёт
        return;
    }

    //Чтение обычных дисковых файлов (VFS)
    if (count > 32768) count = 32768;
    uint8_t *kernel_buf = (uint8_t *)kmalloc(count);
    if (!kernel_buf)
    {
        r->eax = -1;
        return;
    }
    memset(kernel_buf, 0, count);

    //Читаем из честного глобального VFS описания
    int bytes_read = vfs_read(global_fd, kernel_buf, count);
    if (bytes_read > 0) memcpy(user_buf, kernel_buf, bytes_read);
    kfree(kernel_buf);
    r->eax = bytes_read;
}



//SYS_WRITE (ebx = fd, ecx = буфер const void*, edx = count)
void systemcall_file_write(struct regs *r)
{
    int fd_wr = (int)r->ebx;
    const void *user_src = (const void *)r->ecx;
    uint32_t count_wr = r->edx;

    if (!user_src || count_wr == 0)
    {
        r->eax = -1;
        return;
    }

    int global_fd = current_task->fds[fd_wr];
    if (global_fd < 0)
    {
        r->eax = -1;
        return;
    }

    if (global_fd == 1 || global_fd == 2)
    {
        uint32_t written_bytes = 0;
        const char *src_chars = (const char *)user_src;
        while (written_bytes < count_wr)
        {
            char c = src_chars[written_bytes];
            kernel_stdout_write_char(c, console_fgcolor);
            written_bytes++;
        }
        r->eax = written_bytes;
        return;
    }

    if (count_wr > 32768) count_wr = 32768;
    char *kernel_buf_wr = (char *)kmalloc(count_wr + 1);
    if (!kernel_buf_wr)
    {
        r->eax = -1;
        return;
    }

    //Копируем данные через нативный memcpy,
    //это гарантирует корректный маршалинг страниц Ring 3 текущего процесса
    memcpy(kernel_buf_wr, user_src, count_wr);
    kernel_buf_wr[count_wr] = '\0';

    int bytes_written = vfs_write(global_fd, kernel_buf_wr, count_wr);
    kfree(kernel_buf_wr);

    r->eax = bytes_written;
}





//SYS_MKDIR (ebx = Путь const char *path)
void systemcall_mkdir(struct regs *r)
{
    const char *user_path_nm = (const char *)r->ebx;
    if (!user_path_nm)
    {
        r->eax = -1;
        return;
    }
    //Динамическая аллокация в куче Ring 0 для защиты 4 КБ стека ядра
    char *kernel_path_nm = (char *)kmalloc(VFS_PATH_MAX);
    char *absolute_path_nm = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path_nm || !absolute_path_nm)
    {
        if (kernel_path_nm) kfree(kernel_path_nm);
        if (absolute_path_nm) kfree(absolute_path_nm);
        r->eax = -1;
        return;
    }
    memset(kernel_path_nm, 0, VFS_PATH_MAX);
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path_nm[i] != '\0'; i++)
    {
        kernel_path_nm[i] = user_path_nm[i];
    }
    //Преобразуем относительный путь в абсолютный на базе CWD активного процесса
    vfs_resolve_relative_path(kernel_path_nm, absolute_path_nm);
    kfree(kernel_path_nm);
    r->eax = vfs_mkdir(absolute_path_nm);
    kfree(absolute_path_nm);
}


void systemcall_file_unlink(struct regs *r)
{
    const char *user_path_un = (const char *)r->ebx;
    if (!user_path_un)
    {
        r->eax = -1;
        return;
    }
    char *kernel_path_un = (char *)kmalloc(VFS_PATH_MAX);
    char *absolute_path_un = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path_un || !absolute_path_un)
    {
        if (kernel_path_un) kfree(kernel_path_un);
        if (absolute_path_un) kfree(absolute_path_un);
        r->eax = -1;
        return;
    }
    memset(kernel_path_un, 0, VFS_PATH_MAX);
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path_un[i] != '\0'; i++)
    {
        kernel_path_un[i] = user_path_un[i];
    }
    vfs_resolve_relative_path(kernel_path_un, absolute_path_un);
    kfree(kernel_path_un);
    //Передаем в VFS валидный абсолютный путь, начинающийся со слэша '/'
    r->eax = vfs_unlink(absolute_path_un);
    kfree(absolute_path_un);
}

//SYS_RMDIR (ebx = пользовательский путь const char *path)
void systemcall_rmdir(struct regs *r)
{
    const char *user_path_rm = (const char *)r->ebx;
    if (!user_path_rm)
    {
        r->eax = -1;
        return;
    }
    char *kernel_path_rm = (char *)kmalloc(VFS_PATH_MAX);
    char *absolute_path_rm = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path_rm || !absolute_path_rm)
    {
        if (kernel_path_rm) kfree(kernel_path_rm);
        if (absolute_path_rm) kfree(absolute_path_rm);
        r->eax = -1;
        return;
    }
    memset(kernel_path_rm, 0, VFS_PATH_MAX);
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path_rm[i] != '\0'; i++)
    {
        kernel_path_rm[i] = user_path_rm[i];
    }
    //Разрешаем относительные пути и для удаления каталогов
    vfs_resolve_relative_path(kernel_path_rm, absolute_path_rm);
    kfree(kernel_path_rm);
    r->eax = vfs_rmdir(absolute_path_rm);
    kfree(absolute_path_rm);
}



void systemcall_file_rename(struct regs *r)
{
    const char *user_old = (const char *)r->ebx;
    const char *user_new = (const char *)r->ecx;
    if (!user_old || !user_new)
    {
        r->eax = -1;
        return;
    }

    char *k_old = (char *)kmalloc(VFS_PATH_MAX);
    char *k_new = (char *)kmalloc(VFS_PATH_MAX);
    char *abs_old = (char *)kmalloc(VFS_PATH_MAX);
    char *abs_new = (char *)kmalloc(VFS_PATH_MAX);
    if (!k_old || !k_new || !abs_old || !abs_new)
    {
        if (k_old) kfree(k_old);
        if (k_new) kfree(k_new);
        if (abs_old) kfree(abs_old);
        if (abs_new) kfree(abs_new);
        r->eax = -1;
        return;
    }
    memset(k_old, 0, VFS_PATH_MAX);
    memset(k_new, 0, VFS_PATH_MAX);
    for (int i = 0; i < (VFS_PATH_MAX-1) && user_old[i]; i++) k_old[i] = user_old[i];
    for (int i = 0; i < (VFS_PATH_MAX-1) && user_new[i]; i++) k_new[i] = user_new[i];

    vfs_resolve_relative_path(k_old, abs_old);
    vfs_resolve_relative_path(k_new, abs_new);
    kfree(k_old);
    kfree(k_new);
    r->eax = vfs_rename(abs_old, abs_new);
    kfree(abs_old);
    kfree(abs_new);
}

//ebx = const char *path, ecx = fs_stat_t *st
void systemcall_file_stat(struct regs *r)
{
    const char *user_path = (const char *)r->ebx;
    fs_stat_t *user_st = (fs_stat_t *)r->ecx;
    if (!user_path || !user_st)
    {
        r->eax = -1;
        return;
    }
    //Выделяем 4096-байтные буферы в куче ядра
    char *kernel_path = (char *)kmalloc(VFS_PATH_MAX);
    char *absolute_path = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path || !absolute_path)
    {
        if (kernel_path) kfree(kernel_path);
        if (absolute_path) kfree(absolute_path);
        r->eax = -1;
        return;
    }
    //Маршалим имя файла из Ring 3 памяти в кучу ядра
    memset(kernel_path, 0, VFS_PATH_MAX);
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path[i] != '\0'; i++)
    {
        kernel_path[i] = user_path[i];
    }
    //Разрешаем относительные пути и точки "." / ".."
    vfs_resolve_relative_path(kernel_path, absolute_path);
    kfree(kernel_path);
    //Объявляем локальный ядерный буфер метаданных
    fs_stat_t kernel_st;
    memset(&kernel_st, 0, sizeof(fs_stat_t));
    //Запрашиваем данные у VFS и драйвера файловой системы
    int vfs_res = vfs_stat(absolute_path, &kernel_st);
    kfree(absolute_path);
    if (vfs_res != VFS_STATUS_OK)
    {
        r->eax = vfs_res;
        return;
    }
    user_st->size        = kernel_st.size;
    user_st->attributes  = kernel_st.attributes;
    user_st->create_time = kernel_st.create_time; //Пробрасываем ctime из ext2
    user_st->modify_time = kernel_st.modify_time; //Пробрасываем mtime из ext2
    r->eax = VFS_STATUS_OK;
}


void systemcall_file_seek(struct regs *r)
{
    int file_d = (int)r->ebx;
    uint32_t position = r->ecx;
    int global_fd = current_task->fds[file_d];
    if (global_fd < 0)
    {
        r->eax = -1;
        return;
    }
    r->eax = vfs_seek(global_fd, position);
}

//Системный вызов очистки консоли TTY и фреймбуфера
void systemcall_clear_screen(struct regs *r)
{
    kconsole_clear_screen();
    r->eax = 0;
}



void systemcall_spawn_process(struct regs *r)
{
    const char *user_path = (const char *)r->ebx;
    const char *user_cmdline = (const char *)r->ecx;
    if (!user_path)
    {
        r->eax = -1;
        return;
    }

    //Блокируем вытеснение на время спавна:
    //1) task_spawn_elf/builtin переключают CR3 на каталог новой задачи и обратно
    //   Если посреди этого окна придёт таймер, schedule() уйдёт с CR3=new-PD,
    //   а вернётся в продолжение спавна с CR3=родителя — и оставшаяся часть спавна
    //   (memcpy сегментов ELF в user-VA нового процесса, argv push на его стек)
    //   молча запишет в чужую память
    //2) spawn — самый глубокий syscall, вложенный IRQ0+schedule()+cleanup_tasks()
    //   на том же kernel stack лишний раз съедают его запас
    //Приоритет сохраняем: если спавн запущен из контекста, где блокировка уже
    //была выставлена, наружу выйдет с тем же значением.
    uint8_t saved_sched_lock = kernel_schedule_lock;
    kernel_schedule_lock = 1;

    char k_path[128];
    char k_cmdline[256];
    memset(k_path, 0, 128);
    memset(k_cmdline, 0, 256);

    for (int i = 0; i < 127 && user_path[i] != '\0'; i++) k_path[i] = user_path[i];

    if (user_cmdline)
    {
        for (int i = 0; i < 255 && user_cmdline[i] != '\0'; i++) k_cmdline[i] = user_cmdline[i];
    }

    char out_redirect_file[128];
    char in_redirect_file[128];
    memset(out_redirect_file, 0, 128);
    memset(in_redirect_file, 0, 128);
    bool do_out_redirect = false;
    bool do_in_redirect  = false;

    for (int i = 0; k_cmdline[i] != '\0'; i++)
    {
        if (k_cmdline[i] == '>' && (i == 0 || k_cmdline[i-1] == ' '))
        {
            do_out_redirect = true;
            k_cmdline[i] = '\0';

            int skip = i + 1;
            while (k_cmdline[skip] == ' ' || k_cmdline[skip] == '\t') skip++;

            int f_idx = 0;
            while (k_cmdline[skip] != '\0' && k_cmdline[skip] != ' ' && f_idx < 127)
            {
                out_redirect_file[f_idx++] = k_cmdline[skip++];
            }
            out_redirect_file[f_idx] = '\0';
            break;
        }
        if (k_cmdline[i] == '<' && (i == 0 || k_cmdline[i-1] == ' '))
        {
            do_in_redirect = true;
            k_cmdline[i] = '\0';
            int skip = i + 1;
            while (k_cmdline[skip] == ' ' || k_cmdline[skip] == '\t') skip++;
            int f_idx = 0;
            while (k_cmdline[skip] != '\0' && k_cmdline[skip] != ' ' && f_idx < 127)
            {
                in_redirect_file[f_idx++] = k_cmdline[skip++];
            }
            break;
        }
    }

    int end_space = strlen(k_cmdline) - 1;
    while (end_space >= 0 && (k_cmdline[end_space] == ' ' || k_cmdline[end_space] == '\t'))
    {
        k_cmdline[end_space--] = '\0';
    }

    task_t *new_task = task_spawn_from_file(k_path, 0x40000000, k_cmdline[0] != '\0' ? k_cmdline : NULL);
    if (!new_task)
    {
        kernel_schedule_lock = saved_sched_lock;
        r->eax = -1;
        return;
    }

    if (do_out_redirect && out_redirect_file[0] != '\0')
    {
        // Резолвим относительный путь к абсолютному на базе CWD текущего процесса
        char *abs_redirect = (char *)kmalloc(VFS_PATH_MAX);

        if (abs_redirect)
        {
            memset(abs_redirect, 0, VFS_PATH_MAX);
            vfs_resolve_relative_path(out_redirect_file, abs_redirect);

            int global_vfs_fd = vfs_open(abs_redirect,
                                         FS_OPEN_CREATE | FS_OPEN_WRITE | FS_OPEN_TRUNCATE);
            kfree(abs_redirect);

            if (global_vfs_fd >= 0)
            {
                new_task->fds[1] = global_vfs_fd;
            }
        }
    }

    if (do_in_redirect && in_redirect_file[0] != '\0')
    {
        char *abs_redirect = (char *)kmalloc(VFS_PATH_MAX);
        if (abs_redirect)
        {
            memset(abs_redirect, 0, VFS_PATH_MAX);
            vfs_resolve_relative_path(in_redirect_file, abs_redirect);
            int global_vfs_fd = vfs_open(abs_redirect, FS_OPEN_READ);
            kfree(abs_redirect);
            if (global_vfs_fd >= 0)
            {
                new_task->fds[0] = global_vfs_fd;
            }
        }
    }

    kernel_schedule_lock = saved_sched_lock;
    r->eax = new_task->pid;
}




void systemcall_wait_process_with_pid(struct regs *r)
{
    uint32_t target_pid = r->ebx;
    r->eax = -1; //По умолчанию возвращаем ошибку (процесс не найден)
    if (ready_queue)
    {
        task_t *start = ready_queue;
        task_t *t = start;
        bool found = false;
        //Перебираем кольцевую очередь в поисках живого дочернего процесса
        do
        {
            if (t->pid == target_pid)
            {
                found = true;
                break;
            }
            t = t->next;
        }
        while (t != start);
        //Если процесс найден в кольце и он еще выполняет свой код,
        //возвращаем статус PROCESS_IS_ACTIVE
        if (found && t->state != TASK_TERMINATED)
        {
            r->eax = PROCESS_IS_ACTIVE;
            return;
        }
    }
    //Если процесс успешно завершил работу (сделал SYS_EXIT) и исчез из очереди
    r->eax = 0;
}


void systemcall_isatty(struct regs *r)
{
    int fd = (int)r->ebx;
    if (fd < 0 || fd >= MAX_PROCESS_FDS)
    {
        r->eax = 0;
        return;
    }
    int global_fd = current_task->fds[fd];
    // 0 = keyboard marker
    r->eax = (global_fd == 0) ? 1 : 0;
}

//ebx = am_mem_info_t *user_info
void systemcall_get_memory_info(struct regs *r)
{
    am_mem_info_t *user_info = (am_mem_info_t *)r->ebx;
    if (!user_info)
    {
        r->eax = -1;
        return;
    }
    //Объявляем локальный ядерный буфер сбора статистики
    am_mem_info_t kernel_info;
    kernel_info.total_pages = total_pages;
    kernel_info.free_pages  = free_pages;
    kernel_info.used_pages  = total_pages - free_pages;
    kernel_info.heap_total_bytes = total_pages * PAGE_SIZE;
    kernel_info.heap_used_bytes = (total_pages - free_pages) * PAGE_SIZE;
    //Безопасно копируем собранную структуру Ring 0 в память Ring 3
    char *dst = (char *)user_info;
    char *src = (char *)&kernel_info;
    for (uint32_t i = 0; i < sizeof(am_mem_info_t); i++)
    {
        dst[i] = src[i];
    }
    r->eax = 0;
}


void systemcall_set_framebuffer_wallpaper(struct regs *r)
{
    const void *user_buf = (const void *)r->ebx;
    uint32_t size = r->ecx;
    if (!user_buf || size == 0)
    {
        r->eax = -1;
        return;
    }
    if (fb && fb->set_wallpaper)
    {
        uint8_t saved = kernel_schedule_lock;
        kernel_schedule_lock = 1;
        fb->set_wallpaper(user_buf, size);
        kernel_schedule_lock = saved;
        r->eax = 0;
    }
    else
    {
        r->eax = -1;
    }
}

//ebx = int32_t increment (приращение кучи в байтах)
void systemcall_sbrk(struct regs *r)
{
    int32_t increment = (int32_t)r->ebx;
    uint32_t old_heap_next = current_task->heap_next;
    if (increment <= 0)
    {
        r->eax = old_heap_next;
        return;
    }
    //Вычисляем, сколько страниц ОЗУ нужно выделить под этот запрос
    uint32_t pages_needed = (increment + PAGE_SIZE - 1) / PAGE_SIZE;
    //Защитный барьер кучи Ring 3: не подниматься выше нижней границы user stack (task.h)
    if (current_task->heap_next + (pages_needed * PAGE_SIZE) >= USER_STACK_VA_FLOOR)
    {
        r->eax = 0;
        return;
    }
    //Блокируем планировщик таймера на время массового маппинга мегабайтов
    kernel_schedule_lock = 1;
    //Принудительно загружаем физический каталог страниц текущего процесса в CR3
    vmm_switch_directory(current_task->page_dir_phys);
    bool alloc_success = true;
    for (uint32_t i = 0; i < pages_needed; i++)
    {
        uint32_t virt = current_task->heap_next;
        uint32_t phys = 0;
        //Такой же Memory Shield для защищённых sbrk массивов
        for (int attempt = 0; attempt < 1024; attempt++)
        {
            phys = (uint32_t)pmm_alloc_page();
            if (!phys) break;
            //Не отдаем под user-страницу ни одну из KERNEL_STACK_PAGES страниц
            //kernel stack текущей задачи (стек теперь 4 страницы, не одна)
            uint32_t kstack_base = current_task->kernel_stack_phys;
            if (phys >= 0x00100000 && phys < 0xC0000000 &&
                !(kstack_base != 0 &&
                  phys >= kstack_base &&
                  phys < kstack_base + KERNEL_STACK_PAGES * PAGE_SIZE))
            {
                break;
            }
            pmm_free_page((void*)phys);
            phys = 0;
        }
        if (phys == 0)
        {
            alloc_success = false;
            break;
        }
        uint32_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        vmm_map_page(current_task->page_dir, virt, phys, flags);
        //Зануляем по виртуальному адресу кучи Ring 3
        memset((void*)virt, 0, PAGE_SIZE);
        if (!vm_page_add(current_task, virt, phys, flags))
        {
            vmm_unmap_page(current_task->page_dir, virt);
            pmm_free_page((void*)phys);
            alloc_success = false;
            break;
        }
        current_task->heap_next += PAGE_SIZE;
    }
    //Повторно перезагружаем CR3 каталогом текущего процесса для фиксации TLB-кэша
    vmm_switch_directory(current_task->page_dir_phys);
    //Откат частичного выделения: если цикл прервался на i-й странице, у нас замаплены
    //страницы от old_heap_next до current_task->heap_next, возвращаем их PMM и откатываем heap_next
    if (!alloc_success)
    {
        while (current_task->heap_next > old_heap_next)
        {
            current_task->heap_next -= PAGE_SIZE;
            uint32_t virt = current_task->heap_next;
            uint32_t phys = vmm_get_phys_addr(current_task->page_dir, virt);
            if (phys != 0)
            {
                vmm_unmap_page(current_task->page_dir, virt);
                pmm_free_page((void*)(phys & 0xFFFFF000));
                vm_page_remove(current_task, virt);   // ← теперь освобождает
            }
        }
    }
    //Разблокируем кооперативный планировщик ядра
    kernel_schedule_lock = 0;

    if (!alloc_success)
    {
        r->eax = 0;
    }
    else
    {
        r->eax = old_heap_next; //Возвращаем стартовый адрес выделенного региона кучи
    }
}

// ebx = const char *path
void systemcall_chdir(struct regs *r)
{
    const char *user_path = (const char *)r->ebx;
    if (!user_path)
    {
        r->eax = -1;
        return;
    }

    char *kernel_path = (char *)kmalloc(VFS_PATH_MAX);
    char *absolute_path = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path || !absolute_path)
    {
        if (kernel_path) kfree(kernel_path);
        if (absolute_path) kfree(absolute_path);
        r->eax = -1;
        return;
    }
    memset(kernel_path, 0, VFS_PATH_MAX);
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path[i] != '\0'; i++)
    {
        kernel_path[i] = user_path[i];
    }
    vfs_resolve_relative_path(kernel_path, absolute_path);
    kfree(kernel_path);
    //Проверяем, что путь существует и является каталогом — БЕЗ открытия fd
    fs_stat_t st;
    memset(&st, 0, sizeof(st));
    int st_res = vfs_stat(absolute_path, &st);

    if (st_res != VFS_STATUS_OK || !(st.attributes & FS_ATTR_DIRECTORY))
    {
        kfree(absolute_path);
        r->eax = -1;
        return;
    }

    strncpy(current_task->cwd, absolute_path, PATH_LEN_MAX - 1);
    current_task->cwd[PATH_LEN_MAX - 1] = '\0';
    kfree(absolute_path);
    r->eax = 0;
}


//ebx = char *buf, ecx = uint32_t max_len
void systemcall_get_current_work_dir(struct regs *r)
{
    char *user_buf = (char *)r->ebx;
    uint32_t max_len = r->ecx;
    if (!user_buf || max_len == 0)
    {
        r->eax = -1;
        return;
    }
    //Безопасно копируем CWD из дескриптора задачи на стек Ring 3 процесса
    uint32_t len = strlen(current_task->cwd);
    if (len >= max_len) len = max_len - 1;
    for (uint32_t i = 0; i < len; i++)
    {
        user_buf[i] = current_task->cwd[i];
    }
    user_buf[len] = '\0';
    r->eax = 0;
}


void systemcall_file_tell(struct regs *r)
{
    int file_d = (int)r->ebx;
    int global_fd = current_task->fds[file_d];
    if (global_fd < 0)
    {
        r->eax = -1;
        return;
    }
    r->eax = vfs_tell(global_fd);
}


void systemcall_get_bootinfo_struct(struct regs *r)
{
    r->eax = 0;
}


void systemcall_kbd_hit(struct regs *r)
{
    if (kbd_driver_api && kbd_driver_api->has_chars)
    {
        r->eax = kbd_driver_api->has_chars(); //Вернет 1 или 0
    }
    else
    {
        r->eax = 0;
    }
}


void systemcall_framebuffer_clear_tile(struct regs *r)
{
    uint32_t x = r->ebx;
    uint32_t y = r->ecx;
    uint32_t color = r->edx;
    //Если драйвер фреймбуфера загружен — вызываем его функцию fb_clear_tile, восстанавливающую кусок обоев
    if (fb && fb->clear_tile)
    {
        fb->clear_tile(x, y, color);
    }
}


void systemcall_get_system_ticks(struct regs *r)
{
    r->eax = tick_count;
}


void systemcall_system_halt(struct regs *r)
{
    //Пытаемся безопасно размонтировать VFS и проверить открытые дескрипторы
    int shutdown_res = vfs_shutdown_system();
    //Если ФС занята или драйвер отклонил umount — не выключаем ПК
    //Возвращаем код ошибки обратно пользовательской утилите
    if (shutdown_res != HALT_SUCCESS)
    {
        r->eax = shutdown_res;
        return;
    }
    // Теперь имеем полное право намертво заблокировать планировщик
    kernel_halt_all();
    for(;;);
}



void systemcall_dup2(struct regs *r)
{
    int oldfd = (int)r->ebx;
    int newfd = (int)r->ecx;
    if (oldfd < 0 || oldfd >= MAX_PROCESS_FDS ||
            newfd < 0 || newfd >= MAX_PROCESS_FDS)
    {
        r->eax = -1;
        return;
    }
    if (oldfd == newfd)
    {
        r->eax = newfd;
        return;
    }

    int global_old_fd = current_task->fds[oldfd];
    if (global_old_fd < 0)
    {
        r->eax = -1;
        return;
    }

    if (current_task->fds[newfd] >= 0)
    {
        int global_new_fd = current_task->fds[newfd];
        if (global_new_fd > 2)
        {
            int refs = 0;
            for (int i = 0; i < MAX_PROCESS_FDS; i++)
            {
                if (i != newfd && current_task->fds[i] == global_new_fd) refs++;
            }
            // Закрываем только если это последняя ссылка
            if (refs == 0) vfs_close(global_new_fd);
        }
    }
    current_task->fds[newfd] = global_old_fd;
    r->eax = newfd;
}


void systemcall_kbd_set_mode(struct regs *r)
{
    if (r->ebx != 0)
    {
        current_task->tty_flags |= TTY_KBD_RAW;
    }
    else
    {
        current_task->tty_flags &= ~TTY_KBD_RAW;
    }
    kbd_translate_reset();
    r->eax = 0;
}

void systemcall_tty_get_flags(struct regs *r)
{
    r->eax = current_task->tty_flags;
}

void systemcall_tty_set_flags(struct regs *r)
{
    // ebx = new_flags, ecx = mask (0 = писать все биты)
    uint32_t new_flags = r->ebx;
    uint32_t mask      = r->ecx;
    if (mask == 0) mask = 0xFFFFFFFF;

    current_task->tty_flags = (current_task->tty_flags & ~mask)
                              | (new_flags & mask);
    kbd_translate_reset();
    r->eax = 0;
}


//Получить индекс активной раскладки в пуле локалей, 0 = первая зарегистрированная (en),
//1 = вторая (ru), и т. д.
//Возвращает -1, если пул пуст или что-то пошло не так
void systemcall_locale_get(struct regs *r)
{
    const locale_t *active = locale_get_active();
    if (!active)
    {
        r->eax = -1;
        return;
    }

    for (uint8_t i = 0; i < LOCALE_POOL_MAX; i++)
    {
        if (locale_get_by_index(i) == active)
        {
            r->eax = i;
            return;
        }
    }
    r->eax = -1;
}


//SYS_LOCALE_GET_KEYMAP (ebx = locale_keymap_t *user_buf)
void systemcall_locale_get_keymap(struct regs *r)
{
    locale_keymap_t *user_buf = (locale_keymap_t *)r->ebx;
    if (!user_buf)
    {
        r->eax = -1;
        return;
    }
    //Та же защита, что в draw_string: не даём писать в ядерное пространство
    if ((uint32_t)user_buf < 0x40000000)
    {
        r->eax = -1;
        return;
    }

    const locale_t *active = locale_get_active();
    if (!active)
    {
        r->eax = -1;
        return;
    }

    //Собираем таблицу в локальный буфер ядра (1 КБ на стеке — многовато,
    //лучше через kmalloc, чтобы не рисковать переполнением kernel stack)
    locale_keymap_t *kmap = (locale_keymap_t *)kmalloc(sizeof(locale_keymap_t));
    if (!kmap)
    {
        r->eax = -1;
        return;
    }

    for (int sc = 0; sc < 128; sc++)
    {
        kmap->normal[sc] = locale_resolve_key(active, (uint8_t)sc, false);
        kmap->shift [sc] = locale_resolve_key(active, (uint8_t)sc, true );
    }

    memcpy(user_buf, kmap, sizeof(locale_keymap_t));
    kfree(kmap);
    r->eax = 0;
}


//SYS_GET_CONSOLE_GEOMETRY
//Возвращает геометрию текстовой сетки консоли:
//eax = cols, ebx = rows, ecx = cell_w, edx = cell_h
void systemcall_get_console_geometry(struct regs *r)
{
    if (!current_tty)
    {
        r->eax = 0;
        r->ebx = 0;
        r->ecx = 0;
        r->edx = 0;
        return;
    }
    const locale_t *loc = locale_get_by_index(current_tty->active_layout);
    if (!loc || !loc->font)
    {
        //Фоллбек на заведомо безопасный 8×8
        r->eax = current_tty->console_cols;
        r->ebx = current_tty->console_rows;
        r->ecx = 8;
        r->edx = 8;
        return;
    }
    r->eax = current_tty->console_cols;
    r->ebx = current_tty->console_rows;
    r->ecx = loc->font->cell_width;
    r->edx = loc->font->cell_height;
}


//SYS_SCROLL_RECT
//   ebx = x  (пиксели)
//   ecx = y
//   edx = w
//   esi = h
//   edi = dy (знаковое смещение: >0 — вверх, <0 — вниз)
//Полоса, освободившаяся после сдвига, заливается текущим фоном fb-драйвера
//(тот же цвет, что console_bgcolor; редактору достаточно выставить его через
//ANSI-последовательность или через прямой вызов set_bg до начала работы)
void systemcall_scroll_rect(struct regs *r)
{
    if (!fb || !fb->scroll_rect || !fb->get_bg)
    {
        r->eax = -1;
        return;
    }
    int32_t x  = (int32_t)r->ebx;
    int32_t y  = (int32_t)r->ecx;
    int32_t w  = (int32_t)r->edx;
    int32_t h  = (int32_t)r->esi;
    int32_t dy = (int32_t)r->edi;
    if (x < 0 || y < 0 || w <= 0 || h <= 0)
    {
        r->eax = -1;
        return;
    }
    //границы берем из активного режима слоя, а не из bootinfo: после
    //смены разрешения bootinfo описывает несуществующий режим и валидные
    //скроллы отбрасывались бы как "выходящие за экран"
    uint32_t scr_w = 0, scr_h = 0;
    gr_sys_screen_dims(&scr_w, &scr_h);
    if (x + w > (int32_t)scr_w)
    {
        r->eax = -1;
        return;
    }
    if (y + h > (int32_t)scr_h)
    {
        r->eax = -1;
        return;
    }
    if (dy == 0)
    {
        r->eax = 0;
        return;
    }
    uint32_t fill = fb->get_bg();
    r->eax = fb->scroll_rect(x, y, w, h, dy, fill);
}



//SYS_FB_BLIT
//   ebx = x  (пиксели)
//   ecx = y
//   edx = w
//   esi = h
//   edi = const void *src  (буфер в пространстве пользователя,
//пиксели в формате текущего bpp, row-pitch = w * bytes_pp)
void systemcall_fb_blit(struct regs *r)
{
    if (!fb || !fb->blit)
    {
        r->eax = -1;
        return;
    }
    int32_t x = (int32_t)r->ebx;
    int32_t y = (int32_t)r->ecx;
    int32_t w = (int32_t)r->edx;
    int32_t h = (int32_t)r->esi;
    const void *src = (const void *)r->edi;
    if (!src)
    {
        r->eax = -1;
        return;
    }
    //Та же защита от записи в ядерное пространство, что в draw_string
    if ((uint32_t)src < 0x40000000)
    {
        r->eax = -1;
        return;
    }
    if (x < 0 || y < 0 || w <= 0 || h <= 0)
    {
        r->eax = -1;
        return;
    }
    //границы из активного режима слоя: после смены разрешения blit по
    //bootinfo отбрасывал валидные прямоугольники (графика "не видна совсем")
    //или пропускал запись за пределы реального экрана
    uint32_t scr_w = 0, scr_h = 0;
    gr_sys_screen_dims(&scr_w, &scr_h);
    if (x + w > (int32_t)scr_w)
    {
        r->eax = -1;
        return;
    }
    if (y + h > (int32_t)scr_h)
    {
        r->eax = -1;
        return;
    }
    r->eax = fb->blit(x, y, w, h, src);
}


void systemcall_clipboard(struct regs *r)
{
    uint32_t op   = r->ebx;
    uint8_t *buf  = (uint8_t *)r->ecx;
    uint32_t size = r->edx;

    //защита от user-указателей ниже user-base (кроме GET_SIZE)
    if (op != CLIP_OP_GET_SIZE && buf && (uint32_t)buf < 0x40000000)
    {
        r->eax = -1;
        return;
    }

    switch (op)
    {
    case CLIP_OP_GET_SIZE:
        r->eax = kernel_clipboard_get(NULL, 0);
        break;
    case CLIP_OP_GET:
        r->eax = kernel_clipboard_get(buf, size);
        break;
    case CLIP_OP_SET:
        kernel_clipboard_set(buf, size);
        r->eax = 0;
        break;
    case CLIP_OP_CLEAR:
        kernel_clipboard_clear();
        r->eax = 0;
        break;
    default:
        r->eax = -1;
    }
}


void systemcall_usb_count(struct regs *r)
{
    r->eax = usb_registry_count();
}



void systemcall_usb_get_dev(struct regs *r)
{
    int idx = (int)r->ebx;
    u_usb_dev_t *user_out = (u_usb_dev_t *)r->ecx;

    if (!user_out || (uint32_t)user_out < 0x40000000)
    {
        r->eax = -1;
        return;
    }

    usb_dev_record_t rec;
    if (usb_registry_get(idx, &rec) != 0)
    {
        r->eax = -1;
        return;
    }

    user_out->vid          = rec.vid;
    user_out->pid          = rec.pid;
    user_out->dev_class    = rec.dev_class;
    user_out->dev_subclass = rec.dev_subclass;
    user_out->dev_protocol = rec.dev_protocol;
    user_out->port         = rec.port;
    user_out->block_id     = rec.block_id;

    for (int i = 0; i < USB_DEV_NAME_MAX;   i++) user_out->manufacturer[i] = rec.manufacturer[i];
    for (int i = 0; i < USB_DEV_NAME_MAX;   i++) user_out->product[i]      = rec.product[i];
    for (int i = 0; i < USB_DEV_SERIAL_MAX; i++) user_out->serial[i]       = rec.serial[i];

    r->eax = 0;
}



// ---- PCI ----
//   SYS_PCI_COUNT: eax = число устройств (при первом вызове ядро сканирует шины)
//   SYS_PCI_GET_DEV: ebx = idx, ecx = u_pci_dev_t*; -> 0/-1
//   SYS_PCI_READ_CFG: ebx = PCI_BDF(bus,dev,func), ecx = offset (0..255);
//                     -> eax = dword из конфиг-пространства или -1.
//                     Читаются только устройства из реестра — userspace не может
//                     тыкать произвольные конфиг-циклы в живую шину

void systemcall_pci_count(struct regs *r){
    r->eax = (uint32_t)pci_scan_all();
}

void systemcall_pci_get_dev(struct regs *r){
    int idx = (int)r->ebx;
    u_pci_dev_t *user_out = (u_pci_dev_t *)r->ecx;

    if (!user_out || (uint32_t)user_out < 0x40000000){
        r->eax = (uint32_t)-1; return;
    }

    pci_dev_record_t rec;
    if (pci_registry_get(idx, &rec) != 0){
        r->eax = (uint32_t)-1; return;
    }

    *user_out = rec;
    r->eax = 0;
}

void systemcall_pci_read_cfg(struct regs *r){
    uint32_t bdf    = r->ebx;
    uint32_t offset = r->ecx & 0xFFFF;

    uint32_t value = 0;
    if (pci_registry_read_cfg(bdf, (uint8_t)offset, &value) != 0){
        r->eax = (uint32_t)-1;
        return;
    }
    r->eax = value;
}

void systemcall_get_tty_cursor_pos(struct regs *r)
{
    if (!current_tty)
    {
        r->eax = 0;
        r->ebx = 0;
        return;
    }
    r->eax = (uint32_t)(current_tty->cursor_x / current_tty->cell_w);
    r->ebx = (uint32_t)(current_tty->cursor_y / current_tty->cell_h);
}


//SYS_TTY_LINE_EDIT — управление курсором редактора строки из Ring-3.
//Ring-3 шелл (Сехмет) сам рисует строку ввода, а мигающий курсор рисует ядро.
//ebx = 0 — спрятать курсор (строка завершена, идет вывод/выполнение команды)
//ebx = 1 — включить: ecx = якорь строки (младшие 16 бит — колонка, старшие — строка),
//          edx = смещение курсора от якоря в ячейках.
//          Ядро мигает ровно в этой ячейке, переживая переносы и скролл.
//ebx = 2 — задать глиф курсора: ecx = кодпойнт (например 0xB2 из config.cfg)
void systemcall_tty_line_edit(struct regs *r)
{
    if (!current_tty)
    {
        r->eax = -1;
        return;
    }

    if (r->ebx == 0)
    {
        //Строка завершена: стираем нарисованный глиф и отключаем мигание
        kconsole_line_cursor_hide();
        current_tty->line_edit_active = false;
    }
    else if (r->ebx == 1)
    {
        //Сначала стираем глиф со СТАРОЙ ячейки (иначе при сдвиге курсора
        //влево/HOME/END в старой ячейке остается блочный след),
        //затем обновляем якорь/смещение и рисуем курсор на новом месте
        kconsole_line_cursor_hide();
        current_tty->line_anchor_x = (r->ecx & 0xFFFFu) * current_tty->cell_w;
        current_tty->line_anchor_y = (r->ecx >> 16) & 0xFFFFu;
        current_tty->line_cursor_off = r->edx;
        current_tty->line_edit_active = true;
        //Курсор виден сразу после изменения строки и начинает мигать заново
        current_tty->cursor_visible = true;
        current_tty->last_blink_tick = tick_count;
        kconsole_line_cursor_show();
    }
    else if (r->ebx == 2)
    {
        //Глиф курсора. Из config.cfg Сехмета приходит CP866-код
        //(cursor_code=0xB1 и т.п.), а шрифт индексируется Unicode-кодпойнтами:
        //конвертируем честно, чтобы 0xB1 рисовался как ▒, 0x23 — как '#',
        //0xDB — как █ и т.д. для ВСЕХ 256 кодов. Значения выше 0xFF
        //считаются уже готовым кодпойнтом (обратная совместимость)
        uint32_t new_code = cp866_byte_to_codepoint(r->ecx);
        if (new_code != current_tty->cursor_code)
        {
            //Если глиф сейчас нарисован — сразу перерисовываем с новым кодом,
            //чтобы смена курсора была видна, не дожидаясь следующего мигания
            bool was_drawn = current_tty->line_cursor_drawn;
            if (was_drawn) kconsole_line_cursor_hide();
            current_tty->cursor_code = new_code;
            if (was_drawn)
            {
                if (current_tty->line_edit_active)
                {
                    uint32_t cx, cy;
                    kconsole_line_cursor_cell(&cx, &cy);
                    kconsole_line_cursor_draw_at(cx, cy, true);
                }
                else if (current_tty->kernel_line_active)
                {
                    int32_t row = current_tty->kernel_cursor_row;
                    int32_t col = current_tty->kernel_cursor_col;
                    if (row < 0) row = 0;
                    if (col < 0) col = 0;
                    kconsole_line_cursor_draw_at((uint32_t)col, (uint32_t)row, true);
                }
            }
        }
    }

    r->eax = 0;
}



void systemcall_get_mount_count(struct regs *r)
{
    r->eax = vfs_get_mount_count();
}

//ebx = int idx, ecx = u_mount_info_t *user_out
void systemcall_get_mount_info(struct regs *r)
{
    int idx = (int)r->ebx;
    u_mount_info_t *user_out = (u_mount_info_t *)r->ecx;
    if (!user_out || (uint32_t)user_out < 0x40000000)
    {
        r->eax = -1;
        return;
    }

    char k_mp[256];
    uint32_t k_drive = 0;
    const char *k_fsname = "unknown";
    fs_statfs_t k_st;
    memset(&k_st, 0, sizeof(k_st));

    int res = vfs_get_mount_info(idx, k_mp, sizeof(k_mp), &k_drive, &k_fsname, &k_st);
    if (res != FS_OK && res != FS_NOT_SUPPORTED)
    {
        r->eax = res;
        return;
    }

    memset(user_out, 0, sizeof(*user_out));
    user_out->drive_id        = k_drive;
    user_out->statfs_supported= (res == FS_OK) ? 1 : 0;
    for (int i = 0; i < 255; i++)
    {
        user_out->mount_point[i] = k_mp[i];
        if (!k_mp[i]) break;
    }
    user_out->mount_point[255] = '\0';
    for (int i = 0; i < 15; i++)
    {
        user_out->fs_name[i] = k_fsname[i];
        if (!k_fsname[i]) break;
    }
    user_out->fs_name[15] = '\0';

    if (res == FS_OK)
    {
        user_out->block_size   = k_st.block_size;
        user_out->total_blocks = k_st.total_blocks;
        user_out->free_blocks  = k_st.free_blocks;
        user_out->total_inodes = k_st.total_inodes;
        user_out->free_inodes  = k_st.free_inodes;
    }
    r->eax = 0;
}


//Стражи совпадения layout пользовательских зеркал ugraph.h со структурами
//графического слоя: маршалинг в systemcall_graph_info идет побайтово
_Static_assert(sizeof(u_gr_mode_info_t) == sizeof(gr_mode_info_t),
               "u_gr_mode_info_t layout mismatch");
_Static_assert(sizeof(u_gr_fix_info_t) == sizeof(gr_fix_info_t),
               "u_gr_fix_info_t layout mismatch");

//SYS_GRAPH_INFO: информация об активном видеодрайвере графического слоя.
//ebx = op-код GRAPH_INFO_*; ecx = указатель Ring 3 (для GRAPH_INFO_MODE
//ecx = индекс режима, а указатель лежит в edx; для GRAPH_INFO_SET_MODE
//ecx = индекс целевого режима). Возврат: eax = 0/-1, для SET_MODE —
//отрицательный код GR_ERR_*.
//Источник данных — активный драйвер реестра graphic_layer.c: для
//fbdrv-fallback это boot-режим из bootinfo, для vesa_fb — вся таблица
//VBE-режимов, собранная загрузчиком, с возможной сменой режима на ходу
void systemcall_graph_info(struct regs *r)
{
    uint32_t op = r->ebx;

    uint32_t user_ptr = (op == GRAPH_INFO_MODE) ? r->edx : r->ecx;

    //Указатель Ring 3 обязан лежать в пользовательском диапазоне VA
    //(процессы с 0x40000000, ядро с 0xC0000000). SET_MODE указателя не
    //передает: в ecx лежит индекс целевого режима, проверка диапазона
    //к нему не применяется
    if (op != GRAPH_INFO_SET_MODE &&
        (user_ptr < 0x40000000 || user_ptr >= 0xC0000000))
    {
        r->eax = -1;
        return;
    }

    switch (op)
    {
        //Карточка активного драйвера
        case GRAPH_INFO_DRIVER:
        {
            gr_driver_t *drv = graphic_layer_get_active();
            u_gr_driver_info_t *out = (u_gr_driver_info_t *)user_ptr;
            if (!drv)
            {
                r->eax = -1;
                return;
            }
            memset(out, 0, sizeof(u_gr_driver_info_t));
            out->abi_version  = drv->version;
            out->priority     = drv->priority;
            out->current_mode = drv->current_mode;
            out->mode_count   = (uint32_t)gr_get_mode_count();
            out->driver_count = graphic_layer_get_driver_count();
            for (uint32_t i = 0; i < sizeof(out->name); i++)
                out->name[i] = drv->name[i];
            for (uint32_t i = 0; i < sizeof(out->longname); i++)
                out->longname[i] = drv->longname[i];
            r->eax = 0;
            break;
        }

        //Режим по индексу из списка драйвера
        case GRAPH_INFO_MODE:
        {
            gr_mode_info_t mode;
            if (gr_enum_mode(r->ecx, &mode) != GR_OK)
            {
                r->eax = -1;
                return;
            }
            memcpy((void *)user_ptr, &mode, sizeof(gr_mode_info_t));
            r->eax = 0;
            break;
        }

        //Текущий режим (кэш слоя, валидируется при регистрации и смене)
        case GRAPH_INFO_CURRENT:
        {
            gr_mode_info_t mode;
            if (gr_get_current_mode(&mode) != GR_OK)
            {
                r->eax = -1;
                return;
            }
            memcpy((void *)user_ptr, &mode, sizeof(gr_mode_info_t));
            r->eax = 0;
            break;
        }

        //Неизменяемая часть: имя, размеры видеопамяти, визуальный класс
        case GRAPH_INFO_FIX:
        {
            gr_fix_info_t fix;
            if (gr_get_fix_info(&fix) != GR_OK)
            {
                r->eax = -1;
                return;
            }
            memcpy((void *)user_ptr, &fix, sizeof(gr_fix_info_t));
            r->eax = 0;
            break;
        }

        //Смена режима активного драйвера на ходу: ecx = индекс режима
        //(0 .. mode_count-1). Указателя нет — операция только переключает
        //режим. Возврат не -1, а сам отрицательный код GR_ERR_*: Ring 3
        //может отличить "нет такого режима" (-9) от "драйвер не умеет
        //менять режим" (-3, fbdrv-fallback) и "ошибка ввода-вывода" (-7)
        case GRAPH_INFO_SET_MODE:
        {
            //трасса: syscall дошел до ядра с нужным индексом
            com_puts("[GINFO] set_mode idx=");
            com_puthex(r->ecx);
            com_puts("\n");
            //смена режима — атомарная секция ядра: таймер не должен
            //вытеснять контекст задачи посреди gr_set_mode. внутри нее
            //ядро меняет глобальное состояние графического слоя, а
            //тримплин real mode (vbe_rm_exec) уходит в BIOS int 10h;
            //прилетевший посреди этого IRQ0 с schedule() переключает
            //CR3/esp, и контекст продолжения смены режима становится
            //мусором. держим блокировку до конца redraw консолей
            uint8_t saved_sched_lock = kernel_schedule_lock;
            kernel_schedule_lock = 1;

            int rc = gr_set_mode(r->ecx);
            kprintf("set %d\n",rc);
            if (rc != GR_OK)
            {
                kernel_schedule_lock = saved_sched_lock;
                r->eax = (uint32_t)rc;
                return;
            }
            //геометрия консолей привязана к разрешению: пересчитываем окна
            //TTY под новый режим и очищаем экран
            kconsole_mode_changed();
            kernel_schedule_lock = saved_sched_lock;
            r->eax = 0;
            break;
        }

        default:
            r->eax = -1;
            break;
    }
}


void syscalls_table_init(void)
{
    syscalls_table[SYS_FILL_SCREEN].func = &systemcall_fill_screen;
    syscalls_table[SYS_EXIT].func = &systemcall_exit;
    syscalls_table[SYS_YIELD].func = &systemcall_yield;
    syscalls_table[SYS_DRAW_STRING].func = &systemcall_draw_string;
    syscalls_table[SYS_DRAW_CHAR].func = &systemcall_draw_char;
    syscalls_table[SYS_GET_FB_INFO].func = &systemcall_get_fb_info;
    syscalls_table[SYS_ALLOC_PAGE].func = &systemcall_alloc_page;
    syscalls_table[SYS_FREE_PAGE].func = &systemcall_free_page;
    syscalls_table[SYS_OPEN].func = &systemcall_file_open;
    syscalls_table[SYS_READDIR].func = &systemcall_readdir;
    syscalls_table[SYS_CLOSE].func = &systemcall_file_close;
    syscalls_table[SYS_READ].func = &systemcall_file_read;
    syscalls_table[SYS_WRITE].func = &systemcall_file_write;
    syscalls_table[SYS_MKDIR].func = &systemcall_mkdir;
    syscalls_table[SYS_UNLINK].func = &systemcall_file_unlink;
    syscalls_table[SYS_RENAME].func = &systemcall_file_rename;
    syscalls_table[SYS_RMDIR].func = &systemcall_rmdir;
    syscalls_table[SYS_STAT].func = &systemcall_file_stat;
    syscalls_table[SYS_SEEK].func = &systemcall_file_seek;
    syscalls_table[SYS_CLEAR_SCREEN].func = &systemcall_clear_screen;
    syscalls_table[SYS_SPAWN].func = &systemcall_spawn_process;
    syscalls_table[SYS_WAIT_PID].func = &systemcall_wait_process_with_pid;
    syscalls_table[SYS_GET_MEM_INFO].func = &systemcall_get_memory_info;
    syscalls_table[SYS_SET_WALLPAPER].func = &systemcall_set_framebuffer_wallpaper;
    syscalls_table[SYS_SBRK].func = &systemcall_sbrk;
    syscalls_table[SYS_CHDIR].func = &systemcall_chdir;
    syscalls_table[SYS_GETCWD].func = &systemcall_get_current_work_dir;
    syscalls_table[SYS_TELL].func = &systemcall_file_tell;
    syscalls_table[SYS_GET_BOOTINFO].func = &systemcall_get_bootinfo_struct;
    syscalls_table[SYS_KBD_HIT].func = &systemcall_kbd_hit;
    syscalls_table[SYS_CLEAR_TILE].func = &systemcall_framebuffer_clear_tile;
    syscalls_table[SYS_GET_TICKS].func = &systemcall_get_system_ticks;
    syscalls_table[SYS_HALT_OS].func = &systemcall_system_halt;
    syscalls_table[SYS_DUP2].func = &systemcall_dup2;
    syscalls_table[SYS_ISATTY].func = &systemcall_isatty;
    syscalls_table[SYS_KBD_SET_MODE].func = &systemcall_kbd_set_mode;
    syscalls_table[SYS_TTY_GET_FLAGS].func = &systemcall_tty_get_flags;
    syscalls_table[SYS_TTY_SET_FLAGS].func = &systemcall_tty_set_flags;
    syscalls_table[SYS_LOCALE_GET].func = &systemcall_locale_get;
    syscalls_table[SYS_LOCALE_GET_KEYMAP].func = &systemcall_locale_get_keymap;
    syscalls_table[SYS_GET_CONSOLE_GEOMETRY].func = &systemcall_get_console_geometry;
    syscalls_table[SYS_SCROLL_RECT].func = &systemcall_scroll_rect;
    syscalls_table[SYS_FB_BLIT].func = &systemcall_fb_blit;
    syscalls_table[SYS_CLIPBOARD].func = &systemcall_clipboard;
    syscalls_table[SYS_USB_COUNT].func = &systemcall_usb_count;
    syscalls_table[SYS_USB_GET_DEV].func = &systemcall_usb_get_dev;
    syscalls_table[SYS_GET_CURSOR_POS].func = &systemcall_get_tty_cursor_pos;
    syscalls_table[SYS_TTY_LINE_EDIT].func = &systemcall_tty_line_edit;
    syscalls_table[SYS_GET_MOUNT_COUNT].func = &systemcall_get_mount_count;
    syscalls_table[SYS_GET_MOUNT_INFO].func = &systemcall_get_mount_info;
    syscalls_table[SYS_GRAPH_INFO].func = &systemcall_graph_info;
    syscalls_table[SYS_PCI_COUNT].func = &systemcall_pci_count;
    syscalls_table[SYS_PCI_GET_DEV].func = &systemcall_pci_get_dev;
    syscalls_table[SYS_PCI_READ_CFG].func = &systemcall_pci_read_cfg;
}
