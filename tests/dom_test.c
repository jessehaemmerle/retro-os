/* dom_test.c - prueft Zerteiler, Formatvorlagen und Umbruch. */

#include <stdio.h>
#include <string.h>

#include "htmlparse.h"
#include "css.h"
#include "layout.h"
#include "js.h"

#define UNUSED_CONTEXT(x) ((void)(x))

static int checks, failures;

static void ok(const char *name)
{
    checks++;
    printf("  ok    %s\n", name);
}

static void bad(const char *name, const char *detail)
{
    checks++;
    failures++;
    printf("  FEHLER %s - %s\n", name, detail);
}

static void expect_text(const char *name, const char *html,
                        const char *selector, const char *want)
{
    struct document doc;
    char buffer[2048];
    struct node *found[4];

    document_init(&doc);
    html_build(&doc, html, strlen(html));

    if (dom_query(doc.root, selector, found, 4) == 0) {
        bad(name, "nichts gefunden");
        document_free(&doc);
        return;
    }
    dom_text_content(found[0], buffer, sizeof(buffer));
    if (strcmp(buffer, want) != 0) {
        char detail[512];

        snprintf(detail, sizeof(detail), "\"%s\" statt \"%s\"", buffer, want);
        bad(name, detail);
    } else {
        ok(name);
    }
    document_free(&doc);
}

static void expect_count(const char *name, const char *html,
                         const char *selector, size_t want)
{
    struct document doc;
    struct node *found[64];

    document_init(&doc);
    html_build(&doc, html, strlen(html));

    size_t n = dom_query(doc.root, selector, found, 64);

    if (n != want) {
        char detail[128];

        snprintf(detail, sizeof(detail), "%zu statt %zu Treffer", n, want);
        bad(name, detail);
    } else {
        ok(name);
    }
    document_free(&doc);
}

/* Prueft eine berechnete Stileigenschaft. */
static void expect_style(const char *name, const char *html,
                         const char *selector, const char *property,
                         int64_t want)
{
    struct document doc;
    struct stylesheet *sheet = css_create();
    struct node *found[4];

    document_init(&doc);
    html_build(&doc, html, strlen(html));

    /* Alle style-Bloecke einsammeln. */
    for (size_t i = 0; ; i++) {
        struct node *style = dom_by_tag(doc.root, "style", i);

        if (!style)
            break;

        char text[8192];

        dom_raw_text(style, text, sizeof(text));
        css_add(sheet, text, strlen(text));
    }
    css_apply(sheet, doc.root, 16);

    if (dom_query(doc.root, selector, found, 4) == 0) {
        bad(name, "nichts gefunden");
        goto done;
    }

    struct style *st = &found[0]->style;
    int64_t got;

    if (strcmp(property, "color") == 0)
        got = st->color;
    else if (strcmp(property, "background") == 0)
        got = st->background;
    else if (strcmp(property, "font-size") == 0)
        got = st->font_size;
    else if (strcmp(property, "bold") == 0)
        got = st->bold;
    else if (strcmp(property, "italic") == 0)
        got = st->italic;
    else if (strcmp(property, "display") == 0)
        got = st->display;
    else if (strcmp(property, "align") == 0)
        got = st->align;
    else if (strcmp(property, "margin-top") == 0)
        got = st->margin[0];
    else if (strcmp(property, "padding-left") == 0)
        got = st->padding[3];
    else if (strcmp(property, "border-top") == 0)
        got = st->border[0];
    else if (strcmp(property, "width") == 0)
        got = st->width.value;
    else
        got = -1;

    if (got != want) {
        char detail[160];

        snprintf(detail, sizeof(detail), "%s = %lld statt %lld", property,
                 (long long)got, (long long)want);
        bad(name, detail);
    } else {
        ok(name);
    }

done:
    css_free(sheet);
    document_free(&doc);
}

