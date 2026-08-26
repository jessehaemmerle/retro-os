/* terminal.c - Textkonsole mit kleiner Kommandozeile.
 *
 * Die Konsole ist kein zweiter Betriebsmodus, sondern ein ganz normales
 * Fenster: sie schreibt in einen Zeilenpuffer und ruft dieselben
 * Dateisystemfunktionen auf wie der Dateimanager.
 */

#include "apps.h"
#include "usb.h"
#include "arch.h"
#include "block.h"
#include "net.h"
#include "process.h"
#include "thread.h"
#include "boot.h"
#include "font.h"
#include "kstring.h"
#include "mm.h"
#include "power.h"
#include "rtc.h"
#include "theme.h"
#include "widgets.h"

#include <stdarg.h>

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#define TERM_COLS      120
#define TERM_LINES     300
#define TERM_INPUT_MAX 200
#define TERM_LINE_H    16
#define TERM_PAD       6

#define COL_TERM_BG    RGB(0x10, 0x18, 0x14)
#define COL_TERM_FG    RGB(0x7C, 0xE0, 0x8C)
#define COL_TERM_HI    RGB(0xE8, 0xE8, 0xC0)
#define COL_TERM_ERR   RGB(0xE0, 0x70, 0x60)

struct term_state {
    char    lines[TERM_LINES][TERM_COLS];
    uint8_t colors[TERM_LINES];
    int32_t line_count;
    int32_t scroll;

    char    input[TERM_INPUT_MAX + 1];
    int32_t cursor;
    bool    caret_on;

    struct fs_node *cwd;

    /* Laeuft gerade ein Benutzerprogramm in diesem Fenster? */
    struct process *running;
    char    partial[TERM_COLS];
    int32_t partial_len;
};

enum { C_NORMAL, C_HIGHLIGHT, C_ERROR };

/* ------------------------------------------------------------------ */
/* Ausgabe                                                             */
/* ------------------------------------------------------------------ */

static void term_line(struct term_state *st, uint8_t color, const char *text)
{
    if (st->line_count >= TERM_LINES) {
        /* Aelteste Zeile herausschieben. */
        memmove(st->lines[0], st->lines[1],
                sizeof(st->lines) - sizeof(st->lines[0]));
        memmove(st->colors, st->colors + 1, sizeof(st->colors) - 1);
        st->line_count--;
    }

    strlcpy(st->lines[st->line_count], text, TERM_COLS);
    st->colors[st->line_count] = color;
    st->line_count++;
}

static void term_printf(struct term_state *st, uint8_t color, const char *fmt, ...)
{
    char buf[TERM_COLS];
    va_list ap;

    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    term_line(st, color, buf);
}

static void term_scroll_to_end(struct window *win, struct term_state *st);
static struct window *g_term_window;

static void term_scroll_to_end_public(struct term_state *st)
{
    if (g_term_window)
        term_scroll_to_end(g_term_window, st);
}

/* ------------------------------------------------------------------ */
/* Kommandos                                                           */
/* ------------------------------------------------------------------ */

/* Zerlegt die Eingabe in Kommando und bis zu zwei Argumente. */
static int split(char *line, char *argv[], int max)
{
    int argc = 0;

    while (*line && argc < max) {
        while (*line == ' ')
            *line++ = '\0';
        if (!*line)
            break;

        argv[argc++] = line;
        while (*line && *line != ' ')
            line++;
    }
    return argc;
}

