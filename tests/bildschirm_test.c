/* bildschirm_test.c - prueft das Rechnen um Aufloesung und Vergroesserung.
 *
 * Das Setzen eines Modus laesst sich hier nicht pruefen - dafuer
 * braeuchte es die Grafikkarte. Pruefen laesst sich, was davor und
 * danach passiert: das Zerlegen von "1280x800", die Frage, wie weit
 * man vergroessern darf, ohne dass die Arbeitsflaeche unbrauchbar
 * wird, und das Zurueckholen der Fenster in einen kleiner gewordenen
 * Bildschirm. Genau dort steckt der Unsinn, den man erst bemerkt, wenn
 * ein Fenster nicht mehr da ist.
 */

#include <stdio.h>
#include <string.h>

#include "displayutil.h"

static int fehler;
static int geprueft;

static void pruefe(const char *was, bool bedingung)
{
    geprueft++;
    if (!bedingung) {
        printf("  FEHLER: %s\n", was);
        fehler++;
    }
}

static void pruefe_zahl(const char *was, long soll, long ist)
{
    geprueft++;
    if (soll != ist) {
        printf("  FEHLER: %s: erwartet %ld, bekommen %ld\n", was, soll, ist);
        fehler++;
    }
}

/* --- Zerlegen ------------------------------------------------------- */

static void gut(const char *text, int32_t w, int32_t h)
{
    int32_t gw = 0, gh = 0;

    geprueft++;
    if (!disp_parse_mode(text, &gw, &gh) || gw != w || gh != h) {
        printf("  FEHLER: \"%s\" -> %dx%d statt %dx%d\n", text,
               (int)gw, (int)gh, (int)w, (int)h);
        fehler++;
    }
}

static void schlecht(const char *text)
{
    int32_t w = 0, h = 0;

    geprueft++;
    if (disp_parse_mode(text, &w, &h)) {
        printf("  FEHLER: \"%s\" haette abgelehnt gehoert (%dx%d)\n",
               text, (int)w, (int)h);
        fehler++;
    }
}

static void test_zerlegen(void)
{
    printf("Zerlegen\n");

    gut("1280x800", 1280, 800);
    gut("800x600", 800, 600);
    gut("1920x1080x32", 1920, 1080);      /* Farbtiefe wird geschluckt */
    gut("  1024x768  ", 1024, 768);
    gut("1024X768", 1024, 768);           /* grosses X geht auch       */
    gut("4096x4096", 4096, 4096);         /* genau an der Obergrenze   */
    gut("640x400", 640, 400);             /* genau an der Untergrenze  */

    schlecht("");
    schlecht("1280");
    schlecht("1280x");
    schlecht("x800");
    schlecht("1280*800");
    schlecht("1280x800x");
    schlecht("1280x800 zusaetzlich");
    schlecht("abcxdef");
    schlecht("639x400");                  /* zu schmal                 */
    schlecht("640x399");                  /* zu niedrig                */
    schlecht("4097x1000");                /* zu breit                  */
    schlecht("1000x4097");                /* zu hoch                   */
    schlecht("12800x800");                /* fuenf Stellen             */
    schlecht(NULL);

    char text[16];

    disp_format_mode(text, sizeof(text), 1280, 800);
    geprueft++;
    if (strcmp(text, "1280x800") != 0) {
        printf("  FEHLER: geschrieben wurde \"%s\"\n", text);
        fehler++;
    }

    /* Was geschrieben wurde, muss sich wieder lesen lassen. */
    int32_t w = 0, h = 0;

    pruefe("Hin und zurueck", disp_parse_mode(text, &w, &h) &&
                              w == 1280 && h == 800);
}

/* --- Vergroesserung ------------------------------------------------- */

static void test_vergroesserung(void)
{
    printf("Vergroesserung\n");

    /* Die Grenze ist die Arbeitsflaeche: Unter 640x400 logischen
     * Punkten passt kein Fenster mehr sinnvoll hin. */
    pruefe_zahl("800x600 laesst nur einfach", 1, disp_max_scale(800, 600));
    pruefe_zahl("1280x800 laesst doppelt", 2, disp_max_scale(1280, 800));
    pruefe_zahl("1920x1200 laesst dreifach", 3, disp_max_scale(1920, 1200));
    pruefe_zahl("3840x2160 laesst vierfach", 4, disp_max_scale(3840, 2160));
    pruefe_zahl("Nie mehr als vierfach", 4, disp_max_scale(7680, 4320));

    /* Die Breite zaehlt mit: Ein sehr breiter, flacher Schirm darf
     * nicht doppelt vergroessert werden, nur weil er breit ist. */
    pruefe_zahl("1280x400 bleibt einfach", 1, disp_max_scale(1280, 400));

    pruefe_zahl("1024x768 sieht niemand vergroessert", 1,
                disp_auto_scale(1024, 768));
    pruefe_zahl("1280x800 auch nicht", 1, disp_auto_scale(1280, 800));
    pruefe_zahl("1920x1080 auch nicht", 1, disp_auto_scale(1920, 1080));
    pruefe_zahl("2560x1440 wird doppelt", 2, disp_auto_scale(2560, 1440));
    pruefe_zahl("3840x2160 wird dreifach", 3, disp_auto_scale(3840, 2160));

    /* Und die Automatik darf nie ueber das Moegliche hinausgehen -
     * sonst waere die Arbeitsflaeche beim Start schon zu klein. */
    for (int32_t h = 400; h <= 4320; h += 40) {
        int32_t w = h * 16 / 9;

        geprueft++;
        if (disp_auto_scale(w, h) > disp_max_scale(w, h)) {
            printf("  FEHLER: %dx%d: automatisch %u, moeglich %u\n",
                   (int)w, (int)h, disp_auto_scale(w, h), disp_max_scale(w, h));
            fehler++;
            break;
        }
    }
}

