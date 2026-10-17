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

#include "../../include/ustd.h"
#include "../../include/umemory.h"
#include "../../include/uimage.h"
#include "../../../include-kernel/lib.h"
#include "../../include/uconfig.h"
#include "../../include/fileattrs.h"
#include "../../include/syscall.h"
#include "../../../include-kernel/locale_types.h"

#define MAX_CMD_LEN 256
#define MAX_ARGS 16
#define HELP_BUF_SIZE 254
#define MAX_ENV_PATHS 8
#define MAX_PATH_LEN  64

locale_keymap_t g_keymap;
static int g_keymap_locale_idx = -1;

char* appname_default = "\x91\xA5\xE5\xAC\xA5\xE2";

#define HISTORY_MAX 16
char cmd_history[HISTORY_MAX][MAX_CMD_LEN];
int history_count = 0;
int history_write_idx = 0;
int history_browse_idx = -1;

char startup_paths[MAX_ENV_PATHS][MAX_PATH_LEN];
int startup_paths_count = 0;
char greeting[128], greeting2[128], appname[32], prompt_format[128], /*shell_prompt[64],*/ help_path[128], help_notfound[64];
char f_execute_mess[32], wallp_load[32], f_cd_mess[40], exit_mess[50], awake_mess[20], awake_failed_mess[50];

uint32_t cursor_char = '_';
//static bool kbd_lang_ru = false;
static bool g_cursor_blink_on = true;


static void ensure_keymap(void){
    int idx = u_locale_get();
    if (idx < 0) idx = 0;
    if (idx == g_keymap_locale_idx) return;
    if (u_locale_get_keymap(&g_keymap) == 0){
        g_keymap_locale_idx = idx;
    }
}

// UTF-8 encoder: code point → 1..4 байта
static int utf8_encode(uint32_t cp, uint8_t *out){
    if (cp == 0) return 0;
    if (cp < 0x80){ out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800){
        out[0] = 0xC0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3F);
        return 2;
    }
    if (cp < 0x10000){
        out[0] = 0xE0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3F);
        out[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
    out[0] = 0xF0 | (cp >> 18);
    out[1] = 0x80 | ((cp >> 12) & 0x3F);
    out[2] = 0x80 | ((cp >> 6)  & 0x3F);
    out[3] = 0x80 | (cp & 0x3F);
    return 4;
}


void trim_newline(char *str){
    int i = 0;
    while (str[i] != '\0'){
        if (str[i] == '\n' || str[i] == '\r'){ str[i] = '\0'; break; }
        i++;
    }
}


static void build_prompt(char *out, int max_len, const char *cwd){
    if (max_len <= 0) return;
    int oi = 0;
    const char *p = prompt_format;
    while (*p && oi < max_len - 1){
        if (p[0] == '$' && strncmp(p, "$appname", 8) == 0){
            for (int i = 0; appname[i] && oi < max_len - 1; i++)
                out[oi++] = appname[i];
            p += 8;
        }
        else if (p[0] == '$' && strncmp(p, "$cwd", 4) == 0){
            if (cwd){
                for (int i = 0; cwd[i] && oi < max_len - 1; i++)
                    out[oi++] = cwd[i];
            }
            p += 4;
        }
        else {
            out[oi++] = *p++;
        }
    }
    out[oi] = '\0';
}


int parse_command(char *cmdline, char *args[]){
    int argc = 0, i = 0, arg_start = -1;
    while (cmdline[i] != '\0'){
        if (cmdline[i] == ' ' || cmdline[i] == '\t'){
            if (arg_start != -1){
                cmdline[i] = '\0';
                args[argc++] = &cmdline[arg_start];
                arg_start = -1;
            }
        } else {
            if (arg_start == -1) arg_start = i;
        }
        if (argc >= (MAX_ARGS - 1)) break;
        i++;
    }
    if (arg_start != -1 && argc < (MAX_ARGS - 1)) args[argc++] = &cmdline[arg_start];
    args[argc] = NULL;
    return argc;
}

