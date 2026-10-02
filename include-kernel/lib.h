#ifndef LIB_H
#define LIB_H

#include <stddef.h>
#include <stdint.h>   //для uint32_t, uint16_t

void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);

int memcmp(const void *str1, const void *src2, uint32_t n);
size_t strlen(const char *s);
void itoa(int value, char *str, int base);
void uitoa(unsigned int value, char *str, int base);
int strcmp(const char *a,const char *b);
int strncmp(const char *s1, const char *s2, size_t n);
char *strstr(const char *haystack, const char *needle);
char *strrep(char *dest, const char *src, const char *old_sub, const char *new_sub, uint32_t max_len);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, uint32_t n);
char *strtok(char *str, const char *delim);
char *strcat(char *dest, const char *src);
int strcasecmp(const char *s1, const char *s2);
void trim(char *out, const char *in);

#endif
