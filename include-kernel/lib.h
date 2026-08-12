#ifndef LIB_H
#define LIB_H

#include <stddef.h>
#include <stdint.h>   // для uint32_t, uint16_t

void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
size_t strlen(const char *s);
void itoa(int value, char *str, int base);
void uitoa(unsigned int value, char *str, int base);
int strcmp(const char *a,const char *b);
int strncmp(const char *s1, const char *s2, size_t n);
char *strstr(const char *haystack, const char *needle);

#endif
