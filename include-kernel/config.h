#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


// Ядерная структура управления кэшированным файлом конфигурации
typedef struct {
    int fd;                  // Глобальный дескриптор VFS ядра
    bool buffer_used;        // Флаг использования буферизации в ОЗУ
    void *file_buffer_ptr;   // Указатель на буфер в куче ядра (kmalloc)
    size_t file_size;        // Реальный размер файла в байтах
    size_t buffer_pos;       // Текущее смещение каретки чтения в памяти
} cfg_file_t;

cfg_file_t *cfg_open(const char *path, uint32_t flags, bool use_bufferisation);
void cfg_close(cfg_file_t *cfg_file);

//Парсер секционированных конфигурационных файлов
int cfg_read_string(cfg_file_t *cfg_file, const char *section, const char *key, const char *default_val, char *out_value, int max_len);
int cfg_read_int(cfg_file_t *cfg_file, const char *section, const char *key, int default_val);
float cfg_read_float(cfg_file_t *cfg_file, const char *section, const char *key, float default_val);
bool cfg_read_bool(cfg_file_t *cfg_file, const char *section, const char *key, bool default_val);
uint32_t cfg_read_hex(cfg_file_t *cfg_file, const char *section, const char *key, uint32_t default_val);


#endif
