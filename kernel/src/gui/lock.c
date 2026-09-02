/* lock.c - Anmelden, Sperren und Benutzer wechseln.
 *
 * Der Bildschirm zeigt links die Benutzer und rechts das Passwortfeld.
 * Wer keinen Namen anklickt, tippt seinen einfach - deshalb hat auch
 * der Name ein Feld. Mit der Tabulatortaste geht es zwischen beiden hin
 * und her, mit der Eingabetaste weiter.
 *
 * Nach drei Fehlversuchen wird eine Weile gewartet, bevor der naechste
 * angenommen wird. Das haelt niemanden auf, der sein Passwort kennt,
 * und macht das Durchprobieren an der Tastatur aussichtslos.
 */

#include "lock.h"

#include "apps.h"
#include "arch.h"
#include "config.h"
#include "gui.h"
#include "icons.h"
#include "kstring.h"
#include "audit.h"
#include "log.h"
#include "net.h"
#include "perm.h"
#include "rtc.h"
#include "theme.h"
#include "thread.h"
#include "user.h"
#include "widgets.h"

#define PASS_MAX     63
#define NAME_MAX     (USER_NAME_MAX)
#define ROW_H        34
#define LIST_W       260
#define PANEL_W      640
#define PANEL_H      340
#define TRY_LIMIT    3
#define TRY_PAUSE_MS 5000

enum focus_field { F_NAME, F_PASS };

static bool     active;
static enum lock_reason reason;
static char     name_text[NAME_MAX + 1];
static char     pass_text[PASS_MAX + 1];
static int      focus;
static int      hover;              /* -1 = nichts, sonst Zeilennummer */
static bool     hover_button;
static int      scroll;
static char     message[96];
static int      tries;
static uint64_t blocked_until;
static bool     caret;
static uint32_t ticks;

/* Wer war zuletzt angemeldet? Beim Sperren bleibt der Name stehen -
 * nur das Passwort ist wieder leer. */
static struct user *locked_user;

/* ------------------------------------------------------------------ */
/* Masse                                                               */
/* ------------------------------------------------------------------ */

static struct rect panel_rect(void)
{
    struct canvas *s = gfx_screen();

    return rect_make((s->w - PANEL_W) / 2, (s->h - PANEL_H) / 2 - 20,
                     PANEL_W, PANEL_H);
}

static struct rect list_rect(void)
{
    struct rect p = panel_rect();

    return rect_make(p.x + 16, p.y + 60, LIST_W, p.h - 76);
}

static struct rect name_rect(void)
{
    struct rect p = panel_rect();

    return rect_make(p.x + LIST_W + 40, p.y + 96, p.w - LIST_W - 56, 26);
}

static struct rect pass_rect(void)
{
    struct rect r = name_rect();

    r.y += 62;
    return r;
}

static struct rect button_rect(void)
{
    struct rect r = pass_rect();

    return rect_make(r.x + r.w - 140, r.y + 56, 140, 30);
}

static size_t visible_rows(void)
{
    return (size_t)(list_rect().h / ROW_H);
}

/* Nur wer sich anmelden darf, steht auch da. Ein gesperrtes Konto
 * aufzufuehren waere eine Einladung, es auszuprobieren. */
static bool listed(const struct user *u)
{
    return u && u->used && !u->locked;
}

static size_t listed_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < user_count(); i++)
        if (listed(user_at(i)))
            n++;
    return n;
}