static void cmd_help(struct term_state *st)
{
    term_line(st, C_HIGHLIGHT, "Verfuegbare Befehle:");
    term_line(st, C_NORMAL, "  hilfe            diese Uebersicht");
    term_line(st, C_NORMAL, "  ls [pfad]        Ordnerinhalt anzeigen");
    term_line(st, C_NORMAL, "  cd <pfad>        Ordner wechseln");
    term_line(st, C_NORMAL, "  pwd              aktuellen Pfad anzeigen");
    term_line(st, C_NORMAL, "  cat <datei>      Datei ausgeben");
    term_line(st, C_NORMAL, "  mkdir <name>     Ordner anlegen");
    term_line(st, C_NORMAL, "  touch <name>     leere Datei anlegen");
    term_line(st, C_NORMAL, "  schreib <datei> <text>   Text anhaengen");
    term_line(st, C_NORMAL, "  rm <name>        Datei oder Ordner loeschen");
    term_line(st, C_NORMAL, "  edit <datei>     Datei im Editor oeffnen");
    term_line(st, C_NORMAL, "  echo <text>      Text ausgeben");
    term_line(st, C_NORMAL, "  speicher         Speicherbelegung");
    term_line(st, C_NORMAL, "  threads          laufende Threads anzeigen");
    term_line(st, C_NORMAL, "  starte <programm> [text]  Programm in Ring 3 starten");
    term_line(st, C_NORMAL, "  programme        mitgelieferte Programme zeigen");
    term_line(st, C_NORMAL, "  laufzeit         Zeit seit dem Start");
    term_line(st, C_NORMAL, "  datum            Datum und Uhrzeit");
    term_line(st, C_NORMAL, "  version          Systemversion");
    term_line(st, C_NORMAL, "  netz             Netzwerkeinstellungen");
    term_line(st, C_NORMAL, "  ping <ziel>      Erreichbarkeit pruefen");
    term_line(st, C_NORMAL, "  aufloesen <name> Namen in eine Adresse wandeln");
    term_line(st, C_NORMAL, "  holen <adresse> [datei]  Seite abrufen/speichern");
    term_line(st, C_NORMAL, "  platte           Datentraeger anzeigen");
    term_line(st, C_NORMAL, "  usb              Geraete am USB-Bus anzeigen");
    term_line(st, C_NORMAL, "  formatieren      Datentraeger neu formatieren");
    term_line(st, C_NORMAL, "  leeren           Bildschirm loeschen");
    term_line(st, C_NORMAL, "  neustart         Rechner neu starten");
}

static void cmd_ls(struct term_state *st, const char *arg)
{
    struct fs_node *dir = arg ? fs_lookup(st->cwd, arg) : st->cwd;

    if (!dir) {
        term_printf(st, C_ERROR, "ls: \"%s\" nicht gefunden", arg);
        return;
    }
    if (dir->type != FS_DIR) {
        term_printf(st, C_ERROR, "ls: \"%s\" ist eine Datei", dir->name);
        return;
    }

    struct fs_node *entries[256];
    size_t n = fs_list(dir, entries, ARRAY_LEN(entries));

    for (size_t i = 0; i < n; i++) {
        struct fs_node *e = entries[i];
        char size[24];

        if (e->type == FS_DIR)
            ksnprintf(size, sizeof(size), "<ORDNER>");
        else
            fs_format_size(size, sizeof(size), e->size);

        term_printf(st, e->type == FS_DIR ? C_HIGHLIGHT : C_NORMAL,
                    "  %-28s %10s  %02u.%02u.%04u %02u:%02u",
                    e->name, size, e->mtime_day, e->mtime_month,
                    e->mtime_year, e->mtime_hour, e->mtime_min);
    }
    term_printf(st, C_NORMAL, "  %u Eintraege", (unsigned)n);
}

static void cmd_cat(struct term_state *st, const char *arg)
{
    struct fs_node *f = fs_lookup(st->cwd, arg);

    if (!f) {
        term_printf(st, C_ERROR, "cat: \"%s\" nicht gefunden", arg);
        return;
    }
    if (f->type == FS_DIR) {
        term_printf(st, C_ERROR, "cat: \"%s\" ist ein Ordner", arg);
        return;
    }

    if (!fs_load(f)) {
        term_printf(st, C_ERROR, "cat: \"%s\" laesst sich nicht lesen", arg);
        return;
    }

    if (!f->data || f->size == 0) {
        term_line(st, C_NORMAL, "(leere Datei)");
        return;
    }

    char line[TERM_COLS];
    size_t pos = 0;

    while (pos < f->size) {
        size_t n = 0;

        while (pos < f->size && f->data[pos] != '\n' && n < TERM_COLS - 1)
            line[n++] = (char)f->data[pos++];
        line[n] = '\0';

        if (pos < f->size && f->data[pos] == '\n')
            pos++;
        term_line(st, C_NORMAL, line);
    }
}