/* --- Grafikspeicher ------------------------------------------------- */

static void test_speicher(void)
{
    printf("Grafikspeicher\n");

    /* 1920x1080 mit 32 Bit sind 8.294.400 Bytes. */
    pruefe("Genau passend geht", disp_mode_fits(1920, 1080, 8294400));
    pruefe("Ein Byte zu wenig geht nicht",
           !disp_mode_fits(1920, 1080, 8294399));
    pruefe("Reichlich geht", disp_mode_fits(1920, 1080, 16 * 1024 * 1024));
    pruefe("Nichts passt in nichts", !disp_mode_fits(800, 600, 0));
    pruefe("Unsinnige Groessen fallen durch",
           !disp_mode_fits(0, 600, 1024 * 1024));

    /* Die Liste der gaengigen Modi muss aufsteigend und vollstaendig
     * sein - sie ist der Reigen, durch den geblaettert wird. */
    struct disp_mode modes[32];
    size_t n = disp_standard_modes(modes, ARRAY_LEN(modes));

    pruefe("Es gibt Modi", n >= 8);

    bool aufsteigend = true;

    for (size_t i = 1; i < n; i++)
        if ((uint64_t)modes[i].w * modes[i].h <=
            (uint64_t)modes[i - 1].w * modes[i - 1].h)
            aufsteigend = false;
    pruefe("Aufsteigend nach Flaeche", aufsteigend);

    bool alle_gut = true;

    for (size_t i = 0; i < n; i++) {
        char text[16];
        int32_t w = 0, h = 0;

        disp_format_mode(text, sizeof(text), modes[i].w, modes[i].h);
        if (!disp_parse_mode(text, &w, &h) || w != modes[i].w ||
            h != modes[i].h)
            alle_gut = false;
    }
    pruefe("Jeder Modus laesst sich schreiben und wieder lesen", alle_gut);

    /* Weniger Platz als angefragt: Die Liste muss sich kuerzen lassen,
     * ohne dass etwas ueberlaeuft. */
    struct disp_mode wenige[3];

    pruefe_zahl("Nur drei angefragt", 3,
                (long)disp_standard_modes(wenige, 3));
    pruefe_zahl("Keiner angefragt", 0,
                (long)disp_standard_modes(wenige, 0));
}

/* --- Fenster zurueckholen ------------------------------------------- */

static void fenster(const char *was, int32_t x, int32_t y, int32_t w, int32_t h,
                    int32_t sw, int32_t sh,
                    int32_t sx, int32_t sy, int32_t sww, int32_t shh)
{
    disp_fit_window(&x, &y, &w, &h, sw, sh);
    geprueft++;
    if (x != sx || y != sy || w != sww || h != shh) {
        printf("  FEHLER: %s -> %d,%d %dx%d statt %d,%d %dx%d\n", was,
               (int)x, (int)y, (int)w, (int)h,
               (int)sx, (int)sy, (int)sww, (int)shh);
        fehler++;
    }
}

static void test_fenster(void)
{
    printf("Fenster zurueckholen\n");

    /* Was hineinpasst, bleibt unberuehrt. */
    fenster("passt schon", 100, 100, 400, 300, 1280, 800,
            100, 100, 400, 300);

    /* Was rechts oder unten heraussteht, rutscht zurueck - und behaelt
     * dabei seine Groesse. */
    fenster("rechts heraus", 1100, 100, 400, 300, 1280, 800,
            880, 100, 400, 300);
    fenster("unten heraus", 100, 700, 400, 300, 1280, 800,
            100, 500, 400, 300);

    /* Ganz ausserhalb: landet in der Ecke. */
    fenster("weit draussen", 3000, 3000, 400, 300, 1280, 800,
            880, 500, 400, 300);

    /* Zu gross fuer den Schirm: wird kleiner, aber nur so weit wie
     * noetig, und faengt links oben an. */
    fenster("zu breit", 0, 0, 2000, 300, 1280, 800,
            0, 0, 1280, 300);
    fenster("zu gross", 200, 200, 2000, 2000, 1280, 800,
            0, 0, 1280, 800);

    /* Ein Fenster mit negativer Ecke kommt in die Flaeche zurueck. */
    fenster("links oben heraus", -50, -30, 400, 300, 1280, 800,
            0, 0, 400, 300);

    /* Genau passend bleibt genau passend. */
    fenster("randgenau", 880, 500, 400, 300, 1280, 800,
            880, 500, 400, 300);

    /* Und der harte Fall: Der Bildschirm wird halbiert, weil jemand
     * die Vergroesserung verdoppelt hat. */
    fenster("nach dem Verdoppeln", 700, 500, 500, 280, 640, 400,
            140, 120, 500, 280);
}

int main(void)
{
    printf("=== Bildschirm ===\n");

    test_zerlegen();
    test_vergroesserung();
    test_speicher();
    test_fenster();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
