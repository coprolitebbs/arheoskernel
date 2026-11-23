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

#include <stdint.h>
#include <stddef.h>
#if defined(ARHEOS_OS)
#include "../../../include-kernel/lib.h"
#else
#include <string.h>
#endif
#include "include/fm_types.h"
#include "include/fm_platform.h"
#include "include/fm_core.h"

// -------- UI-примитив (fm_ui.c) --------
void ui_init(void);
void ui_shutdown(void);
void ui_begin_frame(void);
void ui_end_frame(void);
void ui_goto(int x, int y);
void ui_color(int fg, int bg, int bold);
void ui_reset_color(void);
void ui_inverse(void);
void ui_puts(const char *s);
void ui_putc(char c);
void ui_pad(int count);
void ui_putn(const char *s, int max);
void ui_putn_eol(const char *s, int max);
void ui_clear_eol(void);

// ============================================================
// Палитра в духе kload_screen.h, сведённая к 8-цветному ANSI
// (kconsole.c понимает только SGR 30-37 / 40-47 + 1/0 bold)
// ============================================================
#define FM_BG          4   // blue   — фон панелей
#define FM_FG          7   // white  — базовый текст на панели
#define FM_BRDR_FG     7   // white  — рамка
#define FM_TITLE_FG    7   // white  — путь в заголовке
#define FM_DIR_FG      3   // yellow — каталоги
#define FM_FILE_FG     7   // white  — файлы
#define FM_SEL_FG      0   // black  — выделенная строка (текст)
#define FM_SEL_BG      6   // cyan   — выделенная строка (фон)
#define FM_STATUS_FG   0   // black  — статус-бар
#define FM_STATUS_BG   6   // cyan
#define FM_HELP_FG     0   // black  — нижняя подсказка
#define FM_HELP_BG     7   // light-gray
#define FM_SB_THUMB_FG 6   // cyan   — бегунок скроллбара
#define FM_SB_TRACK_FG 4   // blue   — трек скроллбара (сливается с bg)

// ============================================================
// Псевдографика (U+2500..U+2518, Block: Box Drawing)
// Все глифы занимают ровно 1 знакоместо; в UTF-8 — 3 байта.
// Если на каком-то шрифте их вдруг нет — определите FM_ASCII_BOX,
// и рамки вернутся к проверенному "+-|".
// ============================================================
//#define FM_ASCII_BOX 1

#ifdef FM_ASCII_BOX
  #define BOX_TL "+"
  #define BOX_TR "+"
  #define BOX_BL "+"
  #define BOX_BR "+"
  #define BOX_H  "-"
  #define BOX_V  "|"
  #define BOX_THUMB " "
  #define BOX_TRACK " "
#else
  #define BOX_TL "\xE2\x94\x8C"   // ┌
  #define BOX_TR "\xE2\x94\x90"   // ┐
  #define BOX_BL "\xE2\x94\x94"   // └
  #define BOX_BR "\xE2\x94\x98"   // ┘
  #define BOX_H  "\xE2\x94\x80"   // ─
  #define BOX_V  "\xE2\x94\x82"   // │
  #define BOX_THUMB "\xE2\x96\x88" // █ (U+2588, есть в шрифте — thoth)
  #define BOX_TRACK "\xE2\x96\x91" // ░ (U+2591, «светлый»)
#endif

// ============================================================
// Панель
// ============================================================

typedef struct {
    char        path[FM_PATH_MAX];
    fm_dirent_t entries[FM_MAX_ENTRIES];
    int         count;
    int         selected;
    int         scroll;
    int         x, y, w, h;
} panel_t;

static panel_t  g_left, g_right;
static panel_t *g_active;
static int      g_quit_requested = 0;
static char     g_status[256];
static uint32_t g_screen_cols, g_screen_rows;

// -------- Утилиты --------