static void cmd_memory(struct term_state *st)
{
    term_printf(st, C_HIGHLIGHT, "Physischer Speicher");
    term_printf(st, C_NORMAL, "  gesamt  : %u MiB",
                (unsigned)(pmm_total_bytes() / (1024 * 1024)));
    term_printf(st, C_NORMAL, "  belegt  : %u MiB",
                (unsigned)(pmm_used_bytes() / (1024 * 1024)));
    term_printf(st, C_NORMAL, "  frei    : %u MiB",
                (unsigned)(pmm_free_bytes() / (1024 * 1024)));
    term_printf(st, C_HIGHLIGHT, "Kernel-Heap");
    term_printf(st, C_NORMAL, "  reserviert : %u KiB",
                (unsigned)(heap_total_bytes() / 1024));
    term_printf(st, C_NORMAL, "  benutzt    : %u KiB",
                (unsigned)(heap_used_bytes() / 1024));
    term_printf(st, C_HIGHLIGHT, "Dateisystem");
    term_printf(st, C_NORMAL, "  %u Eintraege, %u Bytes",
                (unsigned)fs_node_count(), (unsigned)fs_bytes_used());
}

/* Holt eine Seite und zeigt sie an oder legt sie als Datei ab. */
static void cmd_fetch(struct term_state *st, const char *url, const char *target)
{
    struct http_response response;

    if (!url) {
        term_line(st, C_ERROR, "holen: <adresse> [datei]");
        return;
    }

    char full[320];

    /* Ohne Schema wird http angenommen; https bleibt unangetastet. */
    if (strncasecmp(url, "http://", 7) != 0 &&
        strncasecmp(url, "https://", 8) != 0)
        ksnprintf(full, sizeof(full), "http://%s", url);
    else
        strlcpy(full, url, sizeof(full));

    term_printf(st, C_NORMAL, "Rufe %s ab ...", full);

    if (!http_get(full, &response)) {
        term_printf(st, C_ERROR, "holen: %s", response.error);
        return;
    }

    term_printf(st, C_HIGHLIGHT, "%d, %u Byte, %s", response.status,
                (unsigned)response.body_length, response.content_type);
    if (response.security[0])
        term_printf(st, C_HIGHLIGHT, "Verschluesselt: %s", response.security);

    if (target) {
        struct fs_node *file = fs_lookup(st->cwd, target);

        if (!file)
            file = fs_create_path(st->cwd, target, FS_FILE);

        if (!file || file->type != FS_FILE) {
            term_printf(st, C_ERROR, "holen: %s laesst sich nicht anlegen", target);
        } else if (!fs_write(file, response.body, response.body_length)) {
            term_printf(st, C_ERROR, "holen: %s laesst sich nicht schreiben", target);
        } else {
            term_printf(st, C_HIGHLIGHT, "Gespeichert als %s", target);
        }
    } else {
        /* Ohne Zieldatei die ersten Zeilen anzeigen. */
        const char *p = response.body;
        const char *end = response.body + response.body_length;
        char line[TERM_COLS];

        for (int i = 0; i < 20 && p < end; i++) {
            size_t n = 0;

            while (p < end && *p != '\n' && n < TERM_COLS - 1)
                line[n++] = *p++;
            line[n] = '\0';
            if (p < end && *p == '\n')
                p++;
            term_line(st, C_NORMAL, line);
        }
        if (p < end)
            term_line(st, C_HIGHLIGHT, "... (gekuerzt)");
    }

    http_response_free(&response);
}

/* Zeigt, was am USB-Bus haengt. */
static void cmd_usb(struct term_state *st)
{
    char line[128];

    if (usb_device_count() == 0) {
        term_line(st, C_NORMAL, "Keine USB-Geraete gefunden.");
        return;
    }

    for (size_t i = 0; i < usb_device_count(); i++) {
        const struct usb_device_info *info =
            usb_device_details(usb_device_at(i));

        if (!info)
            continue;

        const char *art = "Geraet";

        if (info->interface_class == USB_CLASS_HID) {
            if (info->interface_protocol == HID_PROTOCOL_KEYBOARD)
                art = "Tastatur";
            else if (info->interface_protocol == HID_PROTOCOL_MOUSE)
                art = "Maus";
            else
                art = "Eingabegeraet";
        }

        ksnprintf(line, sizeof(line), "Anschluss %u: %s", info->port, art);
        term_line(st, C_HIGHLIGHT, line);
        ksnprintf(line, sizeof(line),
                  "  %04x:%04x, Klasse %u.%u.%u, %s, %u Byte je Paket",
                  info->vendor_id, info->product_id, info->interface_class,
                  info->interface_subclass, info->interface_protocol,
                  usb_speed_name(info->speed), info->max_packet);
        term_line(st, C_NORMAL, line);
    }
}

