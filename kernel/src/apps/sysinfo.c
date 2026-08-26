/* sysinfo.c - Systeminformationen und das "Ueber"-Fenster. */

#include "apps.h"
#include "apic.h"
#include "input.h"
#include "usb.h"
#include "arch.h"
#include "boot.h"
#include "font.h"
#include "kstring.h"
#include "block.h"
#include "mm.h"
#include "net.h"
#include "process.h"
#include "thread.h"
#include "theme.h"
#include "widgets.h"

struct sys_state {
    char cpu_vendor[13];
    char cpu_brand[49];
};

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t regs[4])
{
    __asm__ volatile("cpuid"
                     : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                     : "a"(leaf), "c"(sub));
}

static void read_cpu(struct sys_state *st)
{
    uint32_t regs[4];

    cpuid(0, 0, regs);
    memcpy(st->cpu_vendor + 0, &regs[1], 4);
    memcpy(st->cpu_vendor + 4, &regs[3], 4);
    memcpy(st->cpu_vendor + 8, &regs[2], 4);
    st->cpu_vendor[12] = '\0';

    /* Der Markenname steht in den erweiterten Blaettern 0x80000002-4. */
    cpuid(0x80000000, 0, regs);
    if (regs[0] >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, regs);
            memcpy(st->cpu_brand + i * 16, regs, 16);
        }
        st->cpu_brand[48] = '\0';
    } else {
        strlcpy(st->cpu_brand, "unbekannt", sizeof(st->cpu_brand));
    }
}

/* Ein Balken mit Beschriftung - fuer Speicherbelegung. */
static void draw_bar(struct canvas *c, int32_t x, int32_t y, int32_t w,
                     uint64_t used, uint64_t total, uint32_t color)
{
    struct rect r = rect_make(x, y, w, 16);

    gfx_fill(c, r, COL_FIELD);
    gfx_bevel_thin(c, r, false);

    if (total > 0) {
        int32_t filled = (int32_t)((uint64_t)(w - 4) * used / total);
        gfx_fill(c, rect_make(x + 2, y + 2, filled, 12), color);
    }
}

static void row(struct canvas *c, int32_t y, const char *label, const char *value)
{
    gfx_text(c, 16, y, label, COL_TEXT_DIM);
    gfx_text(c, 176, y, value, COL_TEXT);
}

