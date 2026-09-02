/* usersapp.c - die Benutzerverwaltung.
 *
 * Links stehen die Benutzer, rechts eine Leiste mit dem, was sich mit
 * dem ausgewaehlten anstellen laesst. Wer kein Verwalter ist, sieht
 * dieselbe Liste, kann darin aber nur eines: sein eigenes Passwort
 * aendern. Das ist Absicht - zu wissen, wer sonst noch an diesem
 * Rechner arbeitet, ist keine Geheimsache; die Konten der anderen
 * anzufassen schon.
 *
 * Gespeichert wird erst auf Knopfdruck. Bis dahin lebt alles nur im
 * Speicher, und ein Fehlgriff kostet nichts weiter als das Fenster
 * zuzumachen.
 */

#include "apps.h"

#include "icons.h"
#include "kstring.h"
#include "mm.h"
#include "perm.h"
#include "theme.h"
#include "user.h"
#include "vfs.h"
#include "widgets.h"

#define ROW_H      36
#define LIST_W     330
#define SIDE_X     (LIST_W + 24)

enum button_id {
    BTN_NEW,
    BTN_PASSWORD,
    BTN_ADMIN,
    BTN_LOCK,
    BTN_DELETE,
    BTN_SAVE,
    BTN_COUNT
};

struct users_ui {
    uint32_t selected;        /* uid des ausgewaehlten Eintrags */
    int      hover;
    int      scroll;
    bool     changed;
    char     status[96];
    /* Ein neuer Benutzer entsteht in zwei Schritten: erst der Name,
     * dann das Passwort. Dazwischen steht er hier. */
    char     pending[USER_NAME_MAX + 1];
};

static struct window *the_window;

/* ------------------------------------------------------------------ */

static struct users_ui *ui_of(void)
{
    return the_window && gui_window_alive(the_window) ? the_window->user : NULL;
}

static struct user *selected_user(struct users_ui *ui)
{
    return user_by_uid(ui->selected);
}

static struct rect list_rect(struct window *win)
{
    return rect_make(10, 10, LIST_W, gui_client_height(win) - 52);
}

static struct rect button_rect(struct window *win, int index)
{
    int32_t w = gui_client_width(win) - SIDE_X - 12;

    return rect_make(SIDE_X, 92 + index * 34, w, 28);
}

static struct rect save_rect(struct window *win)
{
    return rect_make(SIDE_X, gui_client_height(win) - 42,
                     gui_client_width(win) - SIDE_X - 12, 28);
}

static const char *button_label(struct users_ui *ui, int id)
{
    struct user *u = selected_user(ui);

    switch (id) {
    case BTN_NEW:      return "Neuer Benutzer";
    case BTN_PASSWORD: return "Passwort aendern";
    case BTN_ADMIN:    return u && u->admin ? "Verwalterrecht nehmen"
                                            : "Zum Verwalter machen";
    case BTN_LOCK:     return u && u->locked ? "Anmeldung freigeben"
                                             : "Anmeldung sperren";
    case BTN_DELETE:   return "Benutzer entfernen";
    default:           return "Speichern";
    }
}