/* Baut eine Seite auf und misst den Umbruch. */
static void expect_layout(const char *name, const char *html, int32_t width,
                          int32_t min_height, int32_t max_height)
{
    struct document doc;
    struct stylesheet *sheet = css_create();
    struct layout out;

    memset(&out, 0, sizeof(out));
    document_init(&doc);
    html_build(&doc, html, strlen(html));

    for (size_t i = 0; ; i++) {
        struct node *style = dom_by_tag(doc.root, "style", i);

        if (!style)
            break;

        char text[8192];

        dom_raw_text(style, text, sizeof(text));
        css_add(sheet, text, strlen(text));
    }
    css_apply(sheet, doc.root, 16);
    layout_run(&out, doc.body, width, NULL, NULL);

    if (out.height < min_height || out.height > max_height) {
        char detail[160];

        snprintf(detail, sizeof(detail), "Hoehe %d, erwartet %d bis %d",
                 out.height, min_height, max_height);
        bad(name, detail);
    } else {
        ok(name);
    }
    layout_free(&out);
    css_free(sheet);
    document_free(&doc);
}

/* Prueft, wo ein bestimmtes Wort nach dem Umbruch landet. */
static void expect_place(const char *name, const char *html, int32_t width,
                         const char *word, int32_t want_x, int32_t toleranz)
{
    struct document doc;
    struct stylesheet *sheet = css_create();
    struct layout out;

    memset(&out, 0, sizeof(out));
    document_init(&doc);
    html_build(&doc, html, strlen(html));

    for (size_t i = 0; ; i++) {
        struct node *style = dom_by_tag(doc.root, "style", i);

        if (!style)
            break;

        char text[8192];

        dom_raw_text(style, text, sizeof(text));
        css_add(sheet, text, strlen(text));
    }
    css_apply(sheet, doc.root, 16);
    layout_run(&out, doc.body, width, NULL, NULL);

    int32_t got = -100000;

    for (size_t i = 0; i < out.count; i++)
        if (out.items[i].kind == FRAG_TEXT && out.items[i].text &&
            strcmp(out.items[i].text, word) == 0) {
            got = out.items[i].rect.x;
            break;
        }

    int32_t abstand = got > want_x ? got - want_x : want_x - got;

    if (got == -100000) {
        bad(name, "Wort nicht gefunden");
    } else if (abstand > toleranz) {
        char detail[160];

        snprintf(detail, sizeof(detail), "x = %d, erwartet %d (+/- %d)",
                 got, want_x, toleranz);
        bad(name, detail);
    } else {
        ok(name);
    }
    layout_free(&out);
    css_free(sheet);
    document_free(&doc);
}

/* Laesst ein Skript auf einem Dokument laufen und prueft den Text. */
static void expect_script(const char *name, const char *html,
                          const char *script, const char *selector,
                          const char *want)
{
    struct document doc;
    struct js_context *ctx = js_create();
    struct node *found[4];
    char buffer[2048];

    document_init(&doc);
    html_build(&doc, html, strlen(html));
    js_bind_document(ctx, &doc);

    if (!js_run(ctx, script, strlen(script))) {
        bad(name, js_error(ctx) ? js_error(ctx) : "Skript misslungen");
        goto done;
    }
    if (dom_query(doc.root, selector, found, 4) == 0) {
        bad(name, "nichts gefunden");
        goto done;
    }
    dom_text_content(found[0], buffer, sizeof(buffer));
    if (strcmp(buffer, want) != 0) {
        char detail[512];

        snprintf(detail, sizeof(detail), "\"%s\" statt \"%s\"", buffer, want);
        bad(name, detail);
    } else {
        ok(name);
    }

done:
    js_destroy(ctx);
    document_free(&doc);
}

/* Prueft die Zeitgeber: nach genug Takten muss der Text stehen. */
static bool zeitgeber_lief;

static void zeitgeber_hook(void *context)
{
    UNUSED_CONTEXT(context);
    zeitgeber_lief = true;
}

