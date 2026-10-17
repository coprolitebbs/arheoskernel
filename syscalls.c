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
#include "userspace/include/syscall.h"


// Состояние модификаторов клавиатуры в ядре
bool shift_pressed = false;

//таблица векторов системных вызовов
syscall_vectors_table_t syscalls_table[MAX_SYSCALLS]={0};

void syscall_handler(struct regs *r){

    if (r->eax < MAX_SYSCALLS && syscalls_table[r->eax].func){
        syscalls_table[r->eax].func(r);
    } else {
        r->eax = -1;//Неизвестный системный вызов
    }

}

uint32_t tty_get_copied_bytes(void){
    return current_tty ? current_tty->copied_bytes : 0;
}




void systemcall_fill_screen(struct regs *r){
    kernel_fill_screen(r->ebx);
}

void systemcall_exit(struct regs *r){
    current_task->state = TASK_TERMINATED;
    current_task->esp = 0;
    schedule(r);
    for(;;);
}

void systemcall_yield(struct regs *r){
    r->eax = 0;
    schedule(r);
}

void systemcall_draw_string(struct regs *r){
    uint32_t x = r->ebx;
    uint32_t y = r->ecx;
    const char *user_str = (const char *)r->edx;
    uint32_t color = r->esi;
    if (!user_str) return;
    //Проверяем валидность указателя в пространстве пользователя.
    //Строка должна лежать в границах Userspace кучи/стека (выше 0x40000000)
    if ((uint32_t)user_str < 0x40000000){
        r->eax = -1;
        return;
    }
    //Вызываем UTF-8 обёртку отрисовки строк через ядро, которая безопасно перенаправит указатель в драйвер
    kernel_draw_string(x, y, user_str, color);
    r->eax = 0;
}

void systemcall_draw_char(struct regs *r){
    if(r->ebx >= boot_info->width || r->ecx >=boot_info->height) return;
    if(fb && fb->draw_codepoint){
        //fb->draw_char(r->ebx,r->ecx,(unsigned char)r->edx,r->esi);
        fb->draw_codepoint(r->ebx, r->ecx, r->edx, r->esi);
    }

}

void systemcall_get_fb_info(struct regs *r){
    r->eax = boot_info->pitch;
    r->ebx = boot_info->bpp;
    r->ecx = boot_info->width;
    r->edx = boot_info->height;
}


void systemcall_alloc_page(struct regs *r){
    uint32_t virt = current_task->heap_next;
    uint32_t phys = (uint32_t)pmm_alloc_page();
    if (phys == 0) {
        r->eax = 0;
        return;
    }
    //Блокируем планировщик прерываний таймера на время транзакции маппинга
    kernel_schedule_lock = 1;
    //Включаем каталог страниц текущего процесса в CR3, чтобы таблицы были видимы
    vmm_switch_directory(current_task->page_dir_phys);
    uint32_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    //Зануление физической страницы ОЗУ
    memset((void*)phys, 0, PAGE_SIZE);
    //Мапим страницу
    vmm_map_page(current_task->page_dir, virt, phys, flags);

    if(!vm_page_add(current_task, virt, phys, flags)){
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
    r->eax = virt; //Отдаем виртуальный адрес кучи в ghjwtcc Ring 3
}

//ebx = виртуальный адрес освобождаемой страницы (virt)
void systemcall_free_page(struct regs *r){
    uint32_t virt = r->ebx;
    //Адрес должен быть выровнен по границе страницы 4КБ и принадлежать куче Ring 3
    if ((virt & 0xFFF) != 0 || virt < 0x50000000 || virt >= 0x7FFFF000){
        r->eax = -1;
        return;
    }
    //Получаем физический адрес страницы из каталога текущего таска
    uint32_t phys = vmm_get_phys_addr(current_task->page_dir, virt);
    if (phys != 0){
        //Убираем запись из менеджера страниц таска и освобождаем физ. память в PMM
        vm_page_remove(current_task, virt);
        pmm_free_page((void*)(phys & 0xFFFFF000));
        //Уничтожаем маппинг в VMM таблицах страниц
        vmm_unmap_page(current_task->page_dir, virt);
        r->eax = 0; //Успешно
    } else {
        r->eax = -1; //Страница не была замаплена
    }
}

//SYS_OPEN (ebx = const char *path, ecx = flags_o)
void systemcall_file_open(struct regs *r){
    const char *user_path = (const char *)r->ebx;
    uint32_t flags_o = r->ecx;
    if (!user_path){
        r->eax = -1;
        return;
    }
    //Ищем свободный локальный слот внутри текущей задачи (начиная со слота 3)
    int local_fd = -1;
    for (int i = 3; i < MAX_PROCESS_FDS; i++){
        if (current_task->fds[i] == -1){
            local_fd = i;
            break;
        }
    }
    if (local_fd == -1){
        r->eax = -1;
        return;
    }
    //Выделяем память в куче под временный буфер пути Ring 3
    char *kernel_path = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path){
        r->eax = -1;
        return;
    }
    memset(kernel_path, 0, VFS_PATH_MAX);
    //Маршалим строку из пространства пользователя в кучу ядра
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path[i] != '\0'; i++){
        kernel_path[i] = user_path[i];
    }
    //Выделяем память под итоговый абсолютный каноничный путь
    char *absolute_path = (char *)kmalloc(VFS_PATH_MAX);
    if (!absolute_path){
        kfree(kernel_path);
        r->eax = -1;
        return;
    }
    vfs_resolve_relative_path(kernel_path, absolute_path);
    kfree(kernel_path);
    //Открываем файл на уровне глобальной VFS ядра по честному абсолютному пути
    int global_vfs_fd = vfs_open(absolute_path, flags_o);
    kfree(absolute_path);
    if (global_vfs_fd < 0){
        r->eax = global_vfs_fd;
        return;
    }
    //Связываем дескрипторы и возвращаем локальный хэндл в Userspace Ring 3
    current_task->fds[local_fd] = global_vfs_fd;
    r->eax = local_fd;
}