static void cmd_disk(struct term_state *st)
{
    if (block_device_count() == 0) {
        term_line(st, C_ERROR, "Kein Datentraeger gefunden.");
        return;
    }

    for (size_t i = 0; i < block_device_count(); i++) {
        struct block_device *d = block_device_at(i);

        term_printf(st, C_HIGHLIGHT, "%s: %s", d->name, d->model);
        term_printf(st, C_NORMAL, "  %u Sektoren zu %u Byte (%u MiB)",
                    (unsigned)d->sector_count, (unsigned)d->sector_size,
                    (unsigned)(d->sector_count * d->sector_size / (1024 * 1024)));
    }

    if (!fs_disk_mounted()) {
        term_line(st, C_ERROR, "Kein FAT32-Dateisystem eingehaengt.");
        return;
    }

    struct fat_volume *vol = fs_disk_volume();
    char total[24], freetext[24];

    fs_format_size(total, sizeof(total), (size_t)fat_total_bytes(vol));
    fs_format_size(freetext, sizeof(freetext), (size_t)fat_free_bytes(vol));

    term_printf(st, C_HIGHLIGHT, "Eingehaengt: /Festplatte (%s)", fs_disk_name());
    term_printf(st, C_NORMAL, "  Dateisystem : FAT32, %u Byte je Cluster",
                (unsigned)vol->cluster_bytes);
    term_printf(st, C_NORMAL, "  Groesse     : %s", total);
    term_printf(st, C_NORMAL, "  frei        : %s", freetext);
}

static void cmd_network(struct term_state *st)
{
    char text[24];

    if (!g_netif.up) {
        term_line(st, C_ERROR, "Keine Netzwerkkarte gefunden.");
        return;
    }

    mac_format(&g_netif.mac, text, sizeof(text));
    term_printf(st, C_HIGHLIGHT, "%s", e1000_model());
    term_printf(st, C_NORMAL, "  Hardware-Adresse : %s", text);

    if (!net_ready()) {
        term_line(st, C_ERROR, "  Keine IP-Adresse (DHCP ohne Antwort).");
        return;
    }

    ip_format(g_netif.ip, text, sizeof(text));
    term_printf(st, C_NORMAL, "  IP-Adresse       : %s", text);
    ip_format(g_netif.netmask, text, sizeof(text));
    term_printf(st, C_NORMAL, "  Netzmaske        : %s", text);
    ip_format(g_netif.gateway, text, sizeof(text));
    term_printf(st, C_NORMAL, "  Gateway          : %s", text);
    ip_format(g_netif.dns, text, sizeof(text));
    term_printf(st, C_NORMAL, "  Namensserver     : %s", text);
    term_printf(st, C_NORMAL, "  Empfangen        : %u Pakete, %u Byte",
                (unsigned)g_netif.rx_packets, (unsigned)g_netif.rx_bytes);
    term_printf(st, C_NORMAL, "  Gesendet         : %u Pakete, %u Byte",
                (unsigned)g_netif.tx_packets, (unsigned)g_netif.tx_bytes);
}

static void cmd_ping(struct term_state *st, const char *target)
{
    ip_addr_t addr;
    char text[16];

    if (!target) {
        term_line(st, C_ERROR, "ping: Ziel fehlt");
        return;
    }
    if (!net_ready()) {
        term_line(st, C_ERROR, "ping: keine Netzwerkverbindung");
        return;
    }
    if (!dns_resolve(target, &addr)) {
        term_printf(st, C_ERROR, "ping: %s nicht gefunden", target);
        return;
    }

    ip_format(addr, text, sizeof(text));
    term_printf(st, C_HIGHLIGHT, "Ping an %s (%s):", target, text);

    int received = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t rtt = 0;

        if (icmp_ping(addr, 1500, &rtt)) {
            term_printf(st, C_NORMAL, "  Antwort von %s: Zeit %u ms", text,
                        (unsigned)rtt);
            received++;
        } else {
            term_line(st, C_ERROR, "  Zeitueberschreitung");
        }
    }
    term_printf(st, C_HIGHLIGHT, "  %d von 4 Antworten", received);
}

