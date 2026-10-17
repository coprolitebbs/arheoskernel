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

#include "include-kernel/kconsole.h"
#include <stdint.h>
#include <stddef.h>
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/use_drivers.h"
#include "include-kernel/syscalls.h"
#include "include-kernel/config.h"
#include "include-kernel/locale.h"
#include "include-kernel/kbd_translate.h"
#include "include-kernel/lib.h"


uint32_t console_bgcolor = 0x00000000;
uint32_t console_fgcolor = 0x00FFFFFF;

//Состояние UTF-8 декодера для байтового входного потока
static uint32_t utf8_acc   = 0;
static uint32_t utf8_min   = 0;
static int      utf8_need  = 0;

static ansi_state_t ansi_state = ANSI_STATE_NORMAL;
static char ansi_buffer[ANSI_BUF_SIZE];
static uint32_t ansi_buf_idx = 0;

static const uint32_t ansi_color_table[8] = {
    0x00000000, //0: Черный
    0x00AA0000, //1: Красный
    0x0000AA00, //2: Зеленый
    0x00AA5500, //3: Коричневый/Желтый
    0x000000AA, //4: Синий
    0x00AA00AA, //5: Пурпурный
    0x0000AAAA, //6: Циановый
    0x00AAAAAA  //7: Светло-серый
};

tty_t tty_terminals[TTY_TERMINALS_COUNT];
tty_t *current_tty = nullptr;


//Сбрасывает незавершённую UTF-8 последовательность
//Вызывать при переключении TTY/задач и на старте строки
void kconsole_reset_utf8_state(void){
    utf8_acc  = 0;
    utf8_min  = 0;
    utf8_need = 0;
    ansi_state = ANSI_STATE_NORMAL;
    ansi_buf_idx = 0;
}

//Прогоняет один байт через UTF-8 декодер
//Возвращает 0, если последовательность ещё не завершена (нужны ещё байты).
//Возвращает codepoint, если байт завершил последовательность
//Возвращает 0xFFFD на битой последовательности (и сбрасывает состояние)
static uint32_t utf8_feed_byte(uint8_t b){
    if (utf8_need == 0){
        if (b < 0x80) return b;
        else if ((b & 0xE0) == 0xC0){ utf8_acc = b & 0x1F; utf8_need = 1; utf8_min = 0x80;    return 0; }
        else if ((b & 0xF0) == 0xE0){ utf8_acc = b & 0x0F; utf8_need = 2; utf8_min = 0x800;   return 0; }
        else if ((b & 0xF8) == 0xF0){ utf8_acc = b & 0x07; utf8_need = 3; utf8_min = 0x10000; return 0; }
        else return 0xFFFD;  //битый lead-байт
    }
    if ((b & 0xC0) != 0x80){ utf8_need = 0; return 0xFFFD; }  //не continuation
    utf8_acc = (utf8_acc << 6) | (b & 0x3F);
    utf8_need--;
    if (utf8_need > 0) return 0;
    //Проверки переполнения/суррогатов
    if (utf8_acc < utf8_min)                          return 0xFFFD;
    if (utf8_acc >= 0xD800 && utf8_acc <= 0xDFFF)     return 0xFFFD;
    if (utf8_acc > 0x10FFFF)                          return 0xFFFD;
    return utf8_acc;
}




