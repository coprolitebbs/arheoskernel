#include "../include/uimage.h"
#include "../include/ustd.h"
#include "../../include-kernel/lib.h"
#include "../include/umemory.h"
#include "../include/syscall.h"


static uint16_t u_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}


int get_fb_info(uint32_t *pitch, uint32_t *bpp, uint32_t *width, uint32_t *height){
    uint32_t out_pitch, out_bpp, out_width, out_height;

    __asm__ __volatile__(
        "int $0x80"
        : "=a"(out_pitch), "=b"(out_bpp), "=c"(out_width), "=d"(out_height)
        : "a"(SYS_GET_FB_INFO)
        : "memory"
    );

    if (pitch)  *pitch  = out_pitch;
    if (bpp)    *bpp    = out_bpp;
    if (width)  *width  = out_width;
    if (height) *height = out_height;
    return 0;
}



uint8_t* load_tga_to_buffer(const char *path, uint32_t target_w, uint32_t target_h, uint32_t target_bpp, uint32_t target_pitch, uint32_t mode){
    int fd = open(path, FS_OPEN_READ);
    if (fd < 0) return NULL;

    tga_header_t hdr;
    if (u_read(fd, &hdr, sizeof(tga_header_t)) != sizeof(tga_header_t)) {
        close(fd);
        return NULL;
    }

    if (hdr.image_type != 2 || (hdr.bits_per_pixel != 24 && hdr.bits_per_pixel != 32)) {
        close(fd);
        return NULL;
    }

    uint32_t src_w = hdr.width;
    uint32_t src_h = hdr.height;
    uint32_t src_bytes_pp = hdr.bits_per_pixel / 8;
    uint32_t src_size_bytes = src_w * src_h * src_bytes_pp;

    uint8_t *raw_pixels = (uint8_t *)malloc(src_size_bytes);
    if (!raw_pixels) { close(fd); return NULL; }

    if (hdr.id_length > 0) {
        uint8_t dummy;
        for (int i = 0; i < hdr.id_length; i++) u_read(fd, &dummy, 1);
    }

    uint32_t bytes_read = 0;
    while (bytes_read < src_size_bytes) {
        uint32_t chunk = src_size_bytes - bytes_read;
        if (chunk > 256) chunk = 256;
        int r = u_read(fd, raw_pixels + bytes_read, chunk);
        if (r <= 0) break;
        bytes_read += r;
    }
    close(fd);

    if (bytes_read < src_size_bytes) {
        free_pages(raw_pixels,src_size_bytes);
        return NULL;
    }

    uint32_t dest_size_bytes = target_h * target_pitch;
    uint8_t *dest_canvas = (uint8_t *)malloc(dest_size_bytes);
    if (!dest_canvas) {
        free_pages(raw_pixels, src_size_bytes);
        return NULL;
    }
    memset(dest_canvas, 0, dest_size_bytes);

    uint32_t target_bytes_pp = target_bpp / 8;

    for (uint32_t y = 0; y < target_h; y++) {
        for (uint32_t x = 0; x < target_w; x++) {
            uint32_t src_x = 0;
            uint32_t src_y = 0;

            if (mode == 2) {
                //Режим WP_MODE_STRETCH (Растягивание)
                //Целочисленный Nearest Neighbor алгоритм без использования float/FPU
                src_x = (x * src_w) / target_w;
                src_y = (y * src_h) / target_h;
                //Защита от выхода за границы округления
                if (src_x >= src_w) src_x = src_w - 1;
                if (src_y >= src_h) src_y = src_h - 1;
            }
            else if (mode == 1) {
                //Режим WP_MODE_TILE (Замощение / Плитка)
                src_x = x % src_w;
                src_y = y % src_h;
            }
            else {
                //Режим WP_MODE_CENTER (Обычный вывод 1-к-1 с черными краями)
                if (x >= src_w || y >= src_h) {
                    continue;
                }
                src_x = x;
                src_y = y;
            }

            //Учитываем дескриптор направления строк TGA (бит 5)
            uint32_t actual_src_y = src_y;
            if (!(hdr.image_descriptor & 0x20)) {
                actual_src_y = (src_h - 1) - src_y;
            }

            uint32_t src_pixel_offset = (actual_src_y * src_w + src_x) * src_bytes_pp;

            uint8_t b = raw_pixels[src_pixel_offset + 0];
            uint8_t g = raw_pixels[src_pixel_offset + 1];
            uint8_t r = raw_pixels[src_pixel_offset + 2];
            uint8_t a = (src_bytes_pp == 4) ? raw_pixels[src_pixel_offset + 3] : 0xFF;

            uint8_t *dst_ptr = dest_canvas + (y * target_pitch) + (x * target_bytes_pp);

            if (target_bpp == 32) {
                dst_ptr[0] = b;
                dst_ptr[1] = g;
                dst_ptr[2] = r;
                dst_ptr[3] = a;
            }
            else if (target_bpp == 24) {
                dst_ptr[0] = b;
                dst_ptr[1] = g;
                dst_ptr[2] = r;
            }
            else if (target_bpp == 16) {
                *(uint16_t*)dst_ptr = u_rgb565(r, g, b);
            }
            else if (target_bpp == 8) {
                dst_ptr[0] = (uint8_t)((r + g + b) / 3);
            }
        }
    }

    free_pages(raw_pixels, src_size_bytes);
    return dest_canvas;
}


