/* icons.h - die Symbolsammlung der Oberflaeche.
 *
 * Die Bilder stammen aus dem Lucide-Satz (ISC-Lizenz, siehe
 * third_party/lucide/LICENSE) und liegen als fertige Punktbilder im
 * Kernel - erzeugt von scripts/gen_icons.py. Handgemalte Textgrafik
 * war das vorher; ein gepflegter Zeichensatz sieht nicht nur besser
 * aus, er bringt auch fuer jedes neue Programm gleich ein passendes
 * Bild mit.
 *
 * Jedes Symbol gibt es in zwei Groessen. Ein 16er auf das Doppelte zu
 * ziehen wuerde grob aussehen; deshalb ist das 32er eigens gezeichnet.
 * Die Strichzeichnungen tragen eine dunkle Umrandung, damit sie auf
 * dem Desktop wie in einer hellen Werkzeugleiste lesbar bleiben.
 */
#ifndef ICONS_H
#define ICONS_H

#include "gfx.h"

#define ICON_SIZE       16
#define ICON_SIZE_LARGE 32

enum icon_id {
    ICON_FOLDER,
    ICON_FOLDER_OPEN,
    ICON_FILE,
    ICON_FILE_TEXT,
    ICON_DISK,
    ICON_COMPUTER,
    ICON_TERMINAL,
    ICON_EDITOR,
    ICON_INFO,
    ICON_TRASH,
    ICON_TRASH_FULL,
    ICON_UP,
    ICON_BACK,
    ICON_HOME,
    ICON_NEW_FOLDER,
    ICON_NEW_FILE,
    ICON_SETTINGS,
    ICON_BROWSER,
    ICON_RELOAD,
    ICON_NETWORK,
    ICON_CODE,
    ICON_DOWNLOAD,
    ICON_IMAGE,
    ICON_RESTORE,
    ICON_PLAY,
    ICON_SAVE,
    ICON_CLOCK,

    /* Systemmonitor, Aufgaben und Protokoll */
    ICON_MONITOR,
    ICON_TASKS,
    ICON_LOG,
    ICON_WARN,
    ICON_DONE,
    ICON_CALENDAR,
    ICON_FLAG,
    ICON_STOP,

    /* Benutzer, Gruppen und Rechte */
    ICON_USER,
    ICON_USERS,
    ICON_USER_ADD,
    ICON_LOCK,
    ICON_KEY,
    ICON_SHIELD,
    ICON_LOGOUT,

    /* Bueroprogramme */
    ICON_TABLE,
    ICON_DOCUMENT,
    ICON_SLIDES,
    ICON_PRESENT,
    ICON_BOLD,
    ICON_ITALIC,
    ICON_UNDERLINE,
    ICON_ALIGN_LEFT,
    ICON_ALIGN_MID,
    ICON_ALIGN_RIGHT,
    ICON_LIST,
    ICON_HEADING,
    ICON_SUM,
    ICON_PLUS,
    ICON_PREV,
    ICON_NEXT,

    /* Rechner und Bildschirmfoto */
    ICON_CALC,
    ICON_CAMERA,

    /* Archiv, Kalender und Uhr */
    ICON_ARCHIVE,
    ICON_ALARM,

    ICON_COUNT
};

/* Die Punktbilder, ein Wort je Punkt: 0xAARRGGBB. */
extern const uint32_t *const icon_bits16[ICON_COUNT];
extern const uint32_t *const icon_bits32[ICON_COUNT];

/* scale 1 zeichnet das 16er, scale 2 das 32er; groessere Werte ziehen
 * das 32er auf. */
void icon_draw(struct canvas *c, int32_t x, int32_t y, enum icon_id id,
               int32_t scale);

#endif /* ICONS_H */
