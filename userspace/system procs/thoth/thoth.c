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

#include "include/thoth.h"
#include "include/thoth_platform.h"

#if defined(ARHEOS_OS)
    extern void *malloc(size_t);
    extern void  free(void *);
#else
    #include <stdlib.h>
#endif

static editor_t ed;

static bool     blink_on = true;
static uint32_t last_blink_tick = 0;

static uint8_t build_index_buf[4096];

static editor_snapshot_t undo_stack[UNDO_MAX_DEPTH];
static uint32_t          undo_count = 0;
static uint32_t          undo_bytes = 0;

static editor_snapshot_t redo_stack[UNDO_MAX_DEPTH];
static uint32_t          redo_count = 0;
static uint32_t          redo_bytes = 0;

static sel_state_t sel_state = SEL_NONE;
static uint32_t sel_anchor_line = 0;
static uint32_t sel_anchor_byte = 0;
static uint32_t sel_end_line    = 0;
static uint32_t sel_end_byte    = 0;

//низкоуровневые хелперы

static void m_memmove(void *dst, const void *src, uint32_t n){
    char *d = (char*)dst; const char *s = (const char*)src;
    if (d < s) for (uint32_t i=0;i<n;i++) d[i]=s[i];
    else if (d > s) for (uint32_t i=n;i>0;i--) d[i-1]=s[i-1];
}
static uint32_t m_strlen(const char *s){ uint32_t n=0; while(s[n]) n++; return n; }
static void m_strcpy(char *d, const char *s, uint32_t max){
    uint32_t i=0; while (s[i] && i<max-1){ d[i]=s[i]; i++; } d[i]=0;
}

static void m_memcpy_zero(void *p, uint32_t n){ uint8_t *b = (uint8_t*)p; for (uint32_t i=0;i<n;i++) b[i]=0; }



//консоль (ANSI)

static void out_str(const char *s){ uint32_t n=m_strlen(s); if(n) tp_write(TP_STDOUT,s,n); }
static void out_byte(char c){ tp_write(TP_STDOUT,&c,1); }
static void out_uint(uint32_t v){
    char b[12]; int i=0; if(!v){ out_byte('0'); return; }
    while(v){ b[i++]='0'+(v%10); v/=10; } while(i) out_byte(b[--i]);
}
static void esc_goto(uint32_t row, uint32_t col){
    out_str("\x1B["); out_uint(row+1); out_byte(';'); out_uint(col+1); out_byte('H');
}
static void esc_clear_eol(void){ out_str("\x1B[K"); }
static void esc_color(uint32_t fg, uint32_t bg){
    out_str("\x1B["); out_uint(fg); out_byte(';'); out_uint(bg); out_byte('m');
}
static void esc_reset(void){ out_str("\x1B[0m"); }
static void esc_clear_screen(void){ out_str("\x1B[2J\x1B[H"); }

//Индекс

static int ensure_index_capacity(uint32_t need){
    if (ed.line_cap >= need) return 0;
    uint32_t nc = ed.line_cap ? ed.line_cap : 1024;
    while (nc < need) nc *= 2;
    uint32_t *nl = (uint32_t *)malloc(nc * sizeof(uint32_t));
    if (!nl) return -1;
    for (uint32_t i=0;i<ed.line_count;i++) nl[i] = ed.line_offset[i];
    if (ed.line_offset) free(ed.line_offset);
    ed.line_offset = nl;
    ed.line_cap = nc;
    return 0;
}

static int build_index(void){
    ed.line_count = 0;
    if (ensure_index_capacity(1024) < 0){ ed.line_count = 0; return -1; }
    ed.line_offset[0] = 0;

    uint32_t pos = 0;
    int r;
    uint8_t *buf = build_index_buf;
    while ((r = tp_read(ed.fd, buf, sizeof(build_index_buf))) > 0){
        for (int i = 0; i < r; i++){
            if (buf[i] == '\n'){
                if (ensure_index_capacity(ed.line_count + 2) < 0){ ed.line_count = 0; return -1; }
                ed.line_count++;
                ed.line_offset[ed.line_count] = pos + i + 1;
            }
        }
        pos += r;
    }

    if (pos == 0){
        ed.line_count = 1;
        if (ensure_index_capacity(2) < 0){ ed.line_count = 0; return -1; }
        ed.line_offset[0] = 0;
        ed.line_offset[1] = 0;
    } else if (ed.line_offset[ed.line_count] != pos){
        ed.line_count++;
        if (ensure_index_capacity(ed.line_count + 1) < 0){ ed.line_count = 0; return -1; }
        ed.line_offset[ed.line_count] = pos;
    }
    ed.file_size = pos;
    return 0;
}

//Кэш чистых строк

static void cache_invalidate_all(void){
    for (int i = 0; i < CACHE_SIZE; i++){
        if (ed.cache[i].valid && ed.cache[i].text) free(ed.cache[i].text);
        ed.cache[i].valid = false;
        ed.cache[i].text  = NULL;
    }
}

//Грязные строки

static int ensure_dirty_capacity(uint32_t need){
    if (ed.dirty_cap >= need) return 0;
    uint32_t nc = ed.dirty_cap ? ed.dirty_cap : 64;
    while (nc < need) nc *= 2;
    dirty_line_t *nd = (dirty_line_t *)malloc(nc * sizeof(dirty_line_t));
    if (!nd) return -1;
    for (uint32_t i=0;i<ed.dirty_count;i++) nd[i] = ed.dirty[i];
    if (ed.dirty) free(ed.dirty);
    ed.dirty = nd;
    ed.dirty_cap = nc;
    return 0;
}