//SYS_CLOSE (ebx = int file_d)
/*
void systemcall_file_close(struct regs *r){
    int file_d = (int)r->ebx;
    if (file_d < 0 || file_d >= MAX_PROCESS_FDS) {
        r->eax = -1;
        return;
    }
    //Извлекаем связанный глобальный VFS дескриптор
    int global_fd = current_task->fds[file_d];
    if (global_fd < 0) {
        r->eax = -1;
        return;
    }
    //Закрываем файл на уровне глобальной VFS и драйвера файловой системы
    int res = vfs_close(global_fd);
    //Если VFS успешно закрыла файл, освобождаем локальный слот процесса
    if (res == FS_OK){
        current_task->fds[file_d] = -1; //Сбрасываем обратно в 0xFFFFFFFF
    }
    r->eax = res;
}
*/
void systemcall_file_close(struct regs *r){
    int file_d = (int)r->ebx;
    if (file_d < 0 || file_d >= MAX_PROCESS_FDS) { r->eax = -1; return; }
    int global_fd = current_task->fds[file_d];
    if (global_fd < 0) { r->eax = -1; return; }

    // Считаем, есть ли в других слотах процесса ссылка на тот же global_fd
    int refs = 0;
    for (int i = 0; i < MAX_PROCESS_FDS; i++) {
        if (i != file_d && current_task->fds[i] == global_fd) refs++;
    }

    current_task->fds[file_d] = -1;

    // Реально закрываем только когда ссылок больше нет и это не std-поток
    if (refs == 0 && global_fd > 2) {
        r->eax = vfs_close(global_fd);
    } else {
        r->eax = FS_OK;
    }
}

//SYS_READDIR (ebx = file_d, ecx = fs_dirent_t*)
void systemcall_readdir(struct regs *r){
    int file_d = (int)r->ebx;
    fs_dirent_t *user_entry = (fs_dirent_t *)r->ecx;
    if (!user_entry || file_d < 0 || file_d >= MAX_PROCESS_FDS){
        r->eax = -1;
        return;
    }
    int global_fd = current_task->fds[file_d];
    if (global_fd < 0) { r->eax = -1; return; }

    fs_dirent_t kernel_entry;
    memset(&kernel_entry, 0, sizeof(fs_dirent_t));

    int vfs_res = vfs_readdir(global_fd, &kernel_entry);
    if (vfs_res != FS_OK){
        r->eax = vfs_res;
        return;
    }
    user_entry->inode = kernel_entry.inode;
    user_entry->type = kernel_entry.type;
    user_entry->attributes = kernel_entry.attributes;
    user_entry->size       = kernel_entry.size;
    user_entry->id         = kernel_entry.id;

    int i = 0;
    for (i = 0; i < 255; i++){
        user_entry->name[i] = kernel_entry.name[i];
        if (kernel_entry.name[i] == '\0') {
            break;
        }
    }
    user_entry->name[i] = '\0';
    r->eax = FS_OK;
}

