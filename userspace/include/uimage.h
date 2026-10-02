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

int get_fb_info(uint32_t *pitch, uint32_t *bpp, uint32_t *width, uint32_t *height);

//API библиотеки
uint8_t* load_tga_to_buffer(const char *path, uint32_t target_w, uint32_t target_h, uint32_t target_bpp, uint32_t target_pitch, uint32_t mode);
#endif

