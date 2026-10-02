#include "include-kernel/lib.h"

void *memset(void *s, int c, size_t n){
    unsigned char *p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n){
    unsigned char *d = (unsigned char*)dest;
    const unsigned char *s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dest;
}

//Побайтовое сравнение двух буферов в памяти
int memcmp(const void *str1, const void *src2, uint32_t n){
    const uint8_t *s1 = (const uint8_t *)str1;
    const uint8_t *s2 = (const uint8_t *)src2;
    while (n > 0){
        if (*s1 != *s2){
            return (*s1 < *s2) ? -1 : 1;
        }
        s1++;
        s2++;
        n--;
    }
    return 0;
}


size_t strlen(const char *s){
    size_t len = 0;
    while (*s++) len++;
    return len;
}

void itoa(int value, char *str, int base){
    char *ptr = str;
    char *ptr1 = str;
    char tmp_char;
    int tmp_value;

    // Обработка отрицательных чисел (для base 10)
    if (base == 10 && value < 0){
        *ptr++ = '-';
        value = -value;
        ptr1++;
    }

    do {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789ABCDEF"[tmp_value - value * base];
    } while (value);

    *ptr-- = '\0';

    // Разворот строки
    while (ptr1 < ptr){
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
}

void uitoa(unsigned int value, char *str, int base){
    char *ptr = str;
    char *ptr1 = str;
    char tmp_char;
    int tmp_value;

    // Обработка отрицательных чисел (для base 10)
    if (base == 10 && value < 0){
        *ptr++ = '-';
        value = -value;
        ptr1++;
    }

    do {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789ABCDEF"[tmp_value - value * base];
    } while (value);

    *ptr-- = '\0';

    // Разворот строки
    while (ptr1 < ptr){
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
}


int strcmp(const char *a,const char *b){
    while(*a && (*a == *b)){
        a++;
        b++;
    }
    return *(unsigned char*)a - *(unsigned char*)b;
}


int strncmp(const char *s1, const char *s2, size_t n){
    if (n == 0) return 0;
    while (n-- && *s1 && *s1 == *s2){ s1++; s2++; }
    if (n == (size_t)-1) return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

char *strstr(const char *haystack, const char *needle){
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++){
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n){ h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

//Заменяет все вхождения old_sub на new_sub внутри строки src и пишет результат в dest
char *strrep(char *dest, const char *src, const char *old_sub, const char *new_sub, uint32_t max_len) {
    if (!dest || !src || !old_sub || !new_sub || max_len == 0) return dest;
    //Начисто зануляем весь результирующий буфер
    memset(dest, 0, max_len);
    uint32_t dst_idx = 0;
    uint32_t old_len = strlen(old_sub);
    uint32_t new_len = strlen(new_sub);

    //Если искомая подстрока пустая, просто безопасно переносим строку и выходим
    if (old_len == 0) {
        strncpy(dest, src, max_len - 1);
        return dest;
    }

    while (*src != '\0' && dst_idx < (max_len - 1)) {
        //Проверяем совпадение с искомым маркером
        if (strncmp(src, old_sub, old_len) == 0) {
            //Убеждаемся, что замена физически влезет в границы буфера max_len
            if (dst_idx + new_len < (max_len - 1)) {
                for (uint32_t i = 0; i < new_len; i++) {
                    dest[dst_idx++] = new_sub[i];
                }
                src += old_len; //Перепрыгиваем замененный маркер в источнике
            } else {
                break; //Буфер переполнен — аварийно останавливаемся ради безопасности стека
            }
        } else {
            //Копируем символ и инкрементируем оба указателя (и dst_idx, и src)
            dest[dst_idx++] = *src++;
        }
    }

    dest[dst_idx] = '\0'; //Гарантированный терминальный ноль
    return dest;
}


//Копирует одну строку в другую (опасная, без ограничения размера)
char *strcpy(char *dest, const char *src){
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

//Безопасное копирование строки с ограничением по максимальной длине
char *strncpy(char *dest, const char *src, uint32_t n){
    char *d = dest;
    while (n > 0 && *src != '\0'){
        *d++ = *src++;
        n--;
    }
    // Если src короче n, заполняем оставшееся пространство нулями
    while (n > 0){
        *d++ = '\0';
        n--;
    }
    return dest;
}

//Внутренний помощник для strtok: ищет, содержит ли строка символ-разделитель
static bool is_delim(char c, const char *delim){
    while (*delim != '\0'){
        if (c == *delim) return true;
        delim++;
    }
    return false;
}

//Разрезает строку на токены (части) по разделителям
char *strtok(char *str, const char *delim){
    static char *last_str = NULL; // Хранит позицию между вызовами ядра/драйвера
    //Если передан новый указатель — начинаем с него, иначе продолжаем старый
    if (str != NULL){
        last_str = str;
    }
    //Если строки больше нет — токенов больше нет
    if (last_str == NULL || *last_str == '\0'){
        return NULL;
    }
    //Пропускаем ведущие разделители (например, идущие подряд слэши "///")
    while (*last_str != '\0' && is_delim(*last_str, delim)){
        last_str++;
    }

    //Если после пропуска разделителей уперлись в конец строки
    if (*last_str == '\0'){
        last_str = NULL;
        return NULL;
    }

    //Нашли начало токена
    char *token_start = last_str;

    //Ищем конец этого токена
    while (*last_str != '\0'){
        if (is_delim(*last_str, delim)){
            *last_str = '\0'; //Заменяем разделитель на терминальный нуль
            last_str++;       //Сдвигаем внутренний указатель на следующий символ
            return token_start;
        }
        last_str++;
    }

    //Если дошли до конца строки, это был последний токен
    last_str = NULL;
    return token_start;
}

char *strcat(char *dest, const char *src){
    char *rdest = dest;
    // Move pointer to the end of the destination string
    while (*rdest){
        rdest++;
    }
    //Copy source string characters into destination trailing space
    while ((*rdest++ = *src++));
    return dest;
}


int strcasecmp(const char *s1, const char *s2){
    while (*s1 && *s2){
        char c1 = *s1; char c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 = c1 - 'A' + 'a';
        if (c2 >= 'A' && c2 <= 'Z') c2 = c2 - 'A' + 'a';
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    char c1 = *s1; char c2 = *s2;
    if (c1 >= 'A' && c1 <= 'Z') c1 = c1 - 'A' + 'a';
    if (c2 >= 'A' && c2 <= 'Z') c2 = c2 - 'A' + 'a';
    return c1 - c2;
}


void trim(char *out, const char *in){
    int start = 0;
    while (in[start] == ' ' || in[start] == '\t') start++;

    int end = strlen(in) - 1;
    while (end >= start && (in[end] == ' ' || in[end] == '\t' || in[end] == '\r' || in[end] == '\n')) end--;

    int idx = 0;
    for (int i = start; i <= end; i++) out[idx++] = in[i];
    out[idx] = '\0';
}