/* Was darf der, der gerade davorsitzt? */
static bool button_enabled(struct users_ui *ui, int id)
{
    struct user *u = selected_user(ui);
    bool admin = session_is_admin();

    if (id == BTN_PASSWORD) {
        /* Das eigene Passwort darf jeder aendern, fremde nur ein
         * Verwalter. */
        return u && (admin || u == session_user());
    }
    if (!admin)
        return false;

    switch (id) {
    case BTN_NEW:    return true;
    case BTN_ADMIN:  return u && u->uid != UID_ROOT;
    case BTN_LOCK:   return u && u->uid != UID_ROOT && u != session_user();
    case BTN_DELETE: return u && u->uid != UID_ROOT && u != session_user();
    default:         return true;
    }
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static void paint_details(struct canvas *c, struct window *win,
                          struct users_ui *ui)
{
    struct user *u = selected_user(ui);

    if (!u) {
        gfx_text(c, SIDE_X, 16, "Kein Benutzer ausgewaehlt.", COL_TEXT_DIM);
        return;
    }

    char line[96];

    icon_draw(c, SIDE_X, 12, u->admin ? ICON_SHIELD : ICON_USER, 2);
    gfx_text_bold(c, SIDE_X + 40, 14, u->name, COL_TEXT);
    gfx_text(c, SIDE_X + 40, 30, u->full, COL_TEXT_DIM);

    ksnprintf(line, sizeof(line), "Nummer %u, Gruppe %s%s%s",
              (unsigned)u->uid, group_name_of(u->gid),
              u->nopass ? ", ohne Passwort" : "",
              u->locked ? ", gesperrt" : "");
    gfx_text(c, SIDE_X, 52, line, COL_TEXT_DIM);
    gfx_text_clipped(c, SIDE_X, 68, u->home, COL_TEXT_DIM,
                     gui_client_width(win) - SIDE_X - 12);
}

static void users_paint(struct window *win, struct canvas *c)
{
    struct users_ui *ui = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    struct rect l = list_rect(win);

    gfx_fill(&local, l, COL_FIELD);
    gfx_bevel_thin(&local, l, false);
    gfx_set_clip(&local, l);

    size_t count = user_count();
    size_t rows  = (size_t)(l.h / ROW_H);

    for (size_t i = 0; i < rows && i + (size_t)ui->scroll < count; i++) {
        struct user *u = user_at(i + (size_t)ui->scroll);
        struct rect  r = rect_make(l.x + 1, l.y + 1 + (int32_t)i * ROW_H,
                                   l.w - 2, ROW_H);
        bool sel = u->uid == ui->selected;

        if (sel)
            gfx_fill(&local, r, COL_SELECT);
        else if (ui->hover == (int)(i + (size_t)ui->scroll))
            gfx_fill(&local, r, COL_FACE_LIGHT);

        uint32_t fg = sel ? COL_SELECT_TEXT : COL_TEXT;

        icon_draw(&local, r.x + 6, r.y + (r.h - 16) / 2,
                  u->locked ? ICON_LOCK : (u->admin ? ICON_SHIELD : ICON_USER), 1);
        gfx_text_bold(&local, r.x + 30, r.y + 4, u->name, fg);

        char note[80];

        ksnprintf(note, sizeof(note), "%s - %u", u->full, (unsigned)u->uid);
        gfx_text(&local, r.x + 30, r.y + 19, note,
                 sel ? COL_SELECT_TEXT : COL_TEXT_DIM);
    }
    gfx_reset_clip(&local);

    if (count > rows)
        widget_vscroll(&local, rect_make(l.x + l.w - SCROLLBAR_WIDTH, l.y,
                                         SCROLLBAR_WIDTH, l.h),
                       ui->scroll, (int32_t)count, (int32_t)rows);

    paint_details(&local, win, ui);

    for (int i = 0; i < BTN_SAVE; i++)
        widget_button(&local, button_rect(win, i), button_label(ui, i),
                      ui->hover == -100 - i, button_enabled(ui, i));

    widget_button(&local, save_rect(win), "Speichern",
                  ui->hover == -100 - BTN_SAVE, button_enabled(ui, BTN_SAVE));

    if (ui->status[0])
        gfx_text_clipped(&local, 10, local.h - 36, ui->status, COL_TEXT_DIM,
                         LIST_W);
    if (ui->changed)
        gfx_text(&local, 10, local.h - 20, "Es gibt ungespeicherte Aenderungen.",
                 COL_ACCENT);
}

/* ------------------------------------------------------------------ */
/* Handeln                                                             */
/* ------------------------------------------------------------------ */

static void say(struct users_ui *ui, const char *text)
{
    strlcpy(ui->status, text, sizeof(ui->status));
    gui_invalidate();
}

static void do_save(struct users_ui *ui)
{
    if (!fs_disk_mounted()) {
        say(ui, "Ohne Festplatte gilt das nur bis zum Ausschalten.");
        ui->changed = false;
        return;
    }
    bool first = !user_store_exists();

    if (!user_save() || !perm_store_save()) {
        say(ui, "Die Datei liess sich nicht schreiben.");
        return;
    }

    ui->changed = false;

    /* Ein Verwalter ohne Passwort macht den Anmeldebildschirm zur
     * Attrappe - darauf muss hingewiesen werden, sonst merkt es
     * niemand, bis es zu spaet ist. */
    for (size_t i = 0; i < user_count(); i++) {
        struct user *u = user_at(i);

        if ((u->admin || u->uid == UID_ROOT) && u->nopass && !u->locked) {
            char text[96];

            ksnprintf(text, sizeof(text),
                      "Gespeichert - aber %s ist Verwalter ohne Passwort.",
                      u->name);
            say(ui, text);
            return;
        }
    }

    say(ui, first ? "Gespeichert. Ab dem naechsten Start wird angemeldet."
                  : "Gespeichert in " USER_PATH);
}

/* Schritt zwei beim Anlegen: das Passwort. */
static void new_password_entered(const char *text, void *user)
{
    UNUSED(user);

    struct users_ui *ui = ui_of();

    if (!ui || !ui->pending[0])
        return;

    char error[96] = "";
    struct user *u = user_create(ui->pending, ui->pending, text, false,
                                 error, sizeof(error));

    ui->pending[0] = '\0';

    if (!u) {
        say(ui, error[0] ? error : "Der Benutzer liess sich nicht anlegen.");
        return;
    }

    user_ensure_home(u);
    ui->selected = u->uid;
    ui->changed  = true;
    say(ui, "Angelegt. Zum Behalten speichern.");
}

static void new_name_entered(const char *text, void *user)
{
    UNUSED(user);

    struct users_ui *ui = ui_of();

    if (!ui || !text || !text[0])
        return;

    strlcpy(ui->pending, text, sizeof(ui->pending));
    dialog_password("Neuer Benutzer",
                    "Passwort fuer den neuen Benutzer (leer heisst: keines):",
                    new_password_entered, NULL);
}

static void password_entered(const char *text, void *user)
{
    UNUSED(user);

    struct users_ui *ui = ui_of();

    if (!ui)
        return;

    struct user *u = selected_user(ui);

    if (!u)
        return;

    user_set_password(u, text);
    ui->changed = true;
    say(ui, text && text[0] ? "Passwort gesetzt. Zum Behalten speichern."
                            : "Passwort geloescht - die Anmeldung geht jetzt "
                              "ohne.");
}

static void delete_confirmed(bool yes, void *user)
{
    UNUSED(user);

    struct users_ui *ui = ui_of();

    if (!ui || !yes)
        return;

    struct user *u = selected_user(ui);
    char error[96] = "";

    if (!u)
        return;
    if (!user_delete(u, error, sizeof(error))) {
        say(ui, error);
        return;
    }

    ui->selected = UID_ROOT;
    ui->changed  = true;
    say(ui, "Entfernt. Das Heimatverzeichnis bleibt stehen.");
}

static void press(struct users_ui *ui, int id)
{
    struct user *u = selected_user(ui);
    char error[96] = "";

    if (!button_enabled(ui, id)) {
        say(ui, session_is_admin() ? "Das geht bei diesem Benutzer nicht."
                                   : "Dafuer braucht es Verwalterrechte.");
        return;
    }

    switch (id) {
    case BTN_NEW:
        ui->pending[0] = '\0';
        dialog_input("Neuer Benutzer", "Anmeldename:", "", new_name_entered, NULL);
        break;
    case BTN_PASSWORD: {
        char prompt[96];

        ksnprintf(prompt, sizeof(prompt), "Neues Passwort fuer %s:", u->name);
        dialog_password("Passwort aendern", prompt, password_entered, NULL);
        break;
    }
    case BTN_ADMIN:
        u->admin = !u->admin;
        if (u->admin)
            group_add_member(group_by_gid(GID_ROOT), u->uid);
        else
            group_remove_member(group_by_gid(GID_ROOT), u->uid);
        ui->changed = true;
        say(ui, u->admin ? "Ist jetzt Verwalter." : "Ist kein Verwalter mehr.");
        break;
    case BTN_LOCK:
        u->locked = !u->locked;
        ui->changed = true;
        say(ui, u->locked ? "Die Anmeldung ist gesperrt."
                          : "Die Anmeldung ist wieder frei.");
        break;
    case BTN_DELETE:
        ksnprintf(error, sizeof(error), "%s wirklich entfernen?", u->name);
        dialog_confirm("Benutzer entfernen", error, delete_confirmed, NULL);
        break;
    case BTN_SAVE:
        do_save(ui);
        break;
    default:
        break;
    }
    gui_invalidate();
}

static void users_event(struct window *win, const struct gui_event *ev)
{
    struct users_ui *ui = win->user;
    struct rect l = list_rect(win);
    size_t count = user_count();
    size_t rows  = (size_t)(l.h / ROW_H);

    if (ev->type == EV_SCROLL) {
        int32_t max = count > rows ? (int32_t)(count - rows) : 0;

        ui->scroll = MIN(MAX(ui->scroll + ev->scroll, 0), max);
        gui_invalidate();
        return;
    }

    if (ev->type == EV_MOUSE_MOVE) {
        int before = ui->hover;

        ui->hover = -1;
        if (rect_contains(l, ev->x, ev->y)) {
            size_t row = (size_t)((ev->y - l.y - 1) / ROW_H) + (size_t)ui->scroll;

            if (row < count)
                ui->hover = (int)row;
        }
        for (int i = 0; i <= BTN_SAVE; i++) {
            struct rect r = i == BTN_SAVE ? save_rect(win) : button_rect(win, i);

            if (rect_contains(r, ev->x, ev->y))
                ui->hover = -100 - i;
        }
        if (before != ui->hover)
            gui_invalidate();
        return;
    }

    if (ev->type != EV_MOUSE_DOWN || ev->button != MB_LEFT)
        return;

    if (rect_contains(l, ev->x, ev->y)) {
        size_t row = (size_t)((ev->y - l.y - 1) / ROW_H) + (size_t)ui->scroll;

        if (row < count) {
            ui->selected = user_at(row)->uid;
            ui->status[0] = '\0';
            gui_invalidate();
        }
        return;
    }

    for (int i = 0; i <= BTN_SAVE; i++) {
        struct rect r = i == BTN_SAVE ? save_rect(win) : button_rect(win, i);

        if (rect_contains(r, ev->x, ev->y)) {
            press(ui, i);
            return;
        }
    }
}

static void users_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
    if (win == the_window)
        the_window = NULL;
}

