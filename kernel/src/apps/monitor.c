/* monitor.c - der Systemmonitor.
 *
 * Drei Ansichten hinter drei Reitern: die Programme in Ring 3, die
 * Threads des Kerns und eine Uebersicht ueber die Maschine. Getrennt,
 * weil es zwei verschiedene Fragen sind - "was laeuft da und frisst
 * meine Zeit" und "wie steht es um den Rechner".
 *
 * Die Auslastung eines Threads laesst sich nicht ablesen, nur messen:
 * Der Scheduler zaehlt, wie oft jeder drankam. Der Monitor merkt sich
 * diese Zaehler und rechnet beim naechsten Mal den Zuwachs aus. Vor der
 * ersten Messung steht darum ueberall ein Strich und keine Null - eine
 * Null waere gelogen.
 */

#include "apps.h"

#include "arch.h"
#include "audit.h"
#include "cpu.h"
#include "font.h"
#include "kstring.h"
#include "log.h"
#include "mm.h"
#include "process.h"
#include "theme.h"
#include "thread.h"
#include "user.h"
#include "vfs.h"
#include "widgets.h"
#include "lang.h"

#define TAB_H      28
#define HEAD_H     20
#define ROW_H      20
#define STATUS_H   22
#define SAMPLE_MAX THREAD_MAX
#define SAMPLE_MS  1000

enum tab_id {
    TAB_PROGRAMS,
    TAB_THREADS,
    TAB_SYSTEM,
    TAB_COUNT
};

/* Ein Messpunkt je Thread: seine Nummer und der Zaehlerstand beim
 * letzten Blick. */
struct sample {
    uint32_t id;
    uint64_t ticks;
};

struct monitor_ui {
    int      tab;
    int      selected;        /* Zeile in der aktuellen Ansicht */
    int      hover;
    int32_t  scroll;
    char     status[96];

    struct sample last[SAMPLE_MAX];
    size_t        last_count;
    uint64_t      last_total;     /* Summe aller Zaehler beim letzten Mal */
    uint64_t      last_ms;
    bool          measured;

    /* Anteil je Thread in Zehntelprozent, zur Nummer des Threads. */
    struct sample share[SAMPLE_MAX];
    size_t        share_count;
};

/* ------------------------------------------------------------------ */
/* Messen                                                              */
/* ------------------------------------------------------------------ */

static uint64_t previous_ticks(struct monitor_ui *ui, uint32_t id, bool *known)
{
    for (size_t i = 0; i < ui->last_count; i++) {
        if (ui->last[i].id != id)
            continue;
        *known = true;
        return ui->last[i].ticks;
    }
    *known = false;
    return 0;
}

/* Der Anteil eines Threads an der Rechenzeit seit der letzten Messung,
 * in Zehntelprozent. -1 heisst "noch nicht gemessen". */
static int32_t share_of(struct monitor_ui *ui, uint32_t id)
{
    if (!ui->measured)
        return -1;

    for (size_t i = 0; i < ui->share_count; i++)
        if (ui->share[i].id == id)
            return (int32_t)ui->share[i].ticks;
    return -1;
}

static void take_sample(struct monitor_ui *ui)
{
    uint64_t now = timer_ms();

    if (ui->last_ms && now - ui->last_ms < SAMPLE_MS)
        return;

    struct sample fresh[SAMPLE_MAX];
    size_t        count = 0;
    uint64_t      total = 0;
    size_t        threads = thread_count();

    for (size_t i = 0; i < threads && count < SAMPLE_MAX; i++) {
        struct thread *t = thread_at(i);

        if (!t)
            continue;
        fresh[count].id    = t->id;
        fresh[count].ticks = t->cpu_ticks;
        total += t->cpu_ticks;
        count++;
    }

    /* Der erste Durchgang legt nur die Ausgangswerte fest. */
    if (ui->last_ms) {
        uint64_t spent = total > ui->last_total ? total - ui->last_total : 0;

        ui->share_count = 0;
        for (size_t i = 0; i < count; i++) {
            bool known = false;
            uint64_t before = previous_ticks(ui, fresh[i].id, &known);
            uint64_t grew = known && fresh[i].ticks > before
                          ? fresh[i].ticks - before : 0;

            ui->share[ui->share_count].id = fresh[i].id;
            /* In Zehntelprozent, damit ein Thread, der ein halbes
             * Prozent braucht, nicht als Null dasteht. */
            ui->share[ui->share_count].ticks =
                spent ? (grew * 1000) / spent : 0;
            ui->share_count++;
        }
        ui->measured = true;
    }

    memcpy(ui->last, fresh, count * sizeof(fresh[0]));
    ui->last_count = count;
    ui->last_total = total;
    ui->last_ms    = now;
}

