/* config.c - Einstellungen lesen und schreiben. */

#include "config.h"
#include "font.h"
#include "gfx.h"
#include "perm.h"
#include "user.h"
#include "display.h"
#include "keymap.h"
#include "lang.h"
#include "kstring.h"
#include "mm.h"
#include "net.h"
#include "vfs.h"
#include "wallpaper.h"

static struct config current;

/* Die waehlbaren Hintergruende - Verlaeufe von oben nach unten. */
static const struct {
    const char *name;
    uint32_t    top, bottom;
} backgrounds[] = {
    { "Tuerkis",   RGB(0x1B, 0x63, 0x6E), RGB(0x0B, 0x33, 0x3B) },
    { "Nachtblau", RGB(0x14, 0x2A, 0x5A), RGB(0x06, 0x0E, 0x22) },
    { "Waldgruen", RGB(0x1E, 0x5A, 0x2E), RGB(0x0A, 0x24, 0x14) },
    { "Weinrot",   RGB(0x5C, 0x1E, 0x28), RGB(0x24, 0x0A, 0x10) },
    { "Grau",      RGB(0x50, 0x50, 0x58), RGB(0x1E, 0x1E, 0x24) },
};

size_t background_count(void) { return ARRAY_LEN(backgrounds); }

const char *background_name(size_t index)
{
    return index < ARRAY_LEN(backgrounds) ? backgrounds[index].name : "";
}

void background_colors(size_t index, uint32_t *top, uint32_t *bottom)
{
    if (index >= ARRAY_LEN(backgrounds))
        index = 0;
    if (top)
        *top = backgrounds[index].top;
    if (bottom)
        *bottom = backgrounds[index].bottom;
}

struct config *config_current(void) { return &current; }

void config_defaults(void)
{
    memset(&current, 0, sizeof(current));
    strlcpy(current.language, "de", sizeof(current.language));
    strlcpy(current.keymap, "de", sizeof(current.keymap));
    current.clock = CLOCK_LOCAL;
    current.timezone = 60;                 /* Mitteleuropa */
    strlcpy(current.hostname, "retroos", sizeof(current.hostname));
    current.background = 0;
    strlcpy(current.font, font_name(0), sizeof(current.font));
}

/* ------------------------------------------------------------------ */
/* Lesen                                                               */
/* ------------------------------------------------------------------ */

static void trim(char *text)
{
    char *start = text;

    while (*start == ' ' || *start == '\t')
        start++;
    if (start != text)
        memmove(text, start, strlen(start) + 1);

    size_t len = strlen(text);

    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                       text[len - 1] == '\r'))
        text[--len] = '\0';
}

static int32_t to_number(const char *text)
{
    int32_t sign = 1;
    int32_t value = 0;

    if (*text == '-') { sign = -1; text++; }
    else if (*text == '+') { text++; }

    while (*text >= '0' && *text <= '9')
        value = value * 10 + (*text++ - '0');
    return value * sign;
}

static void apply_pair(const char *key, const char *value)
{
    if (strcasecmp(key, "sprache") == 0) {
        strlcpy(current.language, value, sizeof(current.language));
    } else if (strcasecmp(key, "tastatur") == 0) {
        strlcpy(current.keymap, value, sizeof(current.keymap));
    } else if (strcasecmp(key, "uhr") == 0) {
        current.clock = strcasecmp(value, "utc") == 0 ? CLOCK_UTC
                                                      : CLOCK_LOCAL;
    } else if (strcasecmp(key, "zeitzone") == 0) {
        current.timezone = to_number(value);
    } else if (strcasecmp(key, "rechnername") == 0) {
        strlcpy(current.hostname, value, sizeof(current.hostname));
    } else if (strcasecmp(key, "hintergrund") == 0) {
        int32_t n = to_number(value);

        if (n >= 0 && (size_t)n < ARRAY_LEN(backgrounds))
            current.background = (uint32_t)n;
    } else if (strcasecmp(key, "aufloesung") == 0) {
        strlcpy(current.resolution, value, sizeof(current.resolution));
    } else if (strcasecmp(key, "skalierung") == 0) {
        /* "auto" ist keine Zahl und soll auch keine sein: Wer nichts
         * einstellt, soll auf einem anderen Bildschirm wieder etwas
         * Passendes bekommen. */
        current.scale = strcasecmp(value, "auto") == 0
                            ? 0 : (uint32_t)to_number(value);
    } else if (strcasecmp(key, "hintergrundbild") == 0) {
        strlcpy(current.wallpaper, value, sizeof(current.wallpaper));
    } else if (strcasecmp(key, "schrift") == 0) {
        strlcpy(current.font, value, sizeof(current.font));
    }
    /* Unbekannte Schluessel werden stillschweigend uebergangen - eine
     * neuere Fassung darf mehr hineinschreiben. */
}