void autocomplete_file(char *buffer, int *pos, int *curr_idx){
    char cwd[256];
    getcwd(cwd, 255);
    int fd = open(cwd, FS_OPEN_READ);
    if (fd < 0) return;

    char prefix[64];
    int start = *curr_idx;
    while (start > 0 && buffer[start - 1] != ' ') start--;
    int len = *curr_idx - start;
    for (int i = 0; i < len; i++) prefix[i] = buffer[start + i];
    prefix[len] = '\0';
    u_dirent_t entry;
    while (u_readdir(fd, &entry) == FS_OK){
        if (strncmp(entry.name, prefix, len) == 0 && strlen(entry.name) > len){
            const char *match = &entry.name[len];
            int match_len = strlen(match);
            if (*pos + match_len >= MAX_CMD_LEN - 2) break;
            if (*curr_idx < *pos){
                for (int i = *pos - 1; i >= *curr_idx; i--){
                    buffer[i + match_len] = buffer[i];
                }
            }
            for (int i = 0; i < match_len; i++){
                buffer[*curr_idx + i] = match[i];
            }
            *pos += match_len;
            *curr_idx += match_len;
            buffer[*pos] = '\0';
            break;
        }
    }
    close(fd);
}


static int get_visible_length(const char *str, int bytes_limit){
    int visible = 0;
    for (int i = 0; i < bytes_limit && str[i] != '\0'; i++){
        if ((str[i] & 0xC0) != 0x80){
            visible++;
        }
    }
    return visible;
}


static bool is_valid_executable(const char *path){
    int fd = open(path, FS_OPEN_READ);
    if (fd < 0) return false;

    uint8_t magic[4] = {0, 0, 0, 0};
    int n = u_read(fd, magic, 4);
    close(fd);

    if (n < 4) return false;

    // ELF (используется большинством ваших программ)
    if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F')
        return true;


    // Если у вас есть собственный формат .bin — добавьте его сигнатуру сюда,
    // например: if (magic[0] == 'B' && magic[1] == 'I' && magic[2] == 'N') return true;

    return false;
}

static void redraw_line_with_cursor(const char *buffer, int pos, int curr_idx){
    static char out[1024];

    char cwd_buf[256];
    getcwd(cwd_buf, 255);

    // \r \x1B[K + prompt + cwd + "> " + buffer[:curr_idx]
    int n = 0;
    out[n++] = '\r';
    out[n++] = '\x1B'; out[n++] = '['; out[n++] = 'K';

    char prompt_buf[192];
    build_prompt(prompt_buf, sizeof(prompt_buf), cwd_buf);
    for (const char *p = prompt_buf; *p && n < (int)sizeof(out) - 32; p++) out[n++] = *p;

    for (int i = 0; i < curr_idx; i++) out[n++] = buffer[i];

    u_write(STDOUT, out, n);
    //одна ячейка под курсор
    char cur = ' ';
    if (g_cursor_blink_on){
            cur = (char)(cursor_char & 0xFF);
    }
    // Cursor OFF -> пробел, чтобы хвост строки не съезжал влево
    u_write(STDOUT, &cur, 1);
    //buffer[curr_idx:pos] + \x1B[ND
    n = 0;
    for (int i = curr_idx; i < pos; i++) out[n++] = buffer[i];
    int back_chars = get_visible_length(buffer, pos) - get_visible_length(buffer, curr_idx);
    if (back_chars > 0 && n < (int)sizeof(out) - 12){
        out[n++] = '\x1B'; out[n++] = '[';
        char rev[8]; int r = 0, tmp = back_chars;
        while (tmp > 0 && r < 8) { rev[r++] = '0' + (tmp % 10); tmp /= 10; }
        while (r > 0) out[n++] = rev[--r];
        out[n++] = 'D';
    }
    u_write(STDOUT, out, n);
}