/* ------------------------------------------------------------------ */
/* Masse                                                               */
/* ------------------------------------------------------------------ */

static struct rect tab_rect(int index)
{
    return rect_make(6 + index * 118, 4, 114, TAB_H - 4);
}

static struct rect list_rect(struct window *win)
{
    return rect_make(6, TAB_H + HEAD_H, gui_client_width(win) - 12,
                     gui_client_height(win) - TAB_H - HEAD_H - STATUS_H - 42);
}

static struct rect kill_rect(struct window *win)
{
    return rect_make(gui_client_width(win) - 156,
                     gui_client_height(win) - STATUS_H - 34, 150, 28);
}

static int32_t rows_fitting(struct window *win)
{
    return MAX(list_rect(win).h / ROW_H, 1);
}

static size_t row_count(struct monitor_ui *ui)
{
    switch (ui->tab) {
    case TAB_PROGRAMS: return process_count();
    case TAB_THREADS:  return thread_count();
    default:           return 0;
    }
}

static const char *state_name(uint8_t state)
{
    switch (state) {
    case THREAD_READY:    return tr("bereit");
    case THREAD_RUNNING:  return tr("laeuft");
    case THREAD_SLEEPING: return tr("schlaeft");
    case THREAD_BLOCKED:  return tr("wartet");
    default:              return tr("tot");
    }
}

static const char *priority_name(uint8_t priority)
{
    switch (priority) {
    case PRIO_HIGH: return tr("hoch");
    case PRIO_LOW:  return tr("niedrig");
    default:        return tr("normal");
    }
}