static void sys_paint(struct window *win, struct canvas *c)
{
    struct sys_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);
    const struct boot_info *bi = boot_info();
    char buf[96];
    char label[32];
    int32_t y = 14;

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    gfx_text_bold(&local, 16, y, "Prozessor", COL_SELECT);
    y += 22;
    row(&local, y, "Hersteller", st->cpu_vendor);           y += 18;
    row(&local, y, "Modell", st->cpu_brand);                y += 18;
    row(&local, y, "Betriebsart", "Long Mode (64 Bit)");    y += 18;

    if (apic_available())
        ksnprintf(buf, sizeof(buf), "APIC, %u Kern%s%s",
                  (unsigned)apic_cpu_count(),
                  apic_cpu_count() == 1 ? "" : "e",
                  timer_uses_apic() ? ", eigener Zeitgeber" : "");
    else
        strlcpy(buf, "8259A-PIC mit PIT", sizeof(buf));
    row(&local, y, "Unterbrechungen", buf);                 y += 28;

    gfx_text_bold(&local, 16, y, "Speicher", COL_SELECT);
    y += 22;

    ksnprintf(buf, sizeof(buf), "%u MiB von %u MiB belegt",
              (unsigned)(pmm_used_bytes() / (1024 * 1024)),
              (unsigned)(pmm_total_bytes() / (1024 * 1024)));
    row(&local, y, "Arbeitsspeicher", buf);
    y += 18;
    draw_bar(&local, 176, y, MAX(local.w - 200, 80),
             pmm_used_bytes(), pmm_total_bytes(), RGB(0x30, 0x80, 0xC0));
    y += 26;

    ksnprintf(buf, sizeof(buf), "%u KiB von %u KiB benutzt",
              (unsigned)(heap_used_bytes() / 1024),
              (unsigned)(heap_total_bytes() / 1024));
    row(&local, y, "Kernel-Heap", buf);
    y += 18;
    draw_bar(&local, 176, y, MAX(local.w - 200, 80),
             heap_used_bytes(), heap_total_bytes(), RGB(0x40, 0xA0, 0x60));
    y += 30;

    gfx_text_bold(&local, 16, y, "Grafik und Dateien", COL_SELECT);
    y += 22;

    ksnprintf(buf, sizeof(buf), "%u x %u, %u Bit",
              (unsigned)bi->fb_width, (unsigned)bi->fb_height, bi->fb_bpp);
    row(&local, y, "Bildschirm", buf);
    y += 18;

    char size[24];
    fs_format_size(size, sizeof(size), fs_bytes_used());
    ksnprintf(buf, sizeof(buf), "%u Eintraege, %s",
              (unsigned)fs_node_count(), size);
    row(&local, y, "Dateisystem", buf);
    y += 18;

    uint64_t ms = timer_ms();
    ksnprintf(buf, sizeof(buf), "%u:%02u:%02u",
              (unsigned)(ms / 3600000), (unsigned)(ms / 60000 % 60),
              (unsigned)(ms / 1000 % 60));
    row(&local, y, "Laufzeit", buf);
    y += 18;

    ksnprintf(buf, sizeof(buf), "%u", (unsigned)gui_window_count());
    row(&local, y, "Offene Fenster", buf);
    y += 18;

    ksnprintf(buf, sizeof(buf), "%u aktiv", (unsigned)thread_count());
    row(&local, y, "Threads", buf);
    y += 18;

    if (process_count() > 0) {
        struct process *p0 = process_at(0);

        ksnprintf(buf, sizeof(buf), "%u in Ring 3 (%s)",
                  (unsigned)process_count(), p0 ? p0->name : "");
    } else {
        strlcpy(buf, "keine", sizeof(buf));
    }
    row(&local, y, "Programme", buf);
    y += 30;

    gfx_text_bold(&local, 16, y, "Datentraeger", COL_SELECT);
    y += 22;

    if (block_device_count() == 0) {
        row(&local, y, "Laufwerk", "keines gefunden");
        y += 18;
    } else {
        /* Ein heutiger Rechner hat oft mehrere - NVMe und SATA
         * nebeneinander. Alle auffuehren. */
        for (size_t i = 0; i < block_device_count(); i++) {
            struct block_device *d = block_device_at(i);
            uint64_t mib = d->sector_count * d->sector_size / (1024 * 1024);

            if (mib >= 1024)
                ksnprintf(buf, sizeof(buf), "%s (%u,%u GiB)", d->model,
                          (unsigned)(mib / 1024),
                          (unsigned)((mib % 1024) * 10 / 1024));
            else
                ksnprintf(buf, sizeof(buf), "%s (%u MiB)", d->model,
                          (unsigned)mib);
            row(&local, y, d->name, buf);
            y += 18;
        }

        if (fs_disk_mounted()) {
            struct fat_volume *vol = fs_disk_volume();
            uint64_t total = fat_total_bytes(vol);
            uint64_t used  = total - fat_free_bytes(vol);

            ksnprintf(buf, sizeof(buf), "FAT32 \"%s\" unter /Festplatte",
                      fs_disk_name());
            row(&local, y, "Dateisystem", buf);
            y += 18;
            draw_bar(&local, 176, y, MAX(local.w - 200, 80), used, total,
                     RGB(0xC0, 0x80, 0x30));
            y += 24;
        } else {
            row(&local, y, "Dateisystem", "nicht eingehaengt");
            y += 18;
        }
    }

    y += 12;
    gfx_text_bold(&local, 16, y, "Eingabe", COL_SELECT);
    y += 22;

    row(&local, y, "PS/2-Anschluss",
        ps2_present() ? "Tastatur und Maus" : "nicht vorhanden");
    y += 18;

    if (usb_device_count() == 0) {
        row(&local, y, "USB", "keine Geraete");
        y += 18;
    } else {
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
            }
            ksnprintf(buf, sizeof(buf), "%s, %04x:%04x, %s", art,
                      info->vendor_id, info->product_id,
                      usb_speed_name(info->speed));
            ksnprintf(label, sizeof(label), "USB-Anschluss %u",
                      (unsigned)info->port);
            row(&local, y, label, buf);
            y += 18;
        }
    }

    y += 12;
    gfx_text_bold(&local, 16, y, "Netzwerk", COL_SELECT);
    y += 22;

    if (!g_netif.up) {
        row(&local, y, "Karte", "keine gefunden");
        return;
    }

    row(&local, y, "Karte", e1000_model());
    y += 18;

    mac_format(&g_netif.mac, buf, sizeof(buf));
    row(&local, y, "Hardware-Adresse", buf);
    y += 18;

    if (net_ready()) {
        char ip[16], gw[16];

        ip_format(g_netif.ip, ip, sizeof(ip));
        ip_format(g_netif.gateway, gw, sizeof(gw));
        ksnprintf(buf, sizeof(buf), "%s (Gateway %s)", ip, gw);
        row(&local, y, "IP-Adresse", buf);
        y += 18;

        ksnprintf(buf, sizeof(buf), "%u empfangen, %u gesendet",
                  (unsigned)g_netif.rx_packets, (unsigned)g_netif.tx_packets);
        row(&local, y, "Pakete", buf);
    } else {
        row(&local, y, "IP-Adresse", "keine (DHCP ohne Antwort)");
    }
}

