#include "include-kernel/kconsole.h"
#include <stdint.h>
#include <stddef.h>
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/use_drivers.h"
#include "include-kernel/syscalls.h"
#include "include-kernel/config.h"

uint32_t console_bgcolor = 0x00000000;
uint32_t console_fgcolor = 0x00FFFFFF;

static uint8_t utf8_lead_byte = 0;

tty_t tty_terminals[TTY_TERMINALS_COUNT];
tty_t *current_tty = nullptr;


void kconsole_get_render_coords(uint32_t buffer_idx, uint32_t *out_x, uint32_t *out_y){
    if (!current_tty){
        *out_x = 0; *out_y = 0;
        return;
    }

    uint32_t total_chars_offset = (current_tty->input_start_x / 8) + buffer_idx;
    uint32_t cell_x = total_chars_offset % CONSOLE_COLS;

    uint32_t cell_y = current_tty->input_start_y + (total_chars_offset / CONSOLE_COLS);
    if (cell_y >= CONSOLE_ROWS){
        cell_y = CONSOLE_ROWS - 1;
    }

    *out_x = cell_x * 8;
    *out_y = cell_y * LINE_HEIGHT;
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
            fb->draw_char(cx, cy, current_tty->cursor_code, console_fgcolor);
        } else {
            uint32_t cell_x = cx / 8;
            uint32_t cell_y = cy / LINE_HEIGHT;
            char original_char = ' ';

            if (cell_y < CONSOLE_ROWS && cell_x < CONSOLE_COLS){
                original_char = current_tty->matrix[cell_y][cell_x].ascii;
            }
            if (fb && fb->clear_tile) {
                fb->clear_tile(cx, cy, console_bgcolor);
            } else {
                fb->draw_char(cx, cy, current_tty->cursor_code, console_bgcolor);
            }

            //Если на этом месте был символ текста — перерисовываем его поверх обоев
            if (original_char != ' ' && original_char != '\0'){
                fb->draw_char(cx, cy, original_char, console_fgcolor);
            }
        }

        __asm__ __volatile__("mov %0, %%cr3" : : "r"(old_cr3));
    }
}



void kernel_stdout_write_char(char c, uint32_t color){
    if (!fb || !current_tty) return;

    uint8_t b = (uint8_t)c;

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

    //Вспомогательная лямбда/макрос для автоматического сдвига сетки активного ввода вниз
    void sync_input_grid_down(void) {
        //Если строка еще не готова (значит, приложение сейчас висит в SYS_READ и ждет ввода),
        //мы обязаны сдвинуть её стартовую строку вниз вслед за печатью фонового процесса
        if (current_tty->line_ready == false) {
            current_tty->input_start_y++;
            if (current_tty->input_start_y >= CONSOLE_ROWS) {
                current_tty->input_start_y = CONSOLE_ROWS - 1;
            }
        }
    }

    if (current_tty->cursor_x >= (CONSOLE_COLS * 8)){
        current_tty->cursor_x = 0;
        current_tty->cursor_y += LINE_HEIGHT;
        if (current_tty->cursor_y >= (CONSOLE_ROWS * LINE_HEIGHT)){
            kconsole_scroll_up();
        } else {
            sync_input_grid_down();
        }
    }

    if (b == '\n'){
        current_tty->cursor_x = 0;
        current_tty->cursor_y += LINE_HEIGHT;

        if (current_tty->cursor_y >= (CONSOLE_ROWS * LINE_HEIGHT)){
            kconsole_scroll_up();
        } else {
            sync_input_grid_down();
        }
        return;
    }

    if (b == '\r'){
        current_tty->cursor_x = 0;
        return;
    }

    if (b == '\t'){
        current_tty->cursor_x += 32;
        if (current_tty->cursor_x >= (CONSOLE_COLS * 8)){
            current_tty->cursor_x = 0;
            current_tty->cursor_y += LINE_HEIGHT;
            if (current_tty->cursor_y >= (CONSOLE_ROWS * LINE_HEIGHT)){
                kconsole_scroll_up();
            } else {
                sync_input_grid_down();
            }
        }
        return;
    }

    uint32_t cell_x = current_tty->cursor_x / 8;
    uint32_t cell_y = current_tty->cursor_y / LINE_HEIGHT;

    if (cell_y < CONSOLE_ROWS && cell_x < CONSOLE_COLS){
        current_tty->matrix[cell_y][cell_x].ascii = (char)b;
        current_tty->matrix[cell_y][cell_x].color = color;
    }


    fb->draw_char(current_tty->cursor_x, current_tty->cursor_y, b, color);
    current_tty->cursor_x += 8;

    if (current_tty->cursor_x >= (CONSOLE_COLS * 8)){
        current_tty->cursor_x = 0;
        current_tty->cursor_y += LINE_HEIGHT;

        if (current_tty->cursor_y >= (CONSOLE_ROWS * LINE_HEIGHT)){
            kconsole_scroll_up();
        } else {
            sync_input_grid_down();
        }
    }
}