void sekhmet_read_line(char *buffer, int max_len){
    int pos = 0;
    int curr_idx = 0;
    bool shift_active = false;
    bool extended_prefix = false;
    buffer[0] = '\0';

    ensure_keymap();

    //int locale_idx = u_locale_get();
    //if (locale_idx < 0) locale_idx = 0;
    //Состояние мигания курсора
    //Один "тик" = один yield() когда клавиатура молчит.
    //При 100 Гц PIT и частоте ~10 мс это ~10 мс на тик.
    const uint32_t BLINK_TICKS = 40;   //~400 мс
    g_cursor_blink_on = true;
    uint32_t last_blink_tick = get_ticks();
    redraw_line_with_cursor(buffer, pos, curr_idx);
    while (1){
        if (!u_kbhit()){
            yield();
            uint32_t now = get_ticks();
            if ((uint32_t)(now - last_blink_tick) >= BLINK_TICKS){
                last_blink_tick = now;
                g_cursor_blink_on = !g_cursor_blink_on;
                redraw_line_with_cursor(buffer, pos, curr_idx);
            }
            continue;
        }
        ensure_keymap();

        //любое нажатие — гасим мигание, курсор всегда горит при вводе
        g_cursor_blink_on = true;
        last_blink_tick = get_ticks();

        uint32_t ev = u_read_scancode();
        if (ev == 0){ yield(); continue; }

        //locale_idx = u_locale_get();
        //if (locale_idx < 0) locale_idx = 0;

        uint8_t scancode = ev & 0xFF;
        uint8_t type = (ev >> 8) & 0xFF;

        if (scancode == 0x2A || scancode == 0x36){
            if (type == 1) shift_active = true;
            if (type == 0) shift_active = false;
            continue;
        }
        /*
        if (scancode == 0x38){
            if (type == 1) kbd_lang_ru = !kbd_lang_ru;
            continue;
        }*/
        if (scancode == 0xE0){
            if (type == 1) extended_prefix = true;
            continue;
        }
        if (type == 0){
            extended_prefix = false;
            continue;
        }
        if (extended_prefix){
            extended_prefix = false;

            if (scancode == 0x4B){
                if (curr_idx > 0){
                    curr_idx--;
                    if (curr_idx > 0 && (buffer[curr_idx] & 0xC0) == 0x80){
                        curr_idx--;
                    }
                    redraw_line_with_cursor(buffer, pos, curr_idx);
                }
                continue;
            }
            if (scancode == 0x4D){
                if (curr_idx < pos){
                    curr_idx++;
                    if (curr_idx < pos && (buffer[curr_idx] & 0xC0) == 0x80){
                        curr_idx++;
                    }
                    redraw_line_with_cursor(buffer, pos, curr_idx);
                }
                continue;
            }
            if (scancode == 0x48){
                if (history_count > 0){
                    if (history_browse_idx == -1){
                        history_browse_idx = (history_write_idx - 1 + HISTORY_MAX) % HISTORY_MAX;
                    } else {
                        int oldest = (history_write_idx - history_count + HISTORY_MAX) % HISTORY_MAX;
                        if (history_browse_idx != oldest){
                            history_browse_idx = (history_browse_idx - 1 + HISTORY_MAX) % HISTORY_MAX;
                        }
                    }
                    strcpy(buffer, cmd_history[history_browse_idx]);
                    pos = strlen(buffer);
                    curr_idx = pos;
                    redraw_line_with_cursor(buffer, pos, curr_idx);
                }
                continue;
            }
            if (scancode == 0x50){
                if (history_browse_idx != -1){
                    int fresh_idx = (history_write_idx - 1 + HISTORY_MAX) % HISTORY_MAX;
                    if (history_browse_idx == fresh_idx){
                        history_browse_idx = -1;
                        buffer[0] = '\0';
                        pos = 0;
                        curr_idx = 0;
                    } else {
                        history_browse_idx = (history_browse_idx + 1) % HISTORY_MAX;
                        strcpy(buffer, cmd_history[history_browse_idx]);
                        pos = strlen(buffer);
                        curr_idx = pos;
                    }
                    redraw_line_with_cursor(buffer, pos, curr_idx);
                }
                continue;
            }
            if (scancode == 0x47){
                curr_idx = 0;
                redraw_line_with_cursor(buffer, pos, curr_idx);
                continue;
            }
            if (scancode == 0x4F){
                curr_idx = pos;
                redraw_line_with_cursor(buffer, pos, curr_idx);
                continue;
            }
            if (scancode == 0x53){
                if (curr_idx < pos){
                    int del_bytes = 1;
                    if (curr_idx < pos - 1 && (uint8_t)buffer[curr_idx] >= 0xD0 && (uint8_t)buffer[curr_idx] <= 0xD1){
                        del_bytes = 2;
                    }
                    for (int i = curr_idx + del_bytes; i <= pos; i++){
                        buffer[i - del_bytes] = buffer[i];
                    }
                    pos -= del_bytes;
                    buffer[pos] = '\0';
                    redraw_line_with_cursor(buffer, pos, curr_idx);
                }
                continue;
            }
            continue;
        }
        if (scancode == 0x1C){
            buffer[pos] = '\0';
            curr_idx = pos;
            char final_buf[512];
            int f_pos = 0;
            final_buf[f_pos++] = '\r';
            final_buf[f_pos++] = '\x1B';
            final_buf[f_pos++] = '[';
            final_buf[f_pos++] = 'K';

            char cwd_buf2[256];
            getcwd(cwd_buf2, 255);
            char prompt_buf[192];
            build_prompt(prompt_buf, sizeof(prompt_buf), cwd_buf2);
            for (int i = 0; prompt_buf[i] != '\0' && f_pos < (int)sizeof(final_buf) - 4; i++)
                final_buf[f_pos++] = prompt_buf[i];

            for (int i = 0; i < pos; i++) final_buf[f_pos++] = buffer[i];
            final_buf[f_pos++] = '\n';
            final_buf[f_pos] = '\0';
            u_write(STDOUT, final_buf, f_pos);
            if (pos > 0){
                strncpy(cmd_history[history_write_idx], buffer, MAX_CMD_LEN - 1);
                history_write_idx = (history_write_idx + 1) % HISTORY_MAX;
                if (history_count < HISTORY_MAX) history_count++;
            }
            history_browse_idx = -1;
            return;
        }
        if (scancode == 0x0E){
            if (curr_idx > 0){
                int del_bytes = 1;
                if (curr_idx > 1 && (buffer[curr_idx - 1] & 0xC0) == 0x80){
                    del_bytes = 2;
                }
                for (int i = curr_idx; i <= pos; i++){
                    buffer[i - del_bytes] = buffer[i];
                }
                pos -= del_bytes;
                curr_idx -= del_bytes;
                buffer[pos] = '\0';
                redraw_line_with_cursor(buffer, pos, curr_idx);
            }
            continue;
        }
        if (scancode == 0x0F){
            autocomplete_file(buffer, &pos, &curr_idx);
            redraw_line_with_cursor(buffer, pos, curr_idx);
            continue;
        }
        if (scancode < 128){
            uint32_t cp = shift_active ? g_keymap.shift[scancode] : g_keymap.normal[scancode];
            if (cp != 0){
                uint8_t utf8[4];
                int step = utf8_encode(cp, utf8);
                if (step > 0 && pos + step < max_len - 2){
                    if (curr_idx < pos){
                        for (int i = pos; i >= curr_idx; i--)
                            buffer[i + step] = buffer[i];
                    }
                    for (int i = 0; i < step; i++)
                        buffer[curr_idx + i] = utf8[i];
                    curr_idx += step;
                    pos      += step;
                    buffer[pos] = '\0';
                    redraw_line_with_cursor(buffer, pos, curr_idx);
                }
            }
        }
    }
}




