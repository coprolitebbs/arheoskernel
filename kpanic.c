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

#include "include-kernel/kpanic.h"
#include "include-kernel/lib.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/bootinfo.h"



//Рисует по центру экрана окошко "Kernel Panic" с тенью, рамкой,
//красной шапкой и списком пар "LABEL: 0xXXXXXXXX"
//Работает и через fb-драйвер, и через boot_info LFB (ранние стадии).
//Если boot_info == 0 или экран не инициализирован — тихо ничего не рисует,
//вызывающий всё равно попадает в for(;;).
void draw_kernel_panic(const char *title, const panic_field_t *fields, uint32_t count)
{
    if (!boot_info) return;
    uint32_t scr_w = boot_info->width;
    uint32_t scr_h = boot_info->height;
    if (scr_w < 64 || scr_h < 64) return;

    const uint32_t cell_w  = 8;    //шаг шрифта kernel_draw_string / draw_hex_dword
    const uint32_t line_h  = 12;   //межстрочный интервал
    const uint32_t title_h = 22;   //высота цветной шапки
    const uint32_t pad_x   = 20;
    const uint32_t pad_y   = 12;
    const uint32_t footer_h = 18;

    //Заголовок "[!] TITLE"
    uint32_t title_len = 0;
    while (title[title_len]) title_len++;
    uint32_t title_text_len = title_len + 5;   /* " [!] " */

    //Длина самой длинной строки "LABEL: 0xXXXXXXXX" в символах
    uint32_t max_field_len = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t l = 0;
        while (fields[i].label[l]) l++;
        if (l + 12 > max_field_len) max_field_len = l + 12;
    }

    const char *footer = "System halted. Power cycle required.";
    uint32_t footer_len = 0;
    while (footer[footer_len]) footer_len++;

    uint32_t inner_chars = max_field_len;
    if (title_text_len > inner_chars) inner_chars = title_text_len;
    if (footer_len    > inner_chars) inner_chars = footer_len;
    if (inner_chars < 36) inner_chars = 36;

    uint32_t inner_px = inner_chars * cell_w;

    uint32_t box_w = inner_px + 2 * pad_x;
    uint32_t box_h = title_h + pad_y + count * line_h + pad_y + footer_h;

    //Мягкое ограничение по размеру экрана
    if (box_w > scr_w - 4) box_w = scr_w - 4;
    if (box_h > scr_h - 4) box_h = scr_h - 4;

    uint32_t box_x = (scr_w - box_w) / 2;
    uint32_t box_y = (scr_h - box_h) / 2;

    //Палитра
    uint32_t c_shadow    = 0x00000000;   //чёрная тень
    uint32_t c_border    = 0x00CCCCCC;   //светло-серая рамка
    uint32_t c_bg        = 0x00100030;   //тёмно-синий фон
    uint32_t c_title_bg  = 0x00A00000;   //красная шапка
    uint32_t c_title_fg  = 0x00FFFF00;   //жёлтый заголовок
    uint32_t c_body_fg   = 0x00FFFFFF;   //белые поля
    uint32_t c_footer_fg = 0x00888888;   //серый футер

    //Тень → рамка → фон → шапка
    kernel_fill_rect(box_x + 4, box_y + 4, box_w, box_h, c_shadow);
    kernel_fill_rect(box_x,     box_y,     box_w, box_h, c_border);
    kernel_fill_rect(box_x + 1, box_y + 1, box_w - 2, box_h - 2, c_bg);
    kernel_fill_rect(box_x + 1, box_y + 1, box_w - 2, title_h - 1, c_title_bg);

    //Текст заголовка по центру шапки
    uint32_t t_x = box_x + (box_w - title_text_len * cell_w) / 2;
    uint32_t t_y = box_y + 1 + (title_h - 8) / 2;
    kernel_draw_string(t_x,                t_y, " [!] ", c_title_fg);
    kernel_draw_string(t_x + 5 * cell_w,   t_y, title,   c_title_fg);

    //Поля
    uint32_t body_x = box_x + pad_x;
    uint32_t body_y = box_y + title_h + pad_y;

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t l = 0;
        while (fields[i].label[l]) l++;

        char label_buf[64];
        uint32_t k = 0;
        for (uint32_t j = 0; j < l && k < 40; j++) label_buf[k++] = fields[i].label[j];
        label_buf[k++] = ':';
        label_buf[k++] = ' ';
        label_buf[k++] = '0';
        label_buf[k++] = 'x';
        label_buf[k]   = '\0';

        kernel_draw_string(body_x, body_y, label_buf, c_body_fg);
        draw_hex_dword(fields[i].value, body_x + (l + 4) * cell_w, body_y);

        body_y += line_h;
    }

    //Футер по центру
    uint32_t f_x = box_x + (box_w - footer_len * cell_w) / 2;
    uint32_t f_y = body_y + 3;
    kernel_draw_string(f_x, f_y, footer, c_footer_fg);
}