static void share_text(int32_t tenths, char *out, size_t size)
{
    if (tenths < 0)
        strlcpy(out, "-", size);
    else
        ksnprintf(out, size, "%u,%u %%", (unsigned)(tenths / 10),
                  (unsigned)(tenths % 10));
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static void draw_bar(struct canvas *c, struct rect r, uint64_t used,
                     uint64_t total, uint32_t color)
{
    gfx_fill(c, r, COL_FIELD);
    gfx_bevel_thin(c, r, false);

    if (total) {
        int32_t w = (int32_t)((used * (uint64_t)(r.w - 4)) / total);

        gfx_fill(c, rect_make(r.x + 2, r.y + 2, w, r.h - 4), color);
    }
}

static void columns(struct canvas *c, struct window *win, int32_t y,
                    const char *const *names, const int32_t *x, size_t count)
{
    for (size_t i = 0; i < count; i++)
        gfx_text(c, x[i], y, names[i], COL_TEXT_DIM);
    gfx_hline(c, 6, y + FONT_HEIGHT + 2, gui_client_width(win) - 12,
              COL_SHADOW);
}

static void paint_programs(struct canvas *c, struct window *win,
                           struct monitor_ui *ui)
{
    const char *const head[] = { tr("Nummer"), tr("Programm"), tr("Benutzer"),
                                        tr("Kaefig"), tr("Zustand"), tr("Speicher"),
                                        tr("Zeit") };
    const int32_t x[] = { 12, 68, 220, 320, 400, 500, 606 };

    columns(c, win, TAB_H + 2, head, x, ARRAY_LEN(head));

    struct rect l = list_rect(win);

    gfx_fill(c, l, COL_FIELD);
    gfx_bevel_thin(c, l, false);
    gfx_set_clip(c, l);

    int32_t rows = rows_fitting(win);

    for (int32_t i = 0; i < rows; i++) {
        size_t index = (size_t)(ui->scroll + i);
        struct process *p = process_at(index);

        if (!p)
            break;

        struct rect r = rect_make(l.x + 1, l.y + 1 + i * ROW_H, l.w - 2, ROW_H);
        bool sel = (int)index == ui->selected;

        if (sel)
            gfx_fill(c, r, COL_SELECT);

        uint32_t fg = sel ? COL_SELECT_TEXT : COL_TEXT;
        char text[32];

        ksnprintf(text, sizeof(text), "%u", (unsigned)p->pid);
        gfx_text(c, x[0], r.y + 2, text, fg);
        gfx_text_clipped(c, x[1], r.y + 2, p->name, fg, x[2] - x[1] - 8);
        gfx_text_clipped(c, x[2], r.y + 2, user_name_of(p->uid), fg,
                         x[3] - x[2] - 8);

        /* Ein eingesperrtes Programm faellt auf, auch ohne dass man die
         * Zeile lesen muss - deshalb das Schloss davor. */
        if (p->box.active) {
            icon_draw(c, x[3], r.y + 2, ICON_LOCK, 1);
            gfx_text_clipped(c, x[3] + 18, r.y + 2, p->box.profile, fg,
                             x[4] - x[3] - 24);
        } else {
            gfx_text(c, x[3], r.y + 2, "-", COL_TEXT_DIM);
        }

        gfx_text(c, x[4], r.y + 2,
                 p->finished ? tr("beendet")
                             : (p->thread ? state_name(p->thread->state)
                                          : "?"), fg);

        fs_format_size(text, sizeof(text),
                       (size_t)p->space.mapped_pages * PAGE_SIZE);
        gfx_text(c, x[5], r.y + 2, text, fg);

        share_text(p->thread ? share_of(ui, p->thread->id) : -1, text,
                   sizeof(text));
        gfx_text(c, x[6], r.y + 2, text, fg);
    }
    gfx_reset_clip(c);
}

static void paint_threads(struct canvas *c, struct window *win,
                          struct monitor_ui *ui)
{
    const char *const head[] = { "Nr.", tr("Name"), tr("Zustand"), tr("Vorrang"),
                                        tr("Kern"), tr("Rechenzeit"), tr("Anteil") };
    const int32_t x[] = { 12, 58, 250, 350, 440, 500, 600 };

    columns(c, win, TAB_H + 2, head, x, ARRAY_LEN(head));

    struct rect l = list_rect(win);

    gfx_fill(c, l, COL_FIELD);
    gfx_bevel_thin(c, l, false);
    gfx_set_clip(c, l);

    int32_t rows = rows_fitting(win);

    for (int32_t i = 0; i < rows; i++) {
        size_t index = (size_t)(ui->scroll + i);
        struct thread *t = thread_at(index);

        if (!t)
            break;

        struct rect r = rect_make(l.x + 1, l.y + 1 + i * ROW_H, l.w - 2, ROW_H);
        bool sel = (int)index == ui->selected;

        if (sel)
            gfx_fill(c, r, COL_SELECT);

        uint32_t fg = sel ? COL_SELECT_TEXT : COL_TEXT;
        char text[32];

        ksnprintf(text, sizeof(text), "%u", (unsigned)t->id);
        gfx_text(c, x[0], r.y + 2, text, fg);
        gfx_text_clipped(c, x[1], r.y + 2, t->name, fg, x[2] - x[1] - 8);
        gfx_text(c, x[2], r.y + 2, state_name(t->state), fg);
        gfx_text(c, x[3], r.y + 2, priority_name(t->priority), fg);

        ksnprintf(text, sizeof(text), "%u", (unsigned)t->last_cpu);
        gfx_text(c, x[4], r.y + 2, text, fg);

        ksnprintf(text, sizeof(text), "%u", (unsigned)t->cpu_ticks);
        gfx_text(c, x[5], r.y + 2, text, fg);

        share_text(share_of(ui, t->id), text, sizeof(text));
        gfx_text(c, x[6], r.y + 2, text, fg);
    }
    gfx_reset_clip(c);
}

static void paint_system(struct canvas *c, struct window *win,
                         struct monitor_ui *ui)
{
    int32_t y = TAB_H + 14;
    int32_t w = gui_client_width(win) - 32;
    char    text[96];
    char    a[24], b[24];

    UNUSED(ui);

    uint64_t ms = timer_ms();

    gfx_text_bold(c, 16, y, tr("Maschine"), COL_TEXT);
    y += 22;

    unsigned cores = (unsigned)cpu_count();
    unsigned progs = (unsigned)process_count();

    ksnprintf(text, sizeof(text), "%u %s in Betrieb, %u Threads, "
              "%u %s in Ring 3",
              cores, cores == 1 ? tr("Kern") : tr("Kerne"),
              (unsigned)thread_count(),
              progs, progs == 1 ? tr("Programm") : tr("Programme"));
    gfx_text(c, 16, y, text, COL_TEXT);
    y += 18;

    ksnprintf(text, sizeof(text), tr("Laufzeit %u:%02u:%02u"),
              (unsigned)(ms / 3600000), (unsigned)((ms / 60000) % 60),
              (unsigned)((ms / 1000) % 60));
    gfx_text(c, 16, y, text, COL_TEXT);
    y += 30;

    gfx_text_bold(c, 16, y, tr("Arbeitsspeicher"), COL_TEXT);
    y += 22;

    fs_format_size(a, sizeof(a), (size_t)pmm_used_bytes());
    fs_format_size(b, sizeof(b), (size_t)pmm_total_bytes());
    ksnprintf(text, sizeof(text), tr("%s von %s belegt"), a, b);
    gfx_text(c, 16, y, text, COL_TEXT);
    y += 18;
    draw_bar(c, rect_make(16, y, w, 14), pmm_used_bytes(), pmm_total_bytes(),
             RGB(0x2A, 0x8C, 0xD0));
    y += 26;

    fs_format_size(a, sizeof(a), (size_t)pmm_shared_bytes());
    ksnprintf(text, sizeof(text),
              tr("davon %s mehrfach genutzt (Kopie beim Schreiben)"), a);
    gfx_text(c, 16, y, text, COL_TEXT_DIM);
    y += 26;

    fs_format_size(a, sizeof(a), (size_t)heap_used_bytes());
    fs_format_size(b, sizeof(b), (size_t)heap_total_bytes());
    ksnprintf(text, sizeof(text), tr("Kernel-Heap: %s von %s"), a, b);
    gfx_text(c, 16, y, text, COL_TEXT);
    y += 18;
    draw_bar(c, rect_make(16, y, w, 14), heap_used_bytes(), heap_total_bytes(),
             RGB(0x2E, 0xA0, 0x60));
    y += 30;

    gfx_text_bold(c, 16, y, tr("Dateien und Protokoll"), COL_TEXT);
    y += 22;

    fs_format_size(a, sizeof(a), fs_bytes_used());
    ksnprintf(text, sizeof(text), tr("%u Eintraege im Dateibaum, %s belegt"),
              (unsigned)fs_node_count(), a);
    gfx_text(c, 16, y, text, COL_TEXT);
    y += 18;

    ksnprintf(text, sizeof(text),
              tr("%u Meldungen im Protokoll, davon %u Warnungen und %u Fehler"),
              (unsigned)log_count(), (unsigned)log_count_level(LOG_WARN),
              (unsigned)log_count_level(LOG_ERROR));
    gfx_text(c, 16, y, text, COL_TEXT);
    y += 18;

    ksnprintf(text, sizeof(text),
              tr("%u Eintraege in der Pruefspur, davon %u abgewiesen"),
              (unsigned)audit_count(), (unsigned)audit_count_failed());
    gfx_text(c, 16, y, text, COL_TEXT);
}

static void monitor_paint(struct window *win, struct canvas *c)
{
    struct monitor_ui *ui = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    const char *const tabs[TAB_COUNT] = { tr("Programme"), tr("Threads"),
                                                 tr("System") };

    for (int i = 0; i < TAB_COUNT; i++)
        widget_button(&local, tab_rect(i), tabs[i], ui->tab == i, true);

    switch (ui->tab) {
    case TAB_PROGRAMS: paint_programs(&local, win, ui); break;
    case TAB_THREADS:  paint_threads(&local, win, ui);  break;
    default:           paint_system(&local, win, ui);   break;
    }

    if (ui->tab != TAB_SYSTEM) {
        size_t total = row_count(ui);
        int32_t rows = rows_fitting(win);
        struct rect l = list_rect(win);

        if ((int32_t)total > rows)
            widget_vscroll(&local,
                           rect_make(l.x + l.w - SCROLLBAR_WIDTH, l.y,
                                     SCROLLBAR_WIDTH, l.h),
                           ui->scroll, (int32_t)total, rows);
    }

    if (ui->tab == TAB_PROGRAMS) {
        struct process *p = process_at((size_t)ui->selected);

        widget_button(&local, kill_rect(win), tr("Programm beenden"),
                      ui->hover == 100, p && !p->finished);
    }

    char right[48];

    ksnprintf(right, sizeof(right), "%u %s", (unsigned)cpu_count(),
              cpu_count() == 1 ? tr("Kern") : tr("Kerne"));
    widget_statusbar(&local, rect_make(0, local.h - STATUS_H, local.w, STATUS_H),
                     ui->status[0] ? ui->status
                                   : "Die Anteile werden jede Sekunde neu "
                                     "gemessen.",
                     right);
}

/* ------------------------------------------------------------------ */
/* Handeln                                                             */
/* ------------------------------------------------------------------ */

static void clamp(struct window *win, struct monitor_ui *ui)
{
    int32_t max = (int32_t)row_count(ui) - rows_fitting(win);

    if (max < 0)
        max = 0;
    ui->scroll = CLAMP(ui->scroll, 0, max);
}

static void kill_selected(struct window *win, struct monitor_ui *ui)
{
    struct process *p = process_at((size_t)ui->selected);

    if (!p || p->finished) {
        strlcpy(ui->status, tr("Da laeuft nichts mehr."), sizeof(ui->status));
        return;
    }

    /* Ein fremdes Programm abzuschiessen ist Sache des Verwalters -
     * sonst koennte jeder jedem in die Arbeit greifen. */
    if (p->uid != session_uid() && !session_is_admin()) {
        ksnprintf(ui->status, sizeof(ui->status),
                  tr("%s gehoert %s - das darf nur ein Verwalter beenden."),
                  p->name, user_name_of(p->uid));
        return;
    }

    log_warn("monitor", "%s (%u) von %s beendet", p->name,
             (unsigned)p->pid, user_name_of(session_uid()));

    char name[32];

    strlcpy(name, p->name, sizeof(name));
    process_kill(p);
    ksnprintf(ui->status, sizeof(ui->status), tr("%s beendet."), name);
    UNUSED(win);
}

static void monitor_event(struct window *win, const struct gui_event *ev)
{
    struct monitor_ui *ui = win->user;

    if (ev->type == EV_TICK) {
        take_sample(ui);
        gui_invalidate();
        return;
    }

    if (ev->type == EV_SCROLL) {
        ui->scroll -= ev->scroll * 3;
        clamp(win, ui);
        gui_invalidate();
        return;
    }

    if (ev->type == EV_KEY_DOWN) {
        switch (ev->key) {
        case KEY_TAB:
            ui->tab = (ui->tab + 1) % TAB_COUNT;
            ui->scroll = 0;
            ui->selected = 0;
            break;
        case KEY_UP:   ui->selected--; break;
        case KEY_DOWN: ui->selected++; break;
        default: return;
        }

        int32_t last = (int32_t)row_count(ui) - 1;

        ui->selected = CLAMP(ui->selected, 0, MAX(last, 0));

        /* Die Auswahl mitziehen, damit sie sichtbar bleibt. */
        int32_t rows = rows_fitting(win);

        if (ui->selected < ui->scroll)
            ui->scroll = ui->selected;
        else if (ui->selected >= ui->scroll + rows)
            ui->scroll = ui->selected - rows + 1;

        clamp(win, ui);
        gui_invalidate();
        return;
    }

    if (ev->type == EV_MOUSE_MOVE) {
        int before = ui->hover;

        ui->hover = rect_contains(kill_rect(win), ev->x, ev->y) ? 100 : -1;
        if (before != ui->hover)
            gui_invalidate();
        return;
    }

    if (ev->type != EV_MOUSE_DOWN || ev->button != MB_LEFT)
        return;

    for (int i = 0; i < TAB_COUNT; i++) {
        if (!rect_contains(tab_rect(i), ev->x, ev->y))
            continue;
        ui->tab = i;
        ui->scroll = 0;
        ui->selected = 0;
        ui->status[0] = '\0';
        gui_invalidate();
        return;
    }

    if (ui->tab == TAB_PROGRAMS && rect_contains(kill_rect(win), ev->x, ev->y)) {
        kill_selected(win, ui);
        gui_invalidate();
        return;
    }

    struct rect l = list_rect(win);

    if (ui->tab != TAB_SYSTEM && rect_contains(l, ev->x, ev->y)) {
        int32_t row = (ev->y - l.y - 1) / ROW_H + ui->scroll;

        if (row >= 0 && row < (int32_t)row_count(ui)) {
            ui->selected = row;
            ui->status[0] = '\0';
            gui_invalidate();
        }
    }
}

static void monitor_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_monitor(void)
{
    struct window *existing = gui_find_by_paint(monitor_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct monitor_ui *ui = kzalloc(sizeof(*ui));

    if (!ui)
        return;

    ui->hover = -1;

    struct window *win = gui_create_window("Systemmonitor", 0, 0, 720, 420,
                                           WF_CENTER | WF_RESIZABLE,
                                           ICON_MONITOR);
    if (!win) {
        kfree(ui);
        return;
    }

    win->user     = ui;
    win->on_paint = monitor_paint;
    win->on_event = monitor_event;
    win->on_close = monitor_close;
    win->min_w    = 680;
    win->min_h    = 300;

    take_sample(ui);
    gui_focus_window(win);
}
