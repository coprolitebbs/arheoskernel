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

// Состояние модификаторов клавиатуры в ядре
bool shift_pressed = false;






void syscall_handler(struct regs *r){
    switch (r->eax){
        case SYS_DRAW_CHAR:{
			if(r->ebx >= boot_info->width || r->ecx >=boot_info->height) return;
            kernel_draw_char(r->ebx,r->ecx,(unsigned char)r->edx,r->esi);
            break;
        }
        case SYS_FILL_SCREEN:{
            kernel_fill_screen(r->ebx);
            break;
        }
		case SYS_YIELD:{
			r->eax = 0;
			schedule(r);
			break;
		}
		case SYS_EXIT:{
			current_task->state = TASK_TERMINATED;
			current_task->esp = 0;
			schedule(r);
			for(;;);
			break;
		}
		case SYS_GET_FB_INFO:{
			r->eax = boot_info->pitch;
            r->ebx = boot_info->bpp;
            r->ecx = boot_info->width;
            r->edx = boot_info->height;
			break;
		}
        case SYS_ALLOC_PAGE:{
            uint32_t virt = current_task->heap_next;
            uint32_t phys = (uint32_t)pmm_alloc_page();
            if (phys == 0) {
                r->eax = 0;
                break;
            }
            //Блокируем планировщик прерываний таймера на время транзакции маппинга
            kernel_schedule_lock = 1;
            //Включаем каталог страниц текущего процесса в CR3, чтобы таблицы были видимы
            vmm_switch_directory(current_task->page_dir_phys);
            uint32_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
            //Зануление физической страницы ОЗУ
            memset((void*)phys, 0, PAGE_SIZE);
            //kprintf("here2\n");
            //Вызываем маппинг
            vmm_map_page(current_task->page_dir, virt, phys, flags);
            //kprintf("here3\n");
            if(!vm_page_add(current_task, virt, phys, flags)){
                vmm_unmap_page(current_task->page_dir, virt);
                pmm_free_page((void*)phys);
                vmm_switch_directory(current_task->page_dir_phys);
                kernel_schedule_lock = 0;
                r->eax = 0;
                break;
            }
            //kprintf("here4\n");
            //Сброс TLB-кэша процессора (вместо тяжелого invlpg)
            uint32_t flush_cr3;
            __asm__ __volatile__("mov %%cr3, %0\n\t" "mov %0, %%cr3" : "=r"(flush_cr3) :: "memory");

            kernel_schedule_lock = 0;

            current_task->heap_next += PAGE_SIZE;
            r->eax = virt; //Возвращаем виртуальный адрес кучи в Ring 3
            break;
        }
        case SYS_SBRK: { //ebx = int32_t increment (приращение кучи в байтах)
            int32_t increment = (int32_t)r->ebx;
            uint32_t old_heap_next = current_task->heap_next;
            if (increment <= 0) {
                r->eax = old_heap_next;
                break;
            }
            //Вычисляем, сколько страниц ОЗУ нужно выделить под этот запрос
            uint32_t pages_needed = (increment + PAGE_SIZE - 1) / PAGE_SIZE;
            //Защитный барьер кучи Ring 3 (ограничиваем 16 МБ до границы стека)
            if (current_task->heap_next + (pages_needed * PAGE_SIZE) >= 0x7FFFF000) {
                r->eax = 0;
                break;
            }
            //Блокируем планировщик таймера на время массового маппинга мегабайтов
            kernel_schedule_lock = 1;
            //Принудительно загружаем физический каталог страниц текущего процесса в CR3
            vmm_switch_directory(current_task->page_dir_phys);
            bool alloc_success = true;
            for (uint32_t i = 0; i < pages_needed; i++) {
                uint32_t virt = current_task->heap_next;
                uint32_t phys = 0;
                //Такой же Memory Shield для защищённых sbrk массивов
                for (int attempt = 0; attempt < 1024; attempt++) {
                    phys = (uint32_t)pmm_alloc_page();
                    if (!phys) break;
                    uint32_t kstack_page = current_task->kernel_stack_phys;
                    if (phys >= 0x00100000 && phys < 0x20000000 && phys != kstack_page) {
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
            break;
        }
        case SYS_FREE_PAGE:{//ebx = виртуальный адрес освобождаемой страницы (virt)
            uint32_t virt = r->ebx;
            //Адрес должен быть выровнен по границе страницы 4КБ и принадлежать куче Ring 3
            if ((virt & 0xFFF) != 0 || virt < 0x50000000 || virt >= 0x7FFFF000){
                r->eax = -1;
                break;
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
            break;
        }
        case SYS_OPEN:{ //SYS_OPEN (ebx = const char *path, ecx = flags_o)
            const char *user_path = (const char *)r->ebx;
            uint32_t flags_o = r->ecx;
            if (!user_path){
                r->eax = -1;
                break;
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
                break;
            }
            //Выделяем память в куче под временный буфер пути Ring 3
            char *kernel_path = (char *)kmalloc(VFS_PATH_MAX);
            if (!kernel_path){
                r->eax = -1;
                break;
            }
            memset(kernel_path, 0, VFS_PATH_MAX);
            //Безопасно маршалим строку из пространства пользователя в кучу ядра
            for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path[i] != '\0'; i++){
                kernel_path[i] = user_path[i];
            }
            //Выделяем память под итоговый абсолютный каноничный путь
            char *absolute_path = (char *)kmalloc(VFS_PATH_MAX);
            if (!absolute_path){
                kfree(kernel_path);
                r->eax = -1;
                break;
            }
            vfs_resolve_relative_path(kernel_path, absolute_path);
            kfree(kernel_path);
            //kprintf("pth: %s\n", absolute_path);
            //Открываем файл на уровне глобальной VFS ядра по честному абсолютному пути
            int global_vfs_fd = vfs_open(absolute_path, flags_o);
            kfree(absolute_path);

            if (global_vfs_fd < 0){
                r->eax = global_vfs_fd;
                break;
            }
            //Связываем дескрипторы и возвращаем локальный хэндл в Userspace Ring 3
            current_task->fds[local_fd] = global_vfs_fd;
            r->eax = local_fd;
            break;
        }
        case SYS_READDIR: { //SYS_READDIR (ebx = file_d, ecx = fs_dirent_t*)
            int file_d = (int)r->ebx;
            fs_dirent_t *user_entry = (fs_dirent_t *)r->ecx;
            if (!user_entry || file_d < 0 || file_d >= MAX_PROCESS_FDS){
                r->eax = -1;
                break;
            }
            int global_fd = current_task->fds[file_d];
            if (global_fd < 0) { r->eax = -1; break; }

            fs_dirent_t kernel_entry;
            memset(&kernel_entry, 0, sizeof(fs_dirent_t));
            //Передаем в vfs_readdir ИМЕННО глобальный хэндл ядра
            int vfs_res = vfs_readdir(global_fd, &kernel_entry);
            if (vfs_res != FS_OK){
                r->eax = vfs_res;
                break;
            }
            user_entry->inode = kernel_entry.inode;
            user_entry->type = kernel_entry.type; //Передаем тип
            int i = 0;
            for (i = 0; i < 255; i++){
                user_entry->name[i] = kernel_entry.name[i];
                if (kernel_entry.name[i] == '\0') break;
            }
            user_entry->name[i] = '\0';
            r->eax = FS_OK;
            break;
        }
        case SYS_CLOSE:{ //SYS_CLOSE (ebx = int file_d)
            int file_d = (int)r->ebx;
            if (file_d < 0 || file_d >= MAX_PROCESS_FDS) {
                r->eax = -1;
                break;
            }
            //Извлекаем связанный глобальный VFS дескриптор
            int global_fd = current_task->fds[file_d];
            if (global_fd < 0) {
                r->eax = -1;
                break;
            }
            //Закрываем файл на уровне глобальной VFS и драйвера файловой системы
            int res = vfs_close(global_fd);

            //Если VFS успешно закрыла файл, освобождаем локальный слот процесса
            if (res == FS_OK) {
                current_task->fds[file_d] = -1; //Сбрасываем обратно в 0xFFFFFFFF
            }
            r->eax = res;
            break;
        }
        case SYS_READ:{//SYS_READ (ebx = file_d, ecx = буфер void*, edx = count)
            int file_d = (int)r->ebx;
            void *user_buf = (void *)r->ecx;
            char *user_dst = (char *)r->ecx;
            uint32_t count = r->edx;

            if (!user_buf || count == 0){
                r->eax = -1;
                break;
            }
            //Извлекаем глобальный дескриптор файла из локальной таблицы процесса
            int global_fd = current_task->fds[file_d];
            if (global_fd < 0){
                r->eax = -1;
                break;
            }

            //Обработчик записи в STDIN пользовательского приложения
            //Это просто мрак
             if (global_fd == 0){
                if (!kbd_driver_api){ r->eax = -1; break; }
                //Подчисткафлагов при первом переключении процессов
                if (current_tty->copied_bytes == 0 && current_tty->line_ready == false) {
                    while (kbd_driver_api->has_chars() != 0) {
                        kbd_driver_api->get_char_blocked();
                    }
                    current_tty->line_cursor_pos = 0;
                    memset(current_tty->line_buffer, 0, LINE_BUFFER_SIZE);
                    // Запоминаем горизонтальный отступ Shell (input_start_x)
                    current_tty->input_start_x = current_tty->cursor_x;
                    // Запоминаем вертикальный индекс текстовой строки
                    current_tty->input_start_y = current_tty->cursor_y / LINE_HEIGHT;
                    current_tty->last_blink_tick = tick_count;
                    current_tty->cursor_visible = true;
                }

                current_tty->foreground_pid = current_task->pid;
                //Экстренная отмена ввода по комбинации CTRL+C
                if (stdin_cancel_flag == 1){
                    current_tty->line_ready = false; current_task->wait_reason = 0; stdin_cancel_flag = 0;
                    kernel_stdout_write_char('^', console_bgcolor);
                    kernel_stdout_write_char('C', console_bgcolor);
                    kernel_stdout_write_char('\n', console_fgcolor);
                    while (kbd_driver_api->has_chars() != 0) kbd_driver_api->get_char_blocked();
                    for (int i = 0; i < count; i++) user_dst[i] = 0;
                    current_tty->copied_bytes = 0; current_tty->line_cursor_pos = 0;
                    memset(current_tty->line_buffer, 0, LINE_BUFFER_SIZE);
                    r->eax = -1;
                    return;
                }
                //Если пользователь ещё нажимает клавиши и Enter (line_ready)
                //не подтвержден прерыванием irq33_handler — отправляем задачу спать.
                if (current_tty->line_ready == false) {
                    current_task->state = TASK_BLOCKED;
                    current_task->wait_reason = 1;
                    r->eax = KBD_DRIVER_NOT_READY;
                    return; //Уводим приложение в сон, пока прерывание клавиатуры не вернет Enter
                }
                //Enter нажат в прерывании и задача проснулась
                if (current_tty->line_ready == true || current_tty->copied_bytes > 0){
                    //Стираем мигающий курсор с экрана перед выходом его же кодом под цвет фона,
                    //чтобы он не остался висеть артефактом посреди текста
                    uint32_t exit_cx, final_cy;
                    kconsole_get_render_coords(current_tty->line_cursor_pos, &exit_cx, &final_cy);
                    //fb->draw_char(exit_cx, final_cy, current_tty->cursor_code, console_bgcolor);
                    if (fb && fb->clear_tile) {
                        fb->clear_tile(exit_cx, final_cy, console_bgcolor);
                    } else {
                        fb->draw_char(exit_cx, final_cy, current_tty->cursor_code, console_bgcolor);
                    }
                    //подтираем Userspace-буфер перед записью результатов
                    char *user_clear_ptr = (char *)user_buf;
                    for (uint32_t i = 0; i < count; i++) {
                        user_clear_ptr[i] = 0;
                    }
                    //Вычисляем размер порции данных для копирования в Userspace
                    uint32_t final_len = current_tty->copied_bytes;
                    if (final_len > count) {
                        final_len = count;
                    }
                    //Если пользователь нажал пустой Enter на пустой строке
                    if (final_len == 0 && current_tty->line_ready == true) {
                        current_tty->line_ready = false;
                        current_task->wait_reason = 0;
                        r->eax = 0;
                        if (current_task->esp) ((struct regs*)current_task->esp)->eax = 0;
                        return;
                    }

                    //Копируем готовую строку из буфера TTY ядра в буфер процесса Ring 3
                    for (uint32_t i = 0; i < final_len; i++) {
                        user_dst[i] = current_tty->line_buffer[i];
                    }
                    if (final_len < count) {
                        user_dst[final_len] = '\0';
                    }
                    //Обработка остатка строки (если count процесса меньше, чем длина строки в ядре)
                    uint32_t remainder = current_tty->copied_bytes - final_len;
                    if (remainder > 0) {
                        for (uint32_t i = 0; i < remainder; i++) {
                            current_tty->line_buffer[i] = current_tty->line_buffer[final_len + i];
                        }
                        for (uint32_t i = remainder; i < LINE_BUFFER_SIZE; i++) {
                            current_tty->line_buffer[i] = 0;
                        }
                        current_tty->copied_bytes = remainder;
                        current_tty->line_cursor_pos = remainder;
                    } else {
                        //Строка вычитана полностью — очищаем состояние под следующий ввод
                        current_tty->copied_bytes = 0;
                        current_tty->line_cursor_pos = 0;
                        current_tty->line_ready = false;
                        memset(current_tty->line_buffer, 0, LINE_BUFFER_SIZE);
                    }
                    current_task->wait_reason = 0;
                    if (current_task->esp) {
                        ((struct regs*)current_task->esp)->eax = final_len;
                    }
                    r->eax = final_len; //Возвращаем реальное количество прочитанных байт в приложение
                    return;
                }
                current_task->state = TASK_BLOCKED;
                current_task->wait_reason = 1;
                r->eax = KBD_DRIVER_NOT_READY;
                return;
            }

            //Чтение обычных дисковых файлов (VFS) через глобальный дескриптор
            if (count > 4096) count = 4096;
            uint8_t *kernel_buf = (uint8_t *)kmalloc(count);

            if (!kernel_buf){
                r->eax = -1;
                break;
            }
            memset(kernel_buf, 0, count);
            //int bytes_read = vfs_read(file_d, kernel_buf, count);
            int bytes_read = vfs_read(global_fd, kernel_buf, count);
            if (bytes_read > 0){
                memcpy(user_buf, kernel_buf, bytes_read);
            }
            kfree(kernel_buf);

            r->eax = bytes_read;
            break;
        }
        case SYS_WRITE:{//SYS_WRITE (ebx = fd, ecx = буфер const void*, edx = count)
            int fd_wr = (int)r->ebx;
            void *user_buf_wr = (void *)r->ecx;
            char *user_src = (char *)r->ecx;
            uint32_t count_wr = r->edx;
            if (!user_buf_wr || count_wr == 0){
                r->eax = -1;
                break;
            }
            int global_fd = current_task->fds[fd_wr];
            if (global_fd < 0) { r->eax = -1; break; }
            //Локальные потоки вывода (STDOUT == 1, STDERR == 2)
            if (global_fd == 1 || global_fd == 2){
                uint32_t written_bytes = 0;

                while (written_bytes < count_wr){
                    char c = user_src[written_bytes];
                    kernel_stdout_write_char(c, console_fgcolor);
                    written_bytes++;
                }
                r->eax = written_bytes;
                break;
            }
            //Запись в обычные дисковые файлы (VFS)
            if (count_wr > 512) count_wr = 512;
            uint8_t *kernel_buf_wr = (uint8_t *)kmalloc(count_wr);
            if (!kernel_buf_wr) {
                r->eax = -1;
                break;
            }
            memcpy(kernel_buf_wr, user_buf_wr, count_wr);
            int bytes_written = vfs_write(/*fd_wr*/global_fd, kernel_buf_wr, count_wr);
            kfree(kernel_buf_wr);
            r->eax = bytes_written;
            break;
        }
        case SYS_MKDIR:{//SYS_MKDIR (ebx = Путь const char *path)
            const char *user_path_nm = (const char *)r->ebx;
            if (!user_path_nm) {
                r->eax = -1;
                break;
            }
            //Динамическая аллокация в куче Ring 0 для защиты 4 КБ стека ядра
            char *kernel_path_nm = (char *)kmalloc(VFS_PATH_MAX);
            char *absolute_path_nm = (char *)kmalloc(VFS_PATH_MAX);
            if (!kernel_path_nm || !absolute_path_nm) {
                if (kernel_path_nm) kfree(kernel_path_nm);
                if (absolute_path_nm) kfree(absolute_path_nm);
                r->eax = -1;
                break;
            }
            memset(kernel_path_nm, 0, VFS_PATH_MAX);
            for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path_nm[i] != '\0'; i++) {
                kernel_path_nm[i] = user_path_nm[i];
            }
            // Преобразуем относительный путь в абсолютный на базе CWD активного процесса
            vfs_resolve_relative_path(kernel_path_nm, absolute_path_nm);
            kfree(kernel_path_nm);
            r->eax = vfs_mkdir(absolute_path_nm);
            kfree(absolute_path_nm);
            break;
        }
        case SYS_UNLINK:{//SYS_UNLINK (ebx = пользовательский путь const char *path)
            const char *user_path_un = (const char *)r->ebx;
            if (!user_path_un) {
                r->eax = -1;
                break;
            }
            char *kernel_path_un = (char *)kmalloc(VFS_PATH_MAX);
            char *absolute_path_un = (char *)kmalloc(VFS_PATH_MAX);
            if (!kernel_path_un || !absolute_path_un) {
                if (kernel_path_un) kfree(kernel_path_un);
                if (absolute_path_un) kfree(absolute_path_un);
                r->eax = -1;
                break;
            }
            memset(kernel_path_un, 0, VFS_PATH_MAX);
            for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path_un[i] != '\0'; i++) {
                kernel_path_un[i] = user_path_un[i];
            }
            vfs_resolve_relative_path(kernel_path_un, absolute_path_un);
            kfree(kernel_path_un);
            //Передаем в VFS гарантированно валидный абсолютный путь, начинающийся со слэша '/'
            r->eax = vfs_unlink(absolute_path_un);

            kfree(absolute_path_un);
            break;
        }
        case SYS_RMDIR:{ // SYS_RMDIR (ebx = пользовательский путь const char *path)
            const char *user_path_rm = (const char *)r->ebx;
            if (!user_path_rm) {
                r->eax = -1;
                break;
            }
            char *kernel_path_rm = (char *)kmalloc(VFS_PATH_MAX);
            char *absolute_path_rm = (char *)kmalloc(VFS_PATH_MAX);
            if (!kernel_path_rm || !absolute_path_rm) {
                if (kernel_path_rm) kfree(kernel_path_rm);
                if (absolute_path_rm) kfree(absolute_path_rm);
                r->eax = -1;
                break;
            }
            memset(kernel_path_rm, 0, VFS_PATH_MAX);
            for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path_rm[i] != '\0'; i++) {
                kernel_path_rm[i] = user_path_rm[i];
            }
            //Разрешаем относительные пути и для удаления каталогов
            vfs_resolve_relative_path(kernel_path_rm, absolute_path_rm);
            kfree(kernel_path_rm);
            r->eax = vfs_rmdir(absolute_path_rm);
            kfree(absolute_path_rm);
            break;
        }
        case SYS_CLEAR_SCREEN:{//Системный вызов очистки консоли TTY и фреймбуфера
            kconsole_clear_screen();
            r->eax = 0; // Возвращаем статус успеха
            break;
        }
        case SYS_SPAWN: {
            const char *user_path = (const char *)r->ebx;
            const char *user_cmdline = (const char *)r->ecx;
            if (!user_path){
                r->eax = -1;
                break;
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
            break;
        }
        case SYS_WAIT_PID:{
            uint32_t target_pid = r->ebx;
            r->eax = -1; //По умолчанию возвращаем ошибку (процесс не найден)

            if (ready_queue) {
                task_t *start = ready_queue;
                task_t *t = start;
                bool found = false;
                // Перебираем кольцевую очередь в поисках живого дочернего процесса
                do {
                    if (t->pid == target_pid) {
                        found = true;
                        break;
                    }
                    t = t->next;
                } while (t != start);
                //Если процесс найден в кольце и он еще выполняет свой код,
                //просто возвращаем статус PROCESS_IS_ACTIVE
                if (found && t->state != TASK_TERMINATED) {
                    r->eax = PROCESS_IS_ACTIVE;
                    break;
                }
            }
            //Если процесс успешно завершил работу (сделал SYS_EXIT) и исчез из очереди
            r->eax = 0;
            break;
        }
        case SYS_HALT_OS: {
            //Пытаемся безопасно размонтировать VFS и проверить открытые дескрипторы
            int shutdown_res = vfs_shutdown_system();
            //Если ФС занята или драйвер отклонил umount — не выключаем ПК!
            //Возвращаем код ошибки обратно пользовательской утилите.
            if (shutdown_res != HALT_SUCCESS) {
                r->eax = shutdown_res;
                break;
            }
            // Теперь имеем полное право намертво заблокировать планировщик
            kernel_halt_all();

            for(;;);
            break;
        }
        case SYS_CHDIR:{// ebx = const char *path
            const char *user_path = (const char *)r->ebx;
            if (!user_path) { r->eax = -1; break; }
            //Выделяем буферы в куче ядра для защиты 4 КБ стека
            char *kernel_path = (char *)kmalloc(VFS_PATH_MAX);
            char *absolute_path = (char *)kmalloc(VFS_PATH_MAX);
            if (!kernel_path || !absolute_path){
                if (kernel_path) kfree(kernel_path);
                if (absolute_path) kfree(absolute_path);
                r->eax = -1;
                break;
            }
            //Копируем путь из Ring 3 в кучу ядра
            memset(kernel_path, 0, VFS_PATH_MAX);
            for (int i = 0; i < (VFS_PATH_MAX - 1) && user_path[i] != '\0'; i++) {
                kernel_path[i] = user_path[i];
            }
            //Канонизируем путь на базе текущего CWD процесса (обрабатываем точки "." и "..")
            vfs_resolve_relative_path(kernel_path, absolute_path);
            kfree(kernel_path);
            //Проверка на существование каталога
            //Пытаемся открыть путь. Флаг 1 (или флаг каталога, если в VFS он выделен)
            //Если открыть не удалось — значит такого каталога нет, возвращаем ошибку
            int dir_fd = vfs_open(absolute_path, FS_OPEN_READ);
            if (dir_fd < 0) {
                kfree(absolute_path);
                r->eax = -1;
                break;
            }
            vfs_close(dir_fd);
            //Перезаписываем поле cwd текущей активной задачи
            strncpy(current_task->cwd, absolute_path, PATH_LEN_MAX - 1);
            current_task->cwd[PATH_LEN_MAX - 1] = '\0';

            kfree(absolute_path);
            r->eax = 0; //Успешно
            break;
        }
        case SYS_GETCWD: { //ebx = char *buf, ecx = uint32_t max_len
            char *user_buf = (char *)r->ebx;
            uint32_t max_len = r->ecx;
            if (!user_buf || max_len == 0) {
                r->eax = -1;
                break;
            }
            //Безопасно копируем CWD из дескриптора задачи на стек Ring 3 процесса
            uint32_t len = strlen(current_task->cwd);
            if (len >= max_len) len = max_len - 1;
            for (uint32_t i = 0; i < len; i++) {
                user_buf[i] = current_task->cwd[i];
            }
            user_buf[len] = '\0';
            r->eax = 0; //Успешно
            break;
        }
        case SYS_STAT: { //ebx = const char *path, ecx = fs_stat_t *st
            const char *user_path = (const char *)r->ebx;
            fs_stat_t *user_st = (fs_stat_t *)r->ecx;
            //kprintf("herein\n");
            if (!user_path || !user_st){
                r->eax = -1;
                break;
            }
            //Выделяем 4096-байтные буферы в куче ядра
            char *kernel_path = (char *)kmalloc(VFS_PATH_MAX);
            char *absolute_path = (char *)kmalloc(VFS_PATH_MAX);
            if (!kernel_path || !absolute_path){
                if (kernel_path) kfree(kernel_path);
                if (absolute_path) kfree(absolute_path);
                r->eax = -1;
                break;
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
            //kprintf("hereout\n");
            kfree(absolute_path);

            if (vfs_res != VFS_STATUS_OK) {
                r->eax = vfs_res;
                break;
            }
            user_st->size        = kernel_st.size;
            user_st->attributes  = kernel_st.attributes;
            user_st->create_time = kernel_st.create_time; // Пробрасываем ctime из ext2
            user_st->modify_time = kernel_st.modify_time; // Пробрасываем mtime из ext2
            r->eax = VFS_STATUS_OK;
            break;
        }
        case SYS_SEEK: {//ebx = file_d, ecx = position
            int file_d = (int)r->ebx;
            uint32_t position = r->ecx;
            int global_fd = current_task->fds[file_d];
            if (global_fd < 0) { r->eax = -1; break; }
            r->eax = vfs_seek(global_fd, position);
            break;
        }
        case SYS_GET_MEM_INFO: {//ebx = am_mem_info_t *user_info
            am_mem_info_t *user_info = (am_mem_info_t *)r->ebx;
            if (!user_info) {
                r->eax = -1;
                break;
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
            for (uint32_t i = 0; i < sizeof(am_mem_info_t); i++) {
                dst[i] = src[i];
            }
            r->eax = 0;
            break;
        }
        case SYS_SET_WALLPAPER:{//ebx = const void *user_buf, ecx = uint32_t size
            const void *user_buf = (const void *)r->ebx;
            uint32_t size = r->ecx;
            if (!user_buf || size == 0) {
                r->eax = -1;
                break;
            }

            if (fb && fb->set_wallpaper){
                kernel_schedule_lock = 1;
                fb->set_wallpaper(user_buf, size);
                kernel_schedule_lock = 0;
                r->eax = 0;
            } else {
                r->eax = -1;
            }
            break;
        }
        case SYS_TELL: {//ebx = file_d
            int file_d = (int)r->ebx;
            int global_fd = current_task->fds[file_d];
            if (global_fd < 0) { r->eax = -1; break; }
            r->eax = vfs_tell(global_fd);
            break;
        }

        default:
            break;
    }
}

uint32_t tty_get_copied_bytes(void){
    return current_tty ? current_tty->copied_bytes : 0;
}
