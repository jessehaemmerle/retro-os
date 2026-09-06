/* theme.h - die Farbwelt von RetroOS.
 *
 * Bewusst am Look der Neunziger orientiert: grauer Kunststoff, harte
 * 3D-Kanten, kraeftige Titelleisten. Seit es einen Dunkelmodus gibt,
 * steht die Farbwelt aber nicht mehr fest: Die COL_-Namen zeigen nicht
 * mehr auf feste Werte, sondern in die gerade gueltige Tafel.
 *
 * Das kostet je Farbe einen Speicherzugriff und spart, dass 300
 * Stellen im Quelltext wissen muessten, welcher Modus gerade laeuft.
 * Wer eine Farbe braucht, schreibt COL_TEXT wie zuvor und bekommt die
 * richtige.
 *
 * Nicht dabei sind Farben, die keine Meinung haben: das Schwarz einer
 * Zeigerkontur und das Weiss darin bleiben schwarz und weiss.
 */
#ifndef THEME_H
#define THEME_H

#include "gfx.h"

struct theme {
    const char *name;
    bool        dark;

    uint32_t desktop_top, desktop_bottom;
    uint32_t face, face_light, hilight, shadow, dark_edge;
    uint32_t title_a1, title_a2, title_i1, title_i2, title_text;
    uint32_t text, text_dim, select, select_text;
    uint32_t field, taskbar, accent;
};

/* Die gerade gueltige Tafel. Zeigt immer auf eine der eingebauten. */
extern const struct theme *g_theme;

#define COL_DESKTOP_TOP     (g_theme->desktop_top)
#define COL_DESKTOP_BOTTOM  (g_theme->desktop_bottom)

#define COL_FACE            (g_theme->face)
#define COL_FACE_LIGHT      (g_theme->face_light)
#define COL_HILIGHT         (g_theme->hilight)
#define COL_SHADOW          (g_theme->shadow)
#define COL_DARK            (g_theme->dark_edge)
#define COL_BLACK           RGB(0x00, 0x00, 0x00)
#define COL_WHITE           RGB(0xFF, 0xFF, 0xFF)

#define COL_TITLE_A1        (g_theme->title_a1)
#define COL_TITLE_A2        (g_theme->title_a2)
#define COL_TITLE_I1        (g_theme->title_i1)
#define COL_TITLE_I2        (g_theme->title_i2)
#define COL_TITLE_TEXT      (g_theme->title_text)

#define COL_TEXT            (g_theme->text)
#define COL_TEXT_DIM        (g_theme->text_dim)
#define COL_SELECT          (g_theme->select)
#define COL_SELECT_TEXT     (g_theme->select_text)

#define COL_FIELD           (g_theme->field)
#define COL_TASKBAR         (g_theme->taskbar)
#define COL_ACCENT          (g_theme->accent)

#define TITLEBAR_HEIGHT     22
#define TASKBAR_HEIGHT      30
#define BORDER_WIDTH        3

/* Was der Benutzer eingestellt hat. "Automatisch" ist keine eigene
 * Tafel, sondern die Frage nach der Uhrzeit. */
enum theme_mode {
    THEME_HELL,
    THEME_DUNKEL,
    THEME_AUTO,
};

void            theme_set_mode(enum theme_mode mode);
enum theme_mode theme_mode(void);
bool            theme_is_dark(void);
const char     *theme_mode_name(enum theme_mode mode);
/* Aus der Konfigurationsdatei: "hell", "dunkel", "automatisch". */
bool            theme_mode_parse(const char *text, enum theme_mode *out);
const char     *theme_mode_key(enum theme_mode mode);

/* Im automatischen Modus ist es zwischen 19 und 7 Uhr dunkel. Getrennt
 * herausgezogen, weil sich genau hier die Randstunden pruefen
 * lassen. */
bool theme_dark_for_hour(int32_t hour);

/* Rechnet einmal je Minute nach, ob der automatische Modus jetzt
 * umschalten muss. true, wenn sich die Tafel geaendert hat. */
bool theme_tick(int32_t hour);

/* Verdunkelt eine Farbe auf einen Bruchteil (100 = unveraendert).
 * Damit bleibt der gewaehlte Hintergrundverlauf im Dunkelmodus
 * derselbe - nur eben gedaempft. */
uint32_t theme_shade(uint32_t color, int32_t percent);

#endif /* THEME_H */