/* Startet ein Programm; die Ausgabe holt der Takt danach ab. */
static void cmd_start_program(struct window *win, struct term_state *st,
                              const char *path, const char *raw)
{
    char error[128];
    char full[FS_PATH_MAX];

    UNUSED(win);

    if (!path) {
        term_line(st, C_ERROR, "starte: <programm> [text]");
        term_line(st, C_NORMAL, "  z.B.  starte /Programme/hallo.elf");
        return;
    }

    /* Ohne Pfad wird unter /Programme gesucht. */
    if (strchr(path, '/')) {
        strlcpy(full, path, sizeof(full));
    } else if (strchr(path, '.')) {
        ksnprintf(full, sizeof(full), "/Programme/%s", path);
    } else {
        ksnprintf(full, sizeof(full), "/Programme/%s.elf", path);
    }

    /* Alles hinter dem Programmnamen wird uebergeben. */
    const char *args = raw;
    for (int skip = 0; skip < 2 && *args; skip++) {
        while (*args == ' ')
            args++;
        while (*args && *args != ' ')
            args++;
    }
    while (*args == ' ')
        args++;

    struct process *proc = process_start(full, args, error, sizeof(error));

    if (!proc) {
        term_printf(st, C_ERROR, "starte: %s", error);
        return;
    }

    st->running = proc;
    st->partial_len = 0;
    term_printf(st, C_HIGHLIGHT, "[%s laeuft als Nummer %u]", proc->name,
                (unsigned)proc->pid);
}

/* Holt die Ausgabe des laufenden Programms ab und macht Zeilen daraus. */
static void drain_process(struct term_state *st)
{
    char chunk[256];

    if (!st->running)
        return;

    size_t n = process_read_output(st->running, chunk, sizeof(chunk));

    for (size_t i = 0; i < n; i++) {
        char c = chunk[i];

        if (c == '\n' || st->partial_len >= TERM_COLS - 2) {
            st->partial[st->partial_len] = '\0';
            term_line(st, C_NORMAL, st->partial);
            st->partial_len = 0;
            if (c == '\n')
                continue;
        }
        if (c == '\r' || c == '\t')
            c = ' ';
        if (c >= 32 || c < 0)
            st->partial[st->partial_len++] = c;
    }

    if (st->running->finished) {
        if (st->partial_len > 0) {
            st->partial[st->partial_len] = '\0';
            term_line(st, C_NORMAL, st->partial);
            st->partial_len = 0;
        }
        term_printf(st, C_HIGHLIGHT, "[%s beendet, Ergebnis %d]",
                    st->running->name, st->running->exit_code);
        st->running = NULL;
    }

    term_scroll_to_end_public(st);
}

static const char *thread_state_name(uint8_t state)
{
    switch (state) {
    case THREAD_RUNNING:  return "laeuft";
    case THREAD_READY:    return "bereit";
    case THREAD_SLEEPING: return "schlaeft";
    case THREAD_BLOCKED:  return "wartet";
    default:              return "beendet";
    }
}

static void cmd_threads(struct term_state *st)
{
    term_printf(st, C_HIGHLIGHT, "%-4s %-16s %-10s %-6s %s",
                "Nr.", "Name", "Zustand", "Wicht.", "Laeufe");

    for (size_t i = 0; i < thread_count(); i++) {
        struct thread *t = thread_at(i);

        if (!t)
            continue;

        term_printf(st, t == thread_current() ? C_HIGHLIGHT : C_NORMAL,
                    "%-4u %-16s %-10s %-6u %u",
                    (unsigned)t->id, t->name, thread_state_name(t->state),
                    (unsigned)t->priority, (unsigned)t->cpu_ticks);
    }
}

static void term_prompt_line(struct term_state *st, const char *cmd)
{
    char path[FS_PATH_MAX];
    char line[TERM_COLS];

    fs_path(st->cwd, path, sizeof(path));
    ksnprintf(line, sizeof(line), "%s> %s", path, cmd);
    term_line(st, C_HIGHLIGHT, line);
}

