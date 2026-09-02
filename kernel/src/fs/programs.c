/* programs.c - die mitgelieferten Benutzerprogramme.
 *
 * Sie werden beim Uebersetzen als Ganzes in das Kernel-Abbild
 * hineingelegt und beim Start unter /Programme abgelegt. So sind sie auch
 * ohne Festplatte da; kopieren laesst sich von dort alles Weitere.
 */

#include "vfs.h"
#include "kstring.h"

#include <stdarg.h>

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#define PROGRAM(name)                                       \
    extern const uint8_t _binary_##name##_elf_start[];      \
    extern const uint8_t _binary_##name##_elf_end[]

PROGRAM(hallo);
PROGRAM(zaehler);
PROGRAM(katze);
PROGRAM(liste);
PROGRAM(schreiben);
PROGRAM(absturz);
PROGRAM(schutz);
PROGRAM(uhr);
PROGRAM(abrufen);
PROGRAM(server);
PROGRAM(gabeln);
PROGRAM(roehre);

#define ENTRY(name, beschreibung)                                \
    { #name ".elf", _binary_##name##_elf_start,                  \
      _binary_##name##_elf_end, beschreibung }

static const struct {
    const char    *name;
    const uint8_t *start;
    const uint8_t *end;
    const char    *description;
} builtin[] = {
    ENTRY(hallo,     "gibt eine Begruessung aus"),
    ENTRY(zaehler,   "zaehlt langsam hoch"),
    ENTRY(katze,     "gibt eine Datei aus"),
    ENTRY(liste,     "listet einen Ordner auf"),
    ENTRY(schreiben, "legt eine Datei an"),
    ENTRY(absturz,   "greift absichtlich daneben"),
    ENTRY(schutz,    "prueft die Ausfuehrsperre"),
    ENTRY(uhr,       "eine Uhr mit eigenem Fenster"),
    ENTRY(abrufen,   "holt eine Seite aus dem Netz"),
    ENTRY(server,    "liefert die Ablage im Netz aus"),
    ENTRY(gabeln,    "spaltet sich ab und zaehlt getrennt weiter"),
    ENTRY(roehre,    "Roehre und geteilter Speicher zwischen Eltern und Kind"),
};

/* ksnprintf meldet, wieviel gepasst haette - nicht, wieviel es
 * geschrieben hat. Wer den Rueckgabewert einfach aufsummiert, laeuft
 * beim ersten zu kleinen Puffer aus dem Feld heraus und schreibt
 * danach an einer Adresse hinter seinem Ende. Deshalb hier eine
 * Fassung, die den Stand immer im Puffer haelt. */
static void append_line(char *buffer, size_t room, size_t *used,
                        const char *format, ...)
{
    if (*used + 1 >= room)
        return;

    va_list args;

    va_start(args, format);
    int wrote = kvsnprintf(buffer + *used, room - *used, format, args);
    va_end(args);

    if (wrote < 0)
        return;

    *used += (size_t)wrote;
    if (*used >= room)
        *used = room - 1;
}

void programs_install(struct fs_node *directory)
{
    char index[1024];
    size_t used = 0;

    append_line(index, sizeof(index), &used,
                "Mitgelieferte Programme\n"
                "-----------------------\n");

    for (size_t i = 0; i < ARRAY_LEN(builtin); i++) {
        struct fs_node *file = fs_create(directory, builtin[i].name, FS_FILE);
        size_t size = (size_t)(builtin[i].end - builtin[i].start);

        if (file) {
            fs_write(file, builtin[i].start, size);
            file->readonly = true;
        }

        append_line(index, sizeof(index), &used, "  %-14s %s\n",
                    builtin[i].name, builtin[i].description);
    }

    append_line(index, sizeof(index), &used,
                "\nStarten mit:  starte /Programme/hallo.elf\n");

    struct fs_node *readme = fs_create(directory, "liesmich.txt", FS_FILE);
    if (readme) {
        fs_write(readme, index, strlen(index));
        readme->readonly = true;
    }
}