static void parse_and_execute_ansi(char cmd){
    ansi_buffer[ansi_buf_idx] = '\0';
    //Переменные для парсинга числовых аргументов (поддерживаем до 3 параметров)
    int args[3] = {0, 0, 0};
    int arg_count = 0;
    char *p = ansi_buffer;
    while (*p && arg_count < 3){
        if (*p >= '0' && *p <= '9'){
            args[arg_count] = 0;
            while (*p >= '0' && *p <= '9'){
                args[arg_count] = args[arg_count] * 10 + (*p - '0');
                p++;
            }
            arg_count++;
        } else {
            p++;
        }
    }
    switch (cmd){
        case 'm':{ //Спецификатор SGR (Управление цветом и стилем)
            if (arg_count == 0 || args[0] == 0){
                // \x1B[0m — Сброс цветов
                console_fgcolor = DEFAULT_FG_COLOR;
                console_bgcolor = DEFAULT_BG_COLOR;
                if (fb && fb->set_fg) fb->set_fg(DEFAULT_FG_COLOR);
                if (fb && fb->set_bg) fb->set_bg(DEFAULT_BG_COLOR);
            } else {
                for (int i = 0; i < arg_count; i++){
                    int code = args[i];
                    //Цвет текста (30-37)
                    if (code >= 30 && code <= 37){
                        console_fgcolor = ansi_color_table[code - 30];
                        if (fb && fb->set_fg) fb->set_fg(console_fgcolor);
                    }
                    //Цвет фона (40-47)
                    else if (code >= 40 && code <= 47){
                        console_bgcolor = ansi_color_table[code - 40];
                        if (fb && fb->set_bg) fb->set_bg(console_bgcolor);
                    }
                }
            }
            break;
        }
        case 'H': //Спецификатор CUP (Позиционирование курсора)
        case 'f':{
            if (arg_count >= 2){
                //Терминальные координаты ANSI начинаются с 1,1
                int row = args[0] > 0 ? args[0] - 1 : 0;
                int col = args[1] > 0 ? args[1] - 1 : 0;
                //Защита от выхода за границы текстовой матрицы консоли
                if (row >= current_tty->console_rows) row = current_tty->console_rows - 1;//CONSOLE_ROWS - 1;
                if (col >= current_tty->console_cols) col = current_tty->console_cols - 1;//CONSOLE_COLS - 1;
                current_tty->cursor_x = col * current_tty->cell_w;//8;
                current_tty->cursor_y = row * current_tty->cell_h;//LINE_HEIGHT;
            } else {
                // \x1B[H — Перевод в начало (Home)
                current_tty->cursor_x = 0;
                current_tty->cursor_y = 0;
            }
            break;
        }
        case 'J':{ //Спецификатор ED (Очистка экрана)
            if (arg_count > 0 && args[0] == 2) {
                // \x1B[2J — Полная очистка экрана и сброс курсора
                kconsole_clear_screen();
            }
            break;
        }
        case 'K':{ // Erase in Line: 0 = to end, 1 = to start, 2 = whole line
                uint32_t mode = (arg_count > 0) ? args[0] : 0;
                uint32_t row = current_tty->cursor_y / current_tty->cell_h;
                //Да, я и так могу
                uint32_t start_col = (mode == 2) ? 0 : (mode == 1) ? 0 : (current_tty->cursor_x / current_tty->cell_w);
                uint32_t end_col   = (mode == 1) ? (current_tty->cursor_x / current_tty->cell_w) : current_tty->console_cols;
                for (uint32_t c = start_col; c < end_col; c++){
                    if (row < current_tty->console_rows){
                        current_tty->matrix[row][c].codepoint = ' ';
                    }
                    if (fb && fb->clear_tile) fb->clear_tile(c * current_tty->cell_w, row * current_tty->cell_h, console_bgcolor);
                }
            break;
        }
        case 'D':{// Cursor Left
            uint32_t n = (arg_count > 0) ? args[0] : 1;
            uint32_t cell_x = current_tty->cursor_x / current_tty->cell_w;
            if (cell_x >= n) cell_x -= n; else cell_x = 0;
            current_tty->cursor_x = cell_x * current_tty->cell_w;

            break;
        }
        default:
            break;
    }
}


//Преобразует строковое имя комбинации из конфига в её числовой код HOTKEY_*.
//Принимает обе формы записи: "ALT_SHIFT" и "HOTKEY_ALT_SHIFT".
//При опечатке возвращает безопасный дефолт, не падает и не оставляет мусор
static uint8_t cfg_parse_hotkey(const char *name){
    if (!name || name[0] == '\0') return HOTKEY_ALT_SHIFT;
    const char *p = name;
    //Отбрасываем необязательный префикс HOTKEY_
    if (p[0] == 'H' && p[1] == 'O' && p[2] == 'T' && p[3] == 'K' &&
        p[4] == 'E' && p[5] == 'Y' && p[6] == '_'){
        p += 7;
    }
    if (strcmp(p, "NONE") == 0)       return HOTKEY_NONE;
    if (strcmp(p, "ALT_SHIFT") == 0)  return HOTKEY_ALT_SHIFT;
    if (strcmp(p, "CTRL_SHIFT") == 0) return HOTKEY_CTRL_SHIFT;
    if (strcmp(p, "ALT_ALONE") == 0)  return HOTKEY_ALT_ALONE;
    if (strcmp(p, "CAPS_LOCK") == 0)  return HOTKEY_CAPS_LOCK;
    if (strcmp(p, "CTRL_SPACE") == 0) return HOTKEY_CTRL_SPACE;
    return HOTKEY_ALT_SHIFT;
}