static void term_execute(struct window *win, struct term_state *st, char *input)
{
    char *argv[8];
    char  raw[TERM_INPUT_MAX + 1];

    strlcpy(raw, input, sizeof(raw));
    term_prompt_line(st, raw);

    int argc = split(input, argv, 8);
    if (argc == 0)
        return;

    const char *cmd = argv[0];
    const char *a1  = argc > 1 ? argv[1] : NULL;

    if (!strcasecmp(cmd, "hilfe") || !strcasecmp(cmd, "help") ||
        !strcmp(cmd, "?")) {
        cmd_help(st);

    } else if (!strcasecmp(cmd, "ls") || !strcasecmp(cmd, "dir")) {
        cmd_ls(st, a1);

    } else if (!strcasecmp(cmd, "cd")) {
        struct fs_node *dir = a1 ? fs_lookup(st->cwd, a1) : fs_root();

        if (!dir || dir->type != FS_DIR)
            term_printf(st, C_ERROR, "cd: \"%s\" ist kein Ordner", a1 ? a1 : "/");
        else
            st->cwd = dir;

    } else if (!strcasecmp(cmd, "pwd")) {
        char path[FS_PATH_MAX];

        fs_path(st->cwd, path, sizeof(path));
        term_line(st, C_NORMAL, path);

    } else if (!strcasecmp(cmd, "cat") || !strcasecmp(cmd, "type")) {
        if (a1)
            cmd_cat(st, a1);
        else
            term_line(st, C_ERROR, "cat: Dateiname fehlt");

    } else if (!strcasecmp(cmd, "mkdir")) {
        if (!a1)
            term_line(st, C_ERROR, "mkdir: Name fehlt");
        else if (!fs_create(st->cwd, a1, FS_DIR))
            term_printf(st, C_ERROR, "mkdir: \"%s\" konnte nicht angelegt werden", a1);

    } else if (!strcasecmp(cmd, "touch")) {
        if (!a1) {
            term_line(st, C_ERROR, "touch: Name fehlt");
        } else {
            struct fs_node *f = fs_create(st->cwd, a1, FS_FILE);

            if (f)
                fs_write(f, "", 0);
            else
                term_printf(st, C_ERROR, "touch: \"%s\" existiert bereits", a1);
        }

    } else if (!strcasecmp(cmd, "schreib")) {
        struct fs_node *f = a1 ? fs_lookup(st->cwd, a1) : NULL;

        if (!f)
            f = a1 ? fs_create(st->cwd, a1, FS_FILE) : NULL;

        if (!f || f->type != FS_FILE) {
            term_line(st, C_ERROR, "schreib: <datei> <text>");
        } else {
            const char *text = raw;

            /* Hinter Kommando und Dateiname beginnt der Text. */
            for (int skip = 0; skip < 2 && *text; skip++) {
                while (*text == ' ')
                    text++;
                while (*text && *text != ' ')
                    text++;
            }
            while (*text == ' ')
                text++;

            fs_append(f, text, strlen(text));
            fs_append(f, "\n", 1);
        }

    } else if (!strcasecmp(cmd, "rm") || !strcasecmp(cmd, "del")) {
        struct fs_node *n = a1 ? fs_lookup(st->cwd, a1) : NULL;

        if (!n)
            term_printf(st, C_ERROR, "rm: \"%s\" nicht gefunden", a1 ? a1 : "");
        else if (!fs_remove(n))
            term_printf(st, C_ERROR, "rm: \"%s\" ist geschuetzt", a1);

    } else if (!strcasecmp(cmd, "edit")) {
        struct fs_node *f = a1 ? fs_lookup(st->cwd, a1) : NULL;

        if (!f || f->type != FS_FILE)
            term_printf(st, C_ERROR, "edit: \"%s\" nicht gefunden", a1 ? a1 : "");
        else
            editor_open(f);

    } else if (!strcasecmp(cmd, "echo")) {
        const char *text = raw;

        while (*text && *text != ' ')
            text++;
        while (*text == ' ')
            text++;
        term_line(st, C_NORMAL, text);

    } else if (!strcasecmp(cmd, "speicher") || !strcasecmp(cmd, "mem")) {
        cmd_memory(st);

    } else if (!strcasecmp(cmd, "starte") || !strcasecmp(cmd, "run")) {
        cmd_start_program(win, st, a1, raw);

    } else if (!strcasecmp(cmd, "programme")) {
        cmd_ls(st, "/Programme");

    } else if (!strcasecmp(cmd, "threads") || !strcasecmp(cmd, "ps")) {
        cmd_threads(st);

    } else if (!strcasecmp(cmd, "laufzeit") || !strcasecmp(cmd, "uptime")) {
        uint64_t ms = timer_ms();

        term_printf(st, C_NORMAL, "Laufzeit: %u:%02u:%02u",
                    (unsigned)(ms / 3600000), (unsigned)(ms / 60000 % 60),
                    (unsigned)(ms / 1000 % 60));

    } else if (!strcasecmp(cmd, "datum") || !strcasecmp(cmd, "date")) {
        char d[24], t[16];

        rtc_format_date(d, sizeof(d));
        rtc_format_time(t, sizeof(t));
        term_printf(st, C_NORMAL, "%s  %s", d, t);

    } else if (!strcasecmp(cmd, "version") || !strcasecmp(cmd, "ver")) {
        term_line(st, C_HIGHLIGHT, "RetroOS 1.0 (x86-64)");

    } else if (!strcasecmp(cmd, "netz") || !strcasecmp(cmd, "ipconfig")) {
        cmd_network(st);

    } else if (!strcasecmp(cmd, "ping")) {
        cmd_ping(st, a1);

    } else if (!strcasecmp(cmd, "aufloesen") || !strcasecmp(cmd, "nslookup")) {
        ip_addr_t addr;

        if (!a1) {
            term_line(st, C_ERROR, "aufloesen: Name fehlt");
        } else if (dns_resolve(a1, &addr)) {
            char text[16];
            ip_format(addr, text, sizeof(text));
            term_printf(st, C_HIGHLIGHT, "%s hat die Adresse %s", a1, text);
        } else {
            term_printf(st, C_ERROR, "aufloesen: %s nicht gefunden", a1);
        }

    } else if (!strcasecmp(cmd, "holen") || !strcasecmp(cmd, "wget")) {
        cmd_fetch(st, a1, argc > 2 ? argv[2] : NULL);

    } else if (!strcasecmp(cmd, "platte")) {
        cmd_disk(st);

    } else if (!strcasecmp(cmd, "usb")) {
        cmd_usb(st);

    } else if (!strcasecmp(cmd, "formatieren")) {
        if (!a1 || strcasecmp(a1, "wirklich") != 0) {
            term_line(st, C_ERROR,
                      "Das loescht alle Daten auf dem Datentraeger.");
            term_line(st, C_NORMAL,
                      "Zum Bestaetigen: formatieren wirklich [Bezeichnung]");
        } else {
            term_line(st, C_NORMAL, "Formatiere ...");
            if (fs_format_disk(argc > 2 ? argv[2] : "RETROOS"))
                term_line(st, C_HIGHLIGHT, "Fertig. /Festplatte ist wieder da.");
            else
                term_line(st, C_ERROR, "Formatieren fehlgeschlagen.");
        }

    } else if (!strcasecmp(cmd, "leeren") || !strcasecmp(cmd, "clear") ||
               !strcasecmp(cmd, "cls")) {
        st->line_count = 0;
        st->scroll = 0;

    } else if (!strcasecmp(cmd, "neustart") || !strcasecmp(cmd, "reboot")) {
        power_reboot();

    } else {
        term_printf(st, C_ERROR,
                    "Unbekannter Befehl: %s  (\"hilfe\" zeigt alle Befehle)", cmd);
    }

    UNUSED(win);
}

