/* schreiben_test.c - prueft das Dokument der Textverarbeitung.
 *
 * Der Kern ist der Weg nach HTML und zurueck: Was gespeichert und
 * wieder geladen wird, muss dasselbe Dokument ergeben. Dazu die
 * Bearbeitungsschritte, die ein Absatzgefuege durcheinanderbringen
 * koennen - Teilen, Zusammenfuegen, Loeschen.
 */

#include <stdio.h>
#include <string.h>

#include "writedoc.h"

static int fehler;
static int geprueft;

static struct wdoc doc;

static void erwarte(const char *was, const char *ist, const char *soll)
{
    geprueft++;
    if (strcmp(ist, soll) == 0)
        return;
    printf("  FEHLER: %s ergibt \"%s\", erwartet \"%s\"\n", was, ist, soll);
    fehler++;
}

static void erwarte_zahl(const char *was, long ist, long soll)
{
    geprueft++;
    if (ist == soll)
        return;
    printf("  FEHLER: %s ergibt %ld, erwartet %ld\n", was, ist, soll);
    fehler++;
}

/* Schreibt einen ganzen Absatz mit einheitlicher Auszeichnung. */
static void schreibe(int para, const char *text, unsigned char marks)
{
    for (const char *p = text; *p; p++)
        wdoc_insert_char(&doc, para, doc.paras[para].len, *p, marks);
}

static void bearbeiten(void)
{
    printf("Bearbeiten\n");

    wdoc_clear(&doc);
    erwarte_zahl("frisches Dokument", doc.count, 1);

    schreibe(0, "Hallo Welt", 0);
    erwarte("Absatz 0", doc.paras[0].text, "Hallo Welt");
    erwarte_zahl("Zeichen", (long)wdoc_chars(&doc), 10);
    erwarte_zahl("Woerter", (long)wdoc_words(&doc), 2);

    /* Teilen zwischen den Woertern. */
    wdoc_split(&doc, 0, 5);
    erwarte_zahl("nach dem Teilen", doc.count, 2);
    erwarte("erster Teil",  doc.paras[0].text, "Hallo");
    erwarte("zweiter Teil", doc.paras[1].text, " Welt");

    /* Und wieder zusammen. */
    int naht = wdoc_join(&doc, 1);

    erwarte_zahl("nach dem Fuegen", doc.count, 1);
    erwarte_zahl("Nahtstelle", naht, 5);
    erwarte("wieder ganz", doc.paras[0].text, "Hallo Welt");

    /* Zeichen loeschen. */
    wdoc_erase_char(&doc, 0, 0);
    erwarte("erstes Zeichen weg", doc.paras[0].text, "allo Welt");

    /* Einfuegen in der Mitte. */
    wdoc_insert_char(&doc, 0, 0, 'H', 0);
    erwarte("wieder da", doc.paras[0].text, "Hallo Welt");

    /* Ein Absatz bleibt immer stehen. */
    wdoc_remove(&doc, 0);
    erwarte_zahl("letzter Absatz bleibt", doc.count, 1);
}

static void auszeichnungen(void)
{
    printf("Fett und unterstrichen\n");

    wdoc_clear(&doc);
    schreibe(0, "Das ist ", 0);
    schreibe(0, "fett", MARK_BOLD);
    schreibe(0, " und ", 0);
    schreibe(0, "beides", MARK_BOLD | MARK_UNDERLINE);
    schreibe(0, ".", 0);

    erwarte("Text", doc.paras[0].text, "Das ist fett und beides.");

    geprueft++;
    if (!(doc.paras[0].marks[8] & MARK_BOLD)) {
        printf("  FEHLER: Zeichen 8 ist nicht fett\n");
        fehler++;
    }
    geprueft++;
    if (doc.paras[0].marks[0] != 0) {
        printf("  FEHLER: Zeichen 0 traegt eine Auszeichnung\n");
        fehler++;
    }

    /* Beim Teilen muessen die Auszeichnungen mitwandern. */
    wdoc_split(&doc, 0, 8);
    geprueft++;
    if (!(doc.paras[1].marks[0] & MARK_BOLD)) {
        printf("  FEHLER: die Auszeichnung ist beim Teilen verlorengegangen\n");
        fehler++;
    }
}

/* Sucht einen Text im HTML. */
static bool enthaelt(const char *html, const char *was)
{
    return strstr(html, was) != NULL;
}

