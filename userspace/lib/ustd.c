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

#include "../include/ustd.h"
#include <stdbool.h>
#include "../include/syscall.h"
#include "../../include-kernel/locale_types.h"


//Общая функция запуска пользовательского приложения с корректным завершением и очисткой ресурсов
void __attribute__((naked)) _start(void){
    __asm__ __volatile__(
        "xor %%ebp, %%ebp\n\t"     //Обнуляем EBP по стандартам ABI (конец трассировки)
        "movl (%%esp), %%eax\n\t"   //Считываем argc прямо с вершины стека (не сдвигая ESP)
        "leal 4(%%esp), %%ebx\n\t"  //Вычисляем адрес массива указателей argv[] (он на 4 байта ниже)

        // Передаем параметры Си-функции cdecl способом через стек Ring 3
        "pushl %%ebx\n\t"           //Вторым аргументом main заталкиваем argv
        "pushl %%eax\n\t"           //Первым аргументом main заталкиваем argc

        "call main\n\t"            //Вызываем Си-функцию main! Результат (0) вернется в EAX

        //Очищаем стек Ring 3 от двухpush-ей параметров main (восстанавливаем ESP на +8 байт)
        "addl $8, %%esp\n\t"

        //ПомещаемSYS_EXIT строго в регистр EAX
        "movl %0, %%eax\n\t"
        "int $0x80\n\t"            //Дёргаем прерывание ядра

        "1:\n\t"
        "jmp 1b\n\t"               //Защитный барьер зависания
        :
        : "i"(SYS_EXIT)
        : "memory"
    );
    //u_printf(400, 570, 0x00FFFFFF, "exited");
}

void u_draw_char(uint32_t x, uint32_t y, char c, uint32_t color){
    __asm__ __volatile__("int $0x80"::"a"(SYS_DRAW_CHAR),"b"(x),"c"(y),"d"(c),"S"(color):"memory");
}

void u_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t color){
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(SYS_DRAW_STRING), "b"(x), "c"(y), "d"(str), "S"(color)
        : "memory"
    );
}

void u_exit(void){
    __asm__ __volatile__("int $0x80"::"a"(SYS_EXIT):"memory");
    for(;;);
}

void yield(void){
    __asm__ __volatile__("int $0x80"::"a"(SYS_YIELD):"memory");
}

