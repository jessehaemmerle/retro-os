/* oberflaeche_test.c - Dunkelmodus und Benachrichtigungen.
 *
 * Zwei Dinge, die sich ohne Bildschirm pruefen lassen: ab wann der
 * automatische Dunkelmodus dunkel ist - dort liegen die Randstunden,
 * die man leicht um eins verfehlt -, und ob der Ring der Meldungen
 * die richtige Reihenfolge behaelt, wenn er ueberlaeuft.
 */

#include <stdio.h>
#include <string.h>

#include "theme.h"
#include "notify.h"
#include "gui.h"

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

static void pruefe_text(const char *was, const char *soll, const char *ist)
{
    geprueft++;
    if (!ist || strcmp(soll, ist) != 0) {
        printf("  FEHLER: %s: erwartet \"%s\", bekommen \"%s\"\n", was, soll,
               ist ? ist : "(nichts)");
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

/* --- Dunkelmodus ---------------------------------------------------- */

static void test_stunden(void)
{
    printf("Wann es dunkel wird\n");

    /* Die Grenzen: 19 Uhr ist schon dunkel, 18:59 noch nicht; 7 Uhr
     * ist schon hell, 6 Uhr noch nicht. */
    pruefe("18 Uhr hell",  !theme_dark_for_hour(18));
    pruefe("19 Uhr dunkel", theme_dark_for_hour(19));
    pruefe("23 Uhr dunkel", theme_dark_for_hour(23));
    pruefe("0 Uhr dunkel",  theme_dark_for_hour(0));
    pruefe("6 Uhr dunkel",  theme_dark_for_hour(6));
    pruefe("7 Uhr hell",   !theme_dark_for_hour(7));
    pruefe("12 Uhr hell",  !theme_dark_for_hour(12));

    /* Eine Stunde, die es nicht gibt, macht es nicht dunkel. */
    pruefe("negativ",  !theme_dark_for_hour(-1));
    pruefe("zu gross", !theme_dark_for_hour(24));
    pruefe("weit weg", !theme_dark_for_hour(99));
}

static void test_umschalten(void)
{
    printf("Umschalten\n");

    theme_set_mode(THEME_HELL);
    pruefe("hell ist hell", !theme_is_dark());
    pruefe_zahl("Modus gemerkt", THEME_HELL, theme_mode());

    theme_set_mode(THEME_DUNKEL);
    pruefe("dunkel ist dunkel", theme_is_dark());

    /* Fest eingestellt kuemmert die Uhrzeit nicht. */
    pruefe("fest bleibt fest", !theme_tick(12));
    pruefe("immer noch dunkel", theme_is_dark());

    theme_set_mode(THEME_AUTO);
    pruefe("mittags wird es hell", theme_tick(12));
    pruefe("und ist es", !theme_is_dark());
    pruefe("mittags bleibt es hell", !theme_tick(13));

    pruefe("abends wird es dunkel", theme_tick(20));
    pruefe("und ist es", theme_is_dark());
    pruefe("abends bleibt es dunkel", !theme_tick(22));

    /* Zurueck auf hell - der Test darf keinen Zustand hinterlassen. */
    theme_set_mode(THEME_HELL);
}

static void test_schluessel(void)
{
    printf("In der Einstellungsdatei\n");

    enum theme_mode m = THEME_DUNKEL;

    pruefe("hell gelesen", theme_mode_parse("hell", &m) && m == THEME_HELL);
    pruefe("dunkel gelesen", theme_mode_parse("dunkel", &m) && m == THEME_DUNKEL);
    pruefe("automatisch gelesen",
           theme_mode_parse("automatisch", &m) && m == THEME_AUTO);

    pruefe("Unsinn abgelehnt", !theme_mode_parse("bunt", &m));
    pruefe("leer abgelehnt", !theme_mode_parse("", &m));
    pruefe("nichts abgelehnt", !theme_mode_parse(NULL, &m));
    pruefe("kein Ziel", !theme_mode_parse("hell", NULL));

    /* Was geschrieben wird, muss sich wieder lesen lassen. */
    for (int i = 0; i < 3; i++) {
        enum theme_mode zurueck = THEME_HELL;

        pruefe("hin und zurueck",
               theme_mode_parse(theme_mode_key((enum theme_mode)i), &zurueck) &&
               zurueck == (enum theme_mode)i);
    }

    pruefe_text("Name hell", "Hell", theme_mode_name(THEME_HELL));
    pruefe_text("Name dunkel", "Dunkel", theme_mode_name(THEME_DUNKEL));
    pruefe_text("Name automatisch", "Automatisch", theme_mode_name(THEME_AUTO));
}

static void test_abdunkeln(void)
{
    printf("Abdunkeln\n");

    uint32_t weiss = 0x00FFFFFF;

    pruefe_zahl("unveraendert", 0x00FFFFFF, theme_shade(weiss, 100));
    pruefe_zahl("halb", 0x007F7F7F, theme_shade(weiss, 50));
    pruefe_zahl("aus", 0x00000000, theme_shade(weiss, 0));
    pruefe_zahl("negativ ist aus", 0x00000000, theme_shade(weiss, -20));

    /* Kanalweise, ohne dass einer in den naechsten laeuft. */
    pruefe_zahl("kanalweise", 0x00102030, theme_shade(0x00204060, 50));

    /* Und nichts laeuft ueber. */
    pruefe_zahl("kein Ueberlauf", 0x00FFFFFF, theme_shade(weiss, 200));
}

/* --- Benachrichtigungen --------------------------------------------- */

static void test_ring(void)
{
    printf("Der Ring der Meldungen\n");

    notify_clear();
    pruefe_zahl("leer", 0, (long)notify_count());
    pruefe("nichts ungelesen", notify_unread() == 0);
    pruefe("kein Eintrag", notify_at(0) == NULL);

    notify_post(0, "Erste", "eins");
    notify_post(0, "Zweite", "zwei");
    notify_post(0, "Dritte", "drei");

    pruefe_zahl("drei drin", 3, (long)notify_count());
    pruefe_text("neueste zuerst", "Dritte", notify_at(0)->title);
    pruefe_text("dann die zweite", "Zweite", notify_at(1)->title);
    pruefe_text("dann die erste", "Erste", notify_at(2)->title);
    pruefe("nicht mehr", notify_at(3) == NULL);
    pruefe_zahl("drei ungelesen", 3, (long)notify_unread());

    notify_mark_seen();
    pruefe_zahl("nach dem Ansehen", 0, (long)notify_unread());
    pruefe_zahl("aber noch da", 3, (long)notify_count());

    notify_post(0, "Vierte", "vier");
    pruefe_zahl("eine neue ist ungelesen", 1, (long)notify_unread());

    /* Ueberlauf: Die aelteste faellt heraus, die Reihenfolge bleibt. */
    notify_clear();
    for (int i = 0; i < NOTIFY_MAX + 5; i++) {
        char titel[NOTIFY_TITLE];

        snprintf(titel, sizeof(titel), "Nr %d", i);
        notify_post(0, titel, "Text");
    }

    pruefe_zahl("nicht mehr als der Ring", NOTIFY_MAX, (long)notify_count());
    pruefe_text("neueste zuerst", "Nr 36", notify_at(0)->title);
    pruefe_text("aelteste zuletzt", "Nr 5",
                notify_at(NOTIFY_MAX - 1)->title);
    pruefe("dahinter nichts", notify_at(NOTIFY_MAX) == NULL);

    /* Ein zu langer Text wird abgeschnitten und nicht ueber den Rand
     * geschrieben. */
    char lang[400];

    memset(lang, 'x', sizeof(lang) - 1);
    lang[sizeof(lang) - 1] = '\0';
    notify_post(0, lang, lang);
    pruefe_zahl("Titel abgeschnitten", NOTIFY_TITLE - 1,
                (long)strlen(notify_at(0)->title));
    pruefe_zahl("Text abgeschnitten", NOTIFY_TEXT - 1,
                (long)strlen(notify_at(0)->text));

    notify_clear();
    pruefe_zahl("und wieder leer", 0, (long)notify_count());
}

/* --- Menue mit der Tastatur ------------------------------------------ */

/* Baut ein Menue aus einer kurzen Beschreibung: '+' waehlbar, '-'
 * blass, '|' Trennlinie. */
static size_t menue(const char *muster, struct menu_item *out)
{
    size_t n = 0;

    for (const char *p = muster; *p; p++, n++) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].label   = (*p == '|') ? NULL : "Zeile";
        out[n].enabled = (*p == '+');
        out[n].id      = (int)n;
    }
    return n;
}