static void set_status(const char *msg)
{
    int i = 0;
    while (msg[i] && i < 255) { g_status[i] = msg[i]; i++; }
    g_status[i] = '\0';
}

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void join_path(char *out, const char *base, const char *name)
{
    int i = 0;
    while (base[i] && i < FM_PATH_MAX - 2) { out[i] = base[i]; i++; }
    if (i > 0 && out[i-1] != '/' && i < FM_PATH_MAX - 2) out[i++] = '/';
    int j = 0;
    while (name[j] && i < FM_PATH_MAX - 1) out[i++] = name[j++];
    out[i] = '\0';
}

static void parent_path(char *out, const char *path)
{
    if (strcmp(path, "/") == 0) { out[0] = '/'; out[1] = 0; return; }
    int len = strlen(path);
    if (len > 1 && path[len-1] == '/') len--;
    while (len > 1 && path[len-1] != '/') len--;
    if (len == 1) { out[0] = '/'; out[1] = 0; return; }
    int i;
    for (i = 0; i < len - 1; i++) out[i] = path[i];
    out[i] = 0;
}

static int is_dir(const fm_dirent_t *e)
{
    return (e->attributes & FM_ATTR_DIRECTORY) != 0;
}

static int cmp_entries(const fm_dirent_t *a, const fm_dirent_t *b)
{
    int da = is_dir(a) ? 0 : 1;
    int db = is_dir(b) ? 0 : 1;
    if (da != db) return da - db;
    return strcmp(a->name, b->name);
}

static void sort_entries(fm_dirent_t *arr, int n)
{
    for (int i = 1; i < n; i++) {
        fm_dirent_t key = arr[i];
        int j = i - 1;
        while (j >= 0 && cmp_entries(&arr[j], &key) > 0) {
            arr[j+1] = arr[j];
            j--;
        }
        arr[j+1] = key;
    }
}

// -------- Загрузка директории --------

static int load_panel(panel_t *p)
{
    p->count = 0;
    p->selected = 0;
    p->scroll = 0;

    if (strcmp(p->path, "/") != 0) {
        str_copy(p->entries[0].name, "..", FM_NAME_MAX);
        p->entries[0].attributes = FM_ATTR_DIRECTORY;
        p->entries[0].size = 0;
        p->count = 1;
    }

    fm_dir_handle_t *dir = fm_opendir(p->path);
    if (!dir) return FM_ERR;

    fm_dirent_t e;
    while (fm_readdir(dir, &e) == FM_OK) {
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
        if (p->count >= FM_MAX_ENTRIES) break;
        p->entries[p->count++] = e;
    }
    fm_closedir(dir);

    int base = (strcmp(p->path, "/") != 0) ? 1 : 0;
    sort_entries(&p->entries[base], p->count - base);
    return FM_OK;
}

// ============================================================
// Рендеринг
// ============================================================

static void scrollbar_geom(int total, int visible,
                           int *out_thumb_h, int *out_thumb_top,
                           int scroll)
{
    *out_thumb_h   = 0;
    *out_thumb_top = 0;
    if (total <= visible) return;

    int th = (visible * visible) / total;
    if (th < 1) th = 1;
    if (th > visible) th = visible;
    *out_thumb_h = th;

    int max_top = total - visible;
    if (max_top > 0 && visible > th)
        *out_thumb_top = (scroll * (visible - th)) / max_top;
}

