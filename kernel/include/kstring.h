/* kstring.h - minimale Ersatzimplementierung der noetigen libc-Funktionen. */
#ifndef KSTRING_H
#define KSTRING_H

#include "retro.h"

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);

/* Kopiert nach dst und terminiert immer; gibt die Laenge von dst zurueck. */
size_t strlcpy(char *dst, const char *src, size_t size);

/* CRC32 nach dem Verfahren von PNG und GPT. */
uint32_t crc32(const void *data, size_t length);
uint32_t crc32_update(uint32_t crc, const void *data, size_t length);

/* 32-Bit-Fuellen, praktisch fuer Framebuffer-Operationen. */
void   memset32(void *dst, uint32_t value, size_t count);

#endif /* KSTRING_H */