void kconsole_get_render_coords(uint32_t buffer_idx, uint32_t *out_x, uint32_t *out_y){
    if (!current_tty){
        *out_x = 0; *out_y = 0;
        return;
    }

    uint32_t total_chars_offset = (current_tty->input_start_x / current_tty->cell_w) + buffer_idx;
    uint32_t cell_x = total_chars_offset % current_tty->console_cols;

    uint32_t cell_y = current_tty->input_start_y + (total_chars_offset / current_tty->console_cols);
    if (cell_y >= current_tty->console_rows){
        cell_y = current_tty->console_rows - 1;
    }

    *out_x = cell_x * current_tty->cell_w;
    *out_y = cell_y * current_tty->cell_h;
}


void kconsole_update_cursor(void){
    extern volatile uint32_t tick_count;
    if (!current_tty || current_tty->foreground_pid == 0 || current_tty->line_ready) return;

    if (tick_count - current_tty->last_blink_tick >= current_tty->cursor_tick_count){
        current_tty->last_blink_tick = tick_count;
        current_tty->cursor_visible = !current_tty->cursor_visible;

        uint32_t cx, cy;
        kconsole_get_render_coords(current_tty->line_cursor_pos, &cx, &cy);

        uint32_t old_cr3;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(old_cr3));
        if (current_task && current_task->pid != 0) {
            __asm__ __volatile__("mov %0, %%cr3" : : "r"(current_task->page_dir_phys));
        }

        if (current_tty->cursor_visible){
            //fb->draw_char(cx, cy, current_tty->cursor_code, console_fgcolor);
            fb->draw_codepoint(cx, cy, current_tty->cursor_code, console_fgcolor);
        } else {
            uint32_t cell_x = cx / current_tty->cell_w;
            uint32_t cell_y = cy / current_tty->cell_h;
            uint32_t original_char = ' ';

            if (cell_y < current_tty->console_rows && cell_x < current_tty->console_cols){
                original_char = current_tty->matrix[cell_y][cell_x].codepoint;
            }
            if (fb && fb->clear_tile) {
                fb->clear_tile(cx, cy, console_bgcolor);
            } else {
                //fb->draw_char(cx, cy, current_tty->cursor_code, console_bgcolor);
                fb->draw_codepoint(cx, cy, current_tty->cursor_code, console_bgcolor);
            }

            //Если на этом месте был символ текста — перерисовываем его поверх обоев
            if (original_char != ' ' && original_char != '\0'){
                //fb->draw_char(cx, cy, original_char, console_fgcolor);
                fb->draw_codepoint(cx, cy, original_char, console_fgcolor);
            }
        }

        __asm__ __volatile__("mov %0, %%cr3" : : "r"(old_cr3));
    }
}