void systemcall_file_read(struct regs *r){
    //Непрерывные UTF-8 байты, которые не удалось отдать за один SYS_READ
    //Хранятся для следующего вызова SYS_READ от того же task'а (cooked STDIN)
    static uint8_t  cooked_pending_buf[4];
    static int      cooked_pending_len = 0;
    static int      cooked_pending_pos = 0;

    int file_d = (int)r->ebx;
    void *user_buf = (void *)r->ecx;
    uint32_t count = r->edx;

    if (!user_buf || count == 0){ r->eax = -1; return; }

    int global_fd = current_task->fds[file_d];
    if (global_fd < 0){ r->eax = -1; return; }

    if (global_fd == 0){
        if (!kbd_driver_api){ r->eax = -1; return; }

        // ---- RAW-режим (без изменений) ----
        if (current_task->tty_flags & TTY_KBD_RAW){
            if (kbd_driver_api->has_chars()){
                uint32_t packed = kbd_driver_api->get_char_blocked();
                if (count >= 4)      { *(uint32_t*)user_buf = packed; r->eax = 4; }
                else if (count >= 1) { *(uint8_t*)user_buf  = (uint8_t)(packed & 0xFF); r->eax = 1; }
                else                 { r->eax = 0; }
            } else {
                r->eax = 0;
            }
            return;
        }
        // ---- COOKED: сначала доставляем то, что осталось с прошлого раза ----
        if (cooked_pending_pos < cooked_pending_len){
            uint32_t avail = (uint32_t)(cooked_pending_len - cooked_pending_pos);
            uint32_t to_copy = (count < avail) ? count : avail;
            for (uint32_t i = 0; i < to_copy; i++)
                ((uint8_t*)user_buf)[i] = cooked_pending_buf[cooked_pending_pos + i];
            cooked_pending_pos += (int)to_copy;
            if (cooked_pending_pos >= cooked_pending_len){
                cooked_pending_pos = 0;
                cooked_pending_len = 0;
            }
            r->eax = (int32_t)to_copy;
            return;
        }

        // ---- COOKED: иначе берём новое событие из клавиатуры ----
        while (kbd_driver_api->has_chars()){
            uint32_t packed = kbd_driver_api->get_char_blocked();
            uint32_t cp = kbd_translate_event(packed);
            if (cp == 0) continue;   // модификатор — проглотили

            // Кодируем codepoint в UTF-8
            uint8_t utf8[4];
            int n = 0;
            if (cp < 0x80)        { utf8[n++] = (uint8_t)cp; }
            else if (cp < 0x800)  { utf8[n++] = 0xC0 | (cp >> 6); utf8[n++] = 0x80 | (cp & 0x3F); }
            else if (cp < 0x10000){ utf8[n++] = 0xE0 | (cp >> 12); utf8[n++] = 0x80 | ((cp >> 6) & 0x3F);
                                utf8[n++] = 0x80 | (cp & 0x3F); }
            else                  { utf8[n++] = 0xF0 | (cp >> 18);
                                utf8[n++] = 0x80 | ((cp >> 12) & 0x3F);
                                utf8[n++] = 0x80 | ((cp >> 6)  & 0x3F);
                                utf8[n++] = 0x80 | (cp & 0x3F); }

            // Эхо — печатаем ВЕСЬ codepoint сейчас, целиком, а не частями
            if (current_task->tty_flags & TTY_ECHO){
                for (int i = 0; i < n; i++)
                    kernel_stdout_write_char((char)utf8[i], console_fgcolor);
            }

            // Сколько сможем отдать сразу
            uint32_t to_copy = (count < (uint32_t)n) ? count : (uint32_t)n;
            for (uint32_t i = 0; i < to_copy; i++)
                ((uint8_t*)user_buf)[i] = utf8[i];

            // Остаток кладём в pending для следующего SYS_READ
            if (to_copy < (uint32_t)n){
                for (int i = 0; i < n - (int)to_copy; i++)
                    cooked_pending_buf[i] = utf8[to_copy + i];
                cooked_pending_len = n - (int)to_copy;
                cooked_pending_pos = 0;
            }

            r->eax = (int32_t)to_copy;
            return;
        }

        r->eax = 0;   // печатных данных пока нет
        return;
    }

    //Чтение обычных дисковых файлов (VFS)
    if (count > 4096) count = 4096;
    uint8_t *kernel_buf = (uint8_t *)kmalloc(count);
    if (!kernel_buf) { r->eax = -1; return; }
    memset(kernel_buf, 0, count);

    // Читаем из честного глобального VFS описания
    int bytes_read = vfs_read(global_fd, kernel_buf, count);
    if (bytes_read > 0) memcpy(user_buf, kernel_buf, bytes_read);
    kfree(kernel_buf);
    r->eax = bytes_read;
}