//Самодельный printf, пишущий на экран по координатам X и Y с поддержкой UTF-8 строк
void u_printf(int x, int y, uint32_t color, const char *format, ...) {
    if (!format) return;

    __builtin_va_list args;
    __builtin_va_start(args, format);

    int current_x = x;
    const char *ptr = format;

    while (*ptr != '\0') {
        if (*ptr == '%') {
            ptr++;
            if (*ptr == 'c') {
                char char_val = (char)__builtin_va_arg(args, int);
                // Для одиночных ASCII символов оставляем старый побайтовый вывод
                u_draw_char(current_x, y, char_val, color);
                current_x += 8; // Шаг шрифта 8 пикселей
            }
            else if (*ptr == 's') {
                const char *str_val = __builtin_va_arg(args, const char *);
                if (str_val) {
                    // ВАЖНО: Выводим всю строку целиком через новый системный вызов!
                    u_draw_string(current_x, y, str_val, color);

                    // Вычисляем правильный сдвиг X по экрану.
                    // В UTF-8 русская буква занимает 2 байта, но на экране это ОДИН глиф (8 пикселей).
                    // Считаем только реальные символы, а не байты:
                    int visible_chars = 0;
                    const char *s = str_val;
                    while (*s) {
                        if (((*s) & 0xC0) != 0x80) { // Пропускаем продолжения UTF-8 байт (0x80-0xBF)
                            visible_chars++;
                        }
                        s++;
                    }
                    current_x += (visible_chars * 8);
                }
            }
            else if (*ptr == 'd' || *ptr == 'x') {
                uint32_t num_val = __builtin_va_arg(args, uint32_t);
                char buf[32];
                int i = 0;
                uint32_t temp = num_val;
                int base = (*ptr == 'x') ? 16 : 10;

                if (temp == 0) buf[i++] = '0';
                while (temp > 0) {
                    int r = temp % base;
                    buf[i++] = (r < 10) ? (r + '0') : (r - 10 + 'A');
                    temp /= base;
                }
                buf[i] = '\0';

                // Разворачиваем буфер числа
                char rev_buf[32];
                int r_idx = 0;
                for (int j = i - 1; j >= 0; j--) {
                    rev_buf[r_idx++] = buf[j];
                }
                rev_buf[r_idx] = '\0';

                // Числа всегда состоят из ASCII-символов, выводим их через u_draw_string
                u_draw_string(current_x, y, rev_buf, color);
                current_x += (r_idx * 8);
            }
        } else {
            // Если в самом формате printf попался русский текст (например, u_printf(x,y,c, "Привет: %s", name))
            // Нам нужно выкусить этот символ или строку до следующего управляющего флага.
            // Для простоты, выводим текущий символ. Но так как формат может содержать многобайтовый UTF-8,
            // одиночный u_draw_char(current_x, y, *ptr, color) нарисует мусор.
            // Поэтому выводим этот символ как микро-строку из двух байт:
            char temp_utf8[3] = {0, 0, 0};
            int step = 1;
            temp_utf8[0] = *ptr;
            if (((*ptr) & 0xE0) == 0xC0) { // 2-байтовый UTF-8
                temp_utf8[1] = *(ptr + 1);
                step = 2;
            }
            u_draw_string(current_x, y, temp_utf8, color);
            current_x += 8;
            ptr += (step - 1);
        }
        ptr++;
    }
    __builtin_va_end(args);
}



int open(const char *path, uint32_t flags){
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_OPEN),"b"(path),"c"(flags): "memory");
    return ret;
}

int u_kbhit(void){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_KBD_HIT) : "memory");
    return ret;
}

uint32_t u_read_scancode(void){
    uint32_t packed_event = 0;
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_READ), "b"(STDIN), "c"(&packed_event), "d"(4)
        : "memory"
    );
    if (ret > 0){
        return packed_event;
    }
    return 0;
}


int u_write(int fd, const void *buf, uint32_t count){
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_WRITE),"b"(fd),"c"(buf),"d"(count):"memory");
    return ret;
}

int mkdir(const char *path){
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_MKDIR),"b"(path):"memory");
    return ret;
}

int u_readdir(int fd, u_dirent_t *entry){
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_READDIR),"b"(fd),"c"(entry):"memory");
    return ret;
}

int close(int fd){
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_CLOSE),"b"(fd):"memory");
    return ret;
}

int unlink(const char *path) {
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_UNLINK),"b"(path):"memory");
    return ret;
}

int rmdir(const char *path) {
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_RMDIR),"b"(path):"memory");
    return ret;
}

void clear_screen(void){
    __asm__ __volatile__("int $0x80" : : "a"(SYS_CLEAR_SCREEN) : "memory");
}

void u_clear_tile(uint32_t x, uint32_t y, uint32_t color){
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(SYS_CLEAR_TILE), "b"(x), "c"(y), "d"(color)
        : "memory"
    );
}

int u_spawn(const char *path, const char *cmdline){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_SPAWN), "b"(path), "c"(cmdline) : "memory");
    return ret;
}

int u_halt_os(void){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_HALT_OS) : "memory");
    return ret;
}


int u_wait_deity(int pid){
    int ret;
    //uint32_t count = 0;
    while (1) {
        __asm__ __volatile__(
            "int $0x80"
            : "=a"(ret)
            : "a"(SYS_WAIT_PID), "b"(pid)
            : "memory", "cc"
        );

        //u_printf(400, 550, 0x00E0E0E0, "App live, ticks: %d", count++);
        //Если ядро говорит, что порожденный процесс еще активен (-2)
        if (ret == PROCESS_IS_ACTIVE){
            yield(); //Добровольно уступаем процессор, пока ребенок работает
            continue;
        }
        break;
    }
    return ret;
}