static void sys_event(struct window *win, const struct gui_event *ev)
{
    static uint64_t last;

    /* Einmal pro Sekunde reicht - sonst flackert die Anzeige nur. */
    if (ev->type == EV_TICK && timer_ms() - last >= 1000) {
        last = timer_ms();
        gui_invalidate();
    }
}

static void sys_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_sysinfo(void)
{
    struct window *existing = gui_find_by_paint(sys_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct sys_state *st = kzalloc(sizeof(*st));
    if (!st)
        return;

    read_cpu(st);

    struct window *win = gui_create_window("Systeminformation", 0, 0, 600, 640,
                                           WF_CENTER | WF_RESIZABLE, ICON_COMPUTER);
    if (!win) {
        kfree(st);
        return;
    }

    win->user     = st;
    win->on_paint = sys_paint;
    win->on_event = sys_event;
    win->on_close = sys_close;
    win->min_w    = 420;
    win->min_h    = 380;

    gui_focus_window(win);
}

/* ------------------------------------------------------------------ */

static const char *about_text[] = {
    "RetroOS 1.0",
    "",
    "Ein kleines Betriebssystem fuer x86-64-Rechner,",
    "vollstaendig neu geschrieben:",
    "",
    "  Kernel        eigener 64-Bit-Kernel, Long Mode",
    "  Speicher      Bitmap-Seitenverwaltung und Heap",
    "  Treiber       PS/2-Tastatur und -Maus, PIT, RTC",
    "  Grafik        linearer Framebuffer mit Backbuffer",
    "  Oberflaeche   eigenes Fenstersystem",
    "  Dateien       Dateisystem im Arbeitsspeicher",
    "",
    "Bootloader: Limine (BSD-2-Clause)",
    "Alles Uebrige steht unter der MIT-Lizenz.",
};

static void about_paint(struct window *win, struct canvas *c)
{
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);
    gfx_gradient_v(&local, rect_make(0, 0, local.w, 64),
                   COL_TITLE_A1, COL_TITLE_A2);

    icon_draw(&local, 16, 16, ICON_COMPUTER, 2);
    gfx_text_bold(&local, 64, 20, "RetroOS", COL_WHITE);
    gfx_text(&local, 64, 38, "Version 1.0", RGB(0xC0, 0xD8, 0xF0));

    int32_t y = 80;
    for (size_t i = 0; i < ARRAY_LEN(about_text); i++) {
        gfx_text(&local, 20, y, about_text[i],
                 i == 0 ? COL_SELECT : COL_TEXT);
        y += FONT_HEIGHT + 2;
    }
}

void app_about(void)
{
    struct window *existing = gui_find_by_paint(about_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct window *win = gui_create_window("Ueber RetroOS", 0, 0, 470, 420,
                                           WF_CENTER, ICON_INFO);
    if (win) {
        win->on_paint = about_paint;
        gui_focus_window(win);
    }
}