//SYS_WRITE (ebx = fd, ecx = буфер const void*, edx = count)
/*
void systemcall_file_write(struct regs *r){
    int fd_wr = (int)r->ebx;
    void *user_buf_wr = (void *)r->ecx;
    char *user_src = (char *)r->ecx;
    uint32_t count_wr = r->edx;
    if (!user_buf_wr || count_wr == 0){
        r->eax = -1;
        return;
    }
    int global_fd = current_task->fds[fd_wr];
    if (global_fd < 0) { r->eax = -1; return; }
    //Локальные потоки вывода (STDOUT == 1, STDERR == 2)
    if (global_fd == 1 || global_fd == 2){
        uint32_t written_bytes = 0;
        while (written_bytes < count_wr){
            char c = user_src[written_bytes];
            kernel_stdout_write_char(c, console_fgcolor);
            written_bytes++;
        }
        r->eax = written_bytes;
        return;
    }
    //Запись в обычные дисковые файлы (VFS)
    if (count_wr > 512) count_wr = 512;
    uint8_t *kernel_buf_wr = (uint8_t *)kmalloc(count_wr);
    if (!kernel_buf_wr){
        r->eax = -1;
        return;
    }
    memcpy(kernel_buf_wr, user_buf_wr, count_wr);
    int bytes_written = vfs_write(global_fd, kernel_buf_wr, count_wr);
    kfree(kernel_buf_wr);
    r->eax = bytes_written;
}
*/
void systemcall_file_write(struct regs *r){
    int fd_wr = (int)r->ebx;
    const void *user_src = (const void *)r->ecx;
    uint32_t count_wr = r->edx;

    if (!user_src || count_wr == 0){
        r->eax = -1;
        return;
    }

    int global_fd = current_task->fds[fd_wr];
    if (global_fd < 0) {
        r->eax = -1;
        return;
    }

    if (global_fd == 1 || global_fd == 2){
        uint32_t written_bytes = 0;
        const char *src_chars = (const char *)user_src;
        while (written_bytes < count_wr){
            char c = src_chars[written_bytes];
            kernel_stdout_write_char(c, console_fgcolor);
            written_bytes++;
        }
        r->eax = written_bytes;
        return;
    }

    if (count_wr > 4096) count_wr = 4096;
    char *kernel_buf_wr = (char *)kmalloc(count_wr + 1);
    if (!kernel_buf_wr){
        r->eax = -1;
        return;
    }

    // БРОНИРОВАННОЕ ИСПРАВЛЕНИЕ: Копируем данные через нативный memcpy,
    // это гарантирует корректный маршалинг страниц Ring 3 текущего процесса!
    memcpy(kernel_buf_wr, user_src, count_wr);
    kernel_buf_wr[count_wr] = '\0';

    int bytes_written = vfs_write(global_fd, kernel_buf_wr, count_wr);
    kfree(kernel_buf_wr);

    r->eax = bytes_written;
}





//SYS_MKDIR (ebx = Путь const char *path)
void systemcall_mkdir(struct regs *r){
    const char *user_path_nm = (const char *)r->ebx;
    if (!user_path_nm){
        r->eax = -1;
        return;
    }
    //Динамическая аллокация в куче Ring 0 для защиты 4 КБ стека ядра
    char *kernel_path_nm = (char *)kmalloc(VFS_PATH_MAX);
    char *absolute_path_nm = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path_nm || !absolute_path_nm){
        if (kernel_path_nm) kfree(kernel_path_nm);
        if (absolute_path_nm) kfree(absolute_path_nm);
        r->eax = -1;
        return;
    }
    memset(kernel_path_nm, 0, VFS_PATH_MAX);
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path_nm[i] != '\0'; i++){
        kernel_path_nm[i] = user_path_nm[i];
    }
    //Преобразуем относительный путь в абсолютный на базе CWD активного процесса
    vfs_resolve_relative_path(kernel_path_nm, absolute_path_nm);
    kfree(kernel_path_nm);
    r->eax = vfs_mkdir(absolute_path_nm);
    kfree(absolute_path_nm);
}


void systemcall_file_unlink(struct regs *r){
    const char *user_path_un = (const char *)r->ebx;
    if (!user_path_un){
        r->eax = -1;
        return;
    }
    char *kernel_path_un = (char *)kmalloc(VFS_PATH_MAX);
    char *absolute_path_un = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path_un || !absolute_path_un){
        if (kernel_path_un) kfree(kernel_path_un);
        if (absolute_path_un) kfree(absolute_path_un);
        r->eax = -1;
        return;
    }
    memset(kernel_path_un, 0, VFS_PATH_MAX);
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path_un[i] != '\0'; i++){
        kernel_path_un[i] = user_path_un[i];
    }
    vfs_resolve_relative_path(kernel_path_un, absolute_path_un);
    kfree(kernel_path_un);
    //Передаем в VFS валидный абсолютный путь, начинающийся со слэша '/'
    r->eax = vfs_unlink(absolute_path_un);
    kfree(absolute_path_un);
}

//SYS_RMDIR (ebx = пользовательский путь const char *path)
void systemcall_rmdir(struct regs *r){
    const char *user_path_rm = (const char *)r->ebx;
    if (!user_path_rm){
        r->eax = -1;
        return;
    }
    char *kernel_path_rm = (char *)kmalloc(VFS_PATH_MAX);
    char *absolute_path_rm = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path_rm || !absolute_path_rm){
        if (kernel_path_rm) kfree(kernel_path_rm);
        if (absolute_path_rm) kfree(absolute_path_rm);
        r->eax = -1;
        return;
    }
    memset(kernel_path_rm, 0, VFS_PATH_MAX);
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path_rm[i] != '\0'; i++){
        kernel_path_rm[i] = user_path_rm[i];
    }
    //Разрешаем относительные пути и для удаления каталогов
    vfs_resolve_relative_path(kernel_path_rm, absolute_path_rm);
    kfree(kernel_path_rm);
    r->eax = vfs_rmdir(absolute_path_rm);
    kfree(absolute_path_rm);
}

