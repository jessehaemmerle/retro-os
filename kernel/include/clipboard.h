/* clipboard.h - Zwischenablage, von allen Programmen geteilt. */
#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "retro.h"

/* Legt eine Kopie ab; bytes == 0 leert die Ablage. */
bool clipboard_set(const char *text, size_t bytes);

/* Der abgelegte Text, oder NULL. Der Zeiger gehoert der Ablage. */
const char *clipboard_get(size_t *bytes);

bool clipboard_empty(void);
void clipboard_clear(void);

#endif /* CLIPBOARD_H */
