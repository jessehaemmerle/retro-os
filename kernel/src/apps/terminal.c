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
#include "clipboard.h"
#include "config.h"
#include "crypto.h"
#include "lock.h"
#include "audit.h"
#include "firewall.h"
#include "log.h"
#include "perm.h"
#include "sandbox.h"
#include "shellutil.h"
#include "tasks.h"
#include "user.h"
#include "nic.h"
#include "cpu.h"
#include "setup.h"
#include "process.h"
#include "trash.h"
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
#define TERM_HISTORY   20
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

    /* Die zuletzt eingegebenen Zeilen. Sie liegen als Feld und nicht als
     * Ring: Bei zwanzig Zeilen ist das Nachrutschen billiger als das
     * Rechnen mit zwei Zeigern, und "verlauf" kann sie einfach von
     * vorn nach hinten ausgeben. */
    char    history[TERM_HISTORY][TERM_INPUT_MAX + 1];
    size_t  history_count;
    /* Wo die Pfeiltasten gerade stehen; gleich der Anzahl heisst
     * "in der frischen Zeile". */
    size_t  history_pos;

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

static void cmd_ls(struct term_state *st, const char *arg, bool detail)
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

    if (!n && !perm_may(dir, P_R)) {
        term_printf(st, C_ERROR, "ls: \"%s\" darfst du nicht lesen", dir->name);
        return;
    }

    for (size_t i = 0; i < n; i++) {
        struct fs_node *e = entries[i];
        char size[24];

        if (e->type == FS_DIR)
            ksnprintf(size, sizeof(size), "<ORDNER>");
        else
            fs_format_size(size, sizeof(size), e->size);

        if (detail) {
            char mode[11];

            perm_mode_text(e->mode, e->type, mode);
            term_printf(st, e->type == FS_DIR ? C_HIGHLIGHT : C_NORMAL,
                        "  %s %-10s %-10s %-22s %10s",
                        mode, user_name_of(e->uid), group_name_of(e->gid),
                        e->name, size);
        } else {
            term_printf(st, e->type == FS_DIR ? C_HIGHLIGHT : C_NORMAL,
                        "  %-28s %10s  %02u.%02u.%04u %02u:%02u",
                        e->name, size, e->mtime_day, e->mtime_month,
                        e->mtime_year, e->mtime_hour, e->mtime_min);
        }
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
    if (response.truncated)
        term_line(st, C_ERROR, "Achtung: die Antwort ist unvollstaendig.");

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

        switch (info->interface_class) {
        case USB_CLASS_HID:
            if (info->interface_protocol == HID_PROTOCOL_KEYBOARD)
                art = "Tastatur";
            else if (info->interface_protocol == HID_PROTOCOL_MOUSE)
                art = "Maus";
            else
                art = "Eingabegeraet";
            break;
        case USB_CLASS_STORAGE: art = "Speicher";  break;
        case 0x09:              art = "Verteiler"; break;
        case 0x01:              art = "Ton";       break;
        case 0x02:              art = "Netzwerk";  break;
        case 0x07:              art = "Drucker";   break;
        case 0x0E:              art = "Kamera";    break;
        default: break;
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

/* Meldet den Fortschritt der Installation in die Konsole. */
static void install_report(void *user, int percent, const char *text)
{
    struct term_state *st = user;

    term_printf(st, C_NORMAL, "  [%3d %%] %s", percent, text);
}

static void cmd_install(struct term_state *st, const char *target,
                        const char *confirm)
{
    if (!setup_sources_ready()) {
        term_line(st, C_ERROR,
                  "Von diesem Startmedium laesst sich nicht installieren.");
        term_line(st, C_NORMAL,
                  "Noetig ist das RetroOS-Abbild (ISO oder USB-Stick).");
        return;
    }

    /* Ohne Ziel: aufzaehlen, was in Frage kommt. */
    if (!target) {
        term_line(st, C_HIGHLIGHT, "RetroOS auf eine Festplatte bringen");
        term_line(st, C_NORMAL, "Moegliche Ziele:");

        size_t offered = 0;

        for (size_t i = 0; i < block_device_count(); i++) {
            struct block_device *d = block_device_at(i);
            struct setup_plan plan;
            char why[96];

            if (setup_plan_for(d, &plan, why, sizeof(why))) {
                term_printf(st, C_HIGHLIGHT, "  %-6s %s (%u MiB)",
                            d->name, d->model,
                            (unsigned)(d->sector_count / 2048));
                offered++;
            } else {
                term_printf(st, C_NORMAL, "  %-6s %s - %s",
                            d->name, d->model, why);
            }

            /* Passt daneben noch etwas hin? */
            if (setup_plan_beside(d, &plan, why, sizeof(why))) {
                term_printf(st, C_HIGHLIGHT,
                            "         daneben moeglich: %u MiB frei",
                            (unsigned)(plan.data_count / 2048));
                offered++;
            }
        }

        if (!offered) {
            term_line(st, C_ERROR, "Kein geeigneter Datentraeger dabei.");
            return;
        }
        term_line(st, C_NORMAL, "");
        term_line(st, C_NORMAL,
                  "Ganze Platte : installieren <name> wirklich");
        term_line(st, C_NORMAL,
                  "Daneben      : installieren <name> daneben");
        return;
    }

    struct block_device *dev = NULL;

    for (size_t i = 0; i < block_device_count(); i++) {
        struct block_device *d = block_device_at(i);

        if (!strcasecmp(d->name, target)) {
            dev = d;
            break;
        }
    }

    if (!dev) {
        term_printf(st, C_ERROR, "installieren: %s gibt es nicht", target);
        return;
    }

    struct setup_plan plan;
    char why[96];

    /* "installieren <name> daneben" laesst alles Vorhandene stehen. */
    bool beside = confirm && strcasecmp(confirm, "daneben") == 0;

    if (beside) {
        if (!setup_plan_beside(dev, &plan, why, sizeof(why))) {
            term_printf(st, C_ERROR, "installieren: %s", why);
            return;
        }

        char error[96];

        term_printf(st, C_NORMAL, "Installiere neben dem Vorhandenen auf %s ...",
                    dev->name);
        if (setup_run(&plan, install_report, st, error, sizeof(error))) {
            term_line(st, C_HIGHLIGHT, "Fertig.");
            if (plan.fallback_free)
                term_line(st, C_NORMAL,
                          "Der Rechner startet jetzt RetroOS.");
            else
                term_line(st, C_ERROR,
                          "Der uebliche Startpfad war belegt - RetroOS steht "
                          "unter EFI\\RETROOS und muss im Startmenue der "
                          "Firmware gewaehlt werden.");
        } else {
            term_printf(st, C_ERROR, "installieren: %s", error);
        }
        return;
    }

    if (!setup_plan_for(dev, &plan, why, sizeof(why))) {
        term_printf(st, C_ERROR, "installieren: %s", why);
        return;
    }

    if (!confirm || strcasecmp(confirm, "wirklich") != 0) {
        term_printf(st, C_ERROR,
                    "Das loescht alles auf %s (%s).", dev->name, dev->model);
        term_printf(st, C_NORMAL,
                    "  EFI-Abschnitt : %u MiB ab Sektor %u",
                    (unsigned)(plan.esp_count / 2048),
                    (unsigned)plan.esp_start);
        term_printf(st, C_NORMAL,
                    "  Ablage        : %u MiB ab Sektor %u",
                    (unsigned)(plan.data_count / 2048),
                    (unsigned)plan.data_start);
        term_printf(st, C_NORMAL,
                    "Zum Bestaetigen: installieren %s wirklich", dev->name);
        term_line(st, C_NORMAL,
                  "Oder daneben, ohne etwas zu loeschen: installieren <name> daneben");
        return;
    }

    char error[96];

    term_printf(st, C_NORMAL, "Installiere auf %s ...", dev->name);
    if (setup_run(&plan, install_report, st, error, sizeof(error))) {
        term_line(st, C_HIGHLIGHT,
                  "Fertig. Das Startmedium kann jetzt entfernt werden.");
    } else {
        term_printf(st, C_ERROR, "installieren: %s", error);
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
    if (strcmp(nic_model(), nic_family()) == 0)
        term_printf(st, C_HIGHLIGHT, "%s", nic_model());
    else
        term_printf(st, C_HIGHLIGHT, "%s (%s)", nic_model(), nic_family());
    term_printf(st, C_NORMAL, "  Hardware-Adresse : %s", text);

    if (nic_speed())
        term_printf(st, C_NORMAL, "  Verbindung       : %s, %u MBit/s",
                    nic_link_up() ? "steht" : "unterbrochen",
                    (unsigned)nic_speed());
    else
        term_printf(st, C_NORMAL, "  Verbindung       : %s",
                    nic_link_up() ? "steht" : "unterbrochen");

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

    if (nic_seen_count() > 1) {
        term_line(st, C_NORMAL, "  Auf dem Bus gefunden:");
        for (size_t i = 0; i < nic_seen_count(); i++)
            term_printf(st, C_NORMAL, "    %s", nic_seen_at(i));
    }
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
                              const char *path, const char *raw,
                              const char *profile, int skip_words)
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
    for (int skip = 0; skip < skip_words && *args; skip++) {
        while (*args == ' ')
            args++;
        while (*args && *args != ' ')
            args++;
    }
    while (*args == ' ')
        args++;

    struct process *proc = process_start_caged(full, args, profile, error,
                                               sizeof(error));

    if (!proc) {
        term_printf(st, C_ERROR, "starte: %s", error);
        return;
    }

    st->running = proc;
    st->partial_len = 0;

    if (proc->box.active) {
        char darf[96];

        sandbox_text(&proc->box, darf, sizeof(darf));
        term_printf(st, C_HIGHLIGHT,
                    "[%s laeuft als Nummer %u im Kaefig \"%s\": %s]",
                    proc->name, (unsigned)proc->pid, proc->box.profile, darf);
    } else {
        term_printf(st, C_HIGHLIGHT, "[%s laeuft als Nummer %u]", proc->name,
                    (unsigned)proc->pid);
    }
}

/* Holt die Ausgabe des laufenden Programms ab und macht Zeilen daraus. */
static void drain_process(struct term_state *st)
{
    char chunk[256];

    if (!st->running)
        return;

    /* Bis der Puffer leer ist - sonst ginge der letzte Satz eines
     * Programms verloren, das gleich danach endet. */
    size_t n;

    while ((n = process_read_output(st->running, chunk, sizeof(chunk))) > 0) {
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
    }

    if (st->running->finished) {
        if (st->partial_len > 0) {
            st->partial[st->partial_len] = '\0';
            term_line(st, C_NORMAL, st->partial);
            st->partial_len = 0;
        }
        term_printf(st, C_HIGHLIGHT, "[%s beendet, Ergebnis %d]",
                    st->running->name, st->running->exit_code);

        /* Erst jetzt gibt der Prozess seinen Steckplatz her - samt
         * allem, was er noch abgespalten hatte. */
        process_release(st->running);
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

/* Was gerade in Ring 3 laeuft - mit der Verwandtschaft, seit ein
 * Programm sich abspalten kann. */
static void cmd_processes(struct term_state *st)
{
    size_t n = process_count();

    if (n == 0) {
        term_line(st, C_NORMAL, "Zurzeit laeuft kein Programm.");
        return;
    }

    term_printf(st, C_HIGHLIGHT, "%-5s %-6s %-16s %-10s %s",
                "Nr.", "Eltern", "Name", "Zustand", "Speicher");

    for (size_t i = 0; i < n; i++) {
        struct process *p = process_at(i);
        char parent[8] = "-";

        if (!p)
            continue;
        if (p->parent_pid)
            ksnprintf(parent, sizeof(parent), "%u", (unsigned)p->parent_pid);

        term_printf(st, C_NORMAL, "%-5u %-6s %-16s %-10s %u KiB",
                    (unsigned)p->pid, parent, p->name,
                    p->finished ? "beendet" : "laeuft",
                    (unsigned)(p->space.heap_break / 1024));
    }

    term_printf(st, C_NORMAL, "");
    term_printf(st, C_NORMAL, "Mehrfach genutzt: %u KiB",
                (unsigned)(pmm_shared_bytes() / 1024));
}

/* Der Papierkorb von der Konsole aus. Ohne Argument die Liste, sonst
 * "zurueck <Nummer>" oder "leeren". */
static void cmd_trash(struct term_state *st, const char *what, const char *arg)
{
    struct fs_node *korb = trash_dir();

    if (!korb) {
        term_line(st, C_ERROR, "Es gibt keinen Papierkorb.");
        return;
    }

    if (what && !strcasecmp(what, "leeren")) {
        size_t gone = trash_empty();

        term_printf(st, C_NORMAL, "%u Eintraege endgueltig geloescht.",
                    (unsigned)gone);
        return;
    }

    struct fs_node *items[64];
    size_t count = fs_list(korb, items, ARRAY_LEN(items));

    if (what && !strcasecmp(what, "zurueck")) {
        int index = 0;

        for (const char *p = arg; p && *p >= '0' && *p <= '9'; p++)
            index = index * 10 + (*p - '0');

        if (index < 1 || (size_t)index > count) {
            term_printf(st, C_ERROR,
                        "papierkorb zurueck: Nummer 1 bis %u erwartet",
                        (unsigned)count);
            return;
        }

        struct fs_node *pick = items[index - 1];
        char name[FS_NAME_MAX + 1];

        strlcpy(name, pick->name, sizeof(name));
        if (trash_restore(pick))
            term_printf(st, C_NORMAL, "\"%s\" ist wieder da.", name);
        else
            term_printf(st, C_ERROR,
                        "\"%s\" liess sich nicht zurueckholen.", name);
        return;
    }

    if (count == 0) {
        term_line(st, C_NORMAL, "Der Papierkorb ist leer.");
        return;
    }

    term_printf(st, C_HIGHLIGHT, "%-4s %-24s %-10s %s",
                "Nr.", "Name", "Groesse", "Kam von");

    for (size_t i = 0; i < count; i++) {
        char size[24];

        fs_format_size(size, sizeof(size), fs_total_size(items[i]));
        term_printf(st, C_NORMAL, "%-4u %-24s %-10s %s",
                    (unsigned)(i + 1), items[i]->name, size,
                    trash_origin(items[i]));
    }

    char total[24];

    fs_format_size(total, sizeof(total), trash_bytes());
    term_printf(st, C_NORMAL, "");
    term_printf(st, C_NORMAL, "%u Eintraege, %s - \"papierkorb leeren\" "
                "raeumt auf", (unsigned)count, total);
}

/* --- Dateien kopieren, verschieben, durchsuchen --------------------- */

/* Ein Pfad, wie ihn der Benutzer eingetippt hat, als vollstaendiger
 * Pfad - fuer Meldungen und fuer den Vergleich zweier Ziele. */
static void full_path(struct term_state *st, const char *given, char *out,
                      size_t size)
{
    struct fs_node *node = fs_lookup(st->cwd, given);

    if (node) {
        fs_path(node, out, size);
        return;
    }
    strlcpy(out, given, size);
}

/* Kopiert einen Eintrag rekursiv. Der Zielordner muss es schon geben. */
static bool copy_entry(struct fs_node *src, struct fs_node *dest_dir,
                       const char *name)
{
    if (src->type == FS_DIR) {
        struct fs_node *copy = fs_create(dest_dir, name, FS_DIR);

        if (!copy)
            return false;

        struct fs_node *entries[128];
        size_t n = fs_list(src, entries, ARRAY_LEN(entries));

        for (size_t i = 0; i < n; i++)
            if (!copy_entry(entries[i], copy, entries[i]->name))
                return false;
        return true;
    }

    if (!fs_load(src))
        return false;

    struct fs_node *copy = fs_create(dest_dir, name, FS_FILE);

    if (!copy)
        return false;
    if (!src->size)
        return true;
    return fs_write(copy, src->data, src->size);
}

static void cmd_copy(struct term_state *st, const char *from, const char *to)
{
    if (!from || !to) {
        term_line(st, C_ERROR, "kopiere <quelle> <ziel>");
        return;
    }

    struct fs_node *src = fs_lookup(st->cwd, from);

    if (!src) {
        term_printf(st, C_ERROR, "kopiere: \"%s\" nicht gefunden", from);
        return;
    }

    /* Ist das Ziel ein Ordner, landet die Kopie darin - so erwartet man
     * es, und so muss niemand den Namen zweimal tippen. */
    struct fs_node *target = fs_lookup(st->cwd, to);
    struct fs_node *dir = NULL;
    char name[FS_NAME_MAX + 1];

    if (target && target->type == FS_DIR) {
        dir = target;
        strlcpy(name, src->name, sizeof(name));
    } else {
        char parent[FS_PATH_MAX];

        strlcpy(parent, to, sizeof(parent));

        char *cut = strrchr(parent, '/');

        if (cut) {
            *cut = '\0';
            strlcpy(name, cut + 1, sizeof(name));
            dir = fs_lookup(st->cwd, parent[0] ? parent : "/");
        } else {
            strlcpy(name, to, sizeof(name));
            dir = st->cwd;
        }
    }

    if (!dir || dir->type != FS_DIR) {
        term_printf(st, C_ERROR, "kopiere: \"%s\" ist kein Ordner", to);
        return;
    }
    if (fs_find_child(dir, name)) {
        term_printf(st, C_ERROR, "kopiere: \"%s\" gibt es dort schon", name);
        return;
    }

    /* Ein Ordner in sich selbst waere eine Kopie ohne Ende. */
    for (struct fs_node *p = dir; p; p = p->parent) {
        if (p != src)
            continue;
        term_line(st, C_ERROR, "kopiere: das waere eine Kopie in sich selbst");
        return;
    }

    if (!copy_entry(src, dir, name)) {
        term_line(st, C_ERROR, "kopiere: es ging nicht - fehlen die Rechte?");
        return;
    }

    char text[FS_PATH_MAX];

    full_path(st, to, text, sizeof(text));
    term_printf(st, C_NORMAL, "%s kopiert nach %s", src->name, text);
}

static void cmd_move(struct term_state *st, const char *from, const char *to)
{
    if (!from || !to) {
        term_line(st, C_ERROR, "verschiebe <quelle> <ziel>");
        return;
    }

    struct fs_node *src = fs_lookup(st->cwd, from);

    if (!src) {
        term_printf(st, C_ERROR, "verschiebe: \"%s\" nicht gefunden", from);
        return;
    }

    struct fs_node *target = fs_lookup(st->cwd, to);

    /* Zwei Faelle: In einen Ordner hinein, oder unter neuem Namen. */
    if (target && target->type == FS_DIR) {
        if (fs_find_child(target, src->name)) {
            term_printf(st, C_ERROR, "verschiebe: \"%s\" gibt es dort schon",
                        src->name);
            return;
        }

        /* Ueber die Dateisystemgrenze hinweg geht nur kopieren und
         * loeschen - ein Cluster auf der Platte laesst sich nicht in
         * den Arbeitsspeicher umhaengen. */
        if (src->backend != target->backend) {
            if (!copy_entry(src, target, src->name)) {
                term_line(st, C_ERROR, "verschiebe: das Kopieren scheiterte");
                return;
            }
            if (!fs_remove(src)) {
                term_line(st, C_ERROR,
                          "verschiebe: kopiert, aber das Original blieb");
                return;
            }
        } else if (!fs_move(src, target)) {
            term_line(st, C_ERROR, "verschiebe: es ging nicht");
            return;
        }
        term_printf(st, C_NORMAL, "verschoben nach %s", to);
        return;
    }

    if (target) {
        term_printf(st, C_ERROR, "verschiebe: \"%s\" gibt es schon", to);
        return;
    }

    /* Ein neuer Name im selben Ordner ist ein Umbenennen. */
    if (!strchr(to, '/')) {
        if (!fs_rename(src, to)) {
            term_line(st, C_ERROR, "verschiebe: das Umbenennen ging nicht");
            return;
        }
        term_printf(st, C_NORMAL, "umbenannt in %s", to);
        return;
    }

    term_printf(st, C_ERROR, "verschiebe: \"%s\" gibt es nicht", to);
}

/* --- Text ansehen --------------------------------------------------- */

/* Laedt eine Datei und gibt ihren Inhalt samt Laenge zurueck. */
static const char *read_file(struct term_state *st, const char *path,
                             const char *who, size_t *length)
{
    struct fs_node *f = path ? fs_lookup(st->cwd, path) : NULL;

    if (!f) {
        term_printf(st, C_ERROR, "%s: \"%s\" nicht gefunden", who,
                    path ? path : "");
        return NULL;
    }
    if (f->type != FS_FILE) {
        term_printf(st, C_ERROR, "%s: \"%s\" ist ein Ordner", who, path);
        return NULL;
    }
    if (!fs_load(f)) {
        term_printf(st, C_ERROR, "%s: \"%s\" laesst sich nicht lesen", who,
                    path);
        return NULL;
    }

    *length = f->size;
    return (const char *)(f->data ? f->data : (const uint8_t *)"");
}

/* Kopiert die n-te Zeile heraus. Liefert false hinter der letzten. */
static bool line_at(const char *text, size_t length, size_t index,
                    char *out, size_t size)
{
    size_t start = 0;
    size_t line = 0;

    for (size_t i = 0; i <= length; i++) {
        if (i < length && text[i] != '\n')
            continue;
        if (line == index) {
            size_t take = MIN(i - start, size - 1);

            memcpy(out, text + start, take);
            out[take] = '\0';
            return true;
        }
        line++;
        start = i + 1;
        if (i >= length)
            break;
    }
    return false;
}

static size_t count_lines(const char *text, size_t length)
{
    size_t n = 0;

    for (size_t i = 0; i < length; i++)
        if (text[i] == '\n')
            n++;
    /* Eine letzte Zeile ohne Umbruch zaehlt mit. */
    if (length && text[length - 1] != '\n')
        n++;
    return n;
}

/* Zahl aus "-20" oder "20"; 0 heisst "nichts angegeben". */
static size_t number_arg(const char *text)
{
    size_t value = 0;

    if (!text)
        return 0;
    if (*text == '-')
        text++;
    while (*text >= '0' && *text <= '9')
        value = value * 10 + (size_t)(*text++ - '0');
    return *text ? 0 : value;
}

static void cmd_head_tail(struct term_state *st, const char *a1,
                          const char *a2, bool tail)
{
    const char *who = tail ? "ende" : "kopf";
    size_t count = number_arg(a1);
    const char *path = count ? a2 : a1;
    size_t length = 0;

    if (!count)
        count = 10;

    const char *text = read_file(st, path, who, &length);

    if (!text)
        return;

    size_t total = count_lines(text, length);
    size_t first = tail && total > count ? total - count : 0;
    size_t last = tail ? total : MIN(count, total);
    char line[TERM_COLS];

    for (size_t i = first; i < last; i++)
        if (line_at(text, length, i, line, sizeof(line)))
            term_line(st, C_NORMAL, line);

    term_printf(st, C_HIGHLIGHT, "  %u von %u Zeilen",
                (unsigned)(last - first), (unsigned)total);
}

static void cmd_count(struct term_state *st, const char *path)
{
    size_t length = 0;
    const char *text = read_file(st, path, "zaehle", &length);

    if (!text)
        return;

    size_t words = 0;
    bool inside = false;

    for (size_t i = 0; i < length; i++) {
        bool space = text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                     text[i] == '\r';

        if (!space && !inside)
            words++;
        inside = !space;
    }

    term_printf(st, C_NORMAL, "%u Zeilen, %u Woerter, %u Zeichen  %s",
                (unsigned)count_lines(text, length), (unsigned)words,
                (unsigned)length, path);
}

static void cmd_sort(struct term_state *st, const char *path)
{
    size_t length = 0;
    const char *text = read_file(st, path, "sortiere", &length);

    if (!text)
        return;

    size_t total = count_lines(text, length);

    if (total > 200) {
        term_line(st, C_ERROR, "sortiere: hoechstens 200 Zeilen");
        return;
    }

    /* Die Zeilen werden einmal herauskopiert und dann nur noch die
     * Zeiger getauscht - so wandert kein Text hin und her. */
    char (*lines)[TERM_COLS] = kmalloc(total * TERM_COLS);

    if (!lines) {
        term_line(st, C_ERROR, "sortiere: kein Speicher");
        return;
    }

    for (size_t i = 0; i < total; i++)
        if (!line_at(text, length, i, lines[i], TERM_COLS))
            lines[i][0] = '\0';

    for (size_t i = 1; i < total; i++) {
        char key[TERM_COLS];
        size_t k = i;

        strlcpy(key, lines[i], sizeof(key));
        while (k > 0 && strcasecmp(lines[k - 1], key) > 0) {
            memcpy(lines[k], lines[k - 1], TERM_COLS);
            k--;
        }
        memcpy(lines[k], key, TERM_COLS);
    }

    for (size_t i = 0; i < total; i++)
        term_line(st, C_NORMAL, lines[i]);
    term_printf(st, C_HIGHLIGHT, "  %u Zeilen", (unsigned)total);
    kfree(lines);
}

static void cmd_diff(struct term_state *st, const char *a, const char *b)
{
    size_t la = 0, lb = 0;
    const char *ta = read_file(st, a, "vergleiche", &la);

    if (!ta)
        return;

    /* Der erste Inhalt muss herauskopiert werden: Das zweite fs_load
     * kann den Zeiger des ersten ungueltig machen. */
    char *copy = kmalloc(la + 1);

    if (!copy) {
        term_line(st, C_ERROR, "vergleiche: kein Speicher");
        return;
    }
    memcpy(copy, ta, la);
    copy[la] = '\0';

    const char *tb = read_file(st, b, "vergleiche", &lb);

    if (!tb) {
        kfree(copy);
        return;
    }

    size_t na = count_lines(copy, la);
    size_t nb = count_lines(tb, lb);
    char linea[TERM_COLS], lineb[TERM_COLS];
    size_t shown = 0;

    for (size_t i = 0; i < MAX(na, nb); i++) {
        bool ga = line_at(copy, la, i, linea, sizeof(linea));
        bool gb = line_at(tb, lb, i, lineb, sizeof(lineb));

        if (ga && gb && strcmp(linea, lineb) == 0)
            continue;

        term_printf(st, C_ERROR, "Zeile %u:", (unsigned)(i + 1));
        term_printf(st, C_NORMAL, "  < %s", ga ? linea : "(fehlt)");
        term_printf(st, C_NORMAL, "  > %s", gb ? lineb : "(fehlt)");

        if (++shown >= 10) {
            term_line(st, C_HIGHLIGHT, "  ... weitere Unterschiede");
            break;
        }
    }

    if (!shown)
        term_line(st, C_NORMAL, "Die Dateien sind gleich.");
    kfree(copy);
}

static void cmd_hex(struct term_state *st, const char *path, const char *count)
{
    size_t length = 0;
    const char *text = read_file(st, path, "hex", &length);

    if (!text)
        return;

    size_t want = number_arg(count);

    if (!want)
        want = 256;
    if (want > length)
        want = length;

    for (size_t offset = 0; offset < want; offset += 16) {
        char line[TERM_COLS];
        size_t used = 0;
        size_t take = MIN((size_t)16, want - offset);

        ksnprintf(line, sizeof(line), "%08x  ", (unsigned)offset);
        used = strlen(line);

        for (size_t i = 0; i < 16; i++) {
            if (i < take)
                ksnprintf(line + used, sizeof(line) - used, "%02x ",
                          (unsigned char)text[offset + i]);
            else
                ksnprintf(line + used, sizeof(line) - used, "   ");
            used += strlen(line + used);
            if (i == 7) {
                ksnprintf(line + used, sizeof(line) - used, " ");
                used += 1;
            }
        }

        ksnprintf(line + used, sizeof(line) - used, " |");
        used += 2;

        for (size_t i = 0; i < take && used + 2 < sizeof(line); i++) {
            unsigned char c = (unsigned char)text[offset + i];

            line[used++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[used++] = '|';
        line[used] = '\0';
        term_line(st, C_NORMAL, line);
    }

    term_printf(st, C_HIGHLIGHT, "  %u von %u Bytes", (unsigned)want,
                (unsigned)length);
}

/* --- Suchen, Baum, Groesse, Auskunft -------------------------------- */

/* Ein Zaehler, den die rekursiven Laeufe unten mitfuehren - so hoeren
 * sie auf, bevor sie die Konsole vollschreiben. */
struct walk_limit {
    size_t found;
    size_t max;
};

static void grep_file(struct term_state *st, struct fs_node *file,
                      const char *needle, struct walk_limit *limit)
{
    if (limit->found >= limit->max || !fs_is_text(file) || !fs_load(file) ||
        !file->data)
        return;

    const char *text = (const char *)file->data;
    size_t length = file->size;
    size_t total = count_lines(text, length);
    char line[TERM_COLS];
    char path[FS_PATH_MAX];

    fs_path(file, path, sizeof(path));

    size_t needle_len = strlen(needle);

    for (size_t i = 0; i < total && limit->found < limit->max; i++) {
        if (!line_at(text, length, i, line, sizeof(line)))
            break;

        bool hit = false;

        for (const char *p = line; *p && !hit; p++)
            if (strncasecmp(p, needle, needle_len) == 0)
                hit = true;
        if (!hit)
            continue;

        term_printf(st, C_NORMAL, "%s:%u: %s", path, (unsigned)(i + 1), line);
        limit->found++;
    }
}

static void grep_walk(struct term_state *st, struct fs_node *node,
                      const char *needle, struct walk_limit *limit)
{
    if (limit->found >= limit->max)
        return;

    if (node->type == FS_FILE) {
        grep_file(st, node, needle, limit);
        return;
    }

    struct fs_node *entries[128];
    size_t n = fs_list(node, entries, ARRAY_LEN(entries));

    for (size_t i = 0; i < n && limit->found < limit->max; i++)
        grep_walk(st, entries[i], needle, limit);
}

static void cmd_grep(struct term_state *st, const char *needle,
                     const char *where)
{
    if (!needle) {
        term_line(st, C_ERROR, "suche <text> [pfad]");
        return;
    }

    struct fs_node *start = where ? fs_lookup(st->cwd, where) : st->cwd;

    if (!start) {
        term_printf(st, C_ERROR, "suche: \"%s\" nicht gefunden", where);
        return;
    }

    struct walk_limit limit = { 0, 100 };

    grep_walk(st, start, needle, &limit);

    if (!limit.found)
        term_printf(st, C_NORMAL, "\"%s\" kommt darin nicht vor.", needle);
    else
        term_printf(st, C_HIGHLIGHT, "  %u Fundstellen%s",
                    (unsigned)limit.found,
                    limit.found >= limit.max ? " (abgebrochen)" : "");
}

static void find_walk(struct term_state *st, struct fs_node *node,
                      const char *pattern, struct walk_limit *limit)
{
    if (limit->found >= limit->max)
        return;

    if (sh_match(pattern, node->name)) {
        char path[FS_PATH_MAX];
        char size[24];

        fs_path(node, path, sizeof(path));
        if (node->type == FS_DIR)
            strlcpy(size, "<ORDNER>", sizeof(size));
        else
            fs_format_size(size, sizeof(size), node->size);

        term_printf(st, node->type == FS_DIR ? C_HIGHLIGHT : C_NORMAL,
                    "  %-52s %10s", path, size);
        limit->found++;
    }

    if (node->type != FS_DIR)
        return;

    struct fs_node *entries[128];
    size_t n = fs_list(node, entries, ARRAY_LEN(entries));

    for (size_t i = 0; i < n && limit->found < limit->max; i++)
        find_walk(st, entries[i], pattern, limit);
}

static void cmd_find(struct term_state *st, const char *a1, const char *a2)
{
    /* Mit einem Argument ist es das Muster, mit zweien Pfad und
     * Muster - so herum, wie man es hinschreibt. */
    const char *where = a2 ? a1 : NULL;
    const char *pattern = a2 ? a2 : a1;

    if (!pattern) {
        term_line(st, C_ERROR, "finde [pfad] <muster>");
        term_line(st, C_NORMAL, "  z.B.  finde / *.txt");
        return;
    }

    struct fs_node *start = where ? fs_lookup(st->cwd, where) : st->cwd;

    if (!start) {
        term_printf(st, C_ERROR, "finde: \"%s\" nicht gefunden", where);
        return;
    }

    struct walk_limit limit = { 0, 200 };

    find_walk(st, start, pattern, &limit);

    if (!limit.found)
        term_printf(st, C_NORMAL, "Nichts passt auf \"%s\".", pattern);
    else
        term_printf(st, C_HIGHLIGHT, "  %u gefunden%s", (unsigned)limit.found,
                    limit.found >= limit.max ? " (abgebrochen)" : "");
}

static void tree_walk(struct term_state *st, struct fs_node *dir,
                      int depth, int max_depth, const char *prefix,
                      struct walk_limit *limit)
{
    if (depth >= max_depth || limit->found >= limit->max)
        return;

    struct fs_node *entries[128];
    size_t n = fs_list(dir, entries, ARRAY_LEN(entries));

    for (size_t i = 0; i < n && limit->found < limit->max; i++) {
        struct fs_node *e = entries[i];
        bool last = i + 1 == n;
        char line[TERM_COLS];
        char next[TERM_COLS];

        ksnprintf(line, sizeof(line), "%s%s%s%s", prefix,
                  last ? "\\-- " : "|-- ", e->name,
                  e->type == FS_DIR ? "/" : "");
        term_line(st, e->type == FS_DIR ? C_HIGHLIGHT : C_NORMAL, line);
        limit->found++;

        if (e->type != FS_DIR)
            continue;

        ksnprintf(next, sizeof(next), "%s%s", prefix, last ? "    " : "|   ");
        tree_walk(st, e, depth + 1, max_depth, next, limit);
    }
}

static void cmd_tree(struct term_state *st, const char *where,
                     const char *depth_text)
{
    struct fs_node *dir = where ? fs_lookup(st->cwd, where) : st->cwd;

    if (!dir || dir->type != FS_DIR) {
        term_printf(st, C_ERROR, "baum: \"%s\" ist kein Ordner",
                    where ? where : ".");
        return;
    }

    size_t depth = number_arg(depth_text);
    char path[FS_PATH_MAX];

    fs_path(dir, path, sizeof(path));
    term_line(st, C_HIGHLIGHT, path);

    struct walk_limit limit = { 0, 300 };

    tree_walk(st, dir, 0, depth ? (int)depth : 3, "", &limit);
    term_printf(st, C_HIGHLIGHT, "  %u Eintraege%s", (unsigned)limit.found,
                limit.found >= limit.max ? " (abgebrochen)" : "");
}

static void cmd_du(struct term_state *st, const char *where)
{
    struct fs_node *dir = where ? fs_lookup(st->cwd, where) : st->cwd;

    if (!dir) {
        term_printf(st, C_ERROR, "groesse: \"%s\" nicht gefunden", where);
        return;
    }

    char size[24];

    if (dir->type != FS_DIR) {
        fs_format_size(size, sizeof(size), dir->size);
        term_printf(st, C_NORMAL, "%10s  %s", size, dir->name);
        return;
    }

    struct fs_node *entries[128];
    size_t n = fs_list(dir, entries, ARRAY_LEN(entries));

    /* Die groessten zuerst - danach sucht, wer nach Platz sucht. */
    for (size_t i = 1; i < n; i++) {
        struct fs_node *key = entries[i];
        size_t bytes = fs_total_size(key);
        size_t k = i;

        while (k > 0 && fs_total_size(entries[k - 1]) < bytes) {
            entries[k] = entries[k - 1];
            k--;
        }
        entries[k] = key;
    }

    for (size_t i = 0; i < n; i++) {
        fs_format_size(size, sizeof(size), fs_total_size(entries[i]));
        term_printf(st, entries[i]->type == FS_DIR ? C_HIGHLIGHT : C_NORMAL,
                    "%10s  %s%s", size, entries[i]->name,
                    entries[i]->type == FS_DIR ? "/" : "");
    }

    fs_format_size(size, sizeof(size), fs_total_size(dir));
    term_printf(st, C_HIGHLIGHT, "%10s  insgesamt in %s", size, dir->name);
}

static void cmd_stat(struct term_state *st, const char *path)
{
    struct fs_node *n = path ? fs_lookup(st->cwd, path) : NULL;

    if (!n) {
        term_printf(st, C_ERROR, "info: \"%s\" nicht gefunden",
                    path ? path : "");
        return;
    }

    char full[FS_PATH_MAX];
    char size[24];
    char mode[11];

    fs_path(n, full, sizeof(full));
    fs_format_size(size, sizeof(size), fs_total_size(n));
    perm_mode_text(n->mode, n->type, mode);

    term_printf(st, C_HIGHLIGHT, "%s", full);
    term_printf(st, C_NORMAL, "  Art       : %s%s",
                n->type == FS_DIR ? "Ordner" : "Datei",
                n->readonly ? ", nur lesbar" : "");
    term_printf(st, C_NORMAL, "  Groesse   : %s%s", size,
                n->type == FS_DIR ? " mit allem darin" : "");
    if (n->type == FS_DIR)
        term_printf(st, C_NORMAL, "  Eintraege : %u",
                    (unsigned)fs_child_count(n));
    term_printf(st, C_NORMAL, "  Rechte    : %s (%04o)", mode,
                (unsigned)n->mode);
    term_printf(st, C_NORMAL, "  Gehoert   : %s:%s", user_name_of(n->uid),
                group_name_of(n->gid));
    term_printf(st, C_NORMAL, "  Geaendert : %02u.%02u.%04u %02u:%02u",
                n->mtime_day, n->mtime_month, n->mtime_year,
                n->mtime_hour, n->mtime_min);
    term_printf(st, C_NORMAL, "  Liegt     : %s",
                n->backend == FS_BACKEND_FAT ? "auf der Festplatte"
                                             : "im Arbeitsspeicher");
}

static void cmd_checksum(struct term_state *st, const char *path)
{
    size_t length = 0;
    const char *text = read_file(st, path, "pruefsumme", &length);

    if (!text)
        return;

    uint8_t digest[SHA256_SIZE];

    sha256(text, length, digest);

    char hex[SHA256_SIZE * 2 + 1];
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < SHA256_SIZE; i++) {
        hex[i * 2 + 0] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    hex[SHA256_SIZE * 2] = '\0';

    term_printf(st, C_NORMAL, "%s", hex);
    term_printf(st, C_HIGHLIGHT, "  SHA-256 ueber %u Bytes von %s",
                (unsigned)length, path);
}

static void cmd_which(struct term_state *st, const char *name)
{
    if (!name) {
        term_line(st, C_ERROR, "wo <name>");
        return;
    }

    /* Erst die eingebauten Befehle - sie gehen jedem Programm vor. */
    const struct sh_command *cmd = sh_command_find(name);

    if (cmd) {
        term_printf(st, C_HIGHLIGHT, "%s ist ein eingebauter Befehl: %s",
                    cmd->name, cmd->what);
        if (cmd->alias)
            term_printf(st, C_NORMAL, "  Auch als \"%s\"", cmd->alias);
        return;
    }

    char full[FS_PATH_MAX];

    if (strchr(name, '/'))
        strlcpy(full, name, sizeof(full));
    else if (strchr(name, '.'))
        ksnprintf(full, sizeof(full), "/Programme/%s", name);
    else
        ksnprintf(full, sizeof(full), "/Programme/%s.elf", name);

    struct fs_node *node = fs_lookup(st->cwd, full);

    if (!node) {
        term_printf(st, C_ERROR, "\"%s\" ist weder Befehl noch Programm",
                    name);
        return;
    }

    char size[24];

    fs_format_size(size, sizeof(size), node->size);
    term_printf(st, C_NORMAL, "%s  (%s)", full, size);
}

static void cmd_kill(struct term_state *st, const char *text)
{
    size_t pid = number_arg(text);

    if (!pid) {
        term_line(st, C_ERROR, "beende <nummer>");
        return;
    }

    for (size_t i = 0; i < process_count(); i++) {
        struct process *p = process_at(i);

        if (!p || p->pid != (uint32_t)pid)
            continue;
        if (p->finished) {
            term_printf(st, C_ERROR, "%s laeuft schon nicht mehr.", p->name);
            return;
        }

        /* Ein fremdes Programm abzuschiessen ist Sache des Verwalters. */
        if (p->uid != session_uid() && !session_is_admin()) {
            term_printf(st, C_ERROR,
                        "%s gehoert %s - das darf nur ein Verwalter beenden.",
                        p->name, user_name_of(p->uid));
            return;
        }

        char name[32];

        strlcpy(name, p->name, sizeof(name));
        audit(AUDIT_PROCESS, true, "%s (%u) ueber die Konsole beendet", name,
              (unsigned)pid);
        process_kill(p);
        if (st->running && st->running->pid == (uint32_t)pid)
            st->running = NULL;
        term_printf(st, C_NORMAL, "%s beendet.", name);
        return;
    }

    term_printf(st, C_ERROR, "Es laeuft kein Programm mit der Nummer %u",
                (unsigned)pid);
}

static void cmd_calendar(struct term_state *st, const char *a1, const char *a2)
{
    struct datetime now;

    rtc_read(&now);

    size_t month = number_arg(a1);
    size_t year = number_arg(a2);

    /* Ein einzelnes grosses Argument ist ein Jahr und kein Monat. */
    if (month > 12 && !year) {
        year = month;
        month = now.month;
    }
    if (!month)
        month = now.month;
    if (!year)
        year = now.year;

    if (month < 1 || month > 12 || year < 1970 || year > 2999) {
        term_line(st, C_ERROR, "kalender [monat] [jahr]");
        return;
    }

    char text[512];

    sh_calendar((uint16_t)year, (uint8_t)month,
                (year == now.year && month == now.month) ? now.day : 0,
                text, sizeof(text));

    char line[TERM_COLS];
    size_t length = strlen(text);

    for (size_t i = 0; i < count_lines(text, length); i++)
        if (line_at(text, length, i, line, sizeof(line)))
            term_line(st, i < 2 ? C_HIGHLIGHT : C_NORMAL, line);
}

static void cmd_expr(struct term_state *st, const char *text)
{
    int64_t value = 0;
    char error[80];

    if (!text || !text[0]) {
        term_line(st, C_ERROR, "rechne <ausdruck>");
        term_line(st, C_NORMAL, "  z.B.  rechne (3 + 4) * 1024");
        return;
    }

    if (!sh_eval(text, &value, error, sizeof(error))) {
        term_printf(st, C_ERROR, "rechne: %s", error);
        return;
    }

    term_printf(st, C_NORMAL, "%s = %ld", text, (long)value);
}

/* --- Hilfe --------------------------------------------------------- */

static void cmd_help(struct term_state *st, const char *group)
{
    if (group && group[0]) {
        const struct sh_command *one = sh_command_find(group);

        /* "hilfe ls" ist so naheliegend, dass es dasselbe tun soll wie
         * "man ls" - niemand soll erst lernen, welches Wort er braucht. */
        if (one) {
            term_printf(st, C_HIGHLIGHT, "%s - %s", one->usage, one->what);
            if (one->alias)
                term_printf(st, C_NORMAL, "  Auch als \"%s\".", one->alias);
            if (one->detail) {
                char line[TERM_COLS];
                size_t length = strlen(one->detail);

                for (size_t i = 0; i < count_lines(one->detail, length); i++)
                    if (line_at(one->detail, length, i, line, sizeof(line)))
                        term_printf(st, C_NORMAL, "  %s", line);
            }
            return;
        }
    }

    size_t shown = 0;

    for (size_t g = 0; g < sh_group_count(); g++) {
        const char *name = sh_group_at(g);

        if (group && group[0] && strcasecmp(group, name) != 0)
            continue;

        term_printf(st, C_HIGHLIGHT, "%s:", name);

        for (size_t i = 0; i < sh_command_count(); i++) {
            const struct sh_command *c = sh_command_at(i);

            if (strcmp(c->group, name) != 0)
                continue;
            term_printf(st, C_NORMAL, "  %-42s %s", c->usage, c->what);
            shown++;
        }
    }

    if (!shown) {
        term_printf(st, C_ERROR, "\"%s\" ist weder Befehl noch Bereich.",
                    group);
        term_line(st, C_NORMAL,
                  "Bereiche: Dateien, Text, Programme, System, Netz, "
                  "Sicherheit");
        return;
    }

    if (!group || !group[0])
        term_line(st, C_NORMAL,
                  "\"hilfe <bereich>\" zeigt nur einen Teil, "
                  "\"man <befehl>\" alles zu einem Befehl.");
}

/* --- Paketfilter und Pruefspur ------------------------------------- */

static void cmd_firewall(struct term_state *st, int argc, char *argv[])
{
    const char *what = argc > 1 ? argv[1] : NULL;

    if (!what) {
        char text[96];

        term_printf(st, C_HIGHLIGHT, "Paketfilter: %s",
                    fw_enabled() ? "eingeschaltet" : "ausgeschaltet");
        term_printf(st, C_NORMAL, "  Grundeinstellung eingehend: %s",
                    fw_action_name(fw_policy(FW_IN)));
        term_printf(st, C_NORMAL, "  Grundeinstellung ausgehend: %s",
                    fw_action_name(fw_policy(FW_OUT)));

        size_t shown = 0;

        for (size_t i = 0; i < FW_RULES_MAX; i++) {
            const struct fw_rule *r = fw_at(i);

            if (!r)
                continue;
            fw_rule_text(r, text, sizeof(text));
            term_printf(st, C_NORMAL, "  %2u  %s  (%u Treffer)",
                        (unsigned)i, text, (unsigned)r->hits);
            shown++;
        }
        if (!shown)
            term_line(st, C_NORMAL, "  Keine Regeln - es gilt die "
                                    "Grundeinstellung.");

        term_printf(st, C_NORMAL,
                    "  Durchgelassen: %u ein, %u aus; verworfen: %u ein, %u aus",
                    (unsigned)fw_passed(FW_IN), (unsigned)fw_passed(FW_OUT),
                    (unsigned)fw_dropped(FW_IN), (unsigned)fw_dropped(FW_OUT));
        term_line(st, C_NORMAL,
                  "  firewall an|aus | standard <richtung> <tat> | "
                  "regel <richtung> <tat> <protokoll> <ziel> <ports> | "
                  "weg <n> | leeren | speichern");
        return;
    }

    if (!session_can(CAP_NET)) {
        term_line(st, C_ERROR, "Dafuer braucht es das Recht am Netz.");
        return;
    }

    if (!strcasecmp(what, "an") || !strcasecmp(what, "aus")) {
        fw_enable(!strcasecmp(what, "an"));
        term_printf(st, C_NORMAL, "Paketfilter %s.",
                    fw_enabled() ? "eingeschaltet" : "ausgeschaltet");
    } else if (!strcasecmp(what, "standard")) {
        enum fw_action action;

        if (argc < 4 || !fw_parse_action(argv[3], &action)) {
            term_line(st, C_ERROR,
                      "firewall standard eingehend|ausgehend "
                      "erlauben|verwerfen|ablehnen");
            return;
        }
        enum fw_dir dir = !strcasecmp(argv[2], "ausgehend") ? FW_OUT : FW_IN;

        fw_set_policy(dir, action);
        term_printf(st, C_NORMAL, "Grundeinstellung %s: %s", argv[2],
                    fw_action_name(action));
    } else if (!strcasecmp(what, "regel")) {
        enum fw_action action;
        uint8_t   proto, prefix;
        ip_addr_t addr;
        uint16_t  lo, hi;

        if (argc < 7 || !fw_parse_action(argv[3], &action) ||
            !fw_parse_proto(argv[4], &proto) ||
            !fw_parse_target(argv[5], &addr, &prefix) ||
            !fw_parse_ports(argv[6], &lo, &hi)) {
            term_line(st, C_ERROR,
                      "firewall regel <eingehend|ausgehend> "
                      "<erlauben|verwerfen|ablehnen> <alle|tcp|udp|icmp> "
                      "<alle|adresse[/bits]> <alle|port[-port]>");
            return;
        }

        enum fw_dir dir = !strcasecmp(argv[2], "ausgehend") ? FW_OUT : FW_IN;
        int index = fw_add(dir, action, proto, addr, prefix, lo, hi,
                           argc > 7 ? argv[7] : NULL);

        if (index < 0) {
            term_line(st, C_ERROR, "Es ist kein Platz fuer weitere Regeln.");
            return;
        }
        term_printf(st, C_NORMAL, "Regel %d angelegt.", index);
    } else if (!strcasecmp(what, "weg")) {
        size_t index = 0;

        for (const char *p = argc > 2 ? argv[2] : ""; *p >= '0' && *p <= '9'; p++)
            index = index * 10 + (size_t)(*p - '0');
        if (!fw_remove(index)) {
            term_line(st, C_ERROR, "Diese Regel gibt es nicht.");
            return;
        }
        term_printf(st, C_NORMAL, "Regel %u entfernt.", (unsigned)index);
    } else if (!strcasecmp(what, "leeren")) {
        fw_clear();
        term_line(st, C_NORMAL, "Alle Regeln entfernt.");
    } else if (!strcasecmp(what, "speichern")) {
        bool ok = fw_save();

        term_line(st, ok ? C_NORMAL : C_ERROR,
                  ok ? "Gespeichert in " FW_PATH
                     : "Ohne Festplatte laesst sich nichts sichern.");
        return;
    } else {
        term_printf(st, C_ERROR, "Unbekannt: %s", what);
        return;
    }

    if (fs_disk_mounted() && fw_save())
        term_line(st, C_NORMAL, "Gespeichert in " FW_PATH);
}

static void cmd_audit(struct term_state *st, const char *what)
{
    if (!audit_readable()) {
        term_line(st, C_ERROR,
                  "Die Pruefspur lesen darf nur, wer das Recht am "
                  "Protokoll hat.");
        return;
    }

    bool only_failed = what && !strcasecmp(what, "abgewiesen");

    if (what && !strcasecmp(what, "speichern")) {
        bool ok = audit_save();

        term_line(st, ok ? C_NORMAL : C_ERROR,
                  ok ? "Fortgeschrieben in " AUDIT_PATH
                     : "Ohne Festplatte laesst sich nichts sichern.");
        return;
    }

    size_t count = audit_count();
    size_t first = 0;

    /* Ohne Angabe die letzten zwanzig - alles andere passt nicht in
     * ein Konsolenfenster. */
    if (!what && count > 20)
        first = count - 20;

    struct audit_entry e;
    size_t shown = 0;

    term_line(st, C_HIGHLIGHT,
              "Zeit        Art        Ausgang     Benutzer   Gegenstand");

    for (size_t i = first; i < count; i++) {
        if (!audit_get(i, &e))
            break;
        if (only_failed && e.ok)
            continue;
        term_printf(st, e.ok ? C_NORMAL : C_ERROR,
                    "%5u.%03u  %-10s %-11s %-10s %s",
                    (unsigned)(e.ms / 1000), (unsigned)(e.ms % 1000),
                    audit_kind_name(e.kind),
                    e.ok ? "erlaubt" : "abgewiesen",
                    user_name_of(e.uid), e.object);
        shown++;
    }

    term_printf(st, C_HIGHLIGHT,
                "%u von %u Eintraegen, davon %u abgewiesen%s",
                (unsigned)shown, (unsigned)count,
                (unsigned)audit_count_failed(),
                audit_lost() ? " (aeltere sind aus dem Ring gefallen)" : "");
    term_line(st, C_NORMAL, "  pruefspur [alle|abgewiesen|speichern]");
}

/* --- Protokoll und Aufgaben ---------------------------------------- */

static void cmd_log(struct term_state *st, const char *what)
{
    if (what && !strcasecmp(what, "leeren")) {
        if (!session_can(CAP_LOG)) {
            term_line(st, C_ERROR,
                      "Leeren darf nur, wer das Recht am Protokoll hat.");
            return;
        }
        log_clear();
        term_line(st, C_NORMAL, "Protokoll geleert.");
        return;
    }

    if (what && !strcasecmp(what, "speichern")) {
        char path[FS_PATH_MAX];

        user_home_file("protokoll.txt", LOG_PATH_DEFAULT, path, sizeof(path));

        if (log_save(path))
            term_printf(st, C_NORMAL, "Gesichert in %s", path);
        else
            term_printf(st, C_ERROR, "%s liess sich nicht schreiben", path);
        return;
    }

    /* Ohne Angabe die letzten zwanzig, sonst alles ab dieser
     * Dringlichkeit - eine Konsole mit fuenfhundert Zeilen Protokoll
     * hilft niemandem. */
    enum log_level from = LOG_DEBUG;
    bool tail = true;

    if (what && !strcasecmp(what, "warnung")) { from = LOG_WARN;  tail = false; }
    else if (what && !strcasecmp(what, "fehler")) { from = LOG_ERROR; tail = false; }
    else if (what && !strcasecmp(what, "alle"))   { tail = false; }
    else if (what && what[0]) {
        term_line(st, C_ERROR,
                  "protokoll [alle|warnung|fehler|speichern|leeren]");
        return;
    }

    size_t count = log_count();
    size_t first = 0;

    if (tail && count > 20)
        first = count - 20;

    struct log_entry e;
    size_t shown = 0;

    for (size_t i = first; i < count; i++) {
        if (!log_get(i, &e) || e.level < (uint8_t)from)
            continue;
        term_printf(st, e.level >= LOG_WARN ? C_ERROR : C_NORMAL,
                    "[%5u.%03u] %s %-11s %s",
                    (unsigned)(e.ms / 1000), (unsigned)(e.ms % 1000),
                    log_level_short(e.level), e.source, e.text);
        shown++;
    }

    term_printf(st, C_HIGHLIGHT,
                "%u von %u Meldungen - %u Warnungen, %u Fehler",
                (unsigned)shown, (unsigned)count,
                (unsigned)log_count_level(LOG_WARN),
                (unsigned)log_count_level(LOG_ERROR));
    if (log_lost())
        term_printf(st, C_NORMAL, "%u aeltere sind aus dem Ring gefallen.",
                    (unsigned)log_lost());
}

/* Die Aufgabenliste des angemeldeten Benutzers. Sie wird bei jedem
 * Befehl frisch gelesen und gleich wieder geschrieben - so sind Konsole
 * und Fenster nie verschiedener Meinung. */
static void task_path(char *out, size_t size)
{
    user_home_file("Aufgaben.txt", "/Dokumente/Aufgaben.txt", out, size);
}

static bool tasks_read(struct tasklist *list, char *path, size_t size)
{
    task_path(path, size);
    tasks_clear(list);

    struct fs_node *file = fs_lookup(NULL, path);

    if (!file || file->type != FS_FILE || !fs_load(file) || !file->data)
        return false;

    char *text = kmalloc(file->size + 1);

    if (!text)
        return false;

    memcpy(text, file->data, file->size);
    text[file->size] = '\0';
    tasks_from_text(list, text);
    kfree(text);
    return true;
}

static bool tasks_store(struct tasklist *list, const char *path)
{
    size_t cap = TASK_MAX * (TASK_TEXT_MAX + 48) + 256;
    char  *text = kmalloc(cap);

    if (!text)
        return false;

    size_t used = tasks_to_text(list, text, cap);
    struct fs_node *file = fs_lookup(NULL, path);

    if (!file)
        file = fs_create_path(NULL, path, FS_FILE);

    bool ok = file && file->type == FS_FILE && fs_write(file, text, used);

    kfree(text);
    return ok;
}

/* Zwei Reste, weil die Befehle verschieden gebaut sind: "neu" nimmt
 * alles ab dem dritten Wort, "wichtig" und "termin" alles ab dem
 * vierten - dazwischen steht bei ihnen die Nummer. */
static void cmd_tasks(struct term_state *st, const char *what,
                      const char *arg, const char *text_rest,
                      const char *value_rest)
{
    struct tasklist *list = kzalloc(sizeof(*list));
    char path[FS_PATH_MAX];

    if (!list) {
        term_line(st, C_ERROR, "Kein Speicher.");
        return;
    }

    tasks_read(list, path, sizeof(path));

    struct task *view[TASK_MAX];
    size_t n = tasks_sorted(list, view, ARRAY_LEN(view), false);
    bool   dirty = false;

    /* Die Nummer, die der Benutzer eintippt, ist die Zeilennummer der
     * Anzeige - nicht die innere Kennung. Alles andere waere fuer eine
     * Liste, die man ansieht und dann anfasst, unbrauchbar. */
    int index = 0;

    for (const char *p = arg; p && *p >= '0' && *p <= '9'; p++)
        index = index * 10 + (*p - '0');

    struct task *chosen = (index >= 1 && (size_t)index <= n)
                        ? view[index - 1] : NULL;

    if (!what || !what[0]) {
        if (!n) {
            term_line(st, C_NORMAL, "Nichts zu tun.");
        } else {
            term_line(st, C_HIGHLIGHT, "Aufgaben:");
            for (size_t i = 0; i < n; i++) {
                char date[16];

                tasks_format_date(view[i], date, sizeof(date));
                term_printf(st, view[i]->done ? C_NORMAL : C_HIGHLIGHT,
                            "  %2u [%c] %-8s %-11s %s", (unsigned)(i + 1),
                            view[i]->done ? 'x' : ' ',
                            task_prio_name(view[i]->prio), date,
                            view[i]->text);
            }
        }
        term_printf(st, C_NORMAL, "  %u offen von %u - %s",
                    (unsigned)tasks_count(list, true),
                    (unsigned)tasks_count(list, false), path);
        term_line(st, C_NORMAL,
                  "  aufgaben neu <text> | fertig <n> | weg <n> | "
                  "wichtig <n> <stufe> | termin <n> <datum>");
        kfree(list);
        return;
    }

    if (!strcasecmp(what, "neu")) {
        const char *text = text_rest;

        if (!text || !text[0]) {
            term_line(st, C_ERROR, "aufgaben neu <text>");
            kfree(list);
            return;
        }
        if (!tasks_add(list, text)) {
            term_line(st, C_ERROR, "Die Liste ist voll.");
            kfree(list);
            return;
        }
        term_printf(st, C_NORMAL, "Notiert: %s", text);
        dirty = true;
    } else if (!chosen) {
        term_line(st, C_ERROR, "Welche Aufgabe? Nummer aus der Liste angeben.");
        kfree(list);
        return;
    } else if (!strcasecmp(what, "fertig")) {
        chosen->done = !chosen->done;
        term_printf(st, C_NORMAL, "%s: %s", chosen->done ? "Erledigt"
                                                         : "Wieder offen",
                    chosen->text);
        dirty = true;
    } else if (!strcasecmp(what, "weg")) {
        char name[TASK_TEXT_MAX + 1];

        strlcpy(name, chosen->text, sizeof(name));
        tasks_remove(list, chosen->id);
        term_printf(st, C_NORMAL, "Geloescht: %s", name);
        dirty = true;
    } else if (!strcasecmp(what, "wichtig")) {
        uint8_t prio;

        if (!task_prio_parse(value_rest, &prio)) {
            term_line(st, C_ERROR, "hoch, mittel oder niedrig");
            kfree(list);
            return;
        }
        chosen->prio = prio;
        term_printf(st, C_NORMAL, "%s ist jetzt %s", chosen->text,
                    task_prio_name(prio));
        dirty = true;
    } else if (!strcasecmp(what, "termin")) {
        uint16_t year;
        uint8_t  month, day;

        if (!tasks_parse_date(value_rest, &year, &month, &day)) {
            term_line(st, C_ERROR, "TT.MM.JJJJ oder ein Strich");
            kfree(list);
            return;
        }
        chosen->year = year;
        chosen->month = month;
        chosen->day = day;

        char date[16];

        tasks_format_date(chosen, date, sizeof(date));
        term_printf(st, C_NORMAL, "Termin von %s: %s", chosen->text, date);
        dirty = true;
    } else {
        term_printf(st, C_ERROR, "Unbekannt: %s", what);
        kfree(list);
        return;
    }

    if (dirty && !tasks_store(list, path))
        term_printf(st, C_ERROR, "%s liess sich nicht schreiben", path);

    kfree(list);
}

/* --- Benutzer und Rechte ------------------------------------------- */

static void cmd_who(struct term_state *st)
{
    struct user *u = session_user();

    if (!u) {
        term_line(st, C_NORMAL, "Niemand angemeldet - alles laeuft als root.");
        return;
    }

    term_printf(st, C_HIGHLIGHT, "%s (%s)", u->name, u->full);
    term_printf(st, C_NORMAL, "  Nummer    : %u", (unsigned)u->uid);
    term_printf(st, C_NORMAL, "  Heim      : %s", u->home);
    char rechte[80];

    caps_text(u->caps, rechte, sizeof(rechte));
    term_printf(st, C_NORMAL, "  Rolle     : %s",
                u->role[0] ? u->role : caps_role(u->caps));
    term_printf(st, C_NORMAL, "  Darf      : %s", rechte);

    char line[128];
    size_t used = 0;

    for (size_t i = 0; i < group_count(); i++) {
        struct group *g = group_at(i);

        if (!user_in_group(u->uid, g->gid))
            continue;
        ksnprintf(line + used, sizeof(line) - used, "%s%s",
                  used ? ", " : "", g->name);
        used += strlen(line + used);
    }
    term_printf(st, C_NORMAL, "  Gruppen   : %s", used ? line : "keine");
}

static void cmd_groups(struct term_state *st)
{
    term_line(st, C_HIGHLIGHT, "Gruppen:");
    for (size_t i = 0; i < group_count(); i++) {
        struct group *g = group_at(i);
        char line[160];
        size_t used = 0;

        line[0] = '\0';
        for (size_t m = 0; m < g->members; m++) {
            ksnprintf(line + used, sizeof(line) - used, "%s%s",
                      used ? ", " : "", user_name_of(g->member[m]));
            used += strlen(line + used);
        }
        term_printf(st, C_NORMAL, "  %-12s %4u  %s", g->name, (unsigned)g->gid,
                    used ? line : "-");
    }
}

static void cmd_users(struct term_state *st, const char *what,
                      const char *name, const char *extra)
{
    if (!what || !what[0]) {
        term_line(st, C_HIGHLIGHT, "Benutzer:");
        for (size_t i = 0; i < user_count(); i++) {
            struct user *u = user_at(i);

            term_printf(st, C_NORMAL, "  %-14s %5u %-10s %-22s %s",
                        u->name, (unsigned)u->uid, group_name_of(u->gid),
                        u->home,
                        u->locked ? "gesperrt"
                                  : (user_is_admin(u) ? "Verwalter"
                                              : (u->nopass ? "ohne Passwort"
                                                           : "")));
        }
        term_line(st, C_NORMAL,
                  "  benutzer neu|loeschen|passwort|rolle|verwalter "
                  "<name> [wert]");
        return;
    }

    if (!session_can(CAP_USERS)) {
        term_line(st, C_ERROR, "Dafuer braucht es das Recht an den Konten.");
        return;
    }
    if (!name || !name[0]) {
        term_line(st, C_ERROR, "Es fehlt der Name.");
        return;
    }

    char error[96] = "";

    if (!strcasecmp(what, "neu")) {
        struct user *u = user_create(name, name, extra, false,
                                     error, sizeof(error));

        if (!u) {
            term_printf(st, C_ERROR, "%s", error);
            return;
        }
        user_ensure_home(u);
        term_printf(st, C_NORMAL, "%s angelegt, Nummer %u, Heim %s",
                    u->name, (unsigned)u->uid, u->home);
    } else if (!strcasecmp(what, "loeschen")) {
        struct user *u = user_by_name(name);

        if (!user_delete(u, error, sizeof(error))) {
            term_printf(st, C_ERROR, "%s", error);
            return;
        }
        term_printf(st, C_NORMAL, "%s entfernt.", name);
    } else if (!strcasecmp(what, "passwort")) {
        struct user *u = user_by_name(name);

        if (!u) {
            term_printf(st, C_ERROR, "Unbekannter Benutzer: %s", name);
            return;
        }
        user_set_password(u, extra);
        term_printf(st, C_NORMAL, "Passwort von %s gesetzt.", u->name);
    } else if (!strcasecmp(what, "rolle")) {
        struct user *u = user_by_name(name);

        if (!u) {
            term_printf(st, C_ERROR, "Unbekannter Benutzer: %s", name);
            return;
        }
        if (!extra || !user_set_role(u, extra)) {
            char liste[96];
            size_t used = 0;

            liste[0] = '\0';
            for (size_t i = 0; i < role_count(); i++) {
                ksnprintf(liste + used, sizeof(liste) - used, "%s%s",
                          used ? ", " : "", role_name(i));
                used += strlen(liste + used);
            }
            term_printf(st, C_ERROR, "Bekannte Rollen: %s", liste);
            return;
        }
        if (user_is_admin(u))
            group_add_member(group_by_gid(GID_ROOT), u->uid);
        else
            group_remove_member(group_by_gid(GID_ROOT), u->uid);

        char rechte[80];

        caps_text(u->caps, rechte, sizeof(rechte));
        term_printf(st, C_NORMAL, "%s hat jetzt die Rolle %s und darf %s.",
                    u->name, u->role, rechte);
    } else if (!strcasecmp(what, "verwalter")) {
        struct user *u = user_by_name(name);

        if (!u) {
            term_printf(st, C_ERROR, "Unbekannter Benutzer: %s", name);
            return;
        }
        user_set_role(u, user_is_admin(u) ? "benutzer" : "verwalter");
        if (user_is_admin(u))
            group_add_member(group_by_gid(GID_ROOT), u->uid);
        else
            group_remove_member(group_by_gid(GID_ROOT), u->uid);
        term_printf(st, C_NORMAL, "%s ist %s Verwalter.", u->name,
                    user_is_admin(u) ? "jetzt" : "nicht mehr");
    } else {
        term_printf(st, C_ERROR, "Unbekannt: %s", what);
        return;
    }

    if (fs_disk_mounted() && user_save())
        term_line(st, C_NORMAL, "Gespeichert in " USER_PATH);
    else if (!fs_disk_mounted())
        term_line(st, C_NORMAL, "Ohne Festplatte gilt das bis zum Ausschalten.");
}

static void cmd_mode(struct term_state *st, const char *path, const char *mode)
{
    struct fs_node *n = path ? fs_lookup(st->cwd, path) : NULL;

    if (!n) {
        term_printf(st, C_ERROR, "rechte: \"%s\" nicht gefunden",
                    path ? path : "");
        return;
    }

    char text[11];

    if (!mode || !mode[0]) {
        perm_mode_text(n->mode, n->type, text);
        term_printf(st, C_NORMAL, "%s %s %s  %s  (%04o)", text,
                    user_name_of(n->uid), group_name_of(n->gid), n->name,
                    (unsigned)n->mode);
        return;
    }

    uint16_t want;

    if (!perm_parse_mode(mode, &want)) {
        term_line(st, C_ERROR, "rechte: \"750\" oder \"rwxr-x---\" wird erwartet");
        return;
    }
    if (!perm_set_mode(n, want)) {
        term_line(st, C_ERROR, "Das darf nur der Eigentuemer oder ein Verwalter.");
        return;
    }

    perm_mode_text(n->mode, n->type, text);
    term_printf(st, C_NORMAL, "%s  %s", text, n->name);
    if (perm_store_dirty())
        perm_store_save();
}

static void cmd_owner(struct term_state *st, const char *path, const char *who)
{
    struct fs_node *n = path ? fs_lookup(st->cwd, path) : NULL;

    if (!n) {
        term_printf(st, C_ERROR, "besitzer: \"%s\" nicht gefunden",
                    path ? path : "");
        return;
    }
    if (!who || !who[0]) {
        term_printf(st, C_NORMAL, "%s gehoert %s:%s", n->name,
                    user_name_of(n->uid), group_name_of(n->gid));
        return;
    }

    /* "name" oder "name:gruppe" */
    char        buffer[64];
    const char *group_name = NULL;

    strlcpy(buffer, who, sizeof(buffer));

    char *colon = strchr(buffer, ':');

    if (colon) {
        *colon = '\0';
        group_name = colon + 1;
    }

    struct user *u = user_by_name(buffer);

    if (!u) {
        term_printf(st, C_ERROR, "Unbekannter Benutzer: %s", buffer);
        return;
    }

    uint32_t gid = u->gid;

    if (group_name && group_name[0]) {
        struct group *g = group_by_name(group_name);

        if (!g) {
            term_printf(st, C_ERROR, "Unbekannte Gruppe: %s", group_name);
            return;
        }
        gid = g->gid;
    }

    if (!perm_set_owner(n, u->uid, gid)) {
        term_line(st, C_ERROR, "Verschenken darf nur ein Verwalter.");
        return;
    }

    term_printf(st, C_NORMAL, "%s gehoert jetzt %s:%s", n->name, u->name,
                group_name_of(gid));
    if (perm_store_dirty())
        perm_store_save();
}

/* Gibt die Zeile ab dem n-ten Wort zurueck - fuer Argumente, in denen
 * Leerzeichen stehen duerfen. */
static const char *rest_of(const char *raw, int skip)
{
    const char *p = raw;

    for (int i = 0; i < skip && *p; i++) {
        while (*p == ' ')
            p++;
        while (*p && *p != ' ')
            p++;
    }
    while (*p == ' ')
        p++;
    return p;
}

/* Ohne Argument nur die Liste, mit Argument gleich umschalten. Der
 * Name darf Leerzeichen haben ("Ubuntu Mono"), darum kommt hier die
 * ganze restliche Zeile an und nicht nur das erste Wort. */
static void cmd_font(struct term_state *st, const char *name)
{
    if (name && name[0]) {
        if (!font_select_by_name(name)) {
            term_printf(st, C_ERROR, "Unbekannte Schrift: %s", name);
            return;
        }
        strlcpy(config_current()->font, font_name(font_current()),
                sizeof(config_current()->font));
        term_printf(st, C_NORMAL, "Schrift: %s", font_name(font_current()));
        term_line(st, C_NORMAL,
                  "Dauerhaft wird das erst mit Speichern in den Einstellungen.");
        gui_invalidate();
        return;
    }

    term_line(st, C_HIGHLIGHT, "Schriftarten:");
    for (size_t i = 0; i < font_count(); i++)
        term_printf(st, C_NORMAL, "  %c %-18s %s",
                    i == font_current() ? '*' : ' ',
                    font_name(i), font_license(i));
}

static void cmd_threads(struct term_state *st)
{
    term_printf(st, C_HIGHLIGHT, "%-4s %-16s %-10s %-6s %-8s %s",
                "Nr.", "Name", "Zustand", "Wicht.", "Laeufe", "Kern");

    for (size_t i = 0; i < thread_count(); i++) {
        struct thread *t = thread_at(i);

        if (!t)
            continue;

        term_printf(st, t == thread_current() ? C_HIGHLIGHT : C_NORMAL,
                    "%-4u %-16s %-10s %-6u %-8u %u",
                    (unsigned)t->id, t->name, thread_state_name(t->state),
                    (unsigned)t->priority, (unsigned)t->cpu_ticks,
                    (unsigned)t->last_cpu);
    }

    if (cpu_count() > 1) {
        term_line(st, C_NORMAL, "");
        for (uint32_t i = 0; i < cpu_count(); i++) {
            struct cpu *c = cpu_at(i);

            if (c)
                term_printf(st, C_NORMAL,
                            "Kern %u: %s, %u Takte, %u Wechsel", (unsigned)i,
                            c->current ? c->current->name : "-",
                            (unsigned)c->ticks, (unsigned)c->switches);
        }
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
    const char *a2  = argc > 2 ? argv[2] : NULL;

    if (!strcasecmp(cmd, "hilfe") || !strcasecmp(cmd, "help") ||
        !strcmp(cmd, "?")) {
        cmd_help(st, a1);

    } else if (!strcasecmp(cmd, "ls") || !strcasecmp(cmd, "dir")) {
        bool detail = a1 && !strcmp(a1, "-l");

        cmd_ls(st, detail ? a2 : a1, detail);

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

        if (!f && a1)
            f = fs_create_path(st->cwd, a1, FS_FILE);

        if (!a1) {
            term_line(st, C_ERROR, "schreib: <datei> <text>");
        } else if (!f) {
            term_printf(st, C_ERROR, "schreib: \"%s\" laesst sich nicht "
                                     "anlegen - fehlen die Rechte?", a1);
        } else if (f->type != FS_FILE) {
            term_printf(st, C_ERROR, "schreib: \"%s\" ist ein Ordner", a1);
        } else if (!perm_may(f, P_W)) {
            term_printf(st, C_ERROR, "schreib: \"%s\" darfst du nicht "
                                     "beschreiben", a1);
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

        if (!n) {
            term_printf(st, C_ERROR, "rm: \"%s\" nicht gefunden", a1 ? a1 : "");
        } else if (trash_contains(n)) {
            /* Aus dem Korb heraus gibt es kein weiteres Zurueck. */
            if (trash_purge(n))
                term_printf(st, C_NORMAL, "\"%s\" endgueltig geloescht", a1);
            else
                term_printf(st, C_ERROR, "rm: \"%s\" ist geschuetzt", a1);
        } else if (trash_delete(n)) {
            term_printf(st, C_NORMAL,
                        "\"%s\" liegt im Papierkorb - \"papierkorb zurueck\" "
                        "holt es wieder", a1);
        } else {
            term_printf(st, C_ERROR, "rm: \"%s\" ist geschuetzt", a1);
        }

    } else if (!strcasecmp(cmd, "papierkorb")) {
        cmd_trash(st, a1, a2);

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
        cmd_start_program(win, st, a1, raw, NULL, 2);

    } else if (!strcasecmp(cmd, "programme")) {
        cmd_ls(st, "/Programme", false);

    } else if (!strcasecmp(cmd, "man")) {
        const struct sh_command *one = a1 ? sh_command_find(a1) : NULL;

        if (!a1)
            term_line(st, C_ERROR, "man <befehl>");
        else if (!one)
            term_printf(st, C_ERROR, "\"%s\" kennt die Konsole nicht.", a1);
        else
            cmd_help(st, one->name);

    } else if (!strcasecmp(cmd, "kopiere") || !strcasecmp(cmd, "cp")) {
        cmd_copy(st, a1, a2);

    } else if (!strcasecmp(cmd, "verschiebe") || !strcasecmp(cmd, "mv")) {
        cmd_move(st, a1, a2);

    } else if (!strcasecmp(cmd, "kopf") || !strcasecmp(cmd, "head")) {
        cmd_head_tail(st, a1, a2, false);

    } else if (!strcasecmp(cmd, "ende") || !strcasecmp(cmd, "tail")) {
        cmd_head_tail(st, a1, a2, true);

    } else if (!strcasecmp(cmd, "zaehle") || !strcasecmp(cmd, "wc")) {
        cmd_count(st, a1);

    } else if (!strcasecmp(cmd, "sortiere") || !strcasecmp(cmd, "sort")) {
        cmd_sort(st, a1);

    } else if (!strcasecmp(cmd, "vergleiche") || !strcasecmp(cmd, "diff")) {
        cmd_diff(st, a1, a2);

    } else if (!strcasecmp(cmd, "hex")) {
        cmd_hex(st, a1, a2);

    } else if (!strcasecmp(cmd, "suche") || !strcasecmp(cmd, "grep")) {
        cmd_grep(st, a1, a2);

    } else if (!strcasecmp(cmd, "finde") || !strcasecmp(cmd, "find")) {
        cmd_find(st, a1, a2);

    } else if (!strcasecmp(cmd, "baum") || !strcasecmp(cmd, "tree")) {
        cmd_tree(st, a1, a2);

    } else if (!strcasecmp(cmd, "groesse") || !strcasecmp(cmd, "du")) {
        cmd_du(st, a1);

    } else if (!strcasecmp(cmd, "info") || !strcasecmp(cmd, "stat")) {
        cmd_stat(st, a1);

    } else if (!strcasecmp(cmd, "pruefsumme") || !strcasecmp(cmd, "sha256")) {
        cmd_checksum(st, a1);

    } else if (!strcasecmp(cmd, "wo") || !strcasecmp(cmd, "which")) {
        cmd_which(st, a1);

    } else if (!strcasecmp(cmd, "beende") || !strcasecmp(cmd, "kill")) {
        cmd_kill(st, a1);

    } else if (!strcasecmp(cmd, "warte") || !strcasecmp(cmd, "sleep")) {
        size_t ms = number_arg(a1);

        if (!ms) {
            term_line(st, C_ERROR, "warte <millisekunden>");
        } else {
            /* Der Fenster-Thread darf schlafen - die Oberflaeche laeuft
             * in einem anderen weiter. Ueber zehn Sekunden geht es
             * trotzdem nicht: So lange soll niemand rateln, ob die
             * Konsole haengt. */
            thread_sleep((uint32_t)MIN(ms, (size_t)10000));
            term_printf(st, C_NORMAL, "%u ms gewartet.", (unsigned)ms);
        }

    } else if (!strcasecmp(cmd, "kalender") || !strcasecmp(cmd, "cal")) {
        cmd_calendar(st, a1, a2);

    } else if (!strcasecmp(cmd, "rechne") || !strcasecmp(cmd, "expr")) {
        cmd_expr(st, rest_of(raw, 1));

    } else if (!strcasecmp(cmd, "verlauf") || !strcasecmp(cmd, "history")) {
        if (!st->history_count) {
            term_line(st, C_NORMAL, "Noch nichts eingegeben.");
        } else {
            for (size_t i = 0; i < st->history_count; i++)
                term_printf(st, C_NORMAL, "  %2u  %s", (unsigned)(i + 1),
                            st->history[i]);
            term_line(st, C_NORMAL,
                      "  Mit den Pfeiltasten holt man sie zurueck.");
        }

    } else if (!strcasecmp(cmd, "kaefig")) {
        if (!a1) {
            term_line(st, C_HIGHLIGHT, "Kaefigprofile:");
            for (size_t i = 0; i < sandbox_profile_count(); i++) {
                struct sandbox probe;
                char darf[96];

                memset(&probe, 0, sizeof(probe));
                sandbox_apply(&probe, sandbox_profile_name(i), "/Heim");
                sandbox_text(&probe, darf, sizeof(darf));
                term_printf(st, C_NORMAL, "  %-8s %s%s",
                            sandbox_profile_name(i), darf,
                            probe.penalty == SB_KILL
                                ? " (Verstoss beendet das Programm)" : "");
            }
            term_line(st, C_NORMAL, "  kaefig <profil> <programm> [text]");
        } else if (!a2) {
            term_line(st, C_ERROR, "kaefig <profil> <programm> [text]");
        } else {
            cmd_start_program(win, st, a2, raw, a1, 3);
        }

    } else if (!strcasecmp(cmd, "firewall")) {
        cmd_firewall(st, argc, argv);

    } else if (!strcasecmp(cmd, "pruefspur")) {
        cmd_audit(st, a1);

    } else if (!strcasecmp(cmd, "protokoll")) {
        cmd_log(st, a1);

    } else if (!strcasecmp(cmd, "aufgaben")) {
        cmd_tasks(st, a1, a2, rest_of(raw, 2), rest_of(raw, 3));

    } else if (!strcasecmp(cmd, "wer") || !strcasecmp(cmd, "whoami")) {
        cmd_who(st);

    } else if (!strcasecmp(cmd, "gruppen")) {
        cmd_groups(st);

    } else if (!strcasecmp(cmd, "benutzer")) {
        cmd_users(st, a1, a2, argc > 3 ? argv[3] : NULL);

    } else if (!strcasecmp(cmd, "rechte")) {
        cmd_mode(st, a1, a2);

    } else if (!strcasecmp(cmd, "besitzer")) {
        cmd_owner(st, a1, a2);

    } else if (!strcasecmp(cmd, "sperren")) {
        lock_show(LOCK_LOCKED);

    } else if (!strcasecmp(cmd, "schrift")) {
        cmd_font(st, rest_of(raw, 1));

    } else if (!strcasecmp(cmd, "threads") || !strcasecmp(cmd, "ps")) {
        cmd_threads(st);

    } else if (!strcasecmp(cmd, "prozesse")) {
        cmd_processes(st);

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

    } else if (!strcasecmp(cmd, "installieren") ||
               !strcasecmp(cmd, "setup")) {
        cmd_install(st, a1, argc > 2 ? argv[2] : NULL);

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

/* Merkt sich eine ausgefuehrte Zeile. Leere Zeilen und Wiederholungen
 * kommen nicht hinein - sie machen den Verlauf nur laenger, ohne ihn
 * brauchbarer zu machen. */
static void remember(struct term_state *st, const char *line)
{
    if (!line[0])
        return;
    if (st->history_count &&
        strcmp(st->history[st->history_count - 1], line) == 0) {
        st->history_pos = st->history_count;
        return;
    }

    if (st->history_count == TERM_HISTORY) {
        memmove(st->history[0], st->history[1],
                sizeof(st->history) - sizeof(st->history[0]));
        st->history_count--;
    }

    strlcpy(st->history[st->history_count++], line,
            sizeof(st->history[0]));
    st->history_pos = st->history_count;
}

/* Holt eine aeltere Zeile zurueck. Hinter der juengsten steht wieder
 * die leere Eingabe - sonst kaeme man aus dem Verlauf nicht heraus. */
static void recall(struct term_state *st, int direction)
{
    if (!st->history_count)
        return;

    if (direction < 0) {
        if (!st->history_pos)
            return;
        st->history_pos--;
    } else {
        if (st->history_pos >= st->history_count)
            return;
        st->history_pos++;
    }

    if (st->history_pos >= st->history_count)
        st->input[0] = '\0';
    else
        strlcpy(st->input, st->history[st->history_pos], sizeof(st->input));
    st->cursor = (int32_t)strlen(st->input);
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

    /* Strg+V fuegt die Zwischenablage in die Eingabezeile ein,
     * Strg+Umschalt+C legt sie dort hinein. Strg+C allein bleibt der
     * Abbruch - so kennt man es von einer Konsole. */
    if (ev->mods & MOD_CTRL) {
        char c = (ev->ascii >= 'A' && ev->ascii <= 'Z')
                 ? (char)(ev->ascii + 32) : ev->ascii;

        if (c == 'v') {
            size_t bytes = 0;
            const char *text = clipboard_get(&bytes);

            for (size_t i = 0; text && i < bytes && len < TERM_INPUT_MAX; i++) {
                if (text[i] < 32 || (unsigned char)text[i] == 127)
                    continue;      /* Zeilenumbrueche bleiben draussen */
                memmove(&st->input[st->cursor + 1], &st->input[st->cursor],
                        len - (size_t)st->cursor + 1);
                st->input[st->cursor++] = text[i];
                len++;
            }
            st->caret_on = true;
            gui_invalidate();
            return;
        }
        if (c == 'c' && (ev->mods & MOD_SHIFT)) {
            clipboard_set(st->input, len);
            return;
        }
    }

    switch (ev->key) {
    case KEY_ENTER: {
        char line[TERM_INPUT_MAX + 1];

        strlcpy(line, st->input, sizeof(line));
        st->input[0] = '\0';
        st->cursor = 0;
        remember(st, line);
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
    case KEY_UP:
    case KEY_DOWN:
        recall(st, ev->key == KEY_UP ? -1 : 1);
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