bool find_executable(const char *cmd_name, char *out_resolved_path, int max_out_len){
    char temp_buf[256];
    //Проверяем, не передан ли уже абсолютный/относительный явный путь (содержит '/')
    bool explicit_path = false;
    bool has_dot = false;
    for (int i = 0; cmd_name[i] != '\0'; i++){
        if (cmd_name[i] == '/') explicit_path = true;
        if (cmd_name[i] == '.') has_dot = true;
    }
    if (explicit_path){
        //Если указан явный путь, и в нем уже есть расширение (например, /bin/init.elf)
        strncpy(out_resolved_path, cmd_name, max_out_len - 1);
        out_resolved_path[max_out_len - 1] = '\0';
        if (fileexists(out_resolved_path)) return true;
        //Если расширения нет, пробуем сначала авто-подстановку .elf, затем .bin
        if (!has_dot) {
            //Пробуем .elf
            strcpy(temp_buf, cmd_name);
            strcat(temp_buf, ".elf");
            if (fileexists(temp_buf)) {
                strncpy(out_resolved_path, temp_buf, max_out_len - 1);
                out_resolved_path[max_out_len - 1] = '\0';
                return true;
            }
            //Пробуем .bin
            strcpy(temp_buf, cmd_name);
            strcat(temp_buf, ".bin");
            if (fileexists(temp_buf)) {
                strncpy(out_resolved_path, temp_buf, max_out_len - 1);
                out_resolved_path[max_out_len - 1] = '\0';
                return true;
            }
        }
        return false;
    }
    //Ищем по массиву путей поиска (startup_paths) из config.cfg
    for (int i = 0; i < startup_paths_count; i++){
        char base_path[128];
        strcpy(base_path, startup_paths[i]);

        int len = strlen(base_path);
        if (len > 0 && base_path[len - 1] != '/'){
            strcat(base_path, "/");
        }
        //Если пользователь ввел команду сразу с точкой
        if (has_dot) {
            strcpy(temp_buf, base_path);
            strcat(temp_buf, cmd_name);
            if (fileexists(temp_buf)) {
                strncpy(out_resolved_path, temp_buf, max_out_len - 1);
                out_resolved_path[max_out_len - 1] = '\0';
                return true;
            }
        } else {
            //Приоритетный поиск: Проверяем наличие .elf версии программы
            strcpy(temp_buf, base_path);
            strcat(temp_buf, cmd_name);
            strcat(temp_buf, ".elf");
            if (fileexists(temp_buf)){
                strncpy(out_resolved_path, temp_buf, max_out_len - 1);
                out_resolved_path[max_out_len - 1] = '\0';
                return true;
            }
            //Фолбек-поиск: Проверяем наличие старой .bin версии программы
            strcpy(temp_buf, base_path);
            strcat(temp_buf, cmd_name);
            strcat(temp_buf, ".bin");
            if (fileexists(temp_buf)){
                strncpy(out_resolved_path, temp_buf, max_out_len - 1);
                out_resolved_path[max_out_len - 1] = '\0';
                return true;
            }
        }
    }
    //Проверяем локальный путь (текущую рабочую директорию процесса CWD)
    char cwd[256];
    memset(cwd, 0, sizeof(cwd));
    getcwd(cwd, 255);

    int cwd_len = strlen(cwd);
    if (cwd_len > 0 && cwd[cwd_len - 1] != '/') {
        strcat(cwd, "/");
    }

    if (has_dot) {
        strcpy(temp_buf, cwd);
        strcat(temp_buf, cmd_name);
        if (fileexists(temp_buf)) {
            strncpy(out_resolved_path, temp_buf, max_out_len - 1);
            out_resolved_path[max_out_len - 1] = '\0';
            return true;
        }
    } else {
        //Проверяем локальный .elf
        strcpy(temp_buf, cwd);
        strcat(temp_buf, cmd_name);
        strcat(temp_buf, ".elf");
        if (fileexists(temp_buf)){
            strncpy(out_resolved_path, temp_buf, max_out_len - 1);
            out_resolved_path[max_out_len - 1] = '\0';
            return true;
        }

        //Проверяем локальный .bin
        strcpy(temp_buf, cwd);
        strcat(temp_buf, cmd_name);
        strcat(temp_buf, ".bin");
        if (fileexists(temp_buf)){
            strncpy(out_resolved_path, temp_buf, max_out_len - 1);
            out_resolved_path[max_out_len - 1] = '\0';
            return true;
        }
    }

    return false;
}



