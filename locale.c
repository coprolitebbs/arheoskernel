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

#include "include-kernel/locale.h"
#include "include-kernel/config.h"
#include "include-kernel/lib.h"
#include "include-kernel/kernel_heap.h"
#include "userspace/include/fileattrs.h"

#define LOCALE_REGISTRY_MAX 8

static const locale_t *g_locales[LOCALE_REGISTRY_MAX];
static int             g_locale_count;
static const locale_t *g_active;

static locale_t g_locale_pool[LOCALE_POOL_MAX];
static int      g_locale_pool_used;

static const locale_keymap_t g_default_keymap = {
        .normal = {
        [0x01]=27,       //ESC
        [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',
        [0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',[0x0C]='-',[0x0D]='=',
        [0x0E]='\b',     //Backspace
        [0x0F]='\t',     //Tab
        [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
        [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',
        [0x1C]='\n',     //Enter
        [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',
        [0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
        [0x2B]='\\',[0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',
        [0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',[0x39]=' ',
    },
    .shift = {
        [0x01]=27,
        [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',
        [0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',[0x0C]='_',[0x0D]='+',
        [0x0E]='\b',
        [0x0F]='\t',
        [0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',[0x15]='Y',
        [0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',
        [0x1C]='\n',
        [0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',
        [0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',
        [0x2B]='|',[0x2C]='Z',[0x2D]='X',[0x2E]='C',[0x2F]='V',[0x30]='B',
        [0x31]='N',[0x32]='M',[0x33]='<',[0x34]='>',[0x35]='?',[0x39]=' ',
    },
};


static int str_eq(const char *a, const char *b){
    while (*a && *a == *b){ a++; b++; }
    return *a == *b;
}


static locale_keymap_t *locale_keymap_load_file(const char *path){
    cfg_file_t *cfg = cfg_open(path, FS_OPEN_READ, true);
    if (!cfg) return NULL;

    locale_keymap_t *kmap = (locale_keymap_t *)kmalloc(sizeof(locale_keymap_t));
    if (!kmap){ cfg_close(cfg); return NULL; }
    memset(kmap, 0, sizeof(locale_keymap_t));

    static const char hexd[] = "0123456789ABCDEF";
    char key[8];
    key[0] = '0';
    key[1] = 'x';

    for (int sc = 0; sc < 128; sc++){
        key[2] = hexd[(sc >> 4) & 0xF];
        key[3] = hexd[ sc       & 0xF];
        key[4] = '\0';

        kmap->normal[sc] = cfg_read_hex(cfg, "normal", key, 0);
        kmap->shift [sc] = cfg_read_hex(cfg, "shift",  key, 0);
    }

    cfg_close(cfg);
    return kmap;
}


// ---- Построение пути /config/locales/<code>.cfg без snprintf ----
static void build_locale_path(const char *code, char *out, int out_size){
    const char *prefix = "/config/locales/";
    int i = 0;
    while (*prefix && i < out_size - 1) out[i++] = *prefix++;
    while (*code   && i < out_size - 5) out[i++] = *code++;
    const char *suffix = ".cfg";
    while (*suffix && i < out_size - 1) out[i++] = *suffix++;
    out[i] = '\0';
}


void locale_registry_init(void){
    for (int i = 0; i < LOCALE_REGISTRY_MAX; i++) g_locales[i] = 0;
    g_locale_count = 0;
    g_active = 0;
    g_locale_pool_used = 0;
}



int locale_register(const locale_t *loc){
    if (!loc || !loc->code || !loc->font) return -1;
    if (g_locale_count >= LOCALE_REGISTRY_MAX) return -1;
    for (int i = 0; i < g_locale_count; i++)
        if (g_locales[i] == loc) return i;
    g_locales[g_locale_count] = loc;
    if (!g_active) g_active = loc;
    return g_locale_count++;
}

const locale_t *locale_by_code(const char *code){
    if (!code) return 0;
    for (int i = 0; i < g_locale_count; i++)
        if (str_eq(g_locales[i]->code, code)) return g_locales[i];
    return 0;
}

const locale_t *locale_get_active(void){ return g_active; }


int locale_set_active(const char *code){
    const locale_t *l = locale_by_code(code);
    if (!l) return -1;
    g_active = l;
    return 0;
}

locale_t *locale_alloc(const char *code, const char *display_name, const font_t *font){
    if (!code || !font) return 0;
    // Пытаемся загрузить keymap из /config/locales/<code>.cfg.
    // Если файла нет — kmap = NULL, работать будет через дефолт.
    char path[128];
    build_locale_path(code, path, sizeof(path));
    locale_keymap_t *kmap = locale_keymap_load_file(path);

    // Перерегистрация существующей локали — обновляем поля.
    for (int i = 0; i < g_locale_pool_used; i++){
        if (str_eq(g_locale_pool[i].code, code)){
            g_locale_pool[i].display_name = display_name;
            g_locale_pool[i].font         = font;
            if (kmap){
                if (g_locale_pool[i].keymap) kfree(g_locale_pool[i].keymap);
                g_locale_pool[i].keymap = kmap;
            }
            return &g_locale_pool[i];
        }
    }

    if (g_locale_pool_used >= LOCALE_POOL_MAX){
        if (kmap) kfree(kmap);
        return 0;
    }

    locale_t *l = &g_locale_pool[g_locale_pool_used++];
    l->code         = code;
    l->display_name = display_name;
    l->font         = font;
    l->keymap       = kmap;   // может быть NULL — это нормально
    locale_register(l);
    return l;
}


const locale_t *locale_get_by_index(uint8_t idx){
    if (idx >= g_locale_pool_used) return 0;
    return &g_locale_pool[idx];
}



uint32_t locale_resolve_key(const locale_t *loc, uint8_t scancode, bool shift){
    if (scancode >= 128) return 0;

    if (loc && loc->keymap){
        uint32_t cp = shift ? loc->keymap->shift[scancode]
                            : loc->keymap->normal[scancode];
        if (cp != 0) return cp;
    }
    //Фоллбек: чистая US-раскладка
    return shift ? g_default_keymap.shift[scancode]
                 : g_default_keymap.normal[scancode];
}