bool config_load(void)
{
    perm_system_begin();

    struct fs_node *file = fs_lookup(NULL, CONFIG_PATH);

    if (!file || file->type != FS_FILE || !fs_load(file) || !file->data ||
        file->size == 0 || file->size > 8192) {
        perm_system_end();
        return false;
    }

    size_t size = file->size;
    char *text = kmalloc(size + 1);

    if (!text) {
        perm_system_end();
        return false;
    }

    memcpy(text, file->data, size);
    text[size] = '\0';

    char *line = text;

    while (line && *line) {
        char *next = strchr(line, '\n');

        if (next)
            *next++ = '\0';

        /* Alles hinter einem Rautenzeichen ist Anmerkung. */
        char *hash = strchr(line, '#');

        if (hash)
            *hash = '\0';

        char *equals = strchr(line, '=');

        if (equals) {
            *equals = '\0';
            trim(line);
            trim(equals + 1);
            if (line[0])
                apply_pair(line, equals + 1);
        }
        line = next;
    }

    kfree(text);
    perm_system_end();
    return true;
}

/* ------------------------------------------------------------------ */
/* Schreiben                                                           */
/* ------------------------------------------------------------------ */

bool config_save(void)
{
    char text[640];
    char scale_text[8];

    if (current.scale == 0)
        strlcpy(scale_text, "auto", sizeof(scale_text));
    else
        ksnprintf(scale_text, sizeof(scale_text), "%u",
                  (unsigned)current.scale);

    ksnprintf(text, sizeof(text),
              "# Einstellungen von RetroOS\n"
              "# Diese Datei darf von Hand geaendert werden.\n"
              "\n"
              "sprache = %s\n"
              "tastatur = %s\n"
              "uhr = %s\n"
              "zeitzone = %d\n"
              "rechnername = %s\n"
              "hintergrund = %u\n"
              "hintergrundbild = %s\n"
              "aufloesung = %s\n"
              "skalierung = %s\n"
              "schrift = %s\n",
              current.language,
              current.keymap,
              current.clock == CLOCK_UTC ? "utc" : "lokal",
              (int)current.timezone,
              current.hostname,
              (unsigned)current.background,
              current.wallpaper,
              current.resolution,
              scale_text,
              current.font);

    /* Das Schreiben selbst erledigt das System; wer es anstossen darf,
     * entscheidet die Oberflaeche. */
    perm_system_begin();

    struct fs_node *file = fs_lookup(NULL, CONFIG_PATH);

    if (!file)
        file = fs_create_path(NULL, CONFIG_PATH, FS_FILE);

    bool ok = file && file->type == FS_FILE &&
              fs_write(file, text, strlen(text));

    if (ok) {
        /* Lesen darf jeder, aendern nur der Verwalter. */
        file->uid  = UID_ROOT;
        file->gid  = GID_ROOT;
        file->mode = 0644;
        perm_store_record(file);
    }

    perm_system_end();
    return ok;
}

void config_apply(void)
{
    lang_select_by_code(current.language);

    /* Erst der Bildschirm: Was danach kommt, rechnet mit seiner
     * Groesse. Ein Modus, den die Karte nicht kann, faellt aus der
     * Einstellung heraus - sonst stuende bei jedem Start derselbe
     * unmoegliche Wunsch da. */
    int32_t mode_w, mode_h;

    if (current.resolution[0] &&
        disp_parse_mode(current.resolution, &mode_w, &mode_h)) {
        if (!display_set_mode(mode_w, mode_h))
            current.resolution[0] = '\0';
    }
    display_set_scale(current.scale);
    keymap_select(current.keymap);
    strlcpy(g_netif.hostname, current.hostname, sizeof(g_netif.hostname));

    /* Steht ein unbekannter Name in der Datei, bleibt die eingebaute
     * Schrift stehen - lieber etwas Lesbares als gar nichts. */
    if (!font_select_by_name(current.font))
        strlcpy(current.font, font_name(font_current()), sizeof(current.font));

    /* Ein Bild, das es nicht mehr gibt, soll die Einstellung nicht
     * vergiften: Der Eintrag faellt weg, und der Verlauf uebernimmt
     * wieder. Sonst stuende bei jedem Start derselbe tote Pfad da. */
    if (current.wallpaper[0]) {
        perm_system_begin();
        bool ok = wallpaper_set(current.wallpaper);
        perm_system_end();
        if (!ok)
            current.wallpaper[0] = '\0';
    } else {
        wallpaper_set(NULL);
    }
}
