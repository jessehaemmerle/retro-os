/* programs.c - die mitgelieferten Benutzerprogramme.
 *
 * Sie werden beim Uebersetzen als Ganzes in das Kernel-Abbild
 * hineingelegt und beim Start unter /Programme abgelegt. So sind sie auch
 * ohne Festplatte da; kopieren laesst sich von dort alles Weitere.
 */

#include "vfs.h"
#include "kstring.h"

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
};

void programs_install(struct fs_node *directory)
{
    char index[512];
    size_t used = 0;

    used += (size_t)ksnprintf(index + used, sizeof(index) - used,
                              "Mitgelieferte Programme\n"
                              "-----------------------\n");

    for (size_t i = 0; i < ARRAY_LEN(builtin); i++) {
        struct fs_node *file = fs_create(directory, builtin[i].name, FS_FILE);
        size_t size = (size_t)(builtin[i].end - builtin[i].start);

        if (file) {
            fs_write(file, builtin[i].start, size);
            file->readonly = true;
        }

        used += (size_t)ksnprintf(index + used, sizeof(index) - used,
                                  "  %-14s %s\n", builtin[i].name,
                                  builtin[i].description);
    }

    ksnprintf(index + used, sizeof(index) - used,
              "\nStarten mit:  starte /Programme/hallo.elf\n");

    struct fs_node *readme = fs_create(directory, "liesmich.txt", FS_FILE);
    if (readme) {
        fs_write(readme, index, strlen(index));
        readme->readonly = true;
    }
}