static void nach_html(void)
{
    printf("HTML schreiben\n");

    wdoc_clear(&doc);
    doc.paras[0].style = STYLE_H1;
    schreibe(0, "Bericht", 0);

    int p = wdoc_insert(&doc, 0, STYLE_BODY);
    schreibe(p, "Ein Satz mit ", 0);
    schreibe(p, "Nachdruck", MARK_BOLD);
    schreibe(p, ".", 0);

    p = wdoc_insert(&doc, p, STYLE_LIST);
    schreibe(p, "Erster Punkt", 0);
    p = wdoc_insert(&doc, p, STYLE_LIST);
    schreibe(p, "Zweiter Punkt", 0);

    p = wdoc_insert(&doc, p, STYLE_BODY);
    doc.paras[p].align = WA_CENTER;
    schreibe(p, "Mittig", 0);

    p = wdoc_insert(&doc, p, STYLE_BODY);
    schreibe(p, "Zeichen wie < und > und &", 0);

    static char html[16384];
    size_t n = wdoc_to_html(&doc, "Bericht", html, sizeof(html));

    geprueft++;
    if (n == 0 || n >= sizeof(html)) {
        printf("  FEHLER: HTML hat %u Zeichen\n", (unsigned)n);
        fehler++;
    }

    struct {
        const char *was;
    } muss[] = {
        { "<h1>Bericht</h1>" },
        { "<b>Nachdruck</b>" },
        { "<ul>" },
        { "<li>Erster Punkt</li>" },
        { "<li>Zweiter Punkt</li>" },
        { "</ul>" },
        { "text-align:center" },
        { "&lt;" },
        { "&gt;" },
        { "&amp;" },
        { "<title>Bericht</title>" },
    };

    for (size_t i = 0; i < sizeof(muss) / sizeof(muss[0]); i++) {
        geprueft++;
        if (!enthaelt(html, muss[i].was)) {
            printf("  FEHLER: \"%s\" fehlt im HTML\n", muss[i].was);
            fehler++;
        }
    }

    printf("HTML lesen\n");

    struct wdoc zurueck;
    char titel[64];

    wdoc_from_html(&zurueck, html, n, titel, sizeof(titel));

    erwarte("Titel", titel, "Bericht");
    erwarte_zahl("Absaetze", zurueck.count, doc.count);

    if (zurueck.count == doc.count) {
        for (int i = 0; i < doc.count; i++) {
            char was[64];

            snprintf(was, sizeof(was), "Absatz %d", i);
            erwarte(was, zurueck.paras[i].text, doc.paras[i].text);

            geprueft++;
            if (zurueck.paras[i].style != doc.paras[i].style) {
                printf("  FEHLER: Absatz %d hat Vorlage %u, erwartet %u\n",
                       i, zurueck.paras[i].style, doc.paras[i].style);
                fehler++;
            }
            geprueft++;
            if (zurueck.paras[i].align != doc.paras[i].align) {
                printf("  FEHLER: Absatz %d ist anders ausgerichtet\n", i);
                fehler++;
            }
        }

        /* Die Auszeichnung muss den Umweg ueberstehen. */
        const struct paragraph *satz = &zurueck.paras[1];
        int at = -1;

        for (int k = 0; k + 9 <= satz->len; k++) {
            if (strncmp(satz->text + k, "Nachdruck", 9) == 0) {
                at = k;
                break;
            }
        }

        geprueft++;
        if (at < 0 || !(satz->marks[at] & MARK_BOLD)) {
            printf("  FEHLER: \"Nachdruck\" ist nach dem Laden nicht fett\n");
            fehler++;
        }
    }
}

static void fremdes_html(void)
{
    printf("Fremdes HTML\n");

    static const char seite[] =
        "<html><head><title>Fremd</title></head><body>"
        "<h1>Titel</h1>"
        "<div><p>Erster <em>Absatz</em>.</p></div>"
        "<script>console.log(\"weg damit\");</script>"
        "<p>Zweiter\n   Absatz mit   viel Luft.</p>"
        "<ul><li>A</li><li>B</li></ul>"
        "</body></html>";

    struct wdoc fremd;
    char titel[64];

    wdoc_from_html(&fremd, seite, sizeof(seite) - 1, titel, sizeof(titel));

    erwarte("Titel", titel, "Fremd");
    erwarte_zahl("Absaetze", fremd.count, 5);

    if (fremd.count == 5) {
        erwarte("Ueberschrift", fremd.paras[0].text, "Titel");
        erwarte_zahl("als Ueberschrift", fremd.paras[0].style, STYLE_H1);
        erwarte("erster Absatz", fremd.paras[1].text, "Erster Absatz.");
        erwarte("Luft zusammengefaltet", fremd.paras[2].text,
                "Zweiter Absatz mit viel Luft.");
        erwarte("Listenpunkt", fremd.paras[3].text, "A");
        erwarte_zahl("als Liste", fremd.paras[3].style, STYLE_LIST);
    }

    /* Das Skript darf nicht im Text landen. */
    geprueft++;
    for (int i = 0; i < fremd.count; i++) {
        if (strstr(fremd.paras[i].text, "weg damit")) {
            printf("  FEHLER: der Skriptinhalt steht im Dokument\n");
            fehler++;
            break;
        }
    }
}

static void grenzen(void)
{
    printf("Grenzen\n");

    wdoc_clear(&doc);

    /* Mehr Zeichen, als ein Absatz fasst. */
    for (int i = 0; i < PARA_TEXT_MAX + 100; i++)
        wdoc_insert_char(&doc, 0, doc.paras[0].len, 'x', 0);

    geprueft++;
    if (doc.paras[0].len >= PARA_TEXT_MAX) {
        printf("  FEHLER: Absatz laeuft mit %d Zeichen ueber\n",
               doc.paras[0].len);
        fehler++;
    }

    /* Mehr Absaetze, als das Dokument fasst. */
    for (int i = 0; i < DOC_PARAS_MAX + 50; i++)
        wdoc_insert(&doc, doc.count - 1, STYLE_BODY);

    geprueft++;
    if (doc.count > DOC_PARAS_MAX) {
        printf("  FEHLER: %d Absaetze, hoechstens %d erlaubt\n",
               doc.count, DOC_PARAS_MAX);
        fehler++;
    }

    /* Ein zu kleiner Puffer darf nichts zerschreiben. */
    char klein[32];
    size_t n = wdoc_to_html(&doc, "x", klein, sizeof(klein));

    geprueft++;
    if (klein[sizeof(klein) - 1] != '\0' || n < sizeof(klein)) {
        printf("  FEHLER: kleiner Puffer nicht sauber abgeschlossen\n");
        fehler++;
    }
}

int main(void)
{
    printf("=== Textverarbeitung ===\n");

    bearbeiten();
    auszeichnungen();
    nach_html();
    fremdes_html();
    grenzen();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
