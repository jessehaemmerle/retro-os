/* setupapp.c - das Installationsprogramm mit Fenster.
 *
 * Vier Schritte, jeder mit einem eigenen Bild: Ziel waehlen, warnen,
 * arbeiten, fertig. Die Arbeit selbst steckt in setup.c; hier steht nur
 * die Bedienung.
 *
 * Das Schreiben von zwei Dateisystemen und dreieinhalb Megabyte Kernel
 * dauert eine halbe Minute. Waehrenddessen darf die Oberflaeche nicht
 * stehenbleiben, deshalb laeuft die Installation in einem eigenen
 * Thread und meldet ihren Fortschritt zurueck.
 */

#include "apps.h"
#include "block.h"
#include "font.h"
#include "kstring.h"
#include "mm.h"
#include "power.h"
#include "setup.h"
#include "theme.h"
#include "thread.h"
#include "widgets.h"

#define COL_DANGER  RGB(0xA0, 0x10, 0x10)
#define COL_DONE    RGB(0x10, 0x70, 0x20)

#define TARGET_MAX  12

enum setup_phase {
    PHASE_PICK,      /* Welcher Datentraeger?          */
    PHASE_CONFIRM,   /* Letzte Warnung                 */
    PHASE_WORK,      /* Laeuft                         */
    PHASE_DONE,
    PHASE_ERROR,
};

struct target_row {
    struct block_device *dev;
    struct setup_plan    plan;
    bool                 usable;
    char                 why[80];
};

struct setup_ui {
    enum setup_phase phase;

    struct target_row rows[TARGET_MAX];
    size_t            row_count;
    int               chosen;        /* Index in rows, -1 = keiner */

    /* Vom Arbeitsthread beschrieben, vom Zeichnen gelesen. */
    volatile int  percent;
    char          status[80];
    char          error[96];
    volatile bool finished;
    volatile bool failed;

    int  hover;                      /* Knopf unter dem Zeiger, -1 = keiner */
};

/* Die Knoepfe am unteren Rand; ihre Bedeutung haengt am Schritt. */
#define BTN_LEFT   0
#define BTN_RIGHT  1

/* ------------------------------------------------------------------ */
/* Ziele einsammeln                                                    */
/* ------------------------------------------------------------------ */

static void collect_targets(struct setup_ui *ui)
{
    ui->row_count = 0;
    ui->chosen = -1;

    for (size_t i = 0; i < block_device_count() && ui->row_count < TARGET_MAX;
         i++) {
        struct block_device *dev = block_device_at(i);

        /* Erst der Weg daneben - wer schon ein System auf der Platte
         * hat, will es meistens behalten. */
        struct setup_plan plan;
        char why[80];

        if (setup_plan_beside(dev, &plan, why, sizeof(why))) {
            struct target_row *row = &ui->rows[ui->row_count++];

            memset(row, 0, sizeof(*row));
            row->dev = dev;
            row->plan = plan;
            row->usable = true;
            if (ui->chosen < 0)
                ui->chosen = (int)(ui->row_count - 1);
        }

        if (ui->row_count >= TARGET_MAX)
            break;

        struct target_row *row = &ui->rows[ui->row_count++];

        memset(row, 0, sizeof(*row));
        row->dev = dev;
        row->usable = setup_plan_for(dev, &row->plan, row->why,
                                     sizeof(row->why));
        if (row->usable && ui->chosen < 0)
            ui->chosen = (int)(ui->row_count - 1);
    }
}

/* ------------------------------------------------------------------ */
/* Der Arbeitsthread                                                   */
/* ------------------------------------------------------------------ */

static void on_progress(void *user, int percent, const char *text)
{
    struct setup_ui *ui = user;

    strlcpy(ui->status, text, sizeof(ui->status));
    ui->percent = percent;
    gui_invalidate();
}