uint32_t get_ticks(void){
    uint32_t t;
    __asm__ __volatile__("int $0x80" : "=a"(t) : "a"(SYS_GET_TICKS) : "memory");
    return t;
}


int chdir(const char *path){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_CHDIR), "b"(path) : "memory");
    return ret;
}

int getcwd(char *buf, uint32_t max_len){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_GETCWD), "b"(buf), "c"(max_len) : "memory");
    return ret;
}

int stat(const char *path, u_stat_t *st){
    int ret;
    //SYS_STAT (номер 16) передает указатель на путь в EBX, а указатель на структуру метаданных в ECX
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_STAT),"b"(path),"c"(st):"memory");
    return ret;
}

int seek(int fd, uint32_t position){
    int ret;
    //Транслируем локальный fd процесса в глобальный через fds, если вызов идет из Ring 3,
    //но так как в syscalls.c вызов vfs_seek принимает file_d (локальный), шлем его напрямую
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_SEEK), "b"(fd), "c"(position) : "memory");
    return ret;
}

int tell(int fd){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_TELL), "b"(fd) : "memory");
    return ret;
}



int set_wallpaper(const void *buffer, uint32_t size){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_SET_WALLPAPER), "b"(buffer), "c"(size) : "memory");
    return ret;
}


int dup2(int oldfd, int newfd){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_DUP2), "b"(oldfd), "c"(newfd) : "memory");
    return ret;
}

int u_isatty(int fd){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_ISATTY), "b"(fd) : "memory");
    return ret;
}


void u_get_console_geometry(uint32_t *cols, uint32_t *rows,uint32_t *cell_w, uint32_t *cell_h){
    uint32_t c = 0, rr = 0, cw = 0, ch = 0;
    __asm__ __volatile__("int $0x80" : "=a"(c), "=b"(rr), "=c"(cw), "=d"(ch) : "a"(SYS_GET_CONSOLE_GEOMETRY) : "memory");
    if (cols)   *cols   = c;
    if (rows)   *rows   = rr;
    if (cell_w) *cell_w = cw;
    if (cell_h) *cell_h = ch;
}

int u_scroll_rect(int x, int y, int w, int h, int dy){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_SCROLL_RECT), "b"(x), "c"(y), "d"(w), "S"(h), "D"(dy)
        : "memory");
    return ret;
}

int u_fb_blit(int x, int y, int w, int h, const void *src){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_FB_BLIT), "b"(x), "c"(y), "d"(w), "S"(h), "D"(src) : "memory");
    return ret;
}


static void n_put_str(const char *str){
    if (!str) return;
    uint32_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    if (len > 0){
        u_write(STDOUT, str, len); //Пишем в стандартный поток вывода процессов
    }
}