static void draw_entry(fm_dirent_t *e, int sel, int is_active, int content_w)
{
    if (sel && is_active) {
        ui_color(FM_SEL_FG, FM_SEL_BG, 0);         // black on cyan
    } else if (is_dir(e)) {
        ui_color(FM_DIR_FG, FM_BG, 0);             // yellow on blue
    } else {
        ui_color(FM_FILE_FG, FM_BG, 0);            // white on blue
    }

    if (is_dir(e)) {
        ui_putc('['); ui_putc('D'); ui_putc(']'); ui_putc(' ');
        int mn = content_w - 4;
        if (mn < 1) mn = 1;
        int nl = (int)strlen(e->name);
        if (nl > mn) nl = mn;
        for (int i = 0; i < nl; i++) ui_putc(e->name[i]);
        for (int i = nl; i < mn; i++) ui_putc(' ');
    } else if (content_w >= 14) {
        ui_putc(' '); ui_putc(' '); ui_putc(' '); ui_putc(' ');
        int mn = content_w - 13;
        if (mn < 1) mn = 1;
        int nl = (int)strlen(e->name);
        if (nl > mn) nl = mn;
        for (int i = 0; i < nl; i++) ui_putc(e->name[i]);
        for (int i = nl; i < mn; i++) ui_putc(' ');

        char sz[12]; int sn = 0;
        uint32_t v = e->size;
        if (!v) sz[sn++] = '0';
        while (v) { sz[sn++] = '0' + (v % 10); v /= 10; }
        while (sn < 8) sz[sn++] = ' ';
        while (sn) ui_putc(sz[--sn]);
        ui_putc(' ');
    } else {
        ui_putc(' '); ui_putc(' ');
        int mn = content_w - 2;
        if (mn < 1) mn = 1;
        int nl = (int)strlen(e->name);
        if (nl > mn) nl = mn;
        for (int i = 0; i < nl; i++) ui_putc(e->name[i]);
        for (int i = nl; i < mn; i++) ui_putc(' ');
    }
}

static void draw_panel(panel_t *p, int is_active)
{
    int inner_w = p->w - 2;
    int inner_h = p->h - 2;
    if (inner_w < 1 || inner_h < 1) return;

    int show_sb   = (p->count > inner_h);
    int content_w = show_sb ? inner_w - 1 : inner_w;

    // Автоскролл (до отрисовки — чтобы рамка и контент были согласованы)
    if (p->selected < p->scroll) p->scroll = p->selected;
    if (p->selected >= p->scroll + inner_h)
        p->scroll = p->selected - inner_h + 1;
    if (p->scroll < 0) p->scroll = 0;

    int thumb_h = 0, thumb_top = 0;
    if (show_sb)
        scrollbar_geom(p->count, inner_h, &thumb_h, &thumb_top, p->scroll);

    // ---- Верхняя рамка: ┌──── /path ────┐ ----
    ui_goto(p->x, p->y);
    ui_color(FM_BRDR_FG, FM_BG, 0);
    ui_puts(BOX_TL);

    // ВАЖНО: title_w == inner_w, а не inner_w - 2.
    // Два угловых глифа (BOX_TL/BOX_TR) добавляются отдельно,
    // поэтому середина должна занимать ровно внутреннюю ширину.
    int title_w = inner_w;
    if (title_w < 1) title_w = 1;

    int path_len = (int)strlen(p->path);
    if (path_len > title_w) path_len = title_w;
    int lpad = (title_w - path_len) / 2;
    int rpad = title_w - lpad - path_len;

    for (int i = 0; i < lpad; i++) ui_puts(BOX_H);
    ui_color(FM_TITLE_FG, FM_BG, 1);              // путь — bold
    for (int i = 0; i < path_len; i++) ui_putc(p->path[i]);
    ui_color(FM_BRDR_FG, FM_BG, 0);
    for (int i = 0; i < rpad; i++) ui_puts(BOX_H);
    ui_puts(BOX_TR);

    // ---- Контентные строки ----
    for (int row = 0; row < inner_h; row++) {
        int idx = p->scroll + row;
        ui_goto(p->x, p->y + 1 + row);

        // Левая рамка
        ui_color(FM_BRDR_FG, FM_BG, 0);
        ui_puts(BOX_V);

        // Контент
        if (idx < p->count) {
            draw_entry(&p->entries[idx], idx == p->selected, is_active, content_w);
        } else {
            // Пустая строка должна явно закрасить blue-фон, иначе
            // ui_pad напишет пробелы без bg, и старая надпись из
            // предыдущей директории останется в фреймбуфере ARHeOS
            ui_color(FM_FG, FM_BG, 0);
            ui_pad(content_w);
        }

        // Скроллбар
        if (show_sb) {
            if (row >= thumb_top && row < thumb_top + thumb_h) {
                ui_color(FM_SB_THUMB_FG, FM_BG, 1);   // █ cyan bold
                ui_puts(BOX_THUMB);
            } else {
                ui_color(FM_SB_TRACK_FG, FM_BG, 0);   // ░ blue on blue
                ui_puts(BOX_TRACK);
            }
        }

        // Правая рамка
        ui_color(FM_BRDR_FG, FM_BG, 0);
        ui_puts(BOX_V);
    }

    // ---- Нижняя рамка ----
    ui_goto(p->x, p->y + p->h - 1);
    ui_color(FM_BRDR_FG, FM_BG, 0);
    ui_puts(BOX_BL);
    for (int i = 0; i < inner_w; i++) ui_puts(BOX_H);
    ui_puts(BOX_BR);
    ui_reset_color();
}