static void setup_worker(void *argument)
{
    struct setup_ui *ui = argument;

    if (setup_run(&ui->rows[ui->chosen].plan, on_progress, ui,
                  ui->error, sizeof(ui->error))) {
        ui->percent = 100;
    } else {
        ui->failed = true;
    }

    ui->finished = true;
    gui_invalidate();
    thread_exit();
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static struct rect button_rect(const struct window *win, int which)
{
    int32_t h = gui_client_height(win);
    int32_t w = gui_client_width(win);

    return which == BTN_LEFT ? rect_make(w - 260, h - 44, 120, 28)
                             : rect_make(w - 130, h - 44, 120, 28);
}

static struct rect row_rect(int index)
{
    return rect_make(20, 96 + index * 42, 0, 38);
}

static void size_text(char *out, size_t size, uint64_t sectors)
{
    uint64_t mib = sectors / 2048;

    if (mib >= 1024)
        ksnprintf(out, size, "%u,%u GiB", (unsigned)(mib / 1024),
                  (unsigned)((mib % 1024) * 10 / 1024));
    else
        ksnprintf(out, size, "%u MiB", (unsigned)mib);
}

static void draw_header(struct canvas *c, int32_t w, const char *title,
                        const char *subtitle)
{
    gfx_gradient_v(c, rect_make(0, 0, w, 64), COL_TITLE_A1, COL_TITLE_A2);
    icon_draw(c, 16, 16, ICON_DISK, 2);
    gfx_text_bold(c, 64, 18, title, COL_WHITE);
    gfx_text(c, 64, 38, subtitle, RGB(0xC0, 0xD8, 0xF0));
}

static void paint_pick(struct setup_ui *ui, struct canvas *c, int32_t w)
{
    draw_header(c, w, "RetroOS installieren",
                "Auf welchen Datentraeger soll das System?");

    if (ui->row_count == 0) {
        gfx_text(c, 20, 100, "Es wurde kein Datentraeger gefunden.",
                 COL_DANGER);
        return;
    }

    for (size_t i = 0; i < ui->row_count; i++) {
        struct target_row *row = &ui->rows[i];
        struct rect r = row_rect((int)i);

        r.w = w - 40;

        bool active = ui->chosen == (int)i;

        gfx_fill(c, r, active ? COL_SELECT : COL_FIELD);
        gfx_bevel_thin(c, r, false);

        uint32_t text = active ? COL_SELECT_TEXT : COL_TEXT;
        uint32_t dim = active ? RGB(0xC0, 0xD0, 0xE8) : COL_TEXT_DIM;

        icon_draw(c, r.x + 8, r.y + 11, ICON_DISK, 1);

        char line[96], size[24];

        size_text(size, sizeof(size), row->dev->sector_count);
        ksnprintf(line, sizeof(line), "%s - %s (%s)%s", row->dev->name,
                  row->dev->model, size,
                  row->plan.mode == SETUP_BESIDE ? "  -  daneben" : "");
        gfx_text(c, r.x + 32, r.y + 6, line, row->usable ? text : dim);

        if (row->usable) {
            char plan[96], esp[24], data[24];

            size_text(data, sizeof(data), row->plan.data_count);
            if (row->plan.mode == SETUP_BESIDE) {
                ksnprintf(plan, sizeof(plan),
                          "%s freier Platz - alles Vorhandene bleibt", data);
            } else {
                size_text(esp, sizeof(esp), row->plan.esp_count);
                ksnprintf(plan, sizeof(plan),
                          "alles loeschen: %s Startbereich, %s Ablage",
                          esp, data);
            }
            gfx_text(c, r.x + 32, r.y + 20, plan, dim);
        } else {
            gfx_text(c, r.x + 32, r.y + 20, row->why,
                     active ? RGB(0xFF, 0xC0, 0xC0) : COL_DANGER);
        }
    }
}

static void paint_confirm(struct setup_ui *ui, struct canvas *c, int32_t w)
{
    struct target_row *row = &ui->rows[ui->chosen];

    bool beside = row->plan.mode == SETUP_BESIDE;

    draw_header(c, w, beside ? "Daneben einrichten" : "Wirklich?",
                beside ? "Alles Vorhandene bleibt stehen."
                       : "Danach ist der Datentraeger leer.");

    char line[112], size[24];

    size_text(size, sizeof(size), row->dev->sector_count);
    ksnprintf(line, sizeof(line), "Ziel: %s - %s (%s)", row->dev->name,
              row->dev->model, size);

    int32_t y = 96;
    char esp[24], data[24];

    size_text(data, sizeof(data), row->plan.data_count);

    gfx_text_bold(c, 20, y, line, COL_TEXT);          y += 28;

    if (beside) {
        gfx_text_bold(c, 20, y,
                      "Nichts wird geloescht - RetroOS nimmt nur den freien "
                      "Platz.", COL_DONE);
        y += 28;

        ksnprintf(line, sizeof(line), "  Ablage        %s - unter /Festplatte",
                  data);
        gfx_text(c, 20, y, line, COL_TEXT_DIM);       y += 18;
        gfx_text(c, 20, y,
                 "  Startbereich  der vorhandene wird mitbenutzt",
                 COL_TEXT_DIM);
        y += 28;
        gfx_text(c, 20, y,
                 "Ist der uebliche Startpfad belegt, muss RetroOS im",
                 COL_TEXT_DIM);
        y += 16;
        gfx_text(c, 20, y, "Startmenue der Firmware gewaehlt werden.",
                 COL_TEXT_DIM);
        return;
    }

    gfx_text_bold(c, 20, y, "Alles, was darauf steht, geht verloren.",
                  COL_DANGER);                        y += 28;

    gfx_text(c, 20, y, "RetroOS legt zwei Abschnitte an:", COL_TEXT);
    y += 20;

    size_text(esp, sizeof(esp), row->plan.esp_count);

    ksnprintf(line, sizeof(line), "  Startbereich  %s - Bootloader und Kernel",
              esp);
    gfx_text(c, 20, y, line, COL_TEXT_DIM);           y += 18;
    ksnprintf(line, sizeof(line), "  Ablage        %s - unter /Festplatte",
              data);
    gfx_text(c, 20, y, line, COL_TEXT_DIM);           y += 28;

    gfx_text(c, 20, y,
             "Der Rechner startet danach von dieser Platte - ueber UEFI",
             COL_TEXT_DIM);
    y += 16;
    gfx_text(c, 20, y, "wie ueber BIOS.", COL_TEXT_DIM);
}

static void paint_work(struct setup_ui *ui, struct canvas *c, int32_t w)
{
    draw_header(c, w, "Installation laeuft", "Bitte nicht ausschalten.");

    struct rect bar = rect_make(20, 120, w - 40, 22);

    gfx_fill(c, bar, COL_FIELD);
    gfx_bevel_thin(c, bar, false);

    int percent = ui->percent;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    gfx_fill(c, rect_make(bar.x + 2, bar.y + 2,
                          (bar.w - 4) * percent / 100, bar.h - 4),
             COL_SELECT);

    char text[16];

    ksnprintf(text, sizeof(text), "%d %%", percent);
    gfx_text(c, 20, 96, text, COL_TEXT);
    gfx_text(c, 20, 156, ui->status, COL_TEXT_DIM);
}

static void paint_done(struct setup_ui *ui, struct canvas *c, int32_t w)
{
    draw_header(c, w, "Fertig", "RetroOS liegt jetzt auf der Platte.");

    int32_t y = 100;

    gfx_text_bold(c, 20, y, "Die Installation ist abgeschlossen.", COL_DONE);
    y += 30;
    gfx_text(c, 20, y, "Startmedium entfernen und neu starten - der",
             COL_TEXT);
    y += 18;
    gfx_text(c, 20, y, "Rechner bootet dann von der Festplatte.", COL_TEXT);
    y += 30;
    gfx_text(c, 20, y, "Dateien unter /Festplatte bleiben ab jetzt liegen.",
             COL_TEXT_DIM);
    y += 18;
    gfx_text(c, 20, y, "Konten legst du unter \"Benutzer\" an - ab dann",
             COL_TEXT_DIM);
    y += 16;
    gfx_text(c, 20, y, "fragt RetroOS beim Start nach Name und Passwort.",
             COL_TEXT_DIM);

    if (ui->chosen >= 0 &&
        ui->rows[ui->chosen].plan.mode == SETUP_BESIDE &&
        !ui->rows[ui->chosen].plan.fallback_free) {
        y += 26;
        gfx_text(c, 20, y,
                 "Der uebliche Startpfad war belegt: RetroOS steht unter",
                 COL_DANGER);
        y += 16;
        gfx_text(c, 20, y,
                 "EFI\\RETROOS und muss im Startmenue gewaehlt werden.",
                 COL_DANGER);
    }
}

static void paint_error(struct setup_ui *ui, struct canvas *c, int32_t w)
{
    draw_header(c, w, "Fehlgeschlagen", "Die Platte wurde nicht fertig.");

    gfx_text_bold(c, 20, 100, "Die Installation ist gescheitert:",
                  COL_DANGER);
    gfx_text(c, 20, 128, ui->error, COL_TEXT);
    gfx_text(c, 20, 164,
             "Der Datentraeger ist jetzt in einem halben Zustand.",
             COL_TEXT_DIM);
    gfx_text(c, 20, 182, "Ein zweiter Versuch legt ihn neu an.",
             COL_TEXT_DIM);
}

/* Beschriftung der beiden Knoepfe; NULL heisst "nicht anzeigen". */
static void button_labels(const struct setup_ui *ui, const char **left,
                          const char **right)
{
    switch (ui->phase) {
    case PHASE_PICK:
        *left = NULL;
        *right = "Weiter";
        break;
    case PHASE_CONFIRM:
        *left = "Zurueck";
        *right = "Installieren";
        break;
    case PHASE_WORK:
        *left = NULL;
        *right = NULL;
        break;
    case PHASE_DONE:
        *left = NULL;
        *right = "Neu starten";
        break;
    case PHASE_ERROR:
        *left = NULL;
        *right = "Von vorn";
        break;
    }
}

static bool right_enabled(const struct setup_ui *ui)
{
    if (ui->phase != PHASE_PICK)
        return true;
    return ui->chosen >= 0 && ui->rows[ui->chosen].usable;
}

static void setup_paint(struct window *win, struct canvas *c)
{
    struct setup_ui *ui = win->user;
    struct canvas local = gui_client_canvas(win, c);
    int32_t w = local.w;

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    switch (ui->phase) {
    case PHASE_PICK:    paint_pick(ui, &local, w);    break;
    case PHASE_CONFIRM: paint_confirm(ui, &local, w); break;
    case PHASE_WORK:    paint_work(ui, &local, w);    break;
    case PHASE_DONE:    paint_done(ui, &local, w);    break;
    case PHASE_ERROR:   paint_error(ui, &local, w);   break;
    }

    const char *left = NULL, *right = NULL;

    button_labels(ui, &left, &right);

    if (left)
        widget_button(&local, button_rect(win, BTN_LEFT), left,
                      ui->hover == BTN_LEFT, true);
    if (right)
        widget_button(&local, button_rect(win, BTN_RIGHT), right,
                      ui->hover == BTN_RIGHT, right_enabled(ui));
}

/* ------------------------------------------------------------------ */
/* Bedienung                                                           */
/* ------------------------------------------------------------------ */

static void start_install(struct setup_ui *ui)
{
    ui->phase = PHASE_WORK;
    ui->percent = 0;
    ui->finished = false;
    ui->failed = false;
    strlcpy(ui->status, "Wird vorbereitet ...", sizeof(ui->status));

    if (!thread_create("installation", setup_worker, ui, PRIO_NORMAL)) {
        strlcpy(ui->error, "Der Arbeitsthread liess sich nicht starten.",
                sizeof(ui->error));
        ui->phase = PHASE_ERROR;
    }
    gui_invalidate();
}

static void press_right(struct window *win, struct setup_ui *ui)
{
    switch (ui->phase) {
    case PHASE_PICK:
        if (right_enabled(ui))
            ui->phase = PHASE_CONFIRM;
        break;
    case PHASE_CONFIRM:
        start_install(ui);
        return;
    case PHASE_DONE:
        power_reboot();
        return;
    case PHASE_ERROR:
        collect_targets(ui);
        ui->phase = PHASE_PICK;
        break;
    case PHASE_WORK:
        return;
    }
    gui_invalidate();
}

static void setup_event(struct window *win, const struct gui_event *ev)
{
    struct setup_ui *ui = win->user;

    /* Der Arbeitsthread meldet sich nur ueber Merker; hier wird daraus
     * der naechste Schritt. */
    if (ev->type == EV_TICK && ui->phase == PHASE_WORK && ui->finished) {
        ui->phase = ui->failed ? PHASE_ERROR : PHASE_DONE;
        gui_invalidate();
        return;
    }

    const char *left = NULL, *right = NULL;

    button_labels(ui, &left, &right);

    if (ev->type == EV_MOUSE_MOVE) {
        int before = ui->hover;

        ui->hover = -1;
        if (left && rect_contains(button_rect(win, BTN_LEFT), ev->x, ev->y))
            ui->hover = BTN_LEFT;
        else if (right &&
                 rect_contains(button_rect(win, BTN_RIGHT), ev->x, ev->y))
            ui->hover = BTN_RIGHT;
        if (before != ui->hover)
            gui_invalidate();
        return;
    }

    if (ev->type != EV_MOUSE_DOWN || ev->button != MB_LEFT)
        return;

    if (left && rect_contains(button_rect(win, BTN_LEFT), ev->x, ev->y)) {
        if (ui->phase == PHASE_CONFIRM) {
            ui->phase = PHASE_PICK;
            gui_invalidate();
        }
        return;
    }

    if (right && rect_contains(button_rect(win, BTN_RIGHT), ev->x, ev->y)) {
        press_right(win, ui);
        return;
    }

    /* Ein Klick auf eine Zeile waehlt das Ziel. */
    if (ui->phase == PHASE_PICK) {
        for (size_t i = 0; i < ui->row_count; i++) {
            struct rect r = row_rect((int)i);

            r.w = gui_client_width(win) - 40;
            if (rect_contains(r, ev->x, ev->y) && ui->rows[i].usable) {
                ui->chosen = (int)i;
                gui_invalidate();
                return;
            }
        }
    }
}

static void setup_close(struct window *win)
{
    struct setup_ui *ui = win->user;

    /* Waehrend die Installation laeuft, darf der Zustand nicht weg -
     * der Arbeitsthread schreibt noch hinein. */
    if (ui && ui->phase == PHASE_WORK && !ui->finished)
        return;

    kfree(ui);
    win->user = NULL;
}

void app_setup(void)
{
    struct window *existing = gui_find_by_paint(setup_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    if (!setup_sources_ready()) {
        dialog_message("RetroOS installieren",
                       "Von diesem Startmedium laesst sich nicht "
                       "installieren.\nNoetig ist das RetroOS-Abbild "
                       "als ISO oder auf einem USB-Stick.");
        return;
    }

    struct setup_ui *ui = kzalloc(sizeof(*ui));

    if (!ui)
        return;

    ui->phase = PHASE_PICK;
    ui->hover = -1;
    collect_targets(ui);

    struct window *win = gui_create_window("RetroOS installieren", 0, 0,
                                           560, 400, WF_CENTER, ICON_DISK);
    if (!win) {
        kfree(ui);
        return;
    }

    win->user     = ui;
    win->on_paint = setup_paint;
    win->on_event = setup_event;
    win->on_close = setup_close;

    gui_focus_window(win);
}
