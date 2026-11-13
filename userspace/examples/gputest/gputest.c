/*
 * Copyright (C) 2026 ARHeOS by Dmitry H
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

//gputest — Ring-3 тест графического ускорения через активный видеодрайвер.
//
//Прогоняет через примитивы графического слоя (SYS_GR_PRIM -> gr_ops_t)
//все базовые операции, которые на Geode GX1 выполняет аппаратный GP:
//  1. заливки (fill_rect -> GP BLT "источник = none"),
//  2. линии: горизонтальные/вертикальные (GP BLT) и диагональные в обе
//     стороны (GP VECTOR, Bresenham в железе),
//  3. блит картинки из RAM (blit -> программный путь слоя RAM->FB),
//  4. копирование области внутри экрана (copy_area -> GP BLT FB->FB),
//     в том числе с перекрытием (движущийся прямоугольник).
//
//Запуск:
//  gputest            тест в текущем разрешении
//  gputest <N>        сменить режим по индексу (как в graphinfo), прогнать
//                     тест и вернуться в исходный режим. Смена режима — это
//                     ровно тот путь 4F02/4F03/4F01 через RM-трамплин, что
//                     отлаживается на GX1-плате: COM-лог покажет стадию.
//
//Каждый примитив проверяет код возврата: ненулевые коды печатаются — так
//видно, какой примитив драйвер/слой отказались выполнять.

#include "../../include/ustd.h"
#include "../../include/ugraph.h"
#include "../../include/umemory.h"
#include "../../include/syscall.h"

//пауза в тиках ядра (100 Гц): тик = 10 мс
static void pause_ticks(uint32_t ticks)
{
    uint32_t t0 = get_ticks();
    while ((uint32_t)(get_ticks() - t0) < ticks) yield();
}

//упаковка 0x00RRGGBB в формат текущего режима — те же формулы, что
//внутри графического слоя (gr_pack_color), у нас свои, из bitfields режима
static uint32_t scale8(uint32_t v, uint32_t len)
{
    if (len == 0) return 0;
    if (len >= 8) return v & 0xFF;
    return (((v & 0xFF) * ((1u << len) - 1)) + 127) / 255;
}

static uint32_t pack_color(const u_gr_mode_info_t *m, uint32_t rgb)
{
    if (m->bpp >= 24) return rgb;
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8)  & 0xFF;
    uint32_t b =  rgb        & 0xFF;
    return (scale8(r, m->red.length)   << m->red.offset)
           | (scale8(g, m->green.length) << m->green.offset)
           | (scale8(b, m->blue.length)  << m->blue.offset);
}

//обертки над SYS_GR_PRIM
static int p_fill(int x, int y, int w, int h, uint32_t rgb)
{
    int32_t p[6] = { x, y, w, h, (int32_t)rgb, 0 };
    return u_gr_prim(GR_PRIM_FILL_RECT, p);
}

static int p_line(int x0, int y0, int x1, int y1, uint32_t rgb)
{
    int32_t p[6] = { x0, y0, x1, y1, (int32_t)rgb, 0 };
    return u_gr_prim(GR_PRIM_DRAW_LINE, p);
}

static int p_copy(int sx, int sy, int w, int h, int dx, int dy)
{
    int32_t p[6] = { sx, sy, w, h, dx, dy };
    return u_gr_prim(GR_PRIM_COPY_AREA, p);
}

static int p_blit(int x, int y, int w, int h, const void *src)
{
    int32_t p[6] = { x, y, w, h, (int32_t)(uintptr_t)src, 0 };
    return u_gr_prim(GR_PRIM_BLIT, p);
}

//печать кода ошибки примитива (0 — молчим)
static void check_rc(const char *what, int rc)
{
    if (rc != 0)
    {
        printf("[FAIL] ");
        printf(what);
        printf(" rc=");
        printf("%d\n", (uint32_t)rc);
    }
}

//тест 1: заливки — вертикальные цветовые полосы во весь экран
static void test_fill_bars(const u_gr_mode_info_t *m)
{
    static const uint32_t cols[8] =
    {
        0x00000000, 0x00AA0000, 0x0000AA00, 0x00AAAA00,
        0x000000AA, 0x00AA00AA, 0x0000AAAA, 0x00FFFFFF
    };
    int bw = (int)m->xres / 8;
    if (bw <= 0) bw = 1;
    for (int i = 0; i < 8; i++)
        check_rc("fill bar", p_fill(i * bw, 0, bw, (int)m->yres, cols[i]));
    pause_ticks(10);
}

//тест 2: градиент мелкими заливками — интенсивная нагрузка на fill_rect
static void test_fill_gradient(const u_gr_mode_info_t *m)
{
    int steps = 64;
    int bw = (int)m->xres / steps;
    if (bw <= 0) bw = 1;
    for (int i = 0; i < steps; i++)
    {
        uint32_t v = (uint32_t)i * 4;      //0..252
        uint32_t rgb = (v << 16) | ((255 - v) << 8) | v;
        check_rc("fill grad", p_fill(i * bw, 0, bw + 1, (int)m->yres, rgb));
    }
    pause_ticks(10);
}

//тест 3: линии — горизонтальные, вертикальные, диагонали в обе стороны.
//на Geode диагонали идут аппаратным векторным движком GP: если при
//пересечении экрана линии рисуются криво/не до конца — точная настройка
//ошибок Брезенхэма GP в geode_draw_line (LENGTH/ошибки), правится там
static void test_lines(const u_gr_mode_info_t *m)
{
    int w = (int)m->xres, h = (int)m->yres;

    check_rc("clear", p_fill(0, 0, w, h, 0x00003050));
    for (int y = 20; y < h - 20 && y < 220; y += 20)
        check_rc("hline", p_line(10, y, w - 10, y, 0x00FFFFFF));
    for (int x = 20; x < w - 20 && x < 280; x += 20)
        check_rc("vline", p_line(x, 10, x, h - 10, 0x0000FF00));

    //диагонали: из углов в противоположные и в центр — обе балки X- и
    //Y-мажорных направлений и все четыре знака шага
    check_rc("diag 1", p_line(0, 0, w - 1, h - 1, 0x00FFFF00));
    check_rc("diag 2", p_line(w - 1, 0, 0, h - 1, 0x0000FFFF));
    check_rc("diag 3", p_line(0, h - 1, w - 1, 0, 0x00FF00FF));
    check_rc("diag 4", p_line(w / 2, h / 4, w / 4, h / 2, 0x00FF8000));
    check_rc("diag 5", p_line(w / 4, h / 4, w - w / 4, h - h / 4, 0x000080FF));
    pause_ticks(25);
}

//тест 4: блит картинки из RAM. Буфер собирается в формате ТЕКУЩЕГО режима
//(pack_color по bitfields режима), строки без padding: w*(bpp/8) байт
static void test_blit(const u_gr_mode_info_t *m)
{
    int w = (int)m->xres, h = (int)m->yres;
    int iw = 160, ih = 96;
    if (iw > w - 20) iw = w - 20;
    if (ih > h - 20) ih = h - 20;
    if (iw <= 0 || ih <= 0) return;

    uint32_t bytes_pp = m->bpp / 8;
    uint8_t *img = (uint8_t *)malloc((uint32_t)iw * ih * bytes_pp);
    if (!img)
    {
        printf("[FAIL] blit: no memory\n");
        return;
    }

    //картинка: шахматка с градиентной рамкой — сразу видно сдвиг/смещение
    //строк, если blit едет по чужому pitch
    for (int y = 0; y < ih; y++)
    {
        uint8_t *row = img + (uint32_t)y * iw * bytes_pp;
        for (int x = 0; x < iw; x++)
        {
            uint32_t rgb;
            if (x < 8 || y < 8 || x >= iw - 8 || y >= ih - 8)
                rgb = 0x00FFFFFF;
            else if (((x / 16) + (y / 16)) & 1)
                rgb = (((x * 255) / iw) << 16) | (((y * 255) / ih) << 8);
            else
                rgb = (((255 - x) * 255 / iw) << 16) | 0x00000040;
            uint32_t packed = pack_color(m, rgb);
            uint8_t *px = row + (uint32_t)x * bytes_pp;
            if (bytes_pp == 4) *(uint32_t *)px = packed;
            else if (bytes_pp == 2) *(uint16_t *)px = (uint16_t)packed;
            else
            {
                px[0] = packed & 0xFF;
                px[1] = (packed >> 8) & 0xFF;
                px[2] = (packed >> 16) & 0xFF;
            }
        }
    }

    check_rc("clear", p_fill(0, 0, w, h, 0x00202028));
    check_rc("blit c",  p_blit(w / 2 - iw / 2, h / 2 - ih / 2, iw, ih, img));
    check_rc("blit tl", p_blit(8, 8, iw, ih, img));
    check_rc("blit br", p_blit(w - iw - 8, h - ih - 8, iw, ih, img));
    pause_ticks(25);
    free(img);
}

//тест 5: copy_area — движущийся прямоугольник (FB->FB, на Geode это
//аппаратный BLT). Специально гоняем перекрытие: dx>sx и dy>sy, когда
//копирование вниз/вправо затирает еще не прочитанный источник — на GX1
//для этого включается реверс Y в GP_BLT_MODE
static void test_copy_area(const u_gr_mode_info_t *m)
{
    int w = (int)m->xres, h = (int)m->yres;
    int bw = 120, bh = 72;
    if (bw > w - 4) bw = w - 4;
    if (bh > h - 4) bh = h - 4;
    if (bw <= 0 || bh <= 0) return;

    check_rc("clear", p_fill(0, 0, w, h, 0x00404040));
    //эталонный квадрат в левом верхнем углу
    check_rc("src box", p_fill(0, 0, bw, bh, 0x00C04040));
    check_rc("src mark", p_fill(8, 8, bw / 3, bh / 3, 0x0040C040));

    int x = 0, y = 0, dxs = 7, dys = 5;
    for (int step = 0; step < 220; step++)
    {
        int nx = x + dxs, ny = y + dys;
        if (nx < 0 || nx + bw > w)
        {
            dxs = -dxs;
            nx = x + dxs;
        }
        if (ny < 0 || ny + bh > h)
        {
            dys = -dys;
            ny = y + dys;
        }
        //копия со сдвигом ровно на шаг — перекрытие с источником почти всегда
        check_rc("copy", p_copy(x, y, bw, bh, nx, ny));
        x = nx;
        y = ny;
        if ((step & 7) == 0) pause_ticks(1);
    }
    pause_ticks(15);
}

int main(int argc, char *argv[])
{
    u_gr_mode_info_t m;
    u_gr_driver_info_t drv;

    if (u_gr_driver_info(&drv) != 0)
    {
        printf("gputest: no active graphic driver\n");
        return 1;
    }
    if (u_gr_current_mode(&m) != 0 || m.xres == 0 || m.yres == 0)
    {
        printf("gputest: cannot query current mode\n");
        return 1;
    }

    printf("=== GPU acceleration test ===\n");
    printf("driver: ");
    printf(drv.name);
    printf("  modes: ");
    printf("%d\n", drv.mode_count);
    printf("mode: ");
    printf(m.name);
    printf("  pitch: ");
    printf("%d\n", m.pitch);

    //опциональная смена режима: gputest <индекс> — сам путь 4F02 через
    //RM-трамплин и есть то, что ловим на GX1-плате
    uint32_t saved_index = m.index;
    if (argc > 1)
    {
        uint32_t want = 0;
        const char *a = argv[1];
        while (*a >= '0' && *a <= '9')
        {
            want = want * 10 + (uint32_t)(*a - '0');
            a++;
        }
        printf("switching to mode index ");
        printf("%d", want);
        printf(" ...\n");
        int rc = u_gr_set_mode(want);
        if (rc != 0)
        {
            printf("[FAIL] set_mode rc=");
            printf("%d\n", (uint32_t)rc);
            return 1;
        }
        if (u_gr_current_mode(&m) != 0)
        {
            printf("[FAIL] mode query after switch\n");
            return 1;
        }
        printf("now: ");
        printf(m.name);
        printf("\n");
    }

    printf("[1/5] fill bars\n");
    test_fill_bars(&m);
    printf("[2/5] fill gradient\n");
    test_fill_gradient(&m);
    printf("[3/5] lines + diagonals\n");
    test_lines(&m);
    printf("[4/5] blit image\n");
    test_blit(&m);
    printf("[5/5] copy_area bounce\n");
    test_copy_area(&m);
    printf("=== gputest done ===\n");

    //вернуться в исходный режим, если переключались
    if (argc > 1 && m.index != saved_index)
    {
        printf("restoring mode ");
        printf("%d", saved_index);
        printf("\n");
        int rc = u_gr_set_mode(saved_index);
        if (rc != 0)
        {
            printf("[FAIL] restore rc=");
            printf("%d\n", (uint32_t)rc);
        }
    }
    return 0;
}