/* ------------------------------------------------------------------ */
/* Fenster                                                             */
/* ------------------------------------------------------------------ */

static int32_t term_visible_lines(struct window *win)
{
    /* Eine Zeile bleibt fuer die Eingabe reserviert. */
    return MAX((gui_client_height(win) - 2 * TERM_PAD) / TERM_LINE_H - 1, 1);
}

static void term_scroll_to_end(struct window *win, struct term_state *st)
{
    int32_t rows = term_visible_lines(win);

    st->scroll = MAX(st->line_count - rows, 0);
}

static uint32_t term_color(uint8_t code)
{
    switch (code) {
    case C_HIGHLIGHT: return COL_TERM_HI;
    case C_ERROR:     return COL_TERM_ERR;
    default:          return COL_TERM_FG;
    }
}

static void term_paint(struct window *win, struct canvas *c)
{
    struct term_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);
    int32_t rows = term_visible_lines(win);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_TERM_BG);

    for (int32_t i = 0; i < rows; i++) {
        int32_t index = st->scroll + i;

        if (index >= st->line_count)
            break;

        gfx_text(&local, TERM_PAD, TERM_PAD + i * TERM_LINE_H,
                 st->lines[index], term_color(st->colors[index]));
    }

    /* Eingabezeile */
    int32_t y = TERM_PAD + rows * TERM_LINE_H;
    char path[FS_PATH_MAX], prompt[TERM_COLS];

    fs_path(st->cwd, path, sizeof(path));
    ksnprintf(prompt, sizeof(prompt), "%s> ", path);

    gfx_text(&local, TERM_PAD, y, prompt, COL_TERM_HI);

    int32_t px = TERM_PAD + gfx_text_width(prompt);
    gfx_text(&local, px, y, st->input, COL_WHITE);

    if (st->caret_on)
        gfx_fill(&local, rect_make(px + st->cursor * FONT_WIDTH, y,
                                   FONT_WIDTH, FONT_HEIGHT), COL_TERM_FG);
}