static void expect_timer(const char *name, const char *script,
                         uint64_t bis_ms, const char *want)
{
    struct document doc;
    struct js_context *ctx = js_create();
    char buffer[512];

    zeitgeber_lief = false;
    document_init(&doc);
    html_build(&doc, "<body><p id='t'>-</p></body>", 28);
    js_bind_document(ctx, &doc);
    js_on_change(ctx, zeitgeber_hook, NULL);

    if (!js_run(ctx, script, strlen(script))) {
        bad(name, js_error(ctx) ? js_error(ctx) : "Skript misslungen");
        goto done;
    }
    for (uint64_t t = 0; t <= bis_ms; t += 100)
        js_run_timers(ctx, t);

    dom_text_content(dom_by_id(doc.root, "t"), buffer, sizeof(buffer));
    if (strcmp(buffer, want) != 0) {
        char detail[512];

        snprintf(detail, sizeof(detail), "\"%s\" statt \"%s\"", buffer, want);
        bad(name, detail);
    } else if (!zeitgeber_lief) {
        bad(name, "die Aenderung wurde nicht gemeldet");
    } else {
        ok(name);
    }

done:
    js_destroy(ctx);
    document_free(&doc);
}

int main(void)
{
    printf("\nDokumentbaum\n");

    expect_text("Einfacher Absatz", "<p>Hallo</p>", "p", "Hallo");
    expect_text("Verschachtelung",
                "<div><p>Erst <b>fett</b> dann</p></div>", "div p",
                "Erst fett dann");
    expect_text("Fehlende Schlusszeichen",
                "<ul><li>eins<li>zwei<li>drei</ul>", "li", "eins");
    expect_text("Zeichenverweise",
                "<p>Gr&uuml;&szlig;e &amp; Dank &#65;</p>", "p",
                "Gr\xfc\xdf""e & Dank A");
    expect_text("Kommentare uebergehen",
                "<p>vor<!-- versteckt -->nach</p>", "p", "vornach");
    expect_text("Skript ist kein Text",
                "<div>sichtbar<script>var x = '<p>nein</p>'</script></div>",
                "div", "sichtbar");
    expect_text("Titel",
                "<html><head><title>Seitentitel</title></head><body>x</body></html>",
                "title", "Seitentitel");
    expect_text("UTF-8 wird umgesetzt",
                "<p>Gr\xc3\xbc\xc3\x9f""e</p>", "p", "Gr\xfc\xdf""e");

    expect_count("Alle Absaetze", "<p>a</p><p>b</p><p>c</p>", "p", 3);
    expect_count("Nach Klasse",
                 "<p class='x'>a</p><p>b</p><span class='x'>c</span>", ".x", 2);
    expect_count("Nach Kennung", "<div id='eins'></div><div id='zwei'></div>",
                 "#eins", 1);
    expect_count("Nachfahren",
                 "<div><p><b>a</b></p></div><p><b>b</b></p>", "div p b", 1);
    expect_count("Verbund", "<p class='a b'>x</p>", "p.a.b", 1);
    expect_count("Tabellenzellen",
                 "<table><tr><td>1</td><td>2</td></tr>"
                 "<tr><td>3</td><td>4</td></tr></table>", "td", 4);

    printf("\nFormatvorlagen\n");

    expect_style("Farbe aus einer Regel",
                 "<style>p { color: #ff0000 }</style><p>x</p>", "p",
                 "color", 0xFF0000);
    expect_style("Farbname",
                 "<style>p { color: navy }</style><p>x</p>", "p",
                 "color", 0x000080);
    expect_style("rgb-Schreibweise",
                 "<style>p { color: rgb(1, 2, 3) }</style><p>x</p>", "p",
                 "color", 0x010203);
    expect_style("Kurzes Hexadezimal",
                 "<style>p { color: #abc }</style><p>x</p>", "p",
                 "color", 0xAABBCC);
    expect_style("Klasse schlaegt Element",
                 "<style>p { color: red } .x { color: blue }</style>"
                 "<p class='x'>y</p>", "p", "color", 0x0000FF);
    expect_style("Kennung schlaegt Klasse",
                 "<style>#a { color: lime } .x { color: blue }</style>"
                 "<p class='x' id='a'>y</p>", "p", "color", 0x00FF00);
    expect_style("style-Attribut gewinnt",
                 "<style>#a { color: lime }</style>"
                 "<p id='a' style='color: white'>y</p>", "p",
                 "color", 0xFFFFFF);
    expect_style("wichtig gewinnt trotzdem",
                 "<style>#a { color: lime !important }</style>"
                 "<p id='a' style='color: white'>y</p>", "p",
                 "color", 0x00FF00);
    expect_style("Vererbung der Farbe",
                 "<style>div { color: teal }</style><div><p>x</p></div>",
                 "p", "color", 0x008080);
    expect_style("Schriftgroesse in em",
                 "<style>div { font-size: 20px } p { font-size: 1.5em }</style>"
                 "<div><p>x</p></div>", "p", "font-size", 30);
    expect_style("Ueberschrift ist fett",
                 "<h1>x</h1>", "h1", "bold", 1);
    expect_style("em ist kursiv", "<em>x</em>", "em", "italic", 1);
    expect_style("div ist ein Block", "<div>x</div>", "div",
                 "display", DISPLAY_BLOCK);
    expect_style("span bleibt inline", "<span>x</span>", "span",
                 "display", DISPLAY_INLINE);
    expect_style("Ausrichtung erbt",
                 "<style>div { text-align: center }</style><div><p>x</p></div>",
                 "p", "align", ALIGN_CENTER);
    expect_style("Abstand als Kurzschreibweise",
                 "<style>p { margin: 5px 10px 15px 20px }</style><p>x</p>",
                 "p", "margin-top", 5);
    expect_style("Innenabstand",
                 "<style>p { padding: 4px }</style><p>x</p>", "p",
                 "padding-left", 4);
    expect_style("Rahmen in Kurzform",
                 "<style>p { border: 2px solid red }</style><p>x</p>", "p",
                 "border-top", 2);
    expect_style("Breite in Pixeln",
                 "<style>p { width: 300px }</style><p>x</p>", "p",
                 "width", 300);
    expect_style("Regel in einer Bildschirmabfrage",
                 "<style>@media screen { p { color: red } }</style><p>x</p>",
                 "p", "color", 0xFF0000);
    expect_style("Mehrere Selektoren",
                 "<style>h1, h2, p { color: purple }</style><p>x</p>", "p",
                 "color", 0x800080);

    expect_style("Rechnen mit calc",
                 "<style>p { width: calc(100px + 50px) }</style><p>x</p>",
                 "p", "width", 150);
    expect_style("calc mit Vervielfachen",
                 "<style>p { font-size: calc(10px * 3) }</style><p>x</p>",
                 "p", "font-size", 30);
    expect_style("calc mit Schriftmass",
                 "<style>div { font-size: 20px }"
                 "p { font-size: calc(1em + 4px) }</style><div><p>x</p></div>",
                 "p", "font-size", 24);
    expect_style("calc mit Klammern",
                 "<style>p { width: calc((10px + 20px) * 2) }</style><p>x</p>",
                 "p", "width", 60);
    expect_style("Kleinstes von mehreren",
                 "<style>p { font-size: min(40px, 18px, 30px) }</style><p>x</p>",
                 "p", "font-size", 18);
    expect_style("Groesstes von mehreren",
                 "<style>p { font-size: max(12px, 22px) }</style><p>x</p>",
                 "p", "font-size", 22);
    expect_style("Eingegrenzt",
                 "<style>p { font-size: clamp(14px, 40px, 20px) }</style>"
                 "<p>x</p>", "p", "font-size", 20);

    expect_style("Eigene Eigenschaft",
                 "<style>:root { --gross: 28px } p { font-size: var(--gross) }"
                 "</style><p>x</p>", "p", "font-size", 28);
    expect_style("Eigene Eigenschaft vererbt sich",
                 "<style>body { --ton: #123456 } p { color: var(--ton) }"
                 "</style><body><div><p>x</p></div></body>",
                 "p", "color", 0x123456);
    expect_style("Eigene Eigenschaft wird ueberschrieben",
                 "<style>body { --ton: red } div { --ton: blue }"
                 "p { color: var(--ton) }</style>"
                 "<body><div><p>x</p></div></body>", "p", "color", 0x0000FF);
    expect_style("Ersatzwert greift",
                 "<style>p { color: var(--fehlt, green) }</style><p>x</p>",
                 "p", "color", 0x008000);
    expect_style("Eigene Eigenschaft im Rechenausdruck",
                 "<style>:root { --mass: 12px }"
                 "p { font-size: calc(var(--mass) * 2) }</style><p>x</p>",
                 "p", "font-size", 24);
    expect_style("Eigene Eigenschaft aus dem style-Attribut",
                 "<div style=\"--ton: #abcdef\">"
                 "<p style=\"color: var(--ton)\">x</p></div>",
                 "p", "color", 0xABCDEF);

    printf("\nUmbruch\n");

    expect_layout("Leere Seite", "<body></body>", 600, 0, 8);
    expect_layout("Eine Zeile", "<p>Hallo Welt</p>", 600, 20, 60);
    expect_layout("Umbruch bei langem Text",
                  "<p>Wort Wort Wort Wort Wort Wort Wort Wort Wort Wort "
                  "Wort Wort Wort Wort Wort Wort Wort Wort Wort Wort</p>",
                  200, 60, 200);
    expect_layout("Feste Hoehe",
                  "<div style='height: 400px'></div>", 600, 400, 420);
    expect_layout("Aufzaehlung",
                  "<ul><li>eins</li><li>zwei</li><li>drei</li></ul>", 600,
                  60, 160);
    expect_layout("Liste mit Verweisen bricht um",
                  "<ul><li><a href='#'>eins</a></li>"
                  "<li><a href='#'>zwei</a></li>"
                  "<li><a href='#'>drei</a></li></ul>", 600, 60, 160);
    expect_layout("Absaetze stapeln sich",
                  "<p>eins</p><p>zwei</p><p>drei</p>", 600, 100, 200);
    expect_layout("Inline reisst den Fluss nicht ab",
                  "<div><span>a</span></div><div><span>b</span></div>"
                  "<div><span>c</span></div>", 600, 50, 130);
    expect_layout("Verstecktes zaehlt nicht",
                  "<p style='display: none'>x</p>", 600, 0, 8);

    expect_place("Text beginnt links", "<p>Wort</p>", 600, "Wort", 0, 1);
    expect_place("Einrueckung wirkt",
                 "<div style='padding-left: 40px'><p>Wort</p></div>", 600,
                 "Wort", 40, 1);
    expect_place("Mittig ausgerichtet",
                 "<p style='text-align:center'>Wort</p>", 600,
                 "Wort", (600 - 4 * 8) / 2, 2);
    expect_place("Rechts ausgerichtet",
                 "<p style='text-align:right'>Wort</p>", 600,
                 "Wort", 600 - 4 * 8, 2);
    expect_place("Ein mittiger Kasten reisst die Zeile nicht mit",
                 "<div><span style='display:inline-block;width:80px;"
                 "text-align:center'>x</span>Wort</div>", 600, "Wort", 80, 4);
    /* Der Kasten rutscht nach rechts; sein Inhalt wandert mit. Die
     * Ausrichtung wird dabei vererbt, darum steht "x" innen wieder
     * rechts - eigene Ausrichtung im Kasten haelt ihn links. */
    expect_place("Inhalt eines Kastens wandert mit",
                 "<div style='text-align:right'>"
                 "<span style='display:inline-block;width:100px;"
                 "text-align:left'>x</span></div>", 600, "x", 500, 4);
    expect_place("Ausrichtung wird in den Kasten vererbt",
                 "<div style='text-align:right'>"
                 "<span style='display:inline-block;width:100px'>x</span>"
                 "</div>", 600, "x", 592, 4);

    printf("\nSkripte auf dem Baum\n");

    expect_script("Text setzen", "<p id='a'>alt</p>",
                  "document.getElementById('a').textContent = 'neu'",
                  "p", "neu");
    expect_script("HTML setzen", "<div id='a'></div>",
                  "document.getElementById('a').innerHTML = '<b>fett</b>'",
                  "div b", "fett");
    expect_script("Knoten anhaengen", "<div id='a'></div>",
                  "var p = document.createElement('p');"
                  "p.textContent = 'dazu';"
                  "document.getElementById('a').appendChild(p)",
                  "div p", "dazu");
    expect_script("Auswahl ueber querySelector",
                  "<div><span class='x'>alt</span></div>",
                  "document.querySelector('.x').textContent = 'neu'",
                  "span", "neu");
    expect_script("Mehrere aendern",
                  "<ul><li>a</li><li>b</li><li>c</li></ul>",
                  "var alle = document.querySelectorAll('li');"
                  "for (var i = 0; i < alle.length; i++)"
                  "  alle[i].textContent = 'x' + i;",
                  "li", "x0");
    expect_script("Attribut setzen", "<p id='a'>x</p>",
                  "var p = document.getElementById('a');"
                  "p.setAttribute('title', 'Hinweis');"
                  "p.textContent = p.getAttribute('title')",
                  "p", "Hinweis");
    expect_script("Klassen verwalten", "<p id='a' class='alt'>x</p>",
                  "var p = document.getElementById('a');"
                  "p.classList.add('neu'); p.classList.remove('alt');"
                  "p.textContent = p.className",
                  "p", "neu");
    expect_script("Stil setzen und lesen", "<p id='a'>x</p>",
                  "var p = document.getElementById('a');"
                  "p.style.color = 'red';"
                  "p.textContent = p.style.color",
                  "p", "red");
    expect_script("Liste aufbauen", "<ul id='l'></ul>",
                  "var l = document.getElementById('l');"
                  "['a','b','c'].forEach(function (t) {"
                  "  var li = document.createElement('li');"
                  "  li.textContent = t; l.appendChild(li) })",
                  "li", "a");
    expect_script("Knoten entfernen",
                  "<div id='d'><p>eins</p><p>zwei</p></div>",
                  "var d = document.getElementById('d');"
                  "d.removeChild(d.children[0])",
                  "div p", "zwei");
    expect_script("Kinder zaehlen",
                  "<div id='d'><p>a</p><p>b</p><p>c</p></div><output></output>",
                  "document.querySelector('output').textContent ="
                  "  document.getElementById('d').children.length",
                  "output", "3");

    printf("\nZeitgeber\n");

    expect_timer("Einmaliger Zeitgeber",
                 "setTimeout(function () {"
                 "  document.getElementById('t').textContent = 'spaet' }, 300)",
                 1000, "spaet");
    expect_timer("Wiederkehrender Zeitgeber",
                 "var n = 0;"
                 "setInterval(function () { n++;"
                 "  document.getElementById('t').textContent = 'Takt ' + n;"
                 "}, 500)", 2600, "Takt 5");
    expect_timer("Abgebrochener Zeitgeber",
                 "var id = setTimeout(function () {"
                 "  document.getElementById('t').textContent = 'nie' }, 300);"
                 "clearTimeout(id);"
                 "setTimeout(function () {"
                 "  document.getElementById('t').textContent = 'doch' }, 600)",
                 1200, "doch");

    printf("\n%d Pruefungen, %d Fehler\n\n", checks, failures);
    return failures ? 1 : 0;
}