//Канонический потоковый
void printf(const char *format, ...){
    if (!format) return;
    __builtin_va_list args;
    __builtin_va_start(args, format);
    const char *ptr = format;
    const char *start_chunk = format; //Указатель на начало блока чистого текста
    while (*ptr != '\0'){
        if (*ptr == '%'){
            //Перед тем как обработать спецификатор, выплёвываем в STDOUT
            //весь накопленный кусок статического текста до знака '%' за jlby вызов
            uint32_t chunk_len = ptr - start_chunk;
            if (chunk_len > 0){
                u_write(STDOUT, start_chunk, chunk_len);
            }
            ptr++; //Сдвигаемся на символ спецификатора
            if (*ptr == 'c'){
                char char_val = (char)__builtin_va_arg(args, int);
                char char_buf[1];
                char_buf[0] = char_val;
                u_write(STDOUT, char_buf, 1);
            }
            else if (*ptr == 's'){
                const char *str_val = __builtin_va_arg(args, const char *);
                if (str_val){
                    n_put_str(str_val);
                } else {
                    n_put_str("(null)");
                }
            }
            else if (*ptr == 'd' || *ptr == 'x'){
                uint32_t num_val = __builtin_va_arg(args, uint32_t);
                char num_buf[12];
                int i = 0;
                uint32_t temp = num_val;
                int base = (*ptr == 'x') ? 16 : 10;
                if (temp == 0) num_buf[i++] = '0';
                while (temp > 0){
                    int r = temp % base;
                    num_buf[i++] = (r < 10) ? (r + '0') : (r - 10 + 'A');
                    temp /= base;
                }
                char rev_buf[12];
                int rev_idx = 0;
                for (int j = i - 1; j >= 0; j--){
                    rev_buf[rev_idx++] = num_buf[j];
                }
                rev_buf[rev_idx] = '\0';
                n_put_str(rev_buf);
            }
            //Сдвигаем маркер начала следующего текстового блока за спецификатор
            start_chunk = ptr + 1;
        }
        ptr++;
    }
    //Выплёвываем финальный остаток строки, если он остался после последнего '%'
    uint32_t final_chunk_len = ptr - start_chunk;
    if (final_chunk_len > 0) {
        u_write(STDOUT, start_chunk, final_chunk_len);
    }
    __builtin_va_end(args);
}


bool fileexists(const char *path){
    if (!path) return false;
    u_stat_t st;
    //Вызываем ваш системный вызов stat
    int res = stat(path, &st);
    //Если res != 0 (ядро вернуло ошибку, файла нет на диске), выходим
    if (res != 0) {
        return false;
    }
    //Проверяем поле attributes согласно флагам из ustd.h
    //Если это папка (FS_ATTR_DIRECTORY), то для fileexists это false
    if ((st.attributes & FS_ATTR_DIRECTORY) == FS_ATTR_DIRECTORY) {
        return false;
    }
    //Во всех остальных случаях, если объект существует и это не папка — это файл
    return true;
}


bool directoryexists(const char *path){
    if (!path) return false;
    u_stat_t st;
    int res = stat(path, &st);
    //Если папки нет на диске или путь некорректен
    if (res != 0){
        return false;
    }
    //Строго проверяем флаг директории
    return ((st.attributes & FS_ATTR_DIRECTORY) == FS_ATTR_DIRECTORY);
}



int u_read(int fd, void *buf, uint32_t count){
    int ret;

    if (fd == STDIN && u_isatty(STDIN)){
        char *dst = (char *)buf;
        uint32_t got = 0;
        uint32_t limit = (count > 0) ? count - 1 : 0;

        while (got < limit){
            ret = 0;
            __asm__ __volatile__("int $0x80"
                : "=a"(ret)
                : "a"(SYS_READ), "b"(fd), "c"(dst + got), "d"(1)
                : "memory");

            if (ret < 0) return ret;
            if (ret == 0){ yield(); continue; }

            char ch = dst[got];

            if (ch == '\n' || ch == '\r'){
                dst[got++] = '\n';
                break;
            }
            if (ch == '\b'){
                //снимаем последний полный codepoint UTF-8 из dst[0..got)
                if (got > 0){
                    got--;                                              // снимаем последний байт
                    while (got > 0 && ((uint8_t)dst[got] & 0xC0) == 0x80) got--;  // снимаем continuation
                    if (got > 0 && ((uint8_t)dst[got] & 0xC0) == 0xC0) { /* двухбайтовый: 1 оставшийся байт это lead, снимаем его */
                        got--;
                    }
                }
                continue;
            }
            got++;
        }

        dst[got] = '\0';
        return (int)got;
    }

    __asm__ __volatile__("int $0x80"
        : "=a"(ret)
        : "a"(SYS_READ), "b"(fd), "c"(buf), "d"(count)
        : "memory");
    return ret;
}


int u_kbd_set_mode(int mode){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_KBD_SET_MODE), "b"(mode) : "memory");
    return ret;
}


uint32_t u_tty_get_flags(void){
    uint32_t ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_TTY_GET_FLAGS) : "memory");
    return ret;
}