/*
void kernel_stdout_write_char(char c, uint32_t color){
    if (!fb || !current_tty) return;
    uint8_t b = (uint8_t)c;

    if (ansi_state == ANSI_STATE_NORMAL){
        if (b == 0x1B) {
            utf8_lead_byte = 0;
            ansi_state = ANSI_STATE_ESC;
            return;
        }
    }
    else if (ansi_state == ANSI_STATE_ESC){
        if (b == '['){
            ansi_state = ANSI_STATE_CSI;
            ansi_buf_idx = 0;
            return;
        } else {
            ansi_state = ANSI_STATE_NORMAL;
        }
    }
    else if (ansi_state == ANSI_STATE_CSI){
        if ((b >= '0' && b <= '9') || b == ';'){
            if (ansi_buf_idx < (ANSI_BUF_SIZE - 1)){
                ansi_buffer[ansi_buf_idx++] = b;
            }
            return;
        }
        else if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z')){
            parse_and_execute_ansi(b);
            ansi_state = ANSI_STATE_NORMAL;
            return;
        } else {
            ansi_state = ANSI_STATE_NORMAL;
            return;
        }
    }

    void sync_input_grid_down(void){
        if (current_tty->line_ready == false){
            current_tty->input_start_y++;
            if (current_tty->input_start_y >= current_tty->console_rows){
                current_tty->input_start_y = current_tty->console_rows - 1;
            }
        }
    }

    if (b == 0xD0 || b == 0xD1){
        utf8_lead_byte = b;
        return;
    }

    if (utf8_lead_byte != 0){
        uint8_t cp866_char = ' ';
        if (utf8_lead_byte == 0xD0){
            if (b >= 0x90 && b <= 0xBF){
                if (b <= 0xAF) cp866_char = b - 0x90 + 0x80;
                else           cp866_char = b - 0xB0 + 0xA0;
            }
            else if (b == 0x81) cp866_char = 0xF0;
        }
        else if (utf8_lead_byte == 0xD1){
            if (b >= 0x80 && b <= 0x8F)  cp866_char = b - 0x80 + 0xE0;
            else if (b == 0x91)          cp866_char = 0xF1;
        }
        b = cp866_char;
        utf8_lead_byte = 0;
    }

    if (b == '\b'){
        uint32_t current_row = current_tty->cursor_y / current_tty->cell_h;
        if (current_row > current_tty->input_start_y){
            if (current_tty->cursor_x >= current_tty->cell_w) {
                current_tty->cursor_x -= current_tty->cell_w;
            } else {
                current_tty->cursor_y -= current_tty->cell_h;
                current_tty->cursor_x = (current_tty->console_cols - 1) * current_tty->cell_w;
            }
            uint32_t cell_x = current_tty->cursor_x / current_tty->cell_w;
            uint32_t cell_y = current_tty->cursor_y / current_tty->cell_h;
            if (cell_y < current_tty->console_rows && cell_x < current_tty->console_cols){
                current_tty->matrix[cell_y][cell_x].ascii = ' ';
            }
            if (fb && fb->clear_tile){
                fb->clear_tile(current_tty->cursor_x, current_tty->cursor_y, console_bgcolor);
            } else if (fb && fb->draw_char) {
                fb->draw_char(current_tty->cursor_x, current_tty->cursor_y, ' ', console_bgcolor);
            }
        }
        else if (current_row == current_tty->input_start_y){
            if (current_tty->cursor_x > current_tty->input_start_x){
                current_tty->cursor_x -= current_tty->cell_w;
                uint32_t cell_x = current_tty->cursor_x / current_tty->cell_w;
                uint32_t cell_y = current_tty->cursor_y / current_tty->cell_h;
                if (cell_y < current_tty->console_rows && cell_x < current_tty->console_cols){
                    current_tty->matrix[cell_y][cell_x].ascii = ' ';
                }
                if (fb && fb->clear_tile){
                    fb->clear_tile(current_tty->cursor_x, current_tty->cursor_y, console_bgcolor);
                } else if (fb && fb->draw_char){
                    fb->draw_char(current_tty->cursor_x, current_tty->cursor_y, ' ', console_bgcolor);
                }
            }
        }
        return;
    }

    if (current_tty->cursor_x >= (current_tty->console_cols * current_tty->cell_w)){
        current_tty->cursor_x = 0;
        current_tty->cursor_y += current_tty->cell_h;
        if (current_tty->cursor_y >= (current_tty->console_rows * current_tty->cell_h)){
            kconsole_scroll_up();
        } else {
            sync_input_grid_down();
        }
    }

    if (b == '\n'){
        current_tty->cursor_x = 0;
        current_tty->cursor_y += current_tty->cell_h;
        if (current_tty->cursor_y >= (current_tty->console_rows * current_tty->cell_h)){
            kconsole_scroll_up();
        } else {
            sync_input_grid_down();
        }
        return;
    }

    if (b == '\r'){
        utf8_lead_byte = 0;
        ansi_state = ANSI_STATE_NORMAL;
        ansi_buf_idx = 0;
        current_tty->cursor_x = 0;
        return;
    }

    if (b == '\t'){
        current_tty->cursor_x += 4 * current_tty->cell_w;
        if (current_tty->cursor_x >= (current_tty->console_cols * current_tty->cell_w)){
            current_tty->cursor_x = 0;
            current_tty->cursor_y += current_tty->cell_h;
            if (current_tty->cursor_y >= (current_tty->console_rows * current_tty->cell_h)){
                kconsole_scroll_up();
            } else {
                sync_input_grid_down();
            }
        }
        return;
    }

    uint32_t cell_x = current_tty->cursor_x / current_tty->cell_w;
    uint32_t cell_y = current_tty->cursor_y / current_tty->cell_h;
    if (cell_y < current_tty->console_rows && cell_x < current_tty->console_cols){
        current_tty->matrix[cell_y][cell_x].ascii = b;
        current_tty->matrix[cell_y][cell_x].color = color;
    }
    if (fb && fb->draw_char){
        fb->draw_char(current_tty->cursor_x, current_tty->cursor_y, b, color);
    }
    current_tty->cursor_x += current_tty->cell_w;
    if (current_tty->cursor_x >= (current_tty->console_cols * current_tty->cell_w)){
        current_tty->cursor_x = 0;
        current_tty->cursor_y += current_tty->cell_h;
        if (current_tty->cursor_y >= (current_tty->console_rows * current_tty->cell_h)){
            kconsole_scroll_up();
        } else {
            sync_input_grid_down();
        }
    }
}
*/

