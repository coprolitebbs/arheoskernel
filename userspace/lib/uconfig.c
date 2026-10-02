#include "../include/uconfig.h"
#include "../include/ustd.h"
#include "../include/umemory.h"
#include "../../include-kernel/lib.h"

//Сброс позиции в файле
static void cfg_reset_file(cfg_file_t *cfg_file){
    if (cfg_file->buffer_used) {
        cfg_file->buffer_pos = 0;
    } else {
        seek(cfg_file->fd, 0);
    }
}

//Чтение строки из файла
static int cfg_get_line(cfg_file_t *cfg_file, char *buf, int max_len){
    int idx = 0;
    bool has_physical_data = false;
    if (cfg_file->buffer_used && cfg_file->file_buffer_ptr != NULL){
        //Режим буферизации: Чтение из памяти Ring 3
        uint8_t *raw_buf = (uint8_t *)cfg_file->file_buffer_ptr;
        while (idx < (max_len - 1) && cfg_file->buffer_pos < cfg_file->file_size){
            char c = (char)raw_buf[cfg_file->buffer_pos++];
            has_physical_data = true;
            if (c == '\r') continue;
            if (c == '\n') break;
            buf[idx++] = c;
        }
    } else {
        //Обычный режим: Посимвольное чтение с диска через системные вызовы ядра
        char c;
        while (idx < (max_len - 1)){
            int r = u_read(cfg_file->fd, &c, 1);
            if (r <= 0) break;
            has_physical_data = true;
            if (c == '\r') continue;
            if (c == '\n') break;
            buf[idx++] = c;
        }
    }
    buf[idx] = '\0';
    return has_physical_data ? 1 : 0;
}


static void cfg_trim(char *out, const char *in){
    int start = 0;
    while (((unsigned char)in[start] == ' ' || (unsigned char)in[start] == '\t') && in[start] != '\0'){
        start++;
    }
    int end = strlen(in) - 1;
    while (end >= start && ((unsigned char)in[end] == ' ' || (unsigned char)in[end] == '\t' ||
                            (unsigned char)in[end] == '\r' || (unsigned char)in[end] == '\n')){
        end--;
    }
    int idx = 0;
    for (int i = start; i <= end; i++){
        out[idx++] = in[i];
    }
    out[idx] = '\0';
}



//Конструктор: открытие файла, проверка ограничений 64Кб и аллокация буфера
cfg_file_t *cfg_open(const char *path, uint32_t flags, bool use_bufferisation){
    int fd = open(path, flags);
    if (fd < 0) return NULL;

    cfg_file_t *cfg = (cfg_file_t *)malloc(sizeof(cfg_file_t));
    if (!cfg){
        close(fd);
        return NULL;
    }
    cfg->fd = fd;
    cfg->buffer_used = false;
    cfg->file_buffer_ptr = NULL;
    cfg->file_size = 0;
    cfg->buffer_pos = 0;

    if (use_bufferisation){
        u_stat_t st;
        if (stat(path, &st) == 0){
            if (st.size > 0 && st.size <= 65536){
                void *buf = malloc(st.size);
                if (buf) {
                    uint8_t *dst_ptr = (uint8_t *)buf;
                    uint32_t total_bytes_read = 0;
                    bool read_error = false;

                    while (total_bytes_read < st.size) {
                        uint32_t bytes_to_read = st.size - total_bytes_read;
                        if (bytes_to_read > 256) {
                            bytes_to_read = 256;
                        }
                        int r = u_read(fd, dst_ptr + total_bytes_read, bytes_to_read);
                        if (r <= 0) {
                            read_error = true;
                            break;
                        }
                        total_bytes_read += r;
                    }

                    if (!read_error && total_bytes_read > 0) {
                        cfg->buffer_used = true;
                        cfg->file_buffer_ptr = buf;
                        cfg->file_size = (size_t)total_bytes_read;
                    } else {
                        free_pages(buf, st.size);
                    }
                }
            }
        }
    }
/*
    printf("\n[DEBUG] cfg_open: total size parsed into RAM = %d bytes\n", cfg->file_size);

    if (cfg->buffer_used && cfg->file_buffer_ptr != NULL) {
        printf("--- BUFFER CONTENT START ---\n");
        char *ptr = (char *)cfg->file_buffer_ptr;
        for (size_t i = 0; i < cfg->file_size; i++) {
            // Выводим символы поштучно прямо в поток TTY консоли
            printf("%c", ptr[i]);
        }
        printf("\n--- BUFFER CONTENT END ---\n");
    }
*/
    return cfg;
}


//Деструктор: автоматическое закрытие дескрипторов и очистка памяти страниц ОЗУ
void cfg_close(cfg_file_t *cfg_file){
    if (!cfg_file) return;
    if (cfg_file->fd >= 0){
        close(cfg_file->fd);
    }
    if (cfg_file->buffer_used && cfg_file->file_buffer_ptr){
        //Честно возвращаем физические страницы менеджеру PMM ядра
        free_pages(cfg_file->file_buffer_ptr, cfg_file->file_size);
    }
    //Освобождаем память самой структуры
    free_pages(cfg_file, sizeof(cfg_file_t));
}