static int dirty_find(uint32_t line_no){
    int lo = 0, hi = (int)ed.dirty_count;
    while (lo < hi){
        int mid = (lo + hi) / 2;
        if (ed.dirty[mid].line_no == line_no) return mid;
        if (ed.dirty[mid].line_no < line_no) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

static int dirty_put(uint32_t line_no, const char *text, uint32_t len){
    int lo = 0, hi = (int)ed.dirty_count;
    while (lo < hi){
        int mid = (lo + hi) / 2;
        if (ed.dirty[mid].line_no < line_no) lo = mid + 1;
        else hi = mid;
    }
    int pos = lo;

    uint32_t need = len + 1;
    char *new_text = (char *)malloc(need);
    if (!new_text) return -1;
    m_memmove(new_text, text, len);
    new_text[len] = 0;

    if (pos < (int)ed.dirty_count && ed.dirty[pos].line_no == line_no){
        if (ed.dirty[pos].text) free(ed.dirty[pos].text);
        ed.dirty[pos].text = new_text;
        ed.dirty[pos].len  = len;
        ed.dirty[pos].cap  = need;
        return 0;
    }
    if (ensure_dirty_capacity(ed.dirty_count + 1) < 0){ free(new_text); return -1; }
    for (int i = (int)ed.dirty_count; i > pos; i--) ed.dirty[i] = ed.dirty[i-1];
    ed.dirty[pos].line_no = line_no;
    ed.dirty[pos].text = new_text;
    ed.dirty[pos].len  = len;
    ed.dirty[pos].cap  = need;
    ed.dirty_count++;
    return 0;
}

static void dirty_remove_at(int idx){
    if (idx < 0 || idx >= (int)ed.dirty_count) return;
    if (ed.dirty[idx].text) free(ed.dirty[idx].text);
    for (int i = idx; i < (int)ed.dirty_count - 1; i++) ed.dirty[i] = ed.dirty[i+1];
    ed.dirty_count--;
}

static dirty_line_t *materialize_dirty(uint32_t n, uint32_t extra_capacity){
    int idx = dirty_find(n);
    if (idx >= 0){
        dirty_line_t *d = &ed.dirty[idx];
        uint32_t need = d->len + extra_capacity + 1;
        if (d->cap < need){
            char *nt = (char *)malloc(need);
            if (!nt) return NULL;
            m_memmove(nt, d->text, d->len);
            nt[d->len] = 0;
            free(d->text);
            d->text = nt;
            d->cap  = need;
        }
        return d;
    }

    cache_slot_t *slot = &ed.cache[n % CACHE_SIZE];
    const char *cur_text = NULL;
    uint32_t cur_len = 0;

    if (slot->valid && slot->line_no == n){
        cur_text = slot->text;
        cur_len  = slot->len;
    } else {
        uint32_t start = ed.line_offset[n];
        uint32_t end   = ed.line_offset[n + 1];
        uint32_t len   = end - start;
        uint32_t need = len + extra_capacity + 1;
        char *tmp = (char *)malloc(need);
        if (!tmp) return NULL;
        if (len > 0){
            tp_seek(ed.fd, start);
            int r = tp_read(ed.fd, tmp, len);
            if (r < 0 || (uint32_t)r != len){ free(tmp); return NULL; }
            if (tmp[len-1] == '\n') len--;
            if (len > 0 && tmp[len-1] == '\r') len--;
            tmp[len] = 0;
        } else {
            tmp[0] = 0;
        }
        cur_text = tmp;
        cur_len  = len;

        if (ensure_dirty_capacity(ed.dirty_count + 1) < 0){ free(tmp); return NULL; }
        int pos = 0;
        while (pos < (int)ed.dirty_count && ed.dirty[pos].line_no < n) pos++;
        for (int i = (int)ed.dirty_count; i > pos; i--) ed.dirty[i] = ed.dirty[i-1];
        ed.dirty[pos].line_no = n;
        ed.dirty[pos].text = tmp;
        ed.dirty[pos].len  = len;
        ed.dirty[pos].cap  = need;
        ed.dirty_count++;
        return &ed.dirty[pos];
    }

    if (dirty_put(n, cur_text, cur_len) < 0) return NULL;
    idx = dirty_find(n);
    return idx >= 0 ? &ed.dirty[idx] : NULL;
}

static const char *get_line_text(uint32_t n, uint32_t *out_len){
    if (!ed.line_offset || n >= ed.line_count){ *out_len = 0; return ""; }

    int di = dirty_find(n);
    if (di >= 0){ *out_len = ed.dirty[di].len; return ed.dirty[di].text; }

    cache_slot_t *slot = &ed.cache[n % CACHE_SIZE];
    if (slot->valid && slot->line_no == n){
        *out_len = slot->len;
        return slot->text;
    }

    uint32_t start = ed.line_offset[n];
    uint32_t end   = ed.line_offset[n + 1];
    uint32_t len   = end - start;

    if (slot->valid && slot->text){ free(slot->text); slot->text = NULL; slot->valid = false; }

    slot->cap = len + 1;
    slot->text = (char *)malloc(slot->cap);
    if (!slot->text){ *out_len = 0; return ""; }

    if (len > 0){
        tp_seek(ed.fd, start);
        int r = tp_read(ed.fd, slot->text, len);
        if (r < 0 || (uint32_t)r != len){
            free(slot->text); slot->text = NULL; slot->valid = false;
            *out_len = 0; return "";
        }
        if (slot->text[len-1] == '\n') len--;
        if (len > 0 && slot->text[len-1] == '\r') len--;
    }
    slot->text[len] = 0;
    slot->len = len;
    slot->line_no = n;
    slot->valid = true;
    *out_len = len;
    return slot->text;
}

//Вставка/удаление строк

static bool sel_is_active(void){ return sel_state != SEL_NONE; }

static int insert_line_at(uint32_t index){
    if (index > ed.line_count) return -1;
    if (ensure_index_capacity(ed.line_count + 2) < 0) return -1;

    for (uint32_t i = ed.line_count + 1; i > index; i--)
        ed.line_offset[i] = ed.line_offset[i-1];
    ed.line_offset[index] = (index == 0) ? 0 : ed.line_offset[index];
    ed.line_count++;

    for (uint32_t i = 0; i < ed.dirty_count; i++)
        if (ed.dirty[i].line_no >= index) ed.dirty[i].line_no++;

    for (int i = 0; i < CACHE_SIZE; i++){
        if (ed.cache[i].valid && ed.cache[i].line_no >= index){
            free(ed.cache[i].text);
            ed.cache[i].valid = false;
            ed.cache[i].text  = NULL;
        }
    }
    return 0;
}

static int delete_line_at(uint32_t index){
    if (index >= ed.line_count) return -1;

    int di = dirty_find(index);
    if (di >= 0) dirty_remove_at(di);
    for (uint32_t i = 0; i < ed.dirty_count; i++)
        if (ed.dirty[i].line_no > index) ed.dirty[i].line_no--;

    for (uint32_t i = index; i < ed.line_count; i++)
        ed.line_offset[i] = ed.line_offset[i+1];
    ed.line_count--;

    for (int i = 0; i < CACHE_SIZE; i++){
        if (ed.cache[i].valid && ed.cache[i].line_no > index){
            free(ed.cache[i].text);
            ed.cache[i].valid = false;
            ed.cache[i].text  = NULL;
        }
    }
    return 0;
}

static void sel_get_range(uint32_t *out_sl, uint32_t *out_sb,uint32_t *out_el, uint32_t *out_eb){
    uint32_t al = sel_anchor_line, ab = sel_anchor_byte;
    uint32_t el = sel_end_line,    eb = sel_end_byte;

    if (sel_state == SEL_TRACKING){
        //конец следует за курсором
        el = ed.cursor_line;
        eb = ed.cursor_byte;
    }
    //в SEL_LOCKED используем зафиксированные sel_end_*
    if (al < el || (al == el && ab <= eb)){
        *out_sl = al; *out_sb = ab; *out_el = el; *out_eb = eb;
    } else {
        *out_sl = el; *out_sb = eb; *out_el = al; *out_eb = ab;
    }
}

// загрузка/сохранение

static int load_file(const char *path){
    history_clear();
    sel_clear();
    m_strcpy(ed.filename, path, sizeof(ed.filename));
    ed.modified = false;
    ed.is_new = false;
    ed.dirty_count = 0;
    cache_invalidate_all();

    int fd = tp_open(path, TP_OPEN_READ);
    if (fd < 0){
        ed.is_new = true;
        ed.fd = -1;
        if (ensure_index_capacity(2) < 0){
            ed.line_count = 0;
            ed.file_size = 0;
            return 0;
        }
        ed.line_count = 1;
        ed.line_offset[0] = 0;
        ed.line_offset[1] = 0;
        ed.file_size = 0;
        return 0;
    }
    ed.fd = fd;
    if (build_index() < 0){ tp_close(fd); ed.fd = -1; return -1; }
    return 0;
}

static int save_file(void){
    if (!ed.filename[0]) return -1;

    char tmp_path[160];
    int nlen = m_strlen(ed.filename);
    if (nlen + 5 > (int)sizeof(tmp_path)) return -1;
    m_strcpy(tmp_path, ed.filename, sizeof(tmp_path));
    m_strcpy(tmp_path + nlen, ".tmp", 5);

    int tmp_fd = tp_open(tmp_path, TP_OPEN_WRITE | TP_OPEN_CREATE | TP_OPEN_TRUNCATE);
    if (tmp_fd < 0) return -1;

    for (uint32_t i = 0; i < ed.line_count; i++){
        uint32_t len;
        const char *text = get_line_text(i, &len);
        if (len && tp_write(tmp_fd, text, len) < 0){
            tp_close(tmp_fd); tp_unlink(tmp_path); return -1;
        }
        if (i + 1 < ed.line_count){
            if (tp_write(tmp_fd, "\n", 1) < 0){
                tp_close(tmp_fd); tp_unlink(tmp_path); return -1;
            }
        }
    }
    tp_close(tmp_fd);

    if (ed.fd >= 0){ tp_close(ed.fd); ed.fd = -1; }

    if (tp_rename(tmp_path, ed.filename) < 0){
        tp_unlink(tmp_path);
        ed.fd = tp_open(ed.filename, TP_OPEN_READ);
        return -1;
    }

    ed.fd = tp_open(ed.filename, TP_OPEN_READ);
    if (ed.fd < 0) return -1;

    history_clear();
    sel_clear();

    for (uint32_t i = 0; i < ed.dirty_count; i++)
        if (ed.dirty[i].text) free(ed.dirty[i].text);
    ed.dirty_count = 0;
    cache_invalidate_all();

    if (build_index() < 0) return -1;
    ed.modified = false;
    ed.is_new = false;
    return 0;
}

//Курсор/колонки

static uint32_t byte_to_col(const char *text, uint32_t byte_off){
    uint32_t col = 0, i = 0;
    while (i < byte_off && text[i]) {
        unsigned char c = (unsigned char)text[i];
        uint32_t bytes = 1, cols = 1;
        if (c == '\t') cols = 4;
        else if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        col += cols; i += bytes;
    }
    return col;
}
static uint32_t col_to_byte(const char *text, uint32_t line_len, uint32_t target){
    uint32_t col = 0, i = 0;
    while (i < line_len) {
        unsigned char c = (unsigned char)text[i];
        uint32_t bytes = 1, cols = 1;
        if (c == '\t') cols = 4;
        else if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        if (col + cols > target) return i;
        col += cols; i += bytes;
    }
    return i;
}
static uint32_t cursor_col(void){
    if (!ed.line_offset || ed.cursor_line >= ed.line_count) return 0;
    uint32_t len;
    const char *text = get_line_text(ed.cursor_line, &len);
    if (!text) return 0;
    return byte_to_col(text, ed.cursor_byte);
}

static bool scrollbar_visible(void){
    return ed.line_count > ed.edit_rows;
}

static void adjust_scroll(void){
    uint32_t eff_cols = ed.screen_cols;
    if (scrollbar_visible() && eff_cols > 0) eff_cols--;
    if (eff_cols < 1) eff_cols = 1;

    if (ed.cursor_line < ed.scroll_top) ed.scroll_top = ed.cursor_line;
    else if (ed.cursor_line >= ed.scroll_top + ed.edit_rows)
        ed.scroll_top = ed.cursor_line - ed.edit_rows + 1;

    uint32_t col = cursor_col();
    if (col < ed.hscroll) ed.hscroll = col;
    else if (col >= ed.hscroll + eff_cols)
        ed.hscroll = col - eff_cols + 1;
}

static void update_screen_size(void){
    uint32_t cw = 8, ch = 8;
    tp_geometry(&ed.screen_cols, &ed.screen_rows, &cw, &ch);
    ed.edit_rows = (ed.screen_rows > 3) ? (ed.screen_rows - 3) : 1;
    if (ed.line_count == 0) ed.cursor_line = 0;
    else if (ed.cursor_line >= ed.line_count) ed.cursor_line = ed.line_count - 1;
    adjust_scroll();
}

//Редактирование

static void insert_codepoint(uint32_t cp){
    if (cp < 32 && cp != '\t') return;
    if (cp == 0x7F) return;

    unsigned char b[4]; uint32_t n = 0;
    if (cp < 0x80) { b[0]=cp; n=1; }
    else if (cp < 0x800) { b[0]=0xC0|(cp>>6); b[1]=0x80|(cp&0x3F); n=2; }
    else if (cp < 0x10000){ b[0]=0xE0|(cp>>12); b[1]=0x80|((cp>>6)&0x3F); b[2]=0x80|(cp&0x3F); n=3; }
    else { b[0]=0xF0|(cp>>18); b[1]=0x80|((cp>>12)&0x3F); b[2]=0x80|((cp>>6)&0x3F); b[3]=0x80|(cp&0x3F); n=4; }

    dirty_line_t *d = materialize_dirty(ed.cursor_line, n);
    if (!d) return;
    if (d->len + n >= MAX_LINE_LEN) return;

    m_memmove(d->text + ed.cursor_byte + n, d->text + ed.cursor_byte, d->len - ed.cursor_byte);
    for (uint32_t i = 0; i < n; i++) d->text[ed.cursor_byte + i] = b[i];
    d->len += n;
    d->text[d->len] = 0;
    ed.cursor_byte += n;
    ed.modified = true;
}

static void do_backspace(void){
    uint32_t len;
    const char *text = get_line_text(ed.cursor_line, &len);

    if (ed.cursor_byte > 0){
        uint32_t s = ed.cursor_byte - 1;
        while (s > 0 && (text[s] & 0xC0) == 0x80) s--;
        uint32_t del = ed.cursor_byte - s;

        dirty_line_t *d = materialize_dirty(ed.cursor_line, 0);
        if (!d) return;
        m_memmove(d->text + s, d->text + s + del, d->len - s - del);
        d->len -= del;
        d->text[d->len] = 0;
        ed.cursor_byte = s;
        ed.modified = true;
    } else if (ed.cursor_line > 0){
        uint32_t prev_len;
        const char *prev_text = get_line_text(ed.cursor_line - 1, &prev_len);
        char *merged = (char *)malloc(prev_len + len + 1);
        if (!merged) return;
        m_memmove(merged, prev_text, prev_len);
        m_memmove(merged + prev_len, text, len);
        merged[prev_len + len] = 0;

        dirty_put(ed.cursor_line - 1, merged, prev_len + len);
        free(merged);
        delete_line_at(ed.cursor_line);
        ed.cursor_line--;
        ed.cursor_byte = prev_len;
        ed.modified = true;
    }
}

static void do_delete(void){
    uint32_t len;
    const char *text = get_line_text(ed.cursor_line, &len);

    if (ed.cursor_byte < len){
        uint32_t d = 1, nx = ed.cursor_byte + 1;
        while (nx < len && (text[nx] & 0xC0) == 0x80){ d++; nx++; }

        dirty_line_t *dl = materialize_dirty(ed.cursor_line, 0);
        if (!dl) return;
        m_memmove(dl->text + ed.cursor_byte, dl->text + ed.cursor_byte + d, dl->len - ed.cursor_byte - d);
        dl->len -= d;
        dl->text[dl->len] = 0;
        ed.modified = true;
    } else if (ed.cursor_line < ed.line_count - 1){
        uint32_t next_len;
        const char *next_text = get_line_text(ed.cursor_line + 1, &next_len);
        char *merged = (char *)malloc(len + next_len + 1);
        if (!merged) return;
        m_memmove(merged, text, len);
        m_memmove(merged + len, next_text, next_len);
        merged[len + next_len] = 0;
        dirty_put(ed.cursor_line, merged, len + next_len);
        free(merged);
        delete_line_at(ed.cursor_line + 1);
        ed.modified = true;
    }
}

static void do_enter(void){
    uint32_t len;
    const char *text = get_line_text(ed.cursor_line, &len);
    uint32_t tail_len = len - ed.cursor_byte;

    char *head = (char *)malloc(ed.cursor_byte + 1);
    char *tail = (char *)malloc(tail_len + 1);
    if (!head || !tail){ free(head); free(tail); return; }
    m_memmove(head, text, ed.cursor_byte);
    head[ed.cursor_byte] = 0;
    m_memmove(tail, text + ed.cursor_byte, tail_len);
    tail[tail_len] = 0;

    dirty_put(ed.cursor_line, head, ed.cursor_byte);
    insert_line_at(ed.cursor_line + 1);
    dirty_put(ed.cursor_line + 1, tail, tail_len);

    free(head); free(tail);
    ed.cursor_line++;
    ed.cursor_byte = 0;
    ed.modified = true;
}

//Перемещение

static void move_left(void){
    if (ed.cursor_byte > 0){
        uint32_t len;
        const char *text = get_line_text(ed.cursor_line, &len);
        ed.cursor_byte--;
        while (ed.cursor_byte > 0 && (text[ed.cursor_byte] & 0xC0) == 0x80) ed.cursor_byte--;
    } else if (ed.cursor_line > 0){
        ed.cursor_line--;
        uint32_t len;
        get_line_text(ed.cursor_line, &len);
        ed.cursor_byte = len;
    }
}
static void move_right(void){
    uint32_t len;
    const char *text = get_line_text(ed.cursor_line, &len);
    if (ed.cursor_byte < len){
        ed.cursor_byte++;
        while (ed.cursor_byte < len && (text[ed.cursor_byte] & 0xC0) == 0x80) ed.cursor_byte++;
    } else if (ed.cursor_line < ed.line_count - 1){
        ed.cursor_line++;
        ed.cursor_byte = 0;
    }
}
static void move_up(void){
    if (ed.cursor_line == 0) return;
    uint32_t col = cursor_col();
    ed.cursor_line--;
    uint32_t len;
    const char *text = get_line_text(ed.cursor_line, &len);
    ed.cursor_byte = col_to_byte(text, len, col);
}
static void move_down(void){
    if (ed.cursor_line >= ed.line_count - 1) return;
    uint32_t col = cursor_col();
    ed.cursor_line++;
    uint32_t len;
    const char *text = get_line_text(ed.cursor_line, &len);
    ed.cursor_byte = col_to_byte(text, len, col);
}
static void move_home(void){ ed.cursor_byte = 0; }
static void move_end(void){
    uint32_t len;
    get_line_text(ed.cursor_line, &len);
    ed.cursor_byte = len;
}
static void move_pgup(void){
    if (ed.cursor_line >= ed.edit_rows) ed.cursor_line -= ed.edit_rows;
    else ed.cursor_line = 0;
    uint32_t len;
    get_line_text(ed.cursor_line, &len);
    if (ed.cursor_byte > len) ed.cursor_byte = len;
}
static void move_pgdn(void){
    ed.cursor_line += ed.edit_rows;
    if (ed.cursor_line >= ed.line_count) ed.cursor_line = ed.line_count - 1;
    uint32_t len;
    get_line_text(ed.cursor_line, &len);
    if (ed.cursor_byte > len) ed.cursor_byte = len;
}

//Рендер

static void set_status(const char *msg){ m_strcpy(ed.status, msg, sizeof(ed.status)); ed.status_until = tp_ticks() + 200; }
static void clear_status(void){ ed.status[0] = 0; ed.status_until = 0; }


static void render_line_content(uint32_t line_no, uint32_t eff_cols){
    uint32_t len;
    const char *text = get_line_text(line_no, &len);

    uint32_t sel_s = len, sel_e = 0;
    if (sel_is_active()){
        uint32_t sl, sb, el, eb;
        sel_get_range(&sl, &sb, &el, &eb);
        if (line_no >= sl && line_no <= el){
            sel_s = (line_no == sl) ? sb : 0;
            sel_e = (line_no == el) ? eb : len;
        }
    }

    uint32_t col = 0, i = 0, max_col = ed.hscroll + eff_cols;
    bool in_sel = false;
    while (i < len && col < max_col){
        unsigned char c = (unsigned char)text[i];
        uint32_t bytes = 1, cols = 1;
        if (c == '\t') cols = 4;
        else if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;

        bool now_sel = (i >= sel_s && i < sel_e);
        if (now_sel != in_sel){
            out_str(now_sel ? "\x1B[30;47m" : "\x1B[0m");
            in_sel = now_sel;
        }

        if (col + cols > ed.hscroll){
            if (c == '\t'){
                uint32_t s = col > ed.hscroll ? col : ed.hscroll;
                uint32_t e = col + 4; if (e > max_col) e = max_col;
                for (uint32_t k = s; k < e; k++) out_byte(' ');
            } else {
                tp_write(TP_STDOUT, text + i, bytes);
            }
        }
        col += cols; i += bytes;
    }
    if (in_sel) out_str("\x1B[0m");
}


static void draw_cursor_cell(bool visible){
    if (ed.line_count == 0 || ed.cursor_line >= ed.line_count) return;
    uint32_t vl = ed.cursor_line - ed.scroll_top;
    if (vl >= ed.edit_rows) return;
    uint32_t vc = cursor_col() - ed.hscroll;

    uint32_t max_vc = ed.screen_cols;
    if (scrollbar_visible() && max_vc > 0) max_vc--;
    if (vc >= max_vc) return;

    esc_goto(ed.edit_start + vl, vc);

    uint32_t len;
    const char *text = get_line_text(ed.cursor_line, &len);
    uint32_t byte = ed.cursor_byte;

    if (byte < len){
        unsigned char c = (unsigned char)text[byte];
        if (c == '\t'){
            esc_reset();
            if (visible) {
#ifdef _WIN32
                esc_color(30, 47); out_byte(' ');
#else
                out_str("\xE2\x96\x88");
#endif
            }
            else out_byte(' ');
        } else {
            if (visible) esc_color(30, 47);
            else esc_reset();
            uint32_t nbytes = 1;
            if ((c & 0xE0) == 0xC0) nbytes = 2;
            else if ((c & 0xF0) == 0xE0) nbytes = 3;
            else if ((c & 0xF8) == 0xF0) nbytes = 4;
            tp_write(TP_STDOUT, text + byte, nbytes);
        }
    } else {
        esc_reset();
        if (visible) {
#ifdef _WIN32
            esc_color(30, 47); out_byte(' ');
#else
            out_str("\xE2\x96\x88");
#endif
        }
        else out_byte(' ');
    }
    esc_reset();
    esc_goto(ed.edit_start + vl, vc);
}



static void draw_scrollbar_col(uint32_t row_in_view){
    if (!scrollbar_visible()) return;
    if (row_in_view >= ed.edit_rows) return;
    if (ed.screen_cols == 0) return;

    uint32_t visible = ed.edit_rows;
    uint32_t total   = ed.line_count;

    uint32_t thumb_h = (visible * visible) / total;
    if (thumb_h < 1) thumb_h = 1;
    if (thumb_h > visible) thumb_h = visible;

    uint32_t max_top = total - visible;
    uint32_t thumb_top = 0;
    if (max_top > 0 && visible > thumb_h)
        thumb_top = (ed.scroll_top * (visible - thumb_h)) / max_top;

    esc_goto(ed.edit_start + row_in_view, ed.screen_cols - 1);

    if (row_in_view >= thumb_top && row_in_view < thumb_top + thumb_h){
        //Бегунок белый на синем — тот же стиль, что у шапки.
        esc_color(37, 44);
        out_byte(' ');
        esc_reset();
    } else {
        //Трек - обычный пробел на фоне консоли
        esc_reset();
        out_byte(' ');
    }
}


static void render(void){
    esc_goto(0,0); esc_clear_eol(); esc_color(37,44);
    out_byte(' ');
    if (ed.filename[0]) out_str(ed.filename); else out_str("[No Name]");
    if (ed.modified) out_str(" *");
    if (ed.is_new) out_str(" (new)");
    esc_reset();

    bool show_sb = scrollbar_visible();
    uint32_t eff_cols = ed.screen_cols;
    if (show_sb && eff_cols > 0) eff_cols--;

    for (uint32_t i = 0; i < ed.edit_rows; i++){
        esc_goto(ed.edit_start + i, 0); esc_clear_eol(); esc_reset();
        uint32_t fl = ed.scroll_top + i;
        if (fl < ed.line_count) render_line_content(fl, eff_cols);
        if (show_sb)            draw_scrollbar_col(i);
    }

    esc_goto(ed.screen_rows - 2, 0); esc_clear_eol(); esc_color(37,44);
    out_byte(' ');
    if (ed.status[0] && tp_ticks() < ed.status_until){
        out_str(ed.status);
    } else {
        if (ed.status[0]) clear_status();
        out_str("Ln "); out_uint(ed.cursor_line+1);
        out_str("/"); out_uint(ed.line_count);
        out_str("  Col "); out_uint(cursor_col()+1);
        out_str("  "); if (ed.modified) out_str("[Modified]");
    }
    if (sel_is_active()){
        uint32_t sl, sb, el, eb;
        sel_get_range(&sl, &sb, &el, &eb);
        out_str("  [SEL ");
        out_uint(el - sl + 1);
        out_str(sel_state == SEL_LOCKED ? "L]" : "T]");   // L = locked, T = tracking
    }
    esc_reset();

    esc_goto(ed.screen_rows - 1, 0); esc_clear_eol(); esc_color(30,47);
    //out_str(" F1 Help  F2 Save  F10 Quit   ^S Save  ^Q Quit");
        out_str(" F1 Hlp  F2 Sav  F3 Mrk  F5 Cpy  F6 Mov  F8 Del  ^C/X/V  ^Z/Y  F10 Qit");
    esc_reset();

    uint32_t vl = ed.cursor_line - ed.scroll_top;
    uint32_t vc = cursor_col() - ed.hscroll;
    if (vc > ed.screen_cols - 1) vc = ed.screen_cols - 1;
    esc_goto(ed.edit_start + vl, vc);
}

//Диалоги

static void do_save(void){
    if (save_file() < 0) set_status("Save failed!");
    else                 set_status("Saved.");
}

static void draw_help_screen(void){
    esc_clear_screen();
    esc_color(37,44);
    out_str(" thoth editor — help\r\n\r\n");
    esc_reset();
    out_str(
        "  F1         help\r\n"
        "  F2 / ^S    save\r\n"
        "  F3         mark selection (F3 to lock, ESC cancel)\r\n"
        "  F5         copy block to cursor (internal, not clipboard)\r\n"
        "  F6         move block to cursor\r\n"
        "  F8         delete block\r\n"
        "  ^C / ^X    copy / cut to system clipboard\r\n"
        "  ^V         paste from clipboard\r\n"
        "  ^Z / ^Y    undo / redo\r\n"
        "  F10 / ^Q   quit\r\n\r\n"
        "  Arrows     move cursor\r\n"
        "  Home/End   line start/end\r\n"
        "  PgUp/PgDn  page\r\n"
        "  Del        delete under cursor\r\n"
        "  Backspace  delete before cursor\r\n"
        "  Enter      newline\r\n"
        "  Tab        tab\r\n\r\n"
        "  Press any key...\r\n"
    );
}


static void do_help(void){
    draw_help_screen();
    for (;;){
        if (tp_resized()){
            draw_help_screen();
            continue;
        }
        if (!tp_kbhit()){ tp_yield(); continue; }
        thoth_event_t ev = tp_read_event();
        if (ev.kind != EV_NONE) break;   //только реальное нажатие закрывает help
    }
    //Слить всё, что успело накопиться (release модификаторов и т. п.), чтобы события не просочились в редактор после закрытия.
    while (tp_kbhit()) (void)tp_read_event();
    esc_clear_screen();
}


static bool confirm_quit(void){
    m_strcpy(ed.status, "Unsaved changes! Y = quit, N / Esc = cancel", sizeof(ed.status));
    ed.status_until = 0xFFFFFFFF;
    render();
    for (;;){
        if (tp_resized()){
            update_screen_size();
            esc_clear_screen();
            render();
        }
        if (!tp_kbhit()){ tp_yield(); continue; }
        thoth_event_t ev = tp_read_event();
        if (ev.kind == EV_KEY && ev.key == TK_ESC){ clear_status(); return false; }
        if (ev.kind == EV_CHAR){
            uint32_t cp = ev.cp;
            //Переводим в нижний регистр латиницу
            if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';
            //Поддержка кириллической раскладки для удобства
            if (cp == 0x414 || cp == 0x434) cp = 'y'; // 'Д' / 'д'
            if (cp == 0x41D || cp == 0x43D) cp = 'n'; // 'Н' / 'н'

            if (cp == 'y'){ clear_status(); return true; }
            if (cp == 'n'){ clear_status(); return false; }
        }
    }
}


//Буфер обмена

static void snapshot_free(editor_snapshot_t *s){
    if (s->line_offset) free(s->line_offset);
    if (s->dirty){
        for (uint32_t i = 0; i < s->dirty_count; i++)
            if (s->dirty[i].text) free(s->dirty[i].text);
        free(s->dirty);
    }
    m_memcpy_zero(s, sizeof(*s));
}

static int snapshot_create(editor_snapshot_t *s){
    m_memcpy_zero(s, sizeof(*s));
    s->line_count = ed.line_count;
    s->line_cap   = ed.line_count + 1;
    s->line_offset = (uint32_t *)malloc(s->line_cap * sizeof(uint32_t));
    if (!s->line_offset) return -1;
    for (uint32_t i = 0; i < s->line_cap; i++) s->line_offset[i] = ed.line_offset[i];

    s->dirty_count = ed.dirty_count;
    s->dirty_cap   = ed.dirty_count;
    if (s->dirty_count > 0){
        s->dirty = (dirty_line_t *)malloc(sizeof(dirty_line_t) * s->dirty_count);
        if (!s->dirty){ free(s->line_offset); s->line_offset = NULL; return -1; }
        for (uint32_t i = 0; i < s->dirty_count; i++){
            s->dirty[i].line_no = ed.dirty[i].line_no;
            s->dirty[i].len     = ed.dirty[i].len;
            s->dirty[i].cap     = ed.dirty[i].len + 1;
            s->dirty[i].text    = (char *)malloc(s->dirty[i].cap);
            if (!s->dirty[i].text){
                for (uint32_t j = 0; j < i; j++) free(s->dirty[j].text);
                free(s->dirty); s->dirty = NULL;
                free(s->line_offset); s->line_offset = NULL;
                return -1;
            }
            m_memmove(s->dirty[i].text, ed.dirty[i].text, ed.dirty[i].len);
            s->dirty[i].text[ed.dirty[i].len] = 0;
        }
    }

    s->cursor_line = ed.cursor_line;
    s->cursor_byte = ed.cursor_byte;
    s->scroll_top  = ed.scroll_top;
    s->hscroll     = ed.hscroll;
    s->file_size   = ed.file_size;
    s->is_new      = ed.is_new;

    /* rough memory estimate */
    uint32_t total = s->line_cap * sizeof(uint32_t);
    for (uint32_t i = 0; i < s->dirty_count; i++) total += s->dirty[i].cap;
    s->mem_bytes = total;
    return 0;
}

static int snapshot_restore(const editor_snapshot_t *s){
    if (ensure_index_capacity(s->line_cap) < 0) return -1;
    for (uint32_t i = 0; i < s->line_cap; i++) ed.line_offset[i] = s->line_offset[i];
    ed.line_count = s->line_count;

    for (uint32_t i = 0; i < ed.dirty_count; i++)
        if (ed.dirty[i].text) free(ed.dirty[i].text);
    ed.dirty_count = 0;
    if (ensure_dirty_capacity(s->dirty_count ? s->dirty_count : 1) < 0) return -1;
    for (uint32_t i = 0; i < s->dirty_count; i++){
        ed.dirty[i].line_no = s->dirty[i].line_no;
        ed.dirty[i].len     = s->dirty[i].len;
        ed.dirty[i].cap     = s->dirty[i].len + 1;
        ed.dirty[i].text    = (char *)malloc(ed.dirty[i].cap);
        if (!ed.dirty[i].text){ ed.dirty_count = i; return -1; }
        m_memmove(ed.dirty[i].text, s->dirty[i].text, ed.dirty[i].len);
        ed.dirty[i].text[ed.dirty[i].len] = 0;
    }
    ed.dirty_count = s->dirty_count;

    cache_invalidate_all();

    ed.cursor_line = s->cursor_line;
    ed.cursor_byte = s->cursor_byte;
    ed.scroll_top  = s->scroll_top;
    ed.hscroll     = s->hscroll;
    ed.file_size   = s->file_size;
    ed.is_new      = s->is_new;
    ed.modified    = (s->dirty_count > 0) || s->is_new;

    adjust_scroll();
    return 0;
}

static void history_evict(editor_snapshot_t *st, uint32_t *count, uint32_t *bytes){
    if (*count == 0) return;
    *bytes -= st[0].mem_bytes;
    snapshot_free(&st[0]);
    for (uint32_t i = 0; i < *count - 1; i++) st[i] = st[i+1];
    (*count)--;
}

static void push_undo_snapshot(void){
    /* новый edit стирает redo */
    while (redo_count > 0) history_evict(redo_stack, &redo_count, &redo_bytes);

    /* место под новый снапшот */
    while (undo_count >= UNDO_MAX_DEPTH ||
           undo_bytes >= UNDO_MAX_BYTES - 64 * 1024){
        history_evict(undo_stack, &undo_count, &undo_bytes);
    }

    if (snapshot_create(&undo_stack[undo_count]) == 0){
        undo_bytes += undo_stack[undo_count].mem_bytes;
        undo_count++;
    }
    //если malloc упал — просто не будет undo для этого действия
}

static int do_undo(void){
    if (undo_count == 0){ set_status("Nothing to undo"); return -1; }
    while (redo_count >= UNDO_MAX_DEPTH ||
           redo_bytes >= UNDO_MAX_BYTES - 64 * 1024){
        history_evict(redo_stack, &redo_count, &redo_bytes);
    }
    if (snapshot_create(&redo_stack[redo_count]) == 0){
        redo_bytes += redo_stack[redo_count].mem_bytes;
        redo_count++;
    }
    undo_count--;
    editor_snapshot_t s = undo_stack[undo_count];
    undo_bytes -= s.mem_bytes;
    int r = snapshot_restore(&s);
    snapshot_free(&s);
    set_status(r == 0 ? "Undo" : "Undo failed");
    return r;
}

static int do_redo(void){
    if (redo_count == 0){ set_status("Nothing to redo"); return -1; }
    while (undo_count >= UNDO_MAX_DEPTH ||
           undo_bytes >= UNDO_MAX_BYTES - 64 * 1024){
        history_evict(undo_stack, &undo_count, &undo_bytes);
    }
    if (snapshot_create(&undo_stack[undo_count]) == 0){
        undo_bytes += undo_stack[undo_count].mem_bytes;
        undo_count++;
    }
    redo_count--;
    editor_snapshot_t s = redo_stack[redo_count];
    redo_bytes -= s.mem_bytes;
    int r = snapshot_restore(&s);
    snapshot_free(&s);
    set_status(r == 0 ? "Redo" : "Redo failed");
    return r;
}

void history_clear(void){
    while (undo_count > 0) history_evict(undo_stack, &undo_count, &undo_bytes);
    while (redo_count > 0) history_evict(redo_stack, &redo_count, &redo_bytes);
}

//Выделение

void sel_clear(void){
    sel_state = SEL_NONE;
    sel_anchor_line = sel_anchor_byte = 0;
    sel_end_line    = sel_end_byte    = 0;
}



//Универсальный хелпер: извлечь текущее выделение в выделенный malloc буфер
//Возвращает NULL, если выделение пустое или OOM
static uint8_t *sel_extract_bytes(uint32_t *out_len){
    *out_len = 0;
    if (!sel_is_active()) return NULL;

    uint32_t sl, sb, el, eb;
    sel_get_range(&sl, &sb, &el, &eb);
    if (sl == el && sb == eb) return NULL;

    uint32_t total = 0;
    for (uint32_t l = sl; l <= el; l++){
        uint32_t len;
        get_line_text(l, &len);
        uint32_t s = (l == sl) ? sb : 0;
        uint32_t e = (l == el) ? eb : len;
        if (e > s) total += e - s;
        if (l < el) total += 1;
    }
    if (total == 0) return NULL;
    if (total > 1024 * 1024) total = 1024 * 1024;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;

    uint32_t pos = 0;
    for (uint32_t l = sl; l <= el && pos < total; l++){
        uint32_t len;
        const char *text = get_line_text(l, &len);
        uint32_t s = (l == sl) ? sb : 0;
        uint32_t e = (l == el) ? eb : len;
        if (e > len) e = len;
        uint32_t chunk = (e > s) ? (e - s) : 0;
        if (chunk > total - pos) chunk = total - pos;
        if (chunk) m_memmove(buf + pos, text + s, chunk);
        pos += chunk;
        if (l < el && pos < total) buf[pos++] = '\n';
    }
    *out_len = pos;
    return buf;
}

static uint32_t sel_copy_to_clipboard(void){
    uint32_t n = 0;
    uint8_t *buf = sel_extract_bytes(&n);
    if (!buf || n == 0){ if (buf) free(buf); return 0; }
    int r = tp_clipboard_set(buf, n);
    free(buf);
    return r == 0 ? n : 0;
}


static int sel_delete_range(void){
    if (!sel_is_active()) return -1;

    uint32_t sl, sb, el, eb;
    sel_get_range(&sl, &sb, &el, &eb);
    if (sl == el && sb == eb){ sel_clear(); return 0; }

    if (sl == el){
        /* односрочное удаление */
        dirty_line_t *d = materialize_dirty(sl, 0);
        if (!d) return -1;
        if (eb > d->len) eb = d->len;
        if (sb >= eb) return 0;
        m_memmove(d->text + sb, d->text + eb, d->len - eb);
        d->len -= (eb - sb);
        d->text[d->len] = 0;
        ed.cursor_line = sl;
        ed.cursor_byte = sb;
    } else {
        /* многострочное: слить sl (до sb) с el (от eb) */
        uint32_t sl_len, el_len;
        const char *sl_text = get_line_text(sl, &sl_len);
        const char *el_text = get_line_text(el, &el_len);
        if (sb > sl_len) sb = sl_len;
        if (eb > el_len) eb = el_len;

        uint32_t new_len = sb + (el_len - eb);
        char *merged = (char *)malloc(new_len + 1);
        if (!merged) return -1;
        m_memmove(merged, sl_text, sb);
        m_memmove(merged + sb, el_text + eb, el_len - eb);
        merged[new_len] = 0;

        dirty_put(sl, merged, new_len);
        free(merged);

        for (uint32_t i = el; i > sl; i--) delete_line_at(i);

        ed.cursor_line = sl;
        ed.cursor_byte = sb;
    }

    ed.modified = true;
    sel_clear();
    return 0;
}

static uint32_t decode_utf8_cp(const uint8_t *s, uint32_t max, uint32_t *used){
    if (max == 0){ *used = 0; return 0; }
    uint8_t b = s[0];
    if (b < 0x80){ *used = 1; return b; }
    if ((b & 0xE0) == 0xC0){
        if (max < 2 || (s[1] & 0xC0) != 0x80){ *used = 1; return 0xFFFD; }
        *used = 2; return ((b & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((b & 0xF0) == 0xE0){
        if (max < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80){
            *used = 1; return 0xFFFD;
        }
        *used = 3;
        return ((b & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((b & 0xF8) == 0xF0){
        if (max < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80){
            *used = 1; return 0xFFFD;
        }
        *used = 4;
        return ((b & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
               ((s[2] & 0x3F) << 6)  |  (s[3] & 0x3F);
    }
    *used = 1; return 0xFFFD;
}

static int sel_paste_from_clipboard(void){
    uint32_t sz = tp_clipboard_size();
    if (sz == 0){ set_status("Clipboard empty"); return -1; }
    if (sz > 1024 * 1024) sz = 1024 * 1024;

    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) return -1;
    int got = tp_clipboard_get(buf, sz);
    if (got <= 0){ free(buf); set_status("Paste failed"); return -1; }

    /* если есть выделение — заменяем его */
    if (sel_is_active()) sel_delete_range();

    uint32_t i = 0;
    while (i < (uint32_t)got){
        uint32_t used;
        uint32_t cp = decode_utf8_cp(buf + i, (uint32_t)got - i, &used);
        if (used == 0) break;
        if (cp == '\n'){
            do_enter();
        } else if (cp == '\r'){
            /* пропускаем CR */
        } else {
            insert_codepoint(cp);
        }
        i += used;
    }

    free(buf);
    ed.modified = true;
    sel_clear();
    adjust_scroll();
    return 0;
}


//Вставить UTF-8 байты в текущую позицию курсора
static void sel_insert_at_cursor(const uint8_t *buf, uint32_t len){
    uint32_t i = 0;
    while (i < len){
        uint32_t used;
        uint32_t cp = decode_utf8_cp(buf + i, len - i, &used);
        if (used == 0) break;
        if (cp == '\n')      do_enter();
        else if (cp != '\r') insert_codepoint(cp);
        i += used;
    }
}

//F5: копировать выделение в позицию курсора (mcedit Copy)
static int sel_copy_to_cursor(void){
    if (!sel_is_active()){ set_status("No selection"); return -1; }
    if (sel_state != SEL_LOCKED){ set_status("Lock selection first (F3)"); return -1; }

    uint32_t n = 0;
    uint8_t *buf = sel_extract_bytes(&n);
    if (!buf || n == 0){ if (buf) free(buf); set_status("Empty selection"); return -1; }

    uint32_t cl = ed.cursor_line, cb = ed.cursor_byte;
    sel_clear();
    ed.cursor_line = cl;
    ed.cursor_byte = cb;
    if (ed.cursor_line >= ed.line_count) ed.cursor_line = ed.line_count - 1;
    uint32_t line_len;
    get_line_text(ed.cursor_line, &line_len);
    if (ed.cursor_byte > line_len) ed.cursor_byte = line_len;

    sel_insert_at_cursor(buf, n);
    free(buf);
    ed.modified = true;
    adjust_scroll();
    set_status("Block copied to cursor");
    return 0;
}

//F6: вырезать выделение и вставить в позицию курсора (mcedit Move)
static int sel_move_to_cursor(void){
    if (!sel_is_active()){ set_status("No selection"); return -1; }
    if (sel_state != SEL_LOCKED){ set_status("Lock selection first (F3)"); return -1; }

    uint32_t sl, sb, el, eb;
    sel_get_range(&sl, &sb, &el, &eb);
    if (sl == el && sb == eb){ set_status("Empty selection"); return -1; }

    uint32_t cl = ed.cursor_line, cb = ed.cursor_byte;
    int inside = 0;
    if (cl > sl && cl < el) inside = 1;
    else if (cl == sl && cl == el && cb >= sb && cb <= eb) inside = 1;
    else if (cl == sl && cl != el && cb >= sb) inside = 1;
    else if (cl == el && cl != sl && cb <= eb) inside = 1;
    if (inside){ set_status("Cursor inside selection"); return -1; }

    uint32_t n = 0;
    uint8_t *buf = sel_extract_bytes(&n);
    if (!buf || n == 0){ if (buf) free(buf); set_status("Empty selection"); return -1; }

    //Пересчитать позицию курсора после удаления выделения
    uint32_t new_cl = cl, new_cb = cb;
    if (cl > el){
        new_cl = cl - (el - sl);
    } else if (cl == el && cl != sl){
        new_cl = sl;
        new_cb = sb + (cb - eb);
    }
    //else: курсор до выделения — не меняется

    sel_delete_range();

    ed.cursor_line = new_cl;
    if (ed.cursor_line >= ed.line_count) ed.cursor_line = ed.line_count - 1;
    ed.cursor_byte = new_cb;
    uint32_t line_len;
    get_line_text(ed.cursor_line, &line_len);
    if (ed.cursor_byte > line_len) ed.cursor_byte = line_len;

    sel_insert_at_cursor(buf, n);
    free(buf);
    ed.modified = true;
    adjust_scroll();
    set_status("Block moved to cursor");
    return 0;
}

//F8: удалить выделение (mcedit Delete)
static int sel_delete_block(void){
    if (!sel_is_active()){ set_status("No selection"); return -1; }
    uint32_t sl, sb, el, eb;
    sel_get_range(&sl, &sb, &el, &eb);
    if (sl == el && sb == eb){ sel_clear(); set_status("Empty selection"); return 0; }
    sel_delete_range();
    ed.modified = true;
    adjust_scroll();
    set_status("Block deleted");
    return 0;
}




//Обработка событий
static bool is_edit_action(const thoth_event_t *ev) {
    if (ev->kind == EV_CHAR){
        if (ev->cp == 0xFFFF0000u) return false;  //paste маркер
        if (ev->ctrl || ev->cmd){
            uint32_t cp = ev->cp;
            if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';
            return (cp == 'v' || cp == 'x');   /* paste / cut */
        }
        return true;
    }
    if (ev->kind == EV_KEY){
        switch (ev->key){
            case TK_ENTER: case TK_BACKSPACE: case TK_TAB: case TK_DEL:
                return true;
            default: break;
        }
    }
    return false;
}

static void handle_event(const thoth_event_t *ev){
    // Bracketed paste: целый блок пришёл одним маркер-событием.
    // Обрабатываем ДО is_edit_action и undo/redo, и возвращаемся.
    if (ev->kind == EV_CHAR && ev->cp == 0xFFFF0000u){
        const uint8_t *pbuf = NULL;
        uint32_t plen = 0;
        if (tp_take_paste_buf(&pbuf, &plen)){
            push_undo_snapshot();
            if (sel_is_active()) sel_delete_range();
            sel_insert_at_cursor(pbuf, plen);
            ed.modified = true;
            adjust_scroll();
            set_status("Pasted");
        }
        return;
    }

    // undo / redo
    //if (ev->kind == EV_KEY && ev->key == TK_F5){ do_undo(); return; }
    //if (ev->kind == EV_KEY && ev->key == TK_F6){ do_redo(); return; }
    if (ev->kind == EV_CHAR && (ev->ctrl || ev->cmd)){
        uint32_t cp = ev->cp;
        if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';
        if (cp == 'z'){ do_undo(); return; }
        if (cp == 'y'){ do_redo(); return; }
    }
    //F5/F6/F8 — mcedit-style блочные операции
    //F5/F6 требуют LOCKED selection (иначе некуда перемещать)
    //F8 работает и в TRACKING, и в LOCKED — как "удалить видимое"
    if (ev->kind == EV_KEY &&
        (ev->key == TK_F5 || ev->key == TK_F6 || ev->key == TK_F8)){
        if (!sel_is_active()){ set_status("No selection"); return; }
        if (ev->key != TK_F8 && sel_state != SEL_LOCKED){
            set_status("Lock selection first (F3)");
            return;
        }
        push_undo_snapshot();
        if      (ev->key == TK_F5) sel_copy_to_cursor();
        else if (ev->key == TK_F6) sel_move_to_cursor();
        else                       sel_delete_block();
        return;
    }

    //snapshot перед любым изменением буфера
    if (is_edit_action(ev)) push_undo_snapshot();

    //клавиши
    if (ev->kind == EV_KEY){
        switch (ev->key){
            case TK_ENTER:     do_enter();     sel_clear(); adjust_scroll(); return;
            case TK_BACKSPACE: do_backspace(); sel_clear(); adjust_scroll(); return;
            case TK_TAB:       insert_codepoint('\t'); sel_clear(); adjust_scroll(); return;
            case TK_F1:        do_help();      return;
            case TK_F2:        do_save();      return;
            case TK_F3:{
                if (sel_state == SEL_TRACKING){
                    /* второй F3 — зафиксировать конец выделения */
                    uint32_t sl, sb, el, eb;
                    sel_get_range(&sl, &sb, &el, &eb);
                    if (sl == el && sb == eb){
                        sel_clear();
                        set_status("Empty selection");
                        return;
                    }
                    sel_end_line = ed.cursor_line;
                    sel_end_byte = ed.cursor_byte;
                    sel_state = SEL_LOCKED;
                    set_status("Selection locked - Ctrl+C/X, ESC to cancel");
                } else {
                    /* первый F3 из NONE, либо новый F3 из LOCKED */
                    sel_state = SEL_TRACKING;
                    sel_anchor_line = ed.cursor_line;
                    sel_anchor_byte = ed.cursor_byte;
                    sel_end_line    = ed.cursor_line;
                    sel_end_byte    = ed.cursor_byte;
                    set_status("Mark set - move & F3 to lock, ESC to cancel");
                }
                return;
            }
            case TK_F10:
                if (ed.modified && !confirm_quit()) return;
                tp_clear_screen();
                tp_exit();
                break;
            case TK_ESC:
                if (sel_is_active()){ sel_clear(); set_status("Selection cleared"); }
                return;
            case TK_LEFT:  move_left();  adjust_scroll(); return;
            case TK_RIGHT: move_right(); adjust_scroll(); return;
            case TK_UP:    move_up();    adjust_scroll(); return;
            case TK_DOWN:  move_down();  adjust_scroll(); return;
            case TK_HOME:  move_home();  adjust_scroll(); return;
            case TK_END:   move_end();   adjust_scroll(); return;
            case TK_PGUP:  move_pgup();  adjust_scroll(); return;
            case TK_PGDN:  move_pgdn();  adjust_scroll(); return;
            case TK_DEL:   do_delete();  sel_clear(); adjust_scroll(); return;
            default: return;
        }
    }

    //символы
    if (ev->kind == EV_CHAR){
        if (ev->ctrl || ev->cmd){
            uint32_t cp = ev->cp;
            if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';
            if (cp == 's'){ do_save(); return; }
            if (cp == 'q'){
                if (ed.modified && !confirm_quit()) return;
                tp_clear_screen();
                tp_exit();
            }
            if (cp == 'c'){
                if (!sel_is_active()){ set_status("No selection"); return; }
                uint32_t n = sel_copy_to_clipboard();
                if (n) set_status("Copied");
                else   set_status("Copy failed / empty selection");
                return;
            }
            if (cp == 'x'){
                if (!sel_is_active()){ set_status("No selection"); return; }
                uint32_t n = sel_copy_to_clipboard();
                if (!n){ set_status("Cut failed"); return; }
                if (sel_delete_range() == 0) set_status("Cut");
                else                         set_status("Cut delete failed");
                return;
            }
            if (cp == 'v'){
                if (sel_paste_from_clipboard() == 0) set_status("Pasted");
                return;
            }
            // undo/redo (если хотите, чтобы Cmd+Z тоже работал)
            if (cp == 'z') { do_undo(); return; }
            if (cp == 'y') { do_redo(); return; }
            return;
        }
        insert_codepoint(ev->cp);
        sel_clear();
        adjust_scroll();
    }
}


static bool process_events(void){
    bool got = false;
    while (tp_kbhit()){
        thoth_event_t ev = tp_read_event();
        if (ev.kind == EV_NONE) continue;
        handle_event(&ev);
        got = true;
    }
    return got;
}

//main

int main(int argc, char *argv[]){
    if (tp_init() < 0) return 1;

    update_screen_size();

    if (ed.screen_rows < 6 || ed.screen_cols < 30){ tp_shutdown(); return 0; }

    ed.edit_start = 1;
    ed.edit_rows  = ed.screen_rows - 3;

    ed.line_offset = NULL; ed.line_cap = 0; ed.line_count = 0;
    ed.dirty = NULL; ed.dirty_count = 0; ed.dirty_cap = 0;
    ed.fd = -1;
    ed.filename[0] = 0;
    ed.scroll_top = 0; ed.hscroll = 0;
    ed.cursor_line = 0; ed.cursor_byte = 0;
    ed.modified = false; ed.status[0] = 0;
    for (int i = 0; i < CACHE_SIZE; i++){ ed.cache[i].valid = false; ed.cache[i].text = NULL; }

    if (argc >= 2){
        if (load_file(argv[1]) < 0){
            if (ensure_index_capacity(2) < 0) ed.line_count = 0;
            else { ed.line_count = 1; ed.line_offset[0] = 0; ed.line_offset[1] = 0; }
            set_status("Cannot open file!");
        }
    } else {
        ed.is_new = true;
        ed.filename[0] = 0;
        if (ensure_index_capacity(2) < 0) ed.line_count = 0;
        else { ed.line_count = 1; ed.line_offset[0] = 0; ed.line_offset[1] = 0; }
    }

    esc_clear_screen();
    tp_poll_locale();
    last_blink_tick = tp_ticks();
    blink_on = true;

    bool dirty = true;
    for (;;){
        if (tp_resized()){
            update_screen_size();
            esc_clear_screen();
            dirty = true;
        }
        if (dirty){
            render();
            draw_cursor_cell(blink_on);
            dirty = false;
        }
        if (tp_kbhit()){
            blink_on = true;
            last_blink_tick = tp_ticks();
            if (process_events()) dirty = true;
            else                  draw_cursor_cell(true);
        } else {
            tp_yield();
            tp_poll_locale();
            uint32_t now = tp_ticks();
            if ((uint32_t)(now - last_blink_tick) >= BLINK_TICKS){
                last_blink_tick = now;
                blink_on = !blink_on;
                draw_cursor_cell(blink_on);
            }
        }
    }
}