static struct user *listed_at(size_t index)
{
    for (size_t i = 0; i < user_count(); i++) {
        struct user *u = user_at(i);

        if (!listed(u))
            continue;
        if (index-- == 0)
            return u;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Zustand                                                             */
/* ------------------------------------------------------------------ */

bool lock_active(void) { return active; }

void lock_show(enum lock_reason why)
{
    reason = why;
    active = true;
    focus  = F_PASS;
    hover  = -1;
    hover_button = false;
    scroll = 0;
    tries  = 0;
    blocked_until = 0;
    pass_text[0] = '\0';
    message[0]   = '\0';

    if (why == LOCK_LOCKED) {
        locked_user = session_user();
        if (locked_user)
            strlcpy(name_text, locked_user->name, sizeof(name_text));
    } else {
        locked_user = NULL;
        session_logout();
        if (why != LOCK_SWITCH)
            name_text[0] = '\0';
        focus = name_text[0] ? F_PASS : F_NAME;
    }

    if (!name_text[0]) {
        struct user *first = listed_at(0);

        if (first && listed_count() == 1)
            strlcpy(name_text, first->name, sizeof(name_text));
    }

    gui_invalidate();
}

static const char *headline(void)
{
    switch (reason) {
    case LOCK_LOCKED: return "Gesperrt";
    case LOCK_SWITCH: return "Benutzer wechseln";
    case LOCK_LOGOUT: return "Abgemeldet";
    default:          return "Anmelden";
    }
}

static void finish(struct user *u)
{
    active = false;
    pass_text[0] = '\0';
    message[0]   = '\0';

    /* Beim blossen Sperren blieben alle Fenster stehen; dann ist nichts
     * weiter zu tun, als sie wieder freizugeben. */
    if (reason != LOCK_LOCKED || session_user() != u)
        session_login(u);

    gui_invalidate();
}

static void attempt(void)
{
    uint64_t now = timer_ms();

    if (now < blocked_until) {
        ksnprintf(message, sizeof(message),
                  "Noch %u Sekunden warten.",
                  (unsigned)((blocked_until - now + 999) / 1000));
        return;
    }

    struct user *u = user_by_name(name_text);

    /* Beim Sperren kommt nur der wieder herein, der gesperrt hat - sonst
     * waere die Sperre ein bequemer Weg, an eine fremde Sitzung samt
     * geoeffneten Fenstern zu kommen. */
    if (reason == LOCK_LOCKED && locked_user && u != locked_user)
        u = NULL;

    if (u && user_check_password(u, pass_text)) {
        finish(u);
        return;
    }

    /* Fehlversuche gehoeren ins Protokoll - einzeln sind sie belanglos,
     * in Serie sind sie das Interessanteste, was der Rechner zu
     * erzaehlen hat. */
    log_warn("anmeldung", "Fehlversuch fuer \"%s\"",
             name_text[0] ? name_text : "(ohne Namen)");
    audit(AUDIT_LOGIN, false, "%s",
          name_text[0] ? name_text : "(ohne Namen)");

    pass_text[0] = '\0';
    focus = F_PASS;

    if (++tries >= TRY_LIMIT) {
        tries = 0;
        blocked_until = timer_ms() + TRY_PAUSE_MS;
        log_warn("anmeldung", "Drei Fehlversuche - %u Sekunden Pause",
                 (unsigned)(TRY_PAUSE_MS / 1000));
        strlcpy(message, "Zu viele Versuche - bitte kurz warten.",
                sizeof(message));
    } else {
        strlcpy(message, "Name oder Passwort stimmt nicht.", sizeof(message));
    }
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static void paint_row(struct canvas *c, struct rect r, struct user *u,
                      bool selected, bool hot)
{
    if (selected)
        gfx_fill(c, r, COL_SELECT);
    else if (hot)
        gfx_fill(c, r, COL_FACE_LIGHT);

    uint32_t fg = selected ? COL_SELECT_TEXT : COL_TEXT;

    icon_draw(c, r.x + 6, r.y + (r.h - 16) / 2,
              user_is_admin(u) ? ICON_SHIELD : ICON_USER, 1);
    gfx_text_bold(c, r.x + 30, r.y + 4, u->name, fg);
    gfx_text(c, r.x + 30, r.y + 18, u->full,
             selected ? COL_SELECT_TEXT : COL_TEXT_DIM);
}

void lock_paint(struct canvas *c)
{
    struct rect screen = rect_make(0, 0, c->w, c->h);
    uint32_t top, bottom;

    /* Derselbe Verlauf wie auf dem Desktop, nur dunkler: Man soll
     * sehen, dass es der eigene Rechner ist, und zugleich, dass er
     * gerade nicht bedienbar ist. */
    background_colors(config_current()->background, &top, &bottom);
    gfx_gradient_v(c, screen, top / 2 & 0x7F7F7F, bottom / 2 & 0x7F7F7F);

    struct rect p = panel_rect();

    gfx_fill(c, p, COL_FACE);
    gfx_bevel(c, p, true);
    gfx_gradient_v(c, rect_make(p.x + 3, p.y + 3, p.w - 6, 40),
                   COL_TITLE_A1, COL_TITLE_A2);
    icon_draw(c, p.x + 12, p.y + 11, ICON_LOCK, 1);
    gfx_text_bold(c, p.x + 36, p.y + 15, headline(), COL_TITLE_TEXT);

    char right[64];

    ksnprintf(right, sizeof(right), "RetroOS auf %s",
              g_netif.hostname[0] ? g_netif.hostname : "retroos");
    gfx_text(c, p.x + p.w - gfx_text_width(right) - 14, p.y + 15, right,
             COL_TITLE_TEXT);

    /* --- Liste der Benutzer --- */
    struct rect l = list_rect();

    gfx_fill(c, l, COL_FIELD);
    gfx_bevel_thin(c, l, false);
    gfx_set_clip(c, l);

    size_t count = listed_count();
    size_t rows  = visible_rows();

    for (size_t i = 0; i < rows && i + (size_t)scroll < count; i++) {
        struct user *u = listed_at(i + (size_t)scroll);
        struct rect  r = rect_make(l.x + 1, l.y + 1 + (int32_t)i * ROW_H,
                                   l.w - 2, ROW_H);

        paint_row(c, r, u, strcasecmp(u->name, name_text) == 0,
                  hover == (int)(i + (size_t)scroll));
    }
    gfx_reset_clip(c);

    /* --- Felder --- */
    struct rect nr = name_rect();
    struct rect pr = pass_rect();

    gfx_text(c, nr.x, nr.y - 16, "Benutzer", COL_TEXT_DIM);
    widget_field(c, nr, name_text,
                 focus == F_NAME && caret ? (int32_t)strlen(name_text) : -1,
                 focus == F_NAME);

    /* Vom Passwort wird nur die Laenge gezeigt - und die auch nur, weil
     * man sonst nicht merkt, ob die Tastatur ankommt. */
    char stars[PASS_MAX + 1];
    size_t len = strlen(pass_text);

    for (size_t i = 0; i < len; i++)
        stars[i] = '*';
    stars[len] = '\0';

    gfx_text(c, pr.x, pr.y - 16, "Passwort", COL_TEXT_DIM);
    widget_field(c, pr, stars,
                 focus == F_PASS && caret ? (int32_t)len : -1,
                 focus == F_PASS);

    widget_button(c, button_rect(), "Anmelden", hover_button, true);

    /* Die Meldung steht zwischen Feld und Knopf - dort, wo der Blick
     * nach der Eingabe ohnehin hinwandert, und ohne den Knopf zu
     * ueberdecken. */
    if (message[0])
        gfx_text(c, pr.x, pr.y + 32, message, RGB(0xA0, 0x10, 0x10));

    /* --- Fusszeile --- */
    struct datetime dt;

    rtc_read(&dt);

    char clock[64];

    ksnprintf(clock, sizeof(clock), "%02u.%02u.%04u  %02u:%02u",
              dt.day, dt.month, dt.year, dt.hour, dt.minute);
    gfx_text(c, p.x, p.y + p.h + 12, clock, COL_WHITE);

    const char *hint = user_store_exists()
        ? "Tabulator wechselt das Feld, Eingabe meldet an."
        : "Es gibt noch keine Benutzerdatenbank - melde dich als root an.";

    gfx_text(c, p.x + p.w - gfx_text_width(hint), p.y + p.h + 12, hint,
             COL_WHITE);
}

void lock_tick(void)
{
    if (!active)
        return;

    /* Die Schreibmarke blinkt zweimal je Sekunde. */
    if (++ticks % 5 == 0) {
        caret = !caret;
        gui_invalidate();
    }
}

/* ------------------------------------------------------------------ */
/* Eingabe                                                             */
/* ------------------------------------------------------------------ */

static void type_into(char *buffer, size_t size, const struct key_event *ke)
{
    size_t len = strlen(buffer);

    if (ke->key == KEY_BACKSPACE) {
        if (len)
            buffer[len - 1] = '\0';
        return;
    }
    if ((unsigned char)ke->ascii >= 32 && len + 1 < size)
        buffer[len] = ke->ascii, buffer[len + 1] = '\0';
}

void lock_key(const struct key_event *ke)
{
    if (!active || !ke->pressed)
        return;

    switch (ke->key) {
    case KEY_TAB:
        focus = focus == F_NAME ? F_PASS : F_NAME;
        break;
    case KEY_ENTER:
        if (focus == F_NAME && !pass_text[0])
            focus = F_PASS;
        else
            attempt();
        break;
    case KEY_ESCAPE:
        pass_text[0] = '\0';
        message[0]   = '\0';
        break;
    case KEY_UP:
    case KEY_DOWN: {
        /* Mit den Pfeiltasten durch die Liste - dann kommt man auch
         * ohne Maus an einen anderen Namen. */
        size_t count = listed_count();

        if (!count)
            break;

        size_t index = 0;

        for (size_t i = 0; i < count; i++)
            if (strcasecmp(listed_at(i)->name, name_text) == 0)
                index = i;

        if (ke->key == KEY_UP)
            index = index ? index - 1 : count - 1;
        else
            index = index + 1 < count ? index + 1 : 0;

        strlcpy(name_text, listed_at(index)->name, sizeof(name_text));
        pass_text[0] = '\0';
        focus = F_PASS;
        break;
    }
    default:
        if (focus == F_NAME)
            type_into(name_text, sizeof(name_text), ke);
        else
            type_into(pass_text, sizeof(pass_text), ke);
        break;
    }

    caret = true;
    gui_invalidate();
}

void lock_mouse(int32_t x, int32_t y, uint8_t button, bool down)
{
    if (!active)
        return;

    struct rect l = list_rect();
    int before_hover = hover;
    bool before_button = hover_button;

    hover = -1;
    if (rect_contains(l, x, y)) {
        size_t row = (size_t)((y - l.y - 1) / ROW_H) + (size_t)scroll;

        if (row < listed_count())
            hover = (int)row;
    }
    hover_button = rect_contains(button_rect(), x, y);

    if (hover != before_hover || hover_button != before_button)
        gui_invalidate();

    if (!down || button != MB_LEFT)
        return;

    if (hover >= 0) {
        strlcpy(name_text, listed_at((size_t)hover)->name, sizeof(name_text));
        pass_text[0] = '\0';
        message[0]   = '\0';
        focus = F_PASS;
    } else if (rect_contains(name_rect(), x, y)) {
        focus = F_NAME;
    } else if (rect_contains(pass_rect(), x, y)) {
        focus = F_PASS;
    } else if (hover_button) {
        attempt();
    }

    caret = true;
    gui_invalidate();
}