static void draw_status_and_help(void)
{
    int status_y = (int)g_screen_rows - 2;
    int help_y   = (int)g_screen_rows - 1;

    ui_goto(0, status_y);
    ui_color(FM_STATUS_FG, FM_STATUS_BG, 0);
    ui_putn_eol(g_status, (int)g_screen_cols);
    ui_reset_color();

    const char *help =
        " F3 View  F4 Edit  F5 Copy  F6 Move  F7 MkDir  F8 Del  F10 Quit ";
    ui_goto(0, help_y);
    ui_color(FM_HELP_FG, FM_HELP_BG, 1);
    ui_putn_eol(help, (int)g_screen_cols);
    ui_reset_color();
}

static void render_all(void)
{
    ui_begin_frame();
    draw_panel(&g_left,  g_active == &g_left);
    draw_panel(&g_right, g_active == &g_right);
    draw_status_and_help();
    ui_end_frame();
}

// ============================================================
// Модальный ввод (F7 mkdir, F8 delete-prompt)
// ============================================================

static int prompt_input(const char *label, char *out, int max)
{
    int len = 0;
    out[0] = 0;

    while (1) {
        int y = (int)g_screen_rows - 3;

        ui_begin_frame();
        draw_panel(&g_left,  g_active == &g_left);
        draw_panel(&g_right, g_active == &g_right);
        draw_status_and_help();

        ui_goto(0, y);
        ui_color(FM_HELP_FG, FM_HELP_BG, 1);
        for (int i = 0; label[i]; i++) ui_putc(label[i]);
        for (int i = 0; i < len; i++) ui_putc(out[i]);
        ui_putc('_');
        int used = (int)strlen(label) + len + 1;
        if (used < (int)g_screen_cols) ui_pad((int)g_screen_cols - used);
        ui_reset_color();
        ui_end_frame();

        int k = fm_read_key();
        if (k == FM_KEY_ENTER)     { out[len] = 0; return FM_OK; }
        if (k == FM_KEY_ESC)       { out[0] = 0;    return FM_ERR; }
        if (k == FM_KEY_BACKSPACE) { if (len > 0) len--; continue; }
        if (FM_KEY_IS_CHAR(k)) {
            char c = (char)FM_KEY_GET_CHAR(k);
            if (c >= 32 && c < 127 && len < max - 1) out[len++] = c;
        }
    }
}

// ============================================================
// Файловые операции
// ============================================================