void load_config(void){
    cfg_file_t *cfg = cfg_open("/config/sekhmet/config.cfg", FS_OPEN_READ, true);
    if (cfg < 0){
        strcpy(appname, appname_default);
        //strcpy(shell_prompt, appname_default);
        strcpy(prompt_format, "$appname:[$cwd]> ");
        strcpy(help_path, "/config/sekhmet/sekhmet.help");
        printf("%s: %s not found, keeping default parameters.\n",appname,help_path);
        strcpy(startup_paths[0], "/bin");
        startup_paths_count = 1;
        cursor_char = '_'; //Дефолтный фолбек
        return;
    }

    printf("Loading config, please wait");

    //char raw_prompt[64]={0};
    char help_filename[64]={0};

    cfg_read_string(cfg, "main", "appname", appname_default, appname, 32);printf(".");
    //cfg_read_string(cfg, "main", "prompt", "$appname", raw_prompt, 64);printf(".");
    cfg_read_string(cfg, "main", "prompt", "$appname:[$cwd]> ", prompt_format, 128);printf(".");
    cfg_read_string(cfg, "main", "greeting", "greeting err", greeting, 128);printf(".");
    cfg_read_string(cfg, "main", "greeting2", "greeting err", greeting2, 128);printf(".");

    //Парсим и сохраняем кастомный код курсора через встроенную hex-функцию
    cursor_char = cfg_read_hex(cfg, "main", "cursor_code", 0x5F); printf("."); //0x5F — это '_'
    cfg_read_string(cfg, "help", "filename", "sekhmet.hlp", help_filename, 64);printf(".");
    strcpy(help_path, "/config/sekhmet/");
    strcat(help_path, help_filename);

    cfg_read_string(cfg, "help", "notfound", "File not found", help_notfound, 64);printf(".");
    //strrep(shell_prompt, raw_prompt, "$appname", appname, 64);printf(".");
    char raw_paths_buf[256];
    memset(raw_paths_buf, 0, sizeof(raw_paths_buf));
    startup_paths_count = 0;
    cfg_read_string(cfg, "main", "startup_path", "/bin", raw_paths_buf, 256);printf(".");

    if (raw_paths_buf[0] != '\0'){
        char *token = strtok(raw_paths_buf, ";");
        while (token != NULL && startup_paths_count < MAX_ENV_PATHS) {
            strncpy(startup_paths[startup_paths_count], token, MAX_PATH_LEN - 1);
            startup_paths[startup_paths_count][MAX_PATH_LEN - 1] = '\0';
            startup_paths_count++;
            token = strtok(NULL, ";");
        }
    }

    cfg_read_string(cfg, "messages", "f_execute", "Unknown ritual or missing deity", f_execute_mess, 32);printf(".");
    cfg_read_string(cfg, "messages", "wallp_load", "Loading wallpaper...", wallp_load, 32);printf(".");
    cfg_read_string(cfg, "messages", "f_cd", "cd: no such file or directory", f_cd_mess, 40);printf(".");
    cfg_read_string(cfg, "messages", "exit_mess", "shell session dissolved. Looming into darkness", exit_mess, 50);printf(".");
    cfg_read_string(cfg, "messages", "awake_mess", "Awakening entity", awake_mess, 20);printf(".");
    cfg_read_string(cfg, "messages", "awake_failed", "Ritual failed. Cannot awake entity.", awake_failed_mess, 50);printf(".");

    char wp_file[128];
    char wp_flag[32];
    memset(wp_file, 0, 128);
    yield();
    memset(wp_flag, 0, 32);
    cfg_read_string(cfg, "wallpaper", "file", "", wp_file, 128);printf(".");
    cfg_read_string(cfg, "wallpaper", "flag", "stretch", wp_flag, 32);printf(".");

    if (wp_file[0] != '\0' && fileexists(wp_file)){
        printf("\n%s %s\n",wallp_load,wp_file);
        uint32_t mode = 0;
        if (strcmp(wp_flag, "tile") == 0) mode = 1;
        else if (strcmp(wp_flag, "stretch") == 0) mode = 2;
        uint32_t scr_pitch = 0, scr_bpp = 0, scr_width = 0, scr_height = 0;

        get_fb_info(&scr_pitch, &scr_bpp, &scr_width, &scr_height);
        if (scr_width == 0 || scr_height == 0 || scr_bpp == 0){
            scr_width = 1024; scr_height = 768; scr_bpp = 32; scr_pitch = 1024 * 4;
        }

        uint8_t *wp_buffer = load_tga_to_buffer(wp_file, scr_width, scr_height, scr_bpp, scr_pitch, mode);
        if (wp_buffer){
            uint32_t wp_size_bytes = scr_height * scr_pitch;
            int res = set_wallpaper(wp_buffer, wp_size_bytes);
            free_pages(wp_buffer, wp_size_bytes);
            if (res == 0) clear_screen();
        }
    }

    cfg_close(cfg);
    printf("\n");
}



