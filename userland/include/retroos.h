/* retroos.h - die Schnittstelle, die Benutzerprogramme sehen.
 *
 * Programme laufen in Ring 3 und in einem eigenen Adressraum. Sie
 * erreichen den Kernel ausschliesslich ueber die Funktionen hier; ein
 * Fehlgriff im Speicher beendet das Programm, nicht das System.
 */
#ifndef RETROOS_H
#define RETROOS_H

typedef unsigned long size_t;
typedef long          ssize_t;
typedef unsigned char uint8_t;
typedef unsigned int  uint32_t;
typedef unsigned long uint64_t;

#define NULL ((void *)0)

/* --- Systemaufrufe --- */
void    sys_exit(int code) __attribute__((noreturn));
ssize_t sys_write(int fd, const void *buffer, size_t length);
ssize_t sys_read(int fd, void *buffer, size_t length);
int     sys_open(const char *path, int mode);   /* mode 1 = anlegen */
int     sys_close(int fd);
long    sys_seek(int fd, long position);
void   *sys_sbrk(long delta);
void    sys_sleep(unsigned milliseconds);
void    sys_yield(void);
int     sys_getpid(void);
uint64_t sys_uptime(void);
int     sys_args(char *buffer, size_t length);
long    sys_filesize(int fd);
int     sys_readdir(const char *path, unsigned index, char *buffer, size_t length);

/* --- Kleine Bequemlichkeiten --- */
size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);
char   *strcpy(char *dst, const char *src);
void   *memset(void *dst, int value, size_t n);
void   *memcpy(void *dst, const void *src, size_t n);

void    print(const char *text);
void    println(const char *text);
/* Unterstuetzt %s %d %u %x %c %% und die Breite ("%5d"). */
void    printf(const char *format, ...);

void   *malloc(size_t size);
void    free(void *pointer);

int     atoi(const char *text);

#endif /* RETROOS_H */