void kernel_stdout_write_codepoint(uint32_t cp, uint32_t color){
    if (!fb || !current_tty) return;

    void sync_input_grid_down(void){
        if (current_tty->line_ready == false){
            current_tty->input_start_y++;
            if (current_tty->input_start_y >= current_tty->console_rows)
                current_tty->input_start_y = current_tty->console_rows - 1;
        }
    }

    if (cp == '\b'){
        uint32_t current_row = current_tty->cursor_y / current_tty->cell_h;
        if (current_row > current_tty->input_start_y){
            if (current_tty->cursor_x >= current_tty->cell_w) current_tty->cursor_x -= current_tty->cell_w;
            else { current_tty->cursor_y -= current_tty->cell_h;
                   current_tty->cursor_x = (current_tty->console_cols - 1) * current_tty->cell_w; }
            uint32_t cell_x = current_tty->cursor_x / current_tty->cell_w;
            uint32_t cell_y = current_tty->cursor_y / current_tty->cell_h;
            if (cell_y < current_tty->console_rows && cell_x < current_tty->console_cols)
                current_tty->matrix[cell_y][cell_x].codepoint = ' ';
            if (fb && fb->clear_tile) fb->clear_tile(current_tty->cursor_x, current_tty->cursor_y, console_bgcolor);
            else if (fb && fb->draw_codepoint) fb->draw_codepoint(current_tty->cursor_x, current_tty->cursor_y, ' ', console_bgcolor);
        }
        else if (current_row == current_tty->input_start_y){
            if (current_tty->cursor_x > current_tty->input_start_x){
                current_tty->cursor_x -= current_tty->cell_w;
                uint32_t cell_x = current_tty->cursor_x / current_tty->cell_w;
                uint32_t cell_y = current_tty->cursor_y / current_tty->cell_h;
                if (cell_y < current_tty->console_rows && cell_x < current_tty->console_cols)
                    current_tty->matrix[cell_y][cell_x].codepoint = ' ';
                if (fb && fb->clear_tile) fb->clear_tile(current_tty->cursor_x, current_tty->cursor_y, console_bgcolor);
                else if (fb && fb->draw_codepoint) fb->draw_codepoint(current_tty->cursor_x, current_tty->cursor_y, ' ', console_bgcolor);
            }
        }
        return;
    }

    if (current_tty->cursor_x >= (current_tty->console_cols * current_tty->cell_w)){
        current_tty->cursor_x = 0;
        current_tty->cursor_y += current_tty->cell_h;
        if (current_tty->cursor_y >= (current_tty->console_rows * current_tty->cell_h)) kconsole_scroll_up();
        else sync_input_grid_down();
    }

    if (cp == '\n'){
        current_tty->cursor_x = 0;
        current_tty->cursor_y += current_tty->cell_h;
        if (current_tty->cursor_y >= (current_tty->console_rows * current_tty->cell_h)) kconsole_scroll_up();
        else sync_input_grid_down();
        return;
    }

    if (cp == '\r'){ current_tty->cursor_x = 0; return; }

    if (cp == '\t'){
        current_tty->cursor_x += 4 * current_tty->cell_w;
        if (current_tty->cursor_x >= (current_tty->console_cols * current_tty->cell_w)){
            current_tty->cursor_x = 0;
            current_tty->cursor_y += current_tty->cell_h;
            if (current_tty->cursor_y >= (current_tty->console_rows * current_tty->cell_h)) kconsole_scroll_up();
            else sync_input_grid_down();
        }
        return;
    }

    uint32_t cell_x = current_tty->cursor_x / current_tty->cell_w;
    uint32_t cell_y = current_tty->cursor_y / current_tty->cell_h;
    if (cell_y < current_tty->console_rows && cell_x < current_tty->console_cols){
        current_tty->matrix[cell_y][cell_x].codepoint = cp;
        current_tty->matrix[cell_y][cell_x].color = color;
    }
    if (fb && fb->draw_codepoint){
        fb->draw_codepoint(current_tty->cursor_x, current_tty->cursor_y, cp, color);
    }
    current_tty->cursor_x += current_tty->cell_w;
    if (current_tty->cursor_x >= (current_tty->console_cols * current_tty->cell_w)){
        current_tty->cursor_x = 0;
        current_tty->cursor_y += current_tty->cell_h;
        if (current_tty->cursor_y >= (current_tty->console_rows * current_tty->cell_h)) kconsole_scroll_up();
        else sync_input_grid_down();
    }
}