void app_users(void)
{
    struct window *existing = gui_find_by_paint(users_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct users_ui *ui = kzalloc(sizeof(*ui));

    if (!ui)
        return;

    ui->hover = -1;

    struct user *me = session_user();

    ui->selected = me ? me->uid : UID_ROOT;

    if (!session_is_admin())
        strlcpy(ui->status, "Ohne Verwalterrechte laesst sich nur das eigene "
                            "Passwort aendern.", sizeof(ui->status));
    else if (!user_store_exists())
        strlcpy(ui->status, "Noch keine Datenbank - RetroOS meldet sich ohne "
                            "Nachfrage als root an.", sizeof(ui->status));
    else if (!fs_disk_mounted())
        strlcpy(ui->status, "Ohne Festplatte bleibt nichts gespeichert.",
                sizeof(ui->status));

    struct window *win = gui_create_window("Benutzer", 0, 0, 700, 380,
                                           WF_CENTER | WF_RESIZABLE, ICON_USERS);
    if (!win) {
        kfree(ui);
        return;
    }

    win->user     = ui;
    win->on_paint = users_paint;
    win->on_event = users_event;
    win->on_close = users_close;
    win->min_w    = 620;
    win->min_h    = 320;

    the_window = win;
    gui_focus_window(win);
}