//ebx = const char *path, ecx = fs_stat_t *st
void systemcall_file_stat(struct regs *r){
    const char *user_path = (const char *)r->ebx;
    fs_stat_t *user_st = (fs_stat_t *)r->ecx;
    if (!user_path || !user_st){
        r->eax = -1;
        return;
    }
    //Выделяем 4096-байтные буферы в куче ядра
    char *kernel_path = (char *)kmalloc(VFS_PATH_MAX);
    char *absolute_path = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path || !absolute_path){
        if (kernel_path) kfree(kernel_path);
        if (absolute_path) kfree(absolute_path);
        r->eax = -1;
        return;
    }
    //Маршалим имя файла из Ring 3 памяти в кучу ядра
    memset(kernel_path, 0, VFS_PATH_MAX);
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path[i] != '\0'; i++){
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
    if (vfs_res != VFS_STATUS_OK) {
        r->eax = vfs_res;
        return;
    }
    user_st->size        = kernel_st.size;
    user_st->attributes  = kernel_st.attributes;
    user_st->create_time = kernel_st.create_time; //Пробрасываем ctime из ext2
    user_st->modify_time = kernel_st.modify_time; //Пробрасываем mtime из ext2
    r->eax = VFS_STATUS_OK;
}


void systemcall_file_seek(struct regs *r){
    int file_d = (int)r->ebx;
    uint32_t position = r->ecx;
    int global_fd = current_task->fds[file_d];
    if (global_fd < 0) { r->eax = -1; return; }
    r->eax = vfs_seek(global_fd, position);
}

//Системный вызов очистки консоли TTY и фреймбуфера
void systemcall_clear_screen(struct regs *r){
    kconsole_clear_screen();
    r->eax = 0;
}


/*
void systemcall_spawn_process(struct regs *r){
    const char *user_path = (const char *)r->ebx;
    const char *user_cmdline = (const char *)r->ecx;
    if (!user_path){
        r->eax = -1;
        return;
    }
    //Маршалинг строк пути и командной строки из памяти Ring 3 в ядро
    char k_path[128];
    char k_cmdline[128];
    memset(k_path, 0, 128);
    memset(k_cmdline, 0, 128);
    for (int i = 0; i < 127 && user_path[i] != '\0'; i++) k_path[i] = user_path[i];
    if (user_cmdline) {
        for (int i = 0; i < 127 && user_cmdline[i] != '\0'; i++) k_cmdline[i] = user_cmdline[i];
    }
    //Вызываем функцию загрузки бинарника с диска
    task_t *new_task = task_spawn_from_file(k_path, 0x40000000, user_cmdline ? k_cmdline : NULL);
    if (new_task) {
        r->eax = new_task->pid; //Возвращаем PID созданного процесса
    } else {
        r->eax = -1;
    }
}
*/
void systemcall_spawn_process(struct regs *r){
    const char *user_path = (const char *)r->ebx;
    const char *user_cmdline = (const char *)r->ecx;
    if (!user_path){ r->eax = -1; return; }

    char k_path[128];
    char k_cmdline[256];
    memset(k_path, 0, 128);
    memset(k_cmdline, 0, 256);

    for (int i = 0; i < 127 && user_path[i] != '\0'; i++) k_path[i] = user_path[i];

    /*
    int probe_fd = vfs_open(k_path, FS_OPEN_READ);
    if (probe_fd < 0){ r->eax = -1; return; }

    uint8_t magic[4] = {0, 0, 0, 0};
    int probe_bytes = vfs_read(probe_fd, magic, 4);
    vfs_close(probe_fd);
    */

    if (user_cmdline){
        for (int i = 0; i < 255 && user_cmdline[i] != '\0'; i++) k_cmdline[i] = user_cmdline[i];
    }

    char out_redirect_file[128];
    char in_redirect_file[128];
    memset(out_redirect_file, 0, 128);
    memset(in_redirect_file, 0, 128);
    bool do_out_redirect = false;
    bool do_in_redirect  = false;

    for (int i = 0; k_cmdline[i] != '\0'; i++){
        if (k_cmdline[i] == '>' && (i == 0 || k_cmdline[i-1] == ' ')){
            do_out_redirect = true;
            k_cmdline[i] = '\0';

            int skip = i + 1;
            while (k_cmdline[skip] == ' ' || k_cmdline[skip] == '\t') skip++;

            int f_idx = 0;
            while (k_cmdline[skip] != '\0' && k_cmdline[skip] != ' ' && f_idx < 127){
                out_redirect_file[f_idx++] = k_cmdline[skip++];
            }
            out_redirect_file[f_idx] = '\0';
            break;
        }
        if (k_cmdline[i] == '<' && (i == 0 || k_cmdline[i-1] == ' ')){
            do_in_redirect = true;
            k_cmdline[i] = '\0';
            int skip = i + 1;
            while (k_cmdline[skip] == ' ' || k_cmdline[skip] == '\t') skip++;
            int f_idx = 0;
            while (k_cmdline[skip] != '\0' && k_cmdline[skip] != ' ' && f_idx < 127){
                in_redirect_file[f_idx++] = k_cmdline[skip++];
            }
            break;
        }
    }

    int end_space = strlen(k_cmdline) - 1;
    while (end_space >= 0 && (k_cmdline[end_space] == ' ' || k_cmdline[end_space] == '\t')){
        k_cmdline[end_space--] = '\0';
    }

    task_t *new_task = task_spawn_from_file(k_path, 0x40000000, k_cmdline[0] != '\0' ? k_cmdline : NULL);
    if (!new_task){ r->eax = -1; return; }

    if (do_out_redirect && out_redirect_file[0] != '\0'){
        // Резолвим относительный путь к абсолютному на базе CWD текущего процесса
        char *abs_redirect = (char *)kmalloc(VFS_PATH_MAX);
        if (abs_redirect){
            memset(abs_redirect, 0, VFS_PATH_MAX);
            vfs_resolve_relative_path(out_redirect_file, abs_redirect);

            int global_vfs_fd = vfs_open(abs_redirect,
                                     FS_OPEN_CREATE | FS_OPEN_WRITE | FS_OPEN_TRUNCATE);
            kfree(abs_redirect);

            if (global_vfs_fd >= 0){
                new_task->fds[1] = global_vfs_fd;
            }
        }
    }
    if (do_in_redirect && in_redirect_file[0] != '\0'){
        char *abs_redirect = (char *)kmalloc(VFS_PATH_MAX);
            if (abs_redirect){
            memset(abs_redirect, 0, VFS_PATH_MAX);
            vfs_resolve_relative_path(in_redirect_file, abs_redirect);
            int global_vfs_fd = vfs_open(abs_redirect, FS_OPEN_READ);
            kfree(abs_redirect);
            if (global_vfs_fd >= 0){
                new_task->fds[0] = global_vfs_fd;
            }
        }
    }
    r->eax = new_task->pid;
}