void kernel_stdout_write_char(char c, uint32_t color){
    if (!fb || !current_tty) return;
    uint8_t b = (uint8_t)c;

    //ANSI state machine — байтовый, как и раньше
    if (ansi_state == ANSI_STATE_NORMAL){
        if (b == 0x1B){ utf8_need = 0; ansi_state = ANSI_STATE_ESC; return; }
    } else if (ansi_state == ANSI_STATE_ESC){
        if (b == '['){ ansi_state = ANSI_STATE_CSI; ansi_buf_idx = 0; return; }
        else { ansi_state = ANSI_STATE_NORMAL; }
    } else if (ansi_state == ANSI_STATE_CSI){
        if ((b >= '0' && b <= '9') || b == ';'){
            if (ansi_buf_idx < ANSI_BUF_SIZE - 1) ansi_buffer[ansi_buf_idx++] = b;
            return;
        }
        if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z')){
            parse_and_execute_ansi(b);
            ansi_state = ANSI_STATE_NORMAL;
            return;
        }
        ansi_state = ANSI_STATE_NORMAL;
        return;
    }

    //UTF-8 декодирование
    uint32_t cp = utf8_feed_byte(b);
    if (cp == 0) return;  //ещё не собрано

    kernel_stdout_write_codepoint(cp, color);
}



void kconsole_clear_current_line(void){
    if (!fb || !current_tty) return;
    uint32_t bg_color = console_bgcolor;
    //if (fb->get_bg){ bg_color = fb->get_bg(); }

    uint32_t current_row = current_tty->cursor_y / current_tty->cell_h;
    for (uint32_t c = 0; c < current_tty->console_cols; c++){
        if (current_row < current_tty->console_rows){
            current_tty->matrix[current_row][c].codepoint = ' ';
        }
        //fb->draw_char(c * 8, current_tty->cursor_y, 0xDB, bg_color);
        if (fb && fb->clear_tile) {
            fb->clear_tile(c * current_tty->cell_w, current_tty->cursor_y, console_bgcolor);
        } else {
            //fb->draw_char(c * current_tty->cell_w, current_tty->cursor_y, current_tty->cursor_code, console_bgcolor);
            fb->draw_codepoint(c * current_tty->cell_w, current_tty->cursor_y, current_tty->cursor_code, console_bgcolor);
        }
    }
}


void kconsole_scroll_up(void){
    if (!current_tty) return;

    for (uint32_t r = 0; r < current_tty->console_rows - 1; r++){
        for (uint32_t c = 0; c < current_tty->console_cols; c++){
            current_tty->matrix[r][c] = current_tty->matrix[r + 1][c];
        }
    }
    for (uint32_t c = 0; c < current_tty->console_cols; c++){
        current_tty->matrix[current_tty->console_rows - 1][c].codepoint = ' ';
        current_tty->matrix[current_tty->console_rows - 1][c].color = console_fgcolor;
    }

    if (fb && fb->scroll_up){
        fb->scroll_up(0, current_tty->console_rows * current_tty->cell_h, current_tty->cell_h);
    } else {
        kconsole_redraw_screen();
    }
    //Если экран скроллится вверх из-за вывода фонового процесса,
    //мы должны подтянуть логическую сетку ввода вверх вслед за пикселями
    if (current_tty->input_start_y > 0){
        current_tty->input_start_y--;
    }

    current_tty->cursor_y = (current_tty->console_rows - 1) * current_tty->cell_h;
}