static void term_key(struct window *win, const struct gui_event *ev)
{
    struct term_state *st = win->user;
    size_t len = strlen(st->input);

    /* Waehrend ein Programm laeuft, gehen Eingaben dorthin. */
    if (st->running) {
        if ((ev->mods & MOD_CTRL) && (ev->ascii == 'c' || ev->ascii == 'C')) {
            term_printf(st, C_ERROR, "[%s abgebrochen]", st->running->name);
            process_kill(st->running);
            st->running = NULL;
            gui_invalidate();
            return;
        }
        if (ev->key == KEY_ENTER) {
            char line[TERM_INPUT_MAX + 2];

            ksnprintf(line, sizeof(line), "%s\n", st->input);
            process_write_input(st->running, line, strlen(line));
            term_printf(st, C_NORMAL, "%s", st->input);
            st->input[0] = '\0';
            st->cursor = 0;
            gui_invalidate();
            return;
        }
    }

    switch (ev->key) {
    case KEY_ENTER: {
        char line[TERM_INPUT_MAX + 1];

        strlcpy(line, st->input, sizeof(line));
        st->input[0] = '\0';
        st->cursor = 0;
        term_execute(win, st, line);
        term_scroll_to_end(win, st);
        break;
    }
    case KEY_BACKSPACE:
        if (st->cursor > 0) {
            memmove(&st->input[st->cursor - 1], &st->input[st->cursor],
                    len - (size_t)st->cursor + 1);
            st->cursor--;
        }
        break;
    case KEY_DELETE:
        if (st->input[st->cursor])
            memmove(&st->input[st->cursor], &st->input[st->cursor + 1],
                    len - (size_t)st->cursor);
        break;
    case KEY_LEFT:
        if (st->cursor > 0)
            st->cursor--;
        break;
    case KEY_RIGHT:
        if (st->input[st->cursor])
            st->cursor++;
        break;
    case KEY_HOME:
        st->cursor = 0;
        break;
    case KEY_END:
        st->cursor = (int32_t)len;
        break;
    case KEY_PAGEUP:
        st->scroll = MAX(st->scroll - term_visible_lines(win), 0);
        break;
    case KEY_PAGEDOWN:
        st->scroll = MIN(st->scroll + term_visible_lines(win),
                         MAX(st->line_count - term_visible_lines(win), 0));
        break;
    default:
        if (ev->ascii >= 32 && (unsigned char)ev->ascii != 127 &&
            len < TERM_INPUT_MAX) {
            memmove(&st->input[st->cursor + 1], &st->input[st->cursor],
                    len - (size_t)st->cursor + 1);
            st->input[st->cursor++] = ev->ascii;
        } else {
            return;
        }
        break;
    }

    st->caret_on = true;
    gui_invalidate();
}

static void term_event(struct window *win, const struct gui_event *ev)
{
    struct term_state *st = win->user;

    switch (ev->type) {
    case EV_KEY_DOWN:
        term_key(win, ev);
        break;
    case EV_SCROLL: {
        int32_t max_off = MAX(st->line_count - term_visible_lines(win), 0);

        st->scroll = CLAMP(st->scroll - ev->scroll * 3, 0, max_off);
        gui_invalidate();
        break;
    }
    case EV_TICK:
        st->caret_on = !st->caret_on;
        g_term_window = win;

        if (st->running) {
            drain_process(st);
            gui_invalidate();
        }
        gui_invalidate();
        break;
    case EV_RESIZED:
        term_scroll_to_end(win, st);
        gui_invalidate();
        break;
    default:
        break;
    }
}

static void term_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_terminal(void)
{
    struct term_state *st = kzalloc(sizeof(*st));

    if (!st)
        return;

    static int32_t cascade;
    int32_t offset = (cascade++ % 5) * 24;

    struct window *win = gui_create_window("Konsole", 200 + offset, 120 + offset,
                                           640, 380, WF_RESIZABLE, ICON_TERMINAL);
    if (!win) {
        kfree(st);
        return;
    }

    st->cwd = fs_root();
    st->caret_on = true;

    win->user     = st;
    win->on_paint = term_paint;
    win->on_event = term_event;
    win->on_close = term_close;
    win->min_w    = 420;
    win->min_h    = 200;

    term_line(st, C_HIGHLIGHT, "RetroOS-Konsole 1.0");
    term_line(st, C_NORMAL, "\"hilfe\" zeigt alle Befehle.");
    term_line(st, C_NORMAL, "");

    gui_focus_window(win);
}