void systemcall_wait_process_with_pid(struct regs *r){
    uint32_t target_pid = r->ebx;
    r->eax = -1; //По умолчанию возвращаем ошибку (процесс не найден)
    if (ready_queue){
        task_t *start = ready_queue;
        task_t *t = start;
        bool found = false;
        //Перебираем кольцевую очередь в поисках живого дочернего процесса
        do{
            if (t->pid == target_pid){
                found = true;
                break;
            }
            t = t->next;
        } while (t != start);
        //Если процесс найден в кольце и он еще выполняет свой код,
        //возвращаем статус PROCESS_IS_ACTIVE
        if (found && t->state != TASK_TERMINATED){
            r->eax = PROCESS_IS_ACTIVE;
            return;
        }
    }
    //Если процесс успешно завершил работу (сделал SYS_EXIT) и исчез из очереди
    r->eax = 0;
}


void systemcall_isatty(struct regs *r){
    int fd = (int)r->ebx;
    if (fd < 0 || fd >= MAX_PROCESS_FDS){ r->eax = 0; return; }
    int global_fd = current_task->fds[fd];
    // 0 = keyboard marker
    r->eax = (global_fd == 0) ? 1 : 0;
}

//ebx = am_mem_info_t *user_info
void systemcall_get_memory_info(struct regs *r){
    am_mem_info_t *user_info = (am_mem_info_t *)r->ebx;
    if (!user_info){
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
    for (uint32_t i = 0; i < sizeof(am_mem_info_t); i++){
        dst[i] = src[i];
    }
    r->eax = 0;
}


void systemcall_set_framebuffer_wallpaper(struct regs *r){
    const void *user_buf = (const void *)r->ebx;
    uint32_t size = r->ecx;
    if (!user_buf || size == 0){
        r->eax = -1;
        return;
    }
    if (fb && fb->set_wallpaper){
        kernel_schedule_lock = 1;
        fb->set_wallpaper(user_buf, size);
        kernel_schedule_lock = 0;
        r->eax = 0;
    } else {
        r->eax = -1;
    }
}

//ebx = int32_t increment (приращение кучи в байтах)
void systemcall_sbrk(struct regs *r){
    int32_t increment = (int32_t)r->ebx;
    uint32_t old_heap_next = current_task->heap_next;
    if (increment <= 0){
        r->eax = old_heap_next;
        return;
    }
    //Вычисляем, сколько страниц ОЗУ нужно выделить под этот запрос
    uint32_t pages_needed = (increment + PAGE_SIZE - 1) / PAGE_SIZE;
    //Защитный барьер кучи Ring 3 (ограничиваем 16 МБ до границы стека)
    if (current_task->heap_next + (pages_needed * PAGE_SIZE) >= 0x7FFFF000) {
        r->eax = 0;
        return;
    }
    //Блокируем планировщик таймера на время массового маппинга мегабайтов
    kernel_schedule_lock = 1;
    //Принудительно загружаем физический каталог страниц текущего процесса в CR3
    vmm_switch_directory(current_task->page_dir_phys);
    bool alloc_success = true;
    for (uint32_t i = 0; i < pages_needed; i++){
        uint32_t virt = current_task->heap_next;
        uint32_t phys = 0;
        //Такой же Memory Shield для защищённых sbrk массивов
        for (int attempt = 0; attempt < 1024; attempt++){
            phys = (uint32_t)pmm_alloc_page();
            if (!phys) break;
            uint32_t kstack_page = current_task->kernel_stack_phys;
            if (phys >= 0x00100000 && phys < 0x20000000 && phys != kstack_page){
                        break;
            }
            phys = 0;
        }
        if (phys == 0) {
            alloc_success = false;
            break;
        }
        uint32_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        vmm_map_page(current_task->page_dir, virt, phys, flags);
        //Зануляем по виртуальному адресу кучи Ring 3
        memset((void*)virt, 0, PAGE_SIZE);
        if (!vm_page_add(current_task, virt, phys, flags)){
            vmm_unmap_page(current_task->page_dir, virt);
            pmm_free_page((void*)phys);
            alloc_success = false;
            break;
        }
        current_task->heap_next += PAGE_SIZE;
    }
    //Повторно перезагружаем CR3 каталогом текущего процесса для фиксации TLB-кэша
    vmm_switch_directory(current_task->page_dir_phys);
    //Разблокируем кооперативный планировщик ядра
    kernel_schedule_lock = 0;

    if (!alloc_success){
        r->eax = 0;
    } else {
        r->eax = old_heap_next; //Возвращаем стартовый адрес выделенного региона кучи
    }
}

// ebx = const char *path
void systemcall_chdir(struct regs *r){
    const char *user_path = (const char *)r->ebx;
    if (!user_path) { r->eax = -1; return; }

    char *kernel_path = (char *)kmalloc(VFS_PATH_MAX);
    char *absolute_path = (char *)kmalloc(VFS_PATH_MAX);
    if (!kernel_path || !absolute_path){
        if (kernel_path) kfree(kernel_path);
        if (absolute_path) kfree(absolute_path);
        r->eax = -1;
        return;
    }
    memset(kernel_path, 0, VFS_PATH_MAX);
    for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path[i] != '\0'; i++){
        kernel_path[i] = user_path[i];
    }
    vfs_resolve_relative_path(kernel_path, absolute_path);
    kfree(kernel_path);
    //Проверяем, что путь существует и является каталогом — БЕЗ открытия fd
    fs_stat_t st;
    memset(&st, 0, sizeof(st));
    int st_res = vfs_stat(absolute_path, &st);

    if (st_res != VFS_STATUS_OK || !(st.attributes & FS_ATTR_DIRECTORY)){
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
void systemcall_get_current_work_dir(struct regs *r){
    char *user_buf = (char *)r->ebx;
    uint32_t max_len = r->ecx;
    if (!user_buf || max_len == 0){
        r->eax = -1;
        return;
    }
    //Безопасно копируем CWD из дескриптора задачи на стек Ring 3 процесса
    uint32_t len = strlen(current_task->cwd);
    if (len >= max_len) len = max_len - 1;
    for (uint32_t i = 0; i < len; i++){
        user_buf[i] = current_task->cwd[i];
    }
    user_buf[len] = '\0';
    r->eax = 0;
}


void systemcall_file_tell(struct regs *r){
    int file_d = (int)r->ebx;
    int global_fd = current_task->fds[file_d];
    if (global_fd < 0) { r->eax = -1; return; }
    r->eax = vfs_tell(global_fd);
}


void systemcall_get_bootinfo_struct(struct regs *r){
    r->eax = 0;
}


void systemcall_kbd_hit(struct regs *r){
    if (kbd_driver_api && kbd_driver_api->has_chars){
        r->eax = kbd_driver_api->has_chars(); //Вернет 1 или 0
    } else {
        r->eax = 0;
    }
}


void systemcall_framebuffer_clear_tile(struct regs *r){
    uint32_t x = r->ebx;
    uint32_t y = r->ecx;
    uint32_t color = r->edx;
    //Если драйвер фреймбуфера загружен — вызываем его функцию fb_clear_tile, восстанавливающую кусок обоев
    if (fb && fb->clear_tile){
        fb->clear_tile(x, y, color);
    }
}


void systemcall_get_system_ticks(struct regs *r){
    r->eax = tick_count;
}


void systemcall_system_halt(struct regs *r){
    //Пытаемся безопасно размонтировать VFS и проверить открытые дескрипторы
    int shutdown_res = vfs_shutdown_system();
    //Если ФС занята или драйвер отклонил umount — не выключаем ПК
    //Возвращаем код ошибки обратно пользовательской утилите
    if (shutdown_res != HALT_SUCCESS){
        r->eax = shutdown_res;
        return;
    }
    // Теперь имеем полное право намертво заблокировать планировщик
    kernel_halt_all();
    for(;;);
}



void systemcall_dup2(struct regs *r){
    int oldfd = (int)r->ebx;
    int newfd = (int)r->ecx;
    if (oldfd < 0 || oldfd >= MAX_PROCESS_FDS ||
        newfd < 0 || newfd >= MAX_PROCESS_FDS){ r->eax = -1; return; }
    if (oldfd == newfd){ r->eax = newfd; return; }

    int global_old_fd = current_task->fds[oldfd];
    if (global_old_fd < 0){ r->eax = -1; return; }

    if (current_task->fds[newfd] >= 0) {
        int global_new_fd = current_task->fds[newfd];
        if (global_new_fd > 2) {
            int refs = 0;
            for (int i = 0; i < MAX_PROCESS_FDS; i++) {
                if (i != newfd && current_task->fds[i] == global_new_fd) refs++;
            }
            // Закрываем только если это последняя ссылка
            if (refs == 0) vfs_close(global_new_fd);
        }
    }
    current_task->fds[newfd] = global_old_fd;
    r->eax = newfd;
}


void systemcall_kbd_set_mode(struct regs *r){
    if (r->ebx != 0){
        current_task->tty_flags |= TTY_KBD_RAW;
    } else {
        current_task->tty_flags &= ~TTY_KBD_RAW;
    }
    kbd_translate_reset();
    r->eax = 0;
}

void systemcall_tty_get_flags(struct regs *r){
    r->eax = current_task->tty_flags;
}

void systemcall_tty_set_flags(struct regs *r){
    // ebx = new_flags, ecx = mask (0 = писать все биты)
    uint32_t new_flags = r->ebx;
    uint32_t mask      = r->ecx;
    if (mask == 0) mask = 0xFFFFFFFF;

    current_task->tty_flags = (current_task->tty_flags & ~mask)
                            | (new_flags & mask);
    kbd_translate_reset();
    r->eax = 0;
}


//Получить индекс активной раскладки в пуле локалей
//0 = первая зарегистрированная (en), 1 = вторая (ru), и т. д.
//Возвращает -1, если пул пуст или что-то пошло не так
void systemcall_locale_get(struct regs *r){
    const locale_t *active = locale_get_active();
    if (!active){ r->eax = -1; return; }

    for (uint8_t i = 0; i < LOCALE_POOL_MAX; i++){
        if (locale_get_by_index(i) == active){
            r->eax = i;
            return;
        }
    }
    r->eax = -1;
}


// SYS_LOCALE_GET_KEYMAP (ebx = locale_keymap_t *user_buf)
void systemcall_locale_get_keymap(struct regs *r){
    locale_keymap_t *user_buf = (locale_keymap_t *)r->ebx;
    if (!user_buf){ r->eax = -1; return; }
    // Та же защита, что в draw_string: не даём писать в ядерное пространство
    if ((uint32_t)user_buf < 0x40000000){ r->eax = -1; return; }

    const locale_t *active = locale_get_active();
    if (!active){ r->eax = -1; return; }

    // Собираем таблицу в локальный буфер ядра (1 КБ на стеке — многовато,
    // лучше через kmalloc, чтобы не рисковать переполнением kernel stack)
    locale_keymap_t *kmap = (locale_keymap_t *)kmalloc(sizeof(locale_keymap_t));
    if (!kmap){ r->eax = -1; return; }

    for (int sc = 0; sc < 128; sc++){
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
void systemcall_get_console_geometry(struct regs *r){
    if (!current_tty){
        r->eax = 0; r->ebx = 0; r->ecx = 0; r->edx = 0;
        return;
    }
    const locale_t *loc = locale_get_by_index(current_tty->active_layout);
    if (!loc || !loc->font){
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
void systemcall_scroll_rect(struct regs *r){
    if (!fb || !fb->scroll_rect || !fb->get_bg){
        r->eax = -1;
        return;
    }
    int32_t x  = (int32_t)r->ebx;
    int32_t y  = (int32_t)r->ecx;
    int32_t w  = (int32_t)r->edx;
    int32_t h  = (int32_t)r->esi;
    int32_t dy = (int32_t)r->edi;
    if (x < 0 || y < 0 || w <= 0 || h <= 0){ r->eax = -1; return; }
    if (x + w > (int32_t)boot_info->width){  r->eax = -1; return; }
    if (y + h > (int32_t)boot_info->height){ r->eax = -1; return; }
    if (dy == 0){ r->eax = 0; return; }
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
void systemcall_fb_blit(struct regs *r){
    if (!fb || !fb->blit){
        r->eax = -1;
        return;
    }
    int32_t x = (int32_t)r->ebx;
    int32_t y = (int32_t)r->ecx;
    int32_t w = (int32_t)r->edx;
    int32_t h = (int32_t)r->esi;
    const void *src = (const void *)r->edi;
    if (!src){ r->eax = -1; return; }
    //Та же защита от записи в ядерное пространство, что в draw_string
    if ((uint32_t)src < 0x40000000){ r->eax = -1; return; }
    if (x < 0 || y < 0 || w <= 0 || h <= 0){ r->eax = -1; return; }
    if (x + w > (int32_t)boot_info->width){  r->eax = -1; return; }
    if (y + h > (int32_t)boot_info->height){ r->eax = -1; return; }
    r->eax = fb->blit(x, y, w, h, src);
}


void syscalls_table_init(void){
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
}