void kconsole_redraw_screen(void){
    if (!fb || !fb->clear || !fb->draw_codepoint || !current_tty) return;
    fb->clear(console_bgcolor);

    for (uint32_t r = 0; r < current_tty->console_rows; r++){
        uint32_t pixel_y = r * current_tty->cell_h;
        for (uint32_t c = 0; c < current_tty->console_cols; c++){
            uint32_t ch = current_tty->matrix[r][c].codepoint;
            if (ch != ' ' && ch != 0){
                fb->draw_codepoint(c * current_tty->cell_w, pixel_y, ch, current_tty->matrix[r][c].color);
            }
        }
    }
}


uint32_t kconsole_get_char_at(uint32_t col, uint32_t row){
    if (!current_tty || col >= current_tty->console_cols || row >= current_tty->console_rows) return 0;
    return current_tty->matrix[row][col].codepoint;
}


void kconsole_clear_screen(void){
    if (!fb || !current_tty) return;
    for (uint32_t r = 0; r < current_tty->console_rows; r++){
        for (uint32_t c = 0; c < current_tty->console_cols; c++){
            current_tty->matrix[r][c].codepoint = ' ';
            current_tty->matrix[r][c].color = console_fgcolor;
        }
    }
    current_tty->cursor_x = 0;
    current_tty->cursor_y = 0;
    current_tty->input_start_x = 0;
    current_tty->input_start_y = 0;
    current_tty->line_cursor_pos = 0;
    fb->clear(console_bgcolor);
    current_tty->cursor_visible = true;
    current_tty->last_blink_tick = tick_count;
}


//printf for kernel
void kprintf(const char *format, ...){
    if (!format) return;

    __builtin_va_list args;
    __builtin_va_start(args, format);

    const char *ptr = format;
    while (*ptr != '\0') {
        if (*ptr == '%') {
            ptr++;
            if (*ptr == 'c') {
                char char_val = (char)__builtin_va_arg(args, int);
                kernel_stdout_write_char(char_val, console_fgcolor);
            }
            else if (*ptr == 's') {
                const char *str_val = __builtin_va_arg(args, const char *);
                if (str_val) {
                    while (*str_val != '\0') {
                        kernel_stdout_write_char(*str_val, console_fgcolor);
                        str_val++;
                    }
                } else {
                    const char *null_str = "(null)";
                    while (*null_str != '\0') {
                        kernel_stdout_write_char(*null_str, console_fgcolor);
                        null_str++;
                    }
                }
            }
            else if (*ptr == 'd' || *ptr == 'x' || *ptr == 'p') {
                uint32_t num_val = __builtin_va_arg(args, uint32_t);
                char num_buf[32];
                int i = 0;
                uint32_t temp = num_val;
                int base = (*ptr == 'x' || *ptr == 'p') ? 16 : 10;

                if (*ptr == 'p') {
                    kernel_stdout_write_char('0', console_fgcolor);
                    kernel_stdout_write_char('x', console_fgcolor);
                }

                if (temp == 0) num_buf[i++] = '0';
                while (temp > 0) {
                    int r = temp % base;
                    num_buf[i++] = (r < 10) ? (r + '0') : (r - 10 + 'A');
                    temp /= base;
                }

                for (int j = i - 1; j >= 0; j--) {
                    kernel_stdout_write_char(num_buf[j], console_fgcolor);
                }
            }
        } else {
            kernel_stdout_write_char(*ptr, console_fgcolor);
        }
        ptr++;
    }
    __builtin_va_end(args);
}