static void test_menue(void)
{
    printf("Menue mit der Tastatur\n");

    struct menu_item items[16];
    size_t n = menue("+++", items);

    /* Von nirgendwo aus (-1) faengt es oben an, rueckwaerts unten. */
    pruefe_zahl("erste Zeile", 0, menu_next_index(items, n, -1, 1));
    pruefe_zahl("letzte Zeile", 2, menu_next_index(items, n, -1, -1));

    pruefe_zahl("weiter", 1, menu_next_index(items, n, 0, 1));
    pruefe_zahl("zurueck", 0, menu_next_index(items, n, 1, -1));

    /* Reihum. */
    pruefe_zahl("unten wieder oben", 0, menu_next_index(items, n, 2, 1));
    pruefe_zahl("oben wieder unten", 2, menu_next_index(items, n, 0, -1));

    /* Trennlinien und blasse Zeilen werden uebersprungen. */
    n = menue("+|-+", items);
    pruefe_zahl("ueber Linie und Blasses hinweg", 3,
                menu_next_index(items, n, 0, 1));
    pruefe_zahl("und zurueck", 0, menu_next_index(items, n, 3, -1));

    /* Am Anfang und am Ende eine Trennlinie. */
    n = menue("|++|", items);
    pruefe_zahl("Linie am Anfang", 1, menu_next_index(items, n, -1, 1));
    pruefe_zahl("Linie am Ende", 2, menu_next_index(items, n, -1, -1));

    /* Nur eine waehlbare Zeile: Sie bleibt, wo sie ist. */
    n = menue("-+-", items);
    pruefe_zahl("die einzige, vorwaerts", 1, menu_next_index(items, n, 1, 1));
    pruefe_zahl("die einzige, rueckwaerts", 1, menu_next_index(items, n, 1, -1));

    /* Gar nichts waehlbar: Es bleibt stehen, statt sich zu drehen. */
    n = menue("--|--", items);
    pruefe_zahl("nichts waehlbar", 2, menu_next_index(items, n, 2, 1));
    pruefe_zahl("nichts waehlbar, rueckwaerts", -1,
                menu_next_index(items, n, -1, -1));

    /* Unsinn faellt nicht auf die Nase. */
    n = menue("++", items);
    pruefe_zahl("kein Schritt", 1, menu_next_index(items, n, 1, 0));
    pruefe_zahl("leeres Menue", 3, menu_next_index(items, 0, 3, 1));
    pruefe_zahl("kein Menue", 3, menu_next_index(NULL, 4, 3, 1));
}

int main(void)
{
    printf("=== Oberflaeche ===\n");

    test_stunden();
    test_umschalten();
    test_schluessel();
    test_abdunkeln();
    test_ring();
    test_menue();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