static int copy_file(const char *src, const char *dst)
{
    int in = fm_open(src, FM_OPEN_READ);
    if (in < 0) return FM_ERR;
    int out = fm_open(dst, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (out < 0) { fm_close(in); return FM_ERR; }

    char buf[4096];
    int n, ok = 1;
    while ((n = fm_read(in, buf, sizeof(buf))) > 0) {
        if (fm_write(out, buf, (uint32_t)n) != n) { ok = 0; break; }
    }
    if (n < 0) ok = 0;
    fm_close(in);
    fm_close(out);
    return ok ? FM_OK : FM_ERR;
}

static int copy_recursive(const char *src, const char *dst, int depth)
{
    if (depth > 32) return FM_ERR;

    fm_stat_t st;
    if (fm_stat(src, &st) != FM_OK) return FM_ERR;

    if (!(st.attributes & FM_ATTR_DIRECTORY))
        return copy_file(src, dst);

    if (fm_mkdir(dst) != FM_OK) {
        fm_stat_t tmp;
        if (fm_stat(dst, &tmp) != FM_OK || !(tmp.attributes & FM_ATTR_DIRECTORY))
            return FM_ERR;
    }

    fm_dir_handle_t *dir = fm_opendir(src);
    if (!dir) return FM_ERR;

    fm_dirent_t e;
    int res = FM_OK;
    while (fm_readdir(dir, &e) == FM_OK) {
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
        char s[FM_PATH_MAX], d[FM_PATH_MAX];
        join_path(s, src, e.name);
        join_path(d, dst, e.name);
        if (copy_recursive(s, d, depth + 1) != FM_OK) { res = FM_ERR; break; }
    }
    fm_closedir(dir);
    return res;
}

static int delete_recursive(const char *path, int depth)
{
    if (depth > 32) return FM_ERR;

    fm_stat_t st;
    if (fm_stat(path, &st) != FM_OK) return FM_ERR;

    if (!(st.attributes & FM_ATTR_DIRECTORY))
        return fm_unlink(path);

    fm_dir_handle_t *dir = fm_opendir(path);
    if (!dir) return FM_ERR;

    fm_dirent_t e;
    while (fm_readdir(dir, &e) == FM_OK) {
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
        char p[FM_PATH_MAX];
        join_path(p, path, e.name);
        delete_recursive(p, depth + 1);
    }
    fm_closedir(dir);
    return fm_rmdir(path);
}

static int move_path(const char *src, const char *dst)
{
    if (fm_rename(src, dst) == FM_OK) return FM_OK;
    if (copy_recursive(src, dst, 0) != FM_OK) return FM_ERR;
    return delete_recursive(src, 0);
}

static void open_in_thoth(const char *path)
{
    char cmd[FM_PATH_MAX + 16];
    int i = 0;
    const char *p = "thoth ";
    while (*p) cmd[i++] = *p++;
    int j = 0;
    while (path[j] && i < (int)sizeof(cmd) - 1) cmd[i++] = path[j++];
    cmd[i] = 0;

    int pid = fm_spawn("/bin/thoth.elf", cmd);
    if (pid < 0) { set_status("thoth: spawn failed"); return; }
    fm_wait(pid);
}

// ============================================================
// Операции
// ============================================================

static void act_enter(panel_t *p)
{
    if (p->selected >= p->count) return;
    fm_dirent_t *e = &p->entries[p->selected];

    if (strcmp(e->name, "..") == 0) {
        char parent[FM_PATH_MAX];
        parent_path(parent, p->path);
        str_copy(p->path, parent, FM_PATH_MAX);
        load_panel(p);
        return;
    }

    char full[FM_PATH_MAX];
    join_path(full, p->path, e->name);

    if (is_dir(e)) {
        str_copy(p->path, full, FM_PATH_MAX);
        load_panel(p);
    } else {
        open_in_thoth(full);
    }
}

static void act_copy_move(panel_t *src, panel_t *dst, int is_move)
{
    if (src->selected >= src->count) return;
    fm_dirent_t *e = &src->entries[src->selected];
    if (strcmp(e->name, "..") == 0) return;

    char src_path[FM_PATH_MAX], dst_path[FM_PATH_MAX];
    join_path(src_path, src->path, e->name);
    join_path(dst_path, dst->path, e->name);

    set_status(is_move ? "Moving..." : "Copying...");
    render_all();
    fm_yield();

    int r = is_move ? move_path(src_path, dst_path)
                    : copy_recursive(src_path, dst_path, 0);

    set_status(r == FM_OK ? "Done." : "Operation failed.");
    load_panel(src);
    load_panel(dst);
}

static void act_mkdir(panel_t *p)
{
    char name[FM_NAME_MAX];
    if (prompt_input("mkdir: ", name, FM_NAME_MAX) != FM_OK || !name[0]) return;
    char full[FM_PATH_MAX];
    join_path(full, p->path, name);
    if (fm_mkdir(full) != FM_OK) set_status("mkdir: failed");
    else                         set_status("mkdir: ok");
    load_panel(p);
}

static void act_delete(panel_t *p)
{
    if (p->selected >= p->count) return;
    fm_dirent_t *e = &p->entries[p->selected];
    if (strcmp(e->name, "..") == 0) return;

    char prompt[FM_NAME_MAX + 32];
    int i = 0;
    const char *pre = "Delete ";
    while (*pre) prompt[i++] = *pre++;
    int j = 0;
    while (e->name[j]) prompt[i++] = e->name[j++];
    prompt[i++] = '?'; prompt[i++] = ' '; prompt[i] = 0;

    char dummy[4];
    if (prompt_input(prompt, dummy, sizeof(dummy)) != FM_OK) return;

    char full[FM_PATH_MAX];
    join_path(full, p->path, e->name);
    if (delete_recursive(full, 0) != FM_OK) set_status("delete: failed");
    else                                    set_status("delete: ok");
    load_panel(p);
}

// ============================================================
// Главный цикл
// ============================================================

int fm_run(void)
{
    fm_platform_init();
    fm_get_geometry(&g_screen_cols, &g_screen_rows);

    ui_init();

    if (g_screen_rows < 6 || g_screen_cols < 40) {
        ui_goto(0, 0);
        ui_color(FM_FG, FM_BG, 0);
        ui_puts("Terminal too small: need at least 40x6");
        ui_reset_color();
        fm_read_key();
        ui_shutdown();
        fm_platform_shutdown();
        return 0;
    }

    int half = (int)(g_screen_cols - 2) / 2;
    int panel_h = (int)g_screen_rows - 2;

    g_left.x = 0;         g_left.y = 0;  g_left.w = half;                          g_left.h = panel_h;
    g_right.x = half + 2; g_right.y = 0; g_right.w = (int)g_screen_cols - (half + 2); g_right.h = panel_h;

    str_copy(g_left.path,  "/", FM_PATH_MAX);
    str_copy(g_right.path, "/", FM_PATH_MAX);
    load_panel(&g_left);
    load_panel(&g_right);

    g_active = &g_left;
    set_status("AR Commander - Tab: switch panel");
    render_all();

    while (!g_quit_requested) {
        int k = fm_read_key();

        panel_t *p     = g_active;
        panel_t *other = (p == &g_left) ? &g_right : &g_left;

        switch (k) {
        case FM_KEY_UP:
            if (p->selected > 0) p->selected--;
            render_all(); break;
        case FM_KEY_DOWN:
            if (p->selected < p->count - 1) p->selected++;
            render_all(); break;
        case FM_KEY_HOME:
            p->selected = 0; render_all(); break;
        case FM_KEY_END:
            p->selected = p->count - 1; if (p->selected < 0) p->selected = 0;
            render_all(); break;
        case FM_KEY_PGUP:
            p->selected -= (p->h - 2); if (p->selected < 0) p->selected = 0;
            render_all(); break;
        case FM_KEY_PGDN:
            p->selected += (p->h - 2);
            if (p->selected >= p->count) p->selected = p->count - 1;
            if (p->selected < 0) p->selected = 0;
            render_all(); break;

        case FM_KEY_ENTER:
            act_enter(p); render_all(); break;

        case FM_KEY_TAB:
            g_active = other; render_all(); break;

        case FM_KEY_F3:
        case FM_KEY_F4: {
            if (p->selected < p->count) {
                fm_dirent_t *e = &p->entries[p->selected];
                if (strcmp(e->name, "..") != 0 && !is_dir(e)) {
                    char full[FM_PATH_MAX];
                    join_path(full, p->path, e->name);
                    open_in_thoth(full);
                    render_all();
                }
            }
            break;
        }
        case FM_KEY_F5: act_copy_move(p, other, 0); render_all(); break;
        case FM_KEY_F6: act_copy_move(p, other, 1); render_all(); break;
        case FM_KEY_F7: act_mkdir(p);     render_all(); break;
        case FM_KEY_F8: act_delete(p);    render_all(); break;
        case FM_KEY_F10: g_quit_requested = 1; break;
        default: break;
        }
    }

    ui_shutdown();
    fm_platform_shutdown();
    return 0;
}

void fm_request_exit(void) { g_quit_requested = 1; }