void kconsole_init(void){
    kprintf("Console init...");
    if(fb){
        fb->set_bg(console_bgcolor);
        fb->set_fg(console_fgcolor);
        fb->clear(console_bgcolor);
    }

    for (int t = 0; t < TTY_TERMINALS_COUNT; t++) {
        tty_terminals[t].id = t;
        tty_terminals[t].copied_bytes = 0;
        tty_terminals[t].line_ready = false;
        tty_terminals[t].input_start_x = 0;
        tty_terminals[t].input_start_y = 0;
        tty_terminals[t].cursor_x = 0;
        tty_terminals[t].cursor_y = 0;
        tty_terminals[t].foreground_pid = 0;
        tty_terminals[t].cursor_tick_count = MAX_CURSOR_TICK_COUNT;
        tty_terminals[t].line_cursor_pos = 0;
        tty_terminals[t].cursor_visible = false;
        tty_terminals[t].last_blink_tick = 0;
        tty_terminals[t].cursor_code = DEFAULT_CURSOR_CODEPOINT; // 0x2588
        tty_terminals[t].active_layout = 0;
        tty_terminals[t].console_cols = CONSOLE_COLS;
        tty_terminals[t].console_rows = CONSOLE_ROWS;
        tty_terminals[t].cell_w = 8;
        tty_terminals[t].cell_h = LINE_HEIGHT;

        for (uint32_t r = 0; r < CONSOLE_ROWS; r++){
            for (uint32_t c = 0; c < CONSOLE_COLS; c++) {
                tty_terminals[t].matrix[r][c].codepoint = ' ';
                tty_terminals[t].matrix[r][c].color = console_fgcolor;
            }
        }
        for (int i = 0; i < LINE_BUFFER_SIZE; i++) {
            tty_terminals[t].line_buffer[i] = 0;
        }
    }
    current_tty = &tty_terminals[0];
    kprintf("OK\n");
}


void kconsole_reconfigure(cfg_file_t *cfg){
    if (!cfg){
        kprintf("[ACPI] Warning: boot.cfg not found, keeping fallback theme.\n");
        return;
    }
    //Вычитываем шестнадцатеричные палитры из секции [console] с использованием config
    uint32_t new_bg = cfg_read_hex(cfg, "console", "bgcolor", console_bgcolor);
    uint32_t new_fg = cfg_read_hex(cfg, "console", "fgcolor", console_fgcolor);
    console_bgcolor = new_bg;
    console_fgcolor = new_fg;

    char hotkey_name[32] = {0};
    cfg_read_string(cfg, "console", "layout_change_keys", "HOTKEY_ALT_SHIFT", hotkey_name, 32);
    locale_hotkey = cfg_parse_hotkey(hotkey_name);
    //kprintf("[HOTKEY] loaded: %s (%d)\n", hotkey_name, locale_hotkey);
    //При загрузке ядра активной становится первая зарегистрированная локаль ("en" из font_vga_init),
    //находим её индекс в пуле и выставляем всем консолям TTY как стартовый
    const locale_t *active = locale_get_active();
    if (active){
        for (int t = 0; t < TTY_TERMINALS_COUNT; t++){
            for (uint8_t i = 0; i < LOCALE_POOL_MAX; i++){
                const locale_t *l = locale_get_by_index(i);
                if (!l) continue;
                //Сравниваем по строковому коду локали, а не по указателю
                const char *a = l->code;
                const char *b = active->code;
                while (*a && *a == *b){ a++; b++; }
                if (*a == *b){
                    tty_terminals[t].active_layout = i;
                    uint32_t cw = active->font->cell_width;
                    uint32_t ch = active->font->cell_height;
                    if (cw == 0) cw = LINE_HEIGHT;
                    if (ch == 0) ch = LINE_HEIGHT;
                    uint32_t cols = (uint32_t)boot_info->width  / cw;
                    uint32_t rows = (uint32_t)boot_info->height / ch;
                    if (cols > CONSOLE_COLS) cols = CONSOLE_COLS;
                    if (rows > CONSOLE_ROWS) rows = CONSOLE_ROWS;
                    if (cols == 0) cols = 1;
                    if (rows == 0) rows = 1;
                    tty_terminals[t].console_cols = cols;
                    tty_terminals[t].console_rows = rows;
                    tty_terminals[t].cell_w = cw;
                    tty_terminals[t].cell_h = ch;
                    break;
                }
            }
        }
    }



    if (fb){
        //Прописываем новые цвета низкоуровневому видеоадаптеру
        fb->set_bg(console_bgcolor);
        fb->set_fg(console_fgcolor);
        //Накатываем новые цвета текстовых символов на матрицы всех виртуальных консолей TTY
        for (int t = 0; t < TTY_TERMINALS_COUNT; t++){
            for (uint32_t r = 0; r < tty_terminals[t].console_rows; r++){
                for (uint32_t c = 0; c < tty_terminals[t].console_cols; c++){
                    tty_terminals[t].matrix[r][c].color = console_fgcolor;
                }
            }
        }
        //Чисто заливаем видеоматрицу экрана новым цветом фона
        kconsole_clear_screen();
        kprintf("[ACPI] Display reconfigured dynamically. Theme applied successfully.\n");
    }
}