void u_tty_set_flags(uint32_t new_flags, uint32_t mask){
    __asm__ __volatile__("int $0x80" : : "a"(SYS_TTY_SET_FLAGS), "b"(new_flags), "c"(mask) : "memory");
}


int u_locale_get(void){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_LOCALE_GET) : "memory");
    return ret;
}

int u_locale_get_keymap(locale_keymap_t *out){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_LOCALE_GET_KEYMAP), "b"(out) : "memory");
    return ret;
}

int scanf(const char *format, ...){
    if (!format) return 0;
    char input_buf[1024];
    for (int i = 0; i < 1024; i++) input_buf[i] = 0;

    int bytes_received = u_read(STDIN, input_buf, 1023);
    if (bytes_received <= 0) return 0;

    __builtin_va_list args;
    __builtin_va_start(args, format);

    const char *fmt_ptr = format;
    char *str_ptr = input_buf;
    int success_matches = 0;

    while (*fmt_ptr != '\0' && *str_ptr != '\0'){
        if (*fmt_ptr == ' ' || *fmt_ptr == '\t' || *fmt_ptr == '\n' || *fmt_ptr == '\r'){
            fmt_ptr++;
            continue;
        }

        while (*str_ptr == ' ' || *str_ptr == '\t' || *str_ptr == '\n' || *str_ptr == '\r'){
            str_ptr++;
        }

        if (*str_ptr == '\0') break;

        if (*fmt_ptr == '%'){
            fmt_ptr++;

            if (*fmt_ptr == 'c'){
                char *char_dst = __builtin_va_arg(args, char *);
                if (char_dst){
                    *char_dst = *str_ptr;
                    success_matches++;
                }
                str_ptr++;
            }
            else if (*fmt_ptr == 's'){
                char *str_dst = __builtin_va_arg(args, char *);
                if (str_dst){
                    int s_idx = 0;
                    while (*str_ptr != '\0' && *str_ptr != ' ' && *str_ptr != '\t' && *str_ptr != '\n' && *str_ptr != '\r'){
                        str_dst[s_idx++] = *str_ptr++;
                    }
                    str_dst[s_idx] = '\0';
                    success_matches++;
                }
            }
            else if (*fmt_ptr == 'd'){
                int *int_dst = __builtin_va_arg(args, int *);
                if (int_dst){
                    int sign = 1;
                    if (*str_ptr == '-'){
                        sign = -1;
                        str_ptr++;
                    } else if (*str_ptr == '+'){
                        str_ptr++;
                    }

                    int value = 0;
                    int numeric_chars = 0;
                    while (*str_ptr >= '0' && *str_ptr <= '9'){
                        value = value * 10 + (*str_ptr - '0');
                        str_ptr++;
                        numeric_chars++;
                    }

                    if (numeric_chars > 0){
                        *int_dst = value * sign;
                        success_matches++;
                    }
                }
            }
            else if (*fmt_ptr == 'x'){
                uint32_t *hex_dst = __builtin_va_arg(args, uint32_t *);
                if (hex_dst){
                    if (str_ptr[0] == '0' && (str_ptr[1] == 'x' || str_ptr[1] == 'X')){
                        str_ptr += 2;
                    }

                    uint32_t value = 0;
                    int hex_chars = 0;
                    while (1){
                        char h = *str_ptr;
                        if (h >= '0' && h <= '9'){
                            value = value * 16 + (h - '0');
                        } else if (h >= 'a' && h <= 'f'){
                            value = value * 16 + (h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F'){
                            value = value * 16 + (h - 'A' + 10);
                        } else {
                            break;
                        }
                        str_ptr++;
                        hex_chars++;
                    }

                    if (hex_chars > 0){
                        *hex_dst = value;
                        success_matches++;
                    }
                }
            }
            fmt_ptr++;
        } else {
            if (*fmt_ptr == *str_ptr){
                fmt_ptr++;
                str_ptr++;
            } else {
                break;
            }
        }
    }

    __builtin_va_end(args);
    return success_matches;
}