//Функция вычитки и печати внешнего текстового файла справки
void show_external_help(void) {
    int fd = open(help_path, FS_OPEN_READ);
    if (fd < 0) {
        printf("%s: %s\n", appname, help_notfound);
        return;
    }
    char buffer[HELP_BUF_SIZE + 2];
    int bytes_read;
    //Поточно вычитываем файл справки чанками по 254 байта
    while (1) {
        memset(buffer, 0, HELP_BUF_SIZE + 2);
        bytes_read = u_read(fd, buffer, HELP_BUF_SIZE);
        if (bytes_read <= 0) {
            break;
        }
        buffer[bytes_read] = '\0';
        printf("%s", buffer);//Выводим текст в TTY поток консоли Сехмет
    }
    close(fd);
    printf("\n");
}


int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    char cmd_buf[MAX_CMD_LEN];
    char temp_parse_buf[MAX_CMD_LEN];
    char *args[MAX_ARGS];
    char cwd_buf[256];

    //u_kbd_set_mode(INPUT_MODE_COOKED);
    u_tty_set_flags(TTY_KBD_RAW, TTY_KBD_RAW);

    load_config();
    chdir("/");
    printf("\n%s\n", greeting);
    printf("%s\n\n", greeting2);

    while (1){
        memset(cwd_buf, 0, 256);
        getcwd(cwd_buf, 255);

        //printf("%s :\[%s\]>", shell_prompt, cwd_buf);
        char prompt_buf[192];
        build_prompt(prompt_buf, sizeof(prompt_buf), cwd_buf);
        printf("%s", prompt_buf);

        sekhmet_read_line(cmd_buf, MAX_CMD_LEN);

        trim_newline(cmd_buf);
        if (cmd_buf[0] == '\0') continue;

        strcpy(temp_parse_buf, cmd_buf);

        int cmd_argc = parse_command(cmd_buf, args);
        if (cmd_argc == 0) continue;

        int redirect_stdout_fd = -1;
        int redirect_stdin_fd  = -1;
        int saved_stdout       = -1;
        int saved_stdin        = -1;

        // БРОНИРОВАННЫЙ Userspace-парсер знаков перенаправления
        for (int i = 0; i < cmd_argc; i++) {
            if (args[i] != NULL && strcmp(args[i], ">") == 0 && i + 1 < cmd_argc) {
                char *filename = args[i + 1];
                for (int k = 0; filename[k] != '\0'; k++) {
                    if (filename[k] == '\r' || filename[k] == '\n' || filename[k] == ' ') {
                        filename[k] = '\0';
                        break;
                    }
                }
                // 1) Сохраняем настоящий STDOUT в РЕЗЕРВНЫЙ слот (15), не пересекающийся с open()
                saved_stdout = MAX_PROCESS_FDS - 1;
                dup2(STDOUT, saved_stdout);

                // 2) Открываем файл — SYS_OPEN даст ему слот 3
                redirect_stdout_fd = open(filename,
                                  FS_OPEN_CREATE | FS_OPEN_WRITE | FS_OPEN_TRUNCATE);
                if (redirect_stdout_fd >= 0) {
                    // 3) Переключаем STDOUT на файл
                    dup2(redirect_stdout_fd, STDOUT);
                    // close() здесь НЕ вызываем — иначе (без refcount) убьём сам файл.
                } else {
                    // Откат, если файл не открылся
                    dup2(saved_stdout, STDOUT);
                    close(saved_stdout);
                    saved_stdout = -1;
                }

                // Убираем "> filename" из args
                for (int j = i; j < cmd_argc - 2; j++) args[j] = args[j + 2];
                args[cmd_argc - 1] = NULL;
                args[cmd_argc - 2] = NULL;
                cmd_argc -= 2;
                i--;
                break;
            }

        }


        for (int i = 0; i < cmd_argc; i++) {
            if (args[i] != NULL) {
                int len = strlen(args[i]);
                if (args[i][0] == '"' && args[i][len - 1] == '"') {
                    args[i][len - 1] = '\0';
                    args[i] = &args[i][1];
                }
            }
        }

        if (strcmp(args[0], "help") == 0) show_external_help();
        else if (strcmp(args[0], "clear") == 0) clear_screen();
        else if (strcmp(args[0], "cd") == 0) {
            const char *target = "/";
            if (cmd_argc >= 2) target = args[1];
            int res = chdir(target);
            if (res < 0) printf("%s: %s\n", f_cd_mess, target);
        }
        else if (strcmp(args[0], "echo") == 0) {
            for (int i = 1; i < cmd_argc; i++) {
                if (args[i] != NULL) printf("%s", args[i]);
                if (i < (cmd_argc - 1) && args[i + 1] != NULL) printf(" ");
            }
            printf("\n");
        }
        else if (strcmp(args[0], "exit") == 0) {
            printf("%s: %s\n", appname, exit_mess);
            return 0;
        } else {
            char resolved_path[128];
            memset(resolved_path, 0, sizeof(resolved_path));
            if (find_executable(args[0], resolved_path, 128) /*&& is_valid_executable(resolved_path)*/){
                int child_pid = u_spawn(resolved_path, temp_parse_buf);
                if (child_pid >= 0) u_wait_deity(child_pid);
                else printf("%s: %s\n", appname, awake_failed_mess);
            } else {
                printf("%s: %s: %s\n", appname, f_execute_mess, args[0]);
            }
        }

        if (saved_stdout >= 0) {
            dup2(saved_stdout, STDOUT);
            close(saved_stdout);      // освобождаем резервный слот 15
            saved_stdout = -1;
            if (redirect_stdout_fd >= 0) {
                close(redirect_stdout_fd);
                redirect_stdout_fd = -1;
            }        }
        if (saved_stdin >= 0) {
            dup2(saved_stdin, STDIN);
            close(saved_stdin);
            saved_stdin = -1;
            if (redirect_stdin_fd >= 0) {
                close(redirect_stdin_fd);
                redirect_stdin_fd = -1;
            }
        }
    }
    return 0;
}

