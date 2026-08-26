/* serial.h - COM1 als Debug-Konsole. */
#ifndef SERIAL_H
#define SERIAL_H

#include "retro.h"

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

#endif /* SERIAL_H */
