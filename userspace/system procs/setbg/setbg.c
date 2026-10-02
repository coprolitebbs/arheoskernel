#include "../../include/ustd.h"
#include "../../include/umemory.h"
#include "../../include/uimage.h"
#include "../../../include-kernel/lib.h"

int main(int argc, char *argv[]){
    if (argc < 2){
        printf("setbg: missing file operand\n");
        printf("Usage: setbg [-t (tile mode)] [-s (stretch mode)] [file.tga]\n");
        return -1;
    }
    // По умолчанию 0 (Center)
    uint32_t mode = 0;
    const char *filepath = NULL;

    for (int i = 1; i < argc; i++){
        if (strcmp(argv[i], "-t") == 0){
            mode = 1; // TILE
        } else if (strcmp(argv[i], "-s") == 0){
            mode = 2; // STRETCH
        } else {
            filepath = argv[i];
        }
    }

    if (!filepath){
        printf("setbg: no target TGA image specified\n");
        return -1;
    }

    //Запрашиваем параметры графического режима у ядра
    uint32_t scr_pitch = 0, scr_bpp = 0, scr_width = 0, scr_height = 0;
    get_fb_info(&scr_pitch, &scr_bpp, &scr_width, &scr_height);

    //Страховочный фолбек
    if (scr_width == 0 || scr_height == 0 || scr_bpp == 0) {
        scr_width = 1024;
        scr_height = 768;
        scr_bpp = 32;
        scr_pitch = 1024 * 4;
    }

    if (mode == 2) {
        printf("Mode: Stretch to screen size.\n");
    } else if (mode == 1) {
        printf("Mode: Tiled pattern.\n");
    } else {
        printf("Mode: Centered 1:1.\n");
    }

    printf("Decoding wallpaper resource: %s...\n", filepath);

    //Загружаем TGA с передачей выбранного численного режима упаковки
    uint8_t *wp_buffer = load_tga_to_buffer(filepath, scr_width, scr_height, scr_bpp, scr_pitch, mode);
    if (!wp_buffer){
        printf("setbg: encoding crash or missing TGA file: %s\n", filepath);
        return -1;
    }

    //Вычисляем итоговый размер буфера в байтах на базе ядерного pitch
    uint32_t wp_size_bytes = scr_height * scr_pitch;

    printf("Uploading pixels into Ring 0 video memory registries...\n");

    int res = set_wallpaper(wp_buffer, wp_size_bytes);

    //Сразу же очищаем Userspace-память процесса
    free(wp_buffer);

    if (res != 0){
        printf("setbg: critical driver rejection\n");
        return -1;
    }

    //Принудительно вызываем системную очистку экрана
    clear_screen();

    return 0;
}

