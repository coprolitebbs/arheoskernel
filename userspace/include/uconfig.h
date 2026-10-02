#ifndef UCONFIG_H
#define UCONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    int fd;
    bool buffer_used;
    void *file_buffer_ptr;
    size_t file_size;
    size_t buffer_pos;
} cfg_file_t;

//Парсер секционированных конфигурационных файлов для пользовательских программ
cfg_file_t *cfg_open(const char *path, uint32_t flags, bool use_bufferisation);
void cfg_close(cfg_file_t *cfg_file);

int cfg_read_string(cfg_file_t *cfg_file, const char *section, const char *key, const char *default_val, char *out_value, int max_len);
int cfg_read_int(cfg_file_t *cfg_file, const char *section, const char *key, int default_val);
float cfg_read_float(cfg_file_t *cfg_file, const char *section, const char *key, float default_val);
bool cfg_read_bool(cfg_file_t *cfg_file, const char *section, const char *key, bool default_val);
uint32_t cfg_read_hex(cfg_file_t *cfg_file, const char *section, const char *key, uint32_t default_val);


#endif