int cfg_read_string(cfg_file_t *cfg_file, const char *section, const char *key, const char *default_val, char *out_value, int max_len){
    if (!cfg_file || !out_value || max_len <= 0) return 0;
    if (!section || !key){
        if (default_val) strncpy(out_value, default_val, max_len - 1);
        else out_value[0] = '\0';
        out_value[max_len - 1] = '\0';
        return 0;
    }
    //Для буфера ОЗУ всегда начинаем поиск с самого начала файла
    //Это гарантирует, что мы всегда встретим заголовок {секции} раньше, чем сам ключ
    if (cfg_file->buffer_used) {
        cfg_file->buffer_pos = 0;
    } else {
        seek(cfg_file->fd, 0);
    }

    bool section_found = false;
    char line_buf[256];
    char clean_line[256];
    char target_sec[128];
    int sec_idx = 0;

    target_sec[sec_idx++] = '{';
    for (int i = 0; section[i] != '\0' && sec_idx < 126; i++){
        target_sec[sec_idx++] = section[i];
    }
    target_sec[sec_idx++] = '}';
    target_sec[sec_idx] = '\0';
    while (cfg_get_line(cfg_file, line_buf, 256) == 1){
        cfg_trim(clean_line, line_buf);

        if (clean_line[0] == ';' || clean_line[0] == '#' || clean_line[0] == '\0') continue;

        if (clean_line[0] == '{') {
            if (strcmp(clean_line, target_sec) == 0) section_found = true;
            else section_found = false;
            continue;
        }

        if (section_found){
            int eq_idx = -1;
            for (int i = 0; clean_line[i] != '\0'; i++){
                if (clean_line[i] == '=') { eq_idx = i; break; }
            }
            if (eq_idx > 0){
                clean_line[eq_idx] = '\0';
                char parsed_key[128];
                cfg_trim(parsed_key, clean_line);
                if (strcmp(parsed_key, key) == 0){
                    char parsed_val[128];
                    cfg_trim(parsed_val, &clean_line[eq_idx + 1]);
                    strncpy(out_value, parsed_val, max_len - 1);
                    out_value[max_len - 1] = '\0';
                    return 1;//Параметр найден
                }
            }
        }
    }
    //Если дошли до конца буфера/файла и ключ не найден — возвращаем дефолт
    if (default_val) strncpy(out_value, default_val, max_len - 1);
    else out_value[0] = '\0';
    out_value[max_len - 1] = '\0';
    return 0;
}




float cfg_read_float(cfg_file_t *cfg_file, const char *section, const char *key, float default_val){
    char val_str[64];
    if (cfg_read_string(cfg_file, section, key, NULL, val_str, 64)){
        if (val_str[0] == '\0') return default_val;
        float res = 0.0f, sign = 1.0f;
        int i = 0;
        if (val_str[0] == '-') { sign = -1.0f; i++; }
        else if (val_str[0] == '+') i++;
        while (val_str[i] != '\0' && val_str[i] != '.'){
            if (val_str[i] >= '0' && val_str[i] <= '9') res = res * 10.0f + (float)(val_str[i] - '0');
            else return default_val;
            i++;
        }
        if (val_str[i] == '.'){
            i++; float div = 10.0f;
            while (val_str[i] != '\0'){
                if (val_str[i] >= '0' && val_str[i] <= '9'){ res += (float)(val_str[i] - '0') / div; div *= 10.0f; }
                else break;
                i++;
            }
        }
        return res * sign;
    }
    return default_val;
}


bool cfg_read_bool(cfg_file_t *cfg_file, const char *section, const char *key, bool default_val){
    char val_str[64];
    if (cfg_read_string(cfg_file, section, key, NULL, val_str, 64)){
        if (val_str[0] == '\0') return default_val;
        if (strcmp(val_str, "1") == 0 || strcasecmp(val_str, "true") == 0 ||
            strcasecmp(val_str, "yes") == 0 || strcasecmp(val_str, "on") == 0) return true;
        if (strcmp(val_str, "0") == 0 || strcasecmp(val_str, "false") == 0 ||
            strcmp(val_str, "no") == 0 || strcasecmp(val_str, "off") == 0) return false;
    }
    return default_val;
}

uint32_t cfg_read_hex(cfg_file_t *cfg_file, const char *section, const char *key, uint32_t default_val){
    char val_str[64];
    if (cfg_read_string(cfg_file, section, key, NULL, val_str, 64)){
        if (val_str[0] == '\0') return default_val;
        uint32_t res = 0; int i = 0;
        if (val_str[0] == '0' && (val_str[1] == 'x' || val_str[1] == 'X')) i = 2;
        if (val_str[i] == '\0') return default_val;
        while (val_str[i] != '\0'){
            char c = val_str[i]; uint32_t digit = 0;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return default_val;
            res = (res << 4) | digit;
            i++;
        }
        return res;
    }
    return default_val;
}


int cfg_read_int(cfg_file_t *cfg_file, const char *section, const char *key, int default_val){
    char val_str[64];
    if (cfg_read_string(cfg_file, section, key, NULL, val_str, 64)){
        if (val_str[0] == '\0') return default_val;
        int res = 0, sign = 1, i = 0;
        if (val_str[0] == '-') { sign = -1; i++; }
        else if (val_str[0] == '+') i++;
        while (val_str[i] != '\0'){
            if (val_str[i] >= '0' && val_str[i] <= '9'){
                res = res * 10 + (val_str[i] - '0');
            } else break;
            i++;
        }
        return res * sign;
    }
    return default_val;
}