void kconsole_clear_current_line(void){
    if (!fb || !current_tty) return;
    uint32_t bg_color = console_bgcolor;
    //if (fb->get_bg){ bg_color = fb->get_bg(); }

    uint32_t current_row = current_tty->cursor_y / LINE_HEIGHT;
    for (uint32_t c = 0; c < CONSOLE_COLS; c++){
        if (current_row < CONSOLE_ROWS){
            current_tty->matrix[current_row][c].ascii = ' ';
        }
        //fb->draw_char(c * 8, current_tty->cursor_y, 0xDB, bg_color);
        if (fb && fb->clear_tile) {
            fb->clear_tile(c * 8, current_tty->cursor_y, console_bgcolor);
        } else {
            fb->draw_char(c * 8, current_tty->cursor_y, current_tty->cursor_code, console_bgcolor);
        }
    }
}


void kconsole_scroll_up(void){
    if (!current_tty) return;

    for (uint32_t r = 0; r < CONSOLE_ROWS - 1; r++){
        for (uint32_t c = 0; c < CONSOLE_COLS; c++){
            current_tty->matrix[r][c] = current_tty->matrix[r + 1][c];
        }
    }
    for (uint32_t c = 0; c < CONSOLE_COLS; c++){
        current_tty->matrix[CONSOLE_ROWS - 1][c].ascii = ' ';
        current_tty->matrix[CONSOLE_ROWS - 1][c].color = console_fgcolor;
    }

    if (fb && fb->scroll_up) {
        fb->scroll_up(0, CONSOLE_ROWS * LINE_HEIGHT, LINE_HEIGHT);
    } else {
        kconsole_redraw_screen();
    }

    // Если экран скроллится вверх из-за вывода фонового процесса,
    // мы ОБЯЗАНЫ подтянуть логическую сетку ввода Сехмет вверх вслед за пикселями!
    if (current_tty->input_start_y > 0) {
        current_tty->input_start_y--;
    }

    current_tty->cursor_y = (CONSOLE_ROWS - 1) * LINE_HEIGHT;
}


void kconsole_redraw_screen(void){
    if (!fb || !fb->clear || !fb->draw_char || !current_tty) return;
    fb->clear(console_bgcolor);

    for (uint32_t r = 0; r < CONSOLE_ROWS; r++){
        uint32_t pixel_y = r * LINE_HEIGHT;
        for (uint32_t c = 0; c < CONSOLE_COLS; c++){
            char ch = current_tty->matrix[r][c].ascii;
            if (ch != ' ' && ch != '\0'){
                fb->draw_char(c * 8, pixel_y, (unsigned char)ch, current_tty->matrix[r][c].color);
            }
        }
    }
}


char kconsole_get_char_at(uint32_t col, uint32_t row){
    if (!current_tty || col >= CONSOLE_COLS || row >= CONSOLE_ROWS) return '\0';
    return current_tty->matrix[row][col].ascii;
}


void kconsole_clear_screen(void){
    if (!fb || !current_tty) return;
    for (uint32_t r = 0; r < CONSOLE_ROWS; r++){
        for (uint32_t c = 0; c < CONSOLE_COLS; c++){
            current_tty->matrix[r][c].ascii = ' ';
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
        tty_terminals[t].cursor_code = DEFAULT_ASCII_CURSOR_CODE;

        for (uint32_t r = 0; r < CONSOLE_ROWS; r++){
            for (uint32_t c = 0; c < CONSOLE_COLS; c++) {
                tty_terminals[t].matrix[r][c].ascii = ' ';
                tty_terminals[t].matrix[r][c].color = console_fgcolor;
            }
        }
        for (int i = 0; i < LINE_BUFFER_SIZE; i++) {
            tty_terminals[t].line_buffer[i] = 0;
        }
    }
    current_tty = &tty_terminals[0];
}


void kconsole_reconfigure(cfg_file_t *cfg){
    //cfg_file_t *cfg = cfg_open("/config/boot/boot.cfg", FS_OPEN_READ,true);
    if (cfg < 0) {
        kprintf("[ACPI] Warning: boot.cfg not found, keeping fallback theme.\n");
        return;
    }
    // Вычитываем шестнадцатеричные палитры из секции [console] с использованием config
    uint32_t new_bg = cfg_read_hex(cfg, "console", "bgcolor", console_bgcolor);
    uint32_t new_fg = cfg_read_hex(cfg, "console", "fgcolor", console_fgcolor);
    console_bgcolor = new_bg;
    console_fgcolor = new_fg;
    if (fb) {
        // Прописываем новые цвета низкоуровневому видеоадаптеру
        fb->set_bg(console_bgcolor);
        fb->set_fg(console_fgcolor);
        // Накатываем новые цвета текстовых символов на матрицы всех виртуальных консолей TTY
        for (int t = 0; t < TTY_TERMINALS_COUNT; t++){
            for (uint32_t r = 0; r < CONSOLE_ROWS; r++){
                for (uint32_t c = 0; c < CONSOLE_COLS; c++){
                    tty_terminals[t].matrix[r][c].color = console_fgcolor;
                }
            }
        }
        // Чисто заливаем видеоматрицу экрана новым цветом фона
        kconsole_clear_screen();
        kprintf("[ACPI] Display reconfigured dynamically. Theme applied successfully.\n");
    }
    //cfg_close(cfg);
}
