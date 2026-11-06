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

#ifndef UIMAGE_H
#define UIMAGE_H

#include <stdint.h>
#include <stdbool.h>

//Заголовок несжатого TGA-файла (ровно 18 байт, packed)
typedef struct __attribute__((packed)) {
    uint8_t  id_length;
    uint8_t  color_map_type;
    uint8_t  image_type; // 2 - несжатый True-Color (RGB/RGBA)
    uint16_t color_map_first;
    uint16_t color_map_length;
    uint8_t  color_map_size;
    uint16_t x_origin;
    uint16_t y_origin;
    uint16_t width;      //Ширина картинки
    uint16_t height;     //Высота картинки
    uint8_t  bits_per_pixel;// 24 или 32 бит
    uint8_t  image_descriptor;
} tga_header_t;

typedef enum {
    WP_MODE_CENTER = 0, //По центру (старый дефолт с заливкой черным)
    WP_MODE_TILE   = 1, //Плитка / Замощение (-t)
    WP_MODE_STRETCH = 2 //Растягивание на весь экран (-s)
} wp_mode_t;

//Структура с функциями коллбэка прогресса загрузки файла изображения
typedef struct{
    //Стартовая функция
    void (*start_loading_phase)(uint32_t min_val, uint32_t max_val);
    //Шаг
    void(*loading_page_step)(uint32_t current_point);
    //Останавливающая функция
    void (*stop_loading_phase)(void);
} img_loading_struct_t;


int get_fb_info(uint32_t *pitch, uint32_t *bpp, uint32_t *width, uint32_t *height);

//Загрузка TGA в буфер памяти
//img_loading_struct_t *loading_struct - структура с функциями коллбэка прогресса
uint8_t* load_tga_to_buffer(const char *path, uint32_t target_w, uint32_t target_h, uint32_t target_bpp, uint32_t target_pitch, uint32_t mode, img_loading_struct_t *loading_struct);

//Загрузка PNG, аналог load_tga_to_buffer(): те же режимы,
//те же целевые форматы (32/24/16/8 бит) и те же колбэки прогресса.
//Поддерживаются: цветовые типы 0/2/3/4/6, глубины 1/2/4/8/16 бит,
//фильтры строк 0..4, Adam7-интерлейс, tRNS-прозрачность.
//inflate реализован
uint8_t* load_png_to_buffer(const char *path, uint32_t target_w, uint32_t target_h, uint32_t target_bpp, uint32_t target_pitch, uint32_t mode, img_loading_struct_t *loading_struct);

//Универсальный загрузчик: сам выбирает формат по расширению имени файла
//(.png / .tga, регистр не важен). Если расширения нет или оно незнакомое,
//формат опознаётся по содержимому (PNG-сигнатура), иначе файл читается
//как TGA. Параметры, режимы и результат — те же, что у load_tga_to_buffer()
uint8_t* load_image_to_buffer(const char *path, uint32_t target_w, uint32_t target_h, uint32_t target_bpp, uint32_t target_pitch, uint32_t mode, img_loading_struct_t *loading_struct);

//Текст последней ошибки PNG-декодера ("ok" после успешной загрузки)
const char *uimage_png_last_error(void);
#endif
