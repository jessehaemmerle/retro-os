/* web_test.c - prueft das Rechnen hinter dem Browser: Adressen, Kopfzeilen,
 * Stueckelung, Cookies und Formulare.
 *
 * "Die Seite laedt nicht" hat fast nie einen Netzfehler zur Ursache. Es
 * ist eine Weiterleitung auf eine relative Adresse, die niemand
 * zusammensetzt; ein Cookie, das beim Zuruecksenden fehlt, sodass der
 * Server eine Einwilligungsseite statt der Suche schickt; ein "&" im
 * Suchwort, das das Formular in zwei Felder spaltet. Alles das laesst
 * sich ohne Netz und ohne Bildschirm pruefen - mit den Kopfzeilen, die
 * echte Server schicken.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cookiejar.h"
#include "formutil.h"
#include "httputil.h"

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

static void pruefe_text(const char *was, const char *soll, const char *ist)
{
    geprueft++;
    if (strcmp(soll, ist) != 0) {
        printf("  FEHLER: %s: erwartet \"%s\", bekommen \"%s\"\n", was, soll, ist);
        fehler++;
    }
}

/* --- Adressen zerlegen -------------------------------------------------- */

static void test_url_split(void)
{
    char host[128], path[512];
    uint16_t port;
    bool secure;

    printf("Adressen zerlegen\n");

    pruefe("einfach", url_split("http://example.com/a/b", host, sizeof(host), &port, path, sizeof(path), &secure));
    pruefe_text("Rechner", "example.com", host);
    pruefe_text("Pfad", "/a/b", path);
    pruefe_zahl("Port 80", 80, port);
    pruefe("unverschluesselt", !secure);

    url_split("https://example.com", host, sizeof(host), &port, path, sizeof(path), &secure);
    pruefe_zahl("Port 443", 443, port);
    pruefe("verschluesselt", secure);
    pruefe_text("Pfad Wurzel", "/", path);

    url_split("http://localhost:8080/x?y=1", host, sizeof(host), &port, path, sizeof(path), &secure);
    pruefe_text("Rechner mit Port", "localhost", host);
    pruefe_zahl("Port 8080", 8080, port);
    pruefe_text("Pfad mit Anfrage", "/x?y=1", path);

    url_split("http://example.com?q=1", host, sizeof(host), &port, path, sizeof(path), &secure);
    pruefe_text("Anfrage ohne Pfad: Rechner", "example.com", host);
    pruefe_text("Anfrage ohne Pfad: Wurzel", "/?q=1", path);

    url_split("http://example.com/a#marke", host, sizeof(host), &port, path, sizeof(path), &secure);
    pruefe_text("Marke faellt weg", "/a", path);

    url_split("http://example.com#marke", host, sizeof(host), &port, path, sizeof(path), &secure);
    pruefe_text("Marke ohne Pfad", "/", path);

    url_split("example.com/x", host, sizeof(host), &port, path, sizeof(path), &secure);
    pruefe_text("ohne Schema", "example.com", host);
    pruefe("ohne Schema nicht verschluesselt", !secure);

    url_split("HTTPS://Example.COM/", host, sizeof(host), &port, path, sizeof(path), &secure);
    pruefe("Schema gross", secure);

    pruefe("leerer Rechner", !url_split("http:///x", host, sizeof(host), &port, path, sizeof(path), &secure));
    pruefe("Port null", !url_split("http://a:0/", host, sizeof(host), &port, path, sizeof(path), &secure));
    pruefe("Port zu gross", !url_split("http://a:70000/", host, sizeof(host), &port, path, sizeof(path), &secure));
    pruefe("Port riesig", !url_split("http://a:99999999999/", host, sizeof(host), &port, path, sizeof(path), &secure));
    pruefe("ganz leer", !url_split("", host, sizeof(host), &port, path, sizeof(path), &secure));
}

static void test_url_resolve(void)
{
    char out[512];

    printf("Adressen zusammensetzen\n");

    url_resolve("http://a.de/x/y/z.html", "w.html", out, sizeof(out));
    pruefe_text("im selben Ordner", "http://a.de/x/y/w.html", out);

    url_resolve("http://a.de/x/y/z.html", "../w.html", out, sizeof(out));
    pruefe_text("einen Ordner hoch", "http://a.de/x/w.html", out);

    url_resolve("http://a.de/x/y/z.html", "../../../w.html", out, sizeof(out));
    pruefe_text("ueber die Wurzel hinaus", "http://a.de/w.html", out);

    url_resolve("http://a.de/x/y/z.html", "./w.html", out, sizeof(out));
    pruefe_text("Punkt", "http://a.de/x/y/w.html", out);

    url_resolve("http://a.de/x/y/z.html", "a/./b/../c", out, sizeof(out));
    pruefe_text("Punkte mittendrin", "http://a.de/x/y/a/c", out);

    url_resolve("http://a.de/x/y/z.html", "/w.html", out, sizeof(out));
    pruefe_text("absoluter Pfad", "http://a.de/w.html", out);

    url_resolve("https://a.de/x", "//b.de/p", out, sizeof(out));
    pruefe_text("ohne Schema uebernimmt es", "https://b.de/p", out);

    url_resolve("http://a.de/x", "https://b.de/p#m", out, sizeof(out));
    pruefe_text("absolut, Marke faellt weg", "https://b.de/p", out);

    url_resolve("http://a.de/x/y?q=1/2", "z", out, sizeof(out));
    pruefe_text("Schraegstrich in der Anfrage der Basis", "http://a.de/x/z", out);

    url_resolve("http://a.de/x/y?q=1", "?r=2", out, sizeof(out));
    pruefe_text("nur Anfrage", "http://a.de/x/y?r=2", out);

    url_resolve("http://a.de/x/y?q=1", "#m", out, sizeof(out));
    pruefe_text("nur Marke", "http://a.de/x/y?q=1", out);

    url_resolve("http://a.de/x/y#alt", "", out, sizeof(out));
    pruefe_text("leer: Basis ohne Marke", "http://a.de/x/y", out);

    url_resolve("http://a.de:8080/x/y", "z", out, sizeof(out));
    pruefe_text("Port bleibt", "http://a.de:8080/x/z", out);

    url_resolve("http://a.de", "z", out, sizeof(out));
    pruefe_text("Basis ohne Pfad", "http://a.de/z", out);

    url_resolve("http://a.de/x/y", "/s?q=a/../b", out, sizeof(out));
    pruefe_text("Punkte in der Anfrage bleiben", "http://a.de/s?q=a/../b", out);

    url_resolve("http://www.google.com/", "/search?q=x&hl=de", out, sizeof(out));
    pruefe_text("Suche", "http://www.google.com/search?q=x&hl=de", out);

    url_resolve("http://a.de/x/y", "  w  ", out, sizeof(out));
    pruefe("Leerraum vorn wird ignoriert", strncmp(out, "http://a.de/x/w", 15) == 0);

    url_resolve("http://a.de/x/", "..", out, sizeof(out));
    pruefe_text("nur zwei Punkte", "http://a.de/", out);
}

/* --- Kopfzeilen und Stueckelung ---------------------------------------- */

static const char kopf[] =
    "HTTP/1.1 200 OK\r\n"
    "content-type: text/html; charset=ISO-8859-1\r\n"
    "Set-Cookie: a=1; path=/\r\n"
    "set-cookie: b=2; Secure\r\n"
    "SET-COOKIE: c=3\r\n"
    "Location:/neu\r\n"
    "X-Leer:\r\n"
    "Content-Encoding:   gzip  \r\n";

static void test_kopfzeilen(void)
{
    char v[128];

    printf("Kopfzeilen\n");

    pruefe("Typ gefunden", http_header_get(kopf, "Content-Type", 0, v, sizeof(v)));
    pruefe_text("Typ klein geschrieben", "text/html; charset=ISO-8859-1", v);

    pruefe("erstes Cookie", http_header_get(kopf, "Set-Cookie", 0, v, sizeof(v)));
    pruefe_text("a", "a=1; path=/", v);
    pruefe("zweites Cookie", http_header_get(kopf, "Set-Cookie", 1, v, sizeof(v)));
    pruefe_text("b", "b=2; Secure", v);
    pruefe("drittes Cookie", http_header_get(kopf, "Set-Cookie", 2, v, sizeof(v)));
    pruefe_text("c", "c=3", v);
    pruefe("kein viertes", !http_header_get(kopf, "Set-Cookie", 3, v, sizeof(v)));

    pruefe("ohne Leerzeichen nach dem Doppelpunkt", http_header_get(kopf, "Location", 0, v, sizeof(v)));
    pruefe_text("Ziel", "/neu", v);

    pruefe("leerer Wert", http_header_get(kopf, "X-Leer", 0, v, sizeof(v)));
    pruefe_text("leer", "", v);

    pruefe("Wert ohne Leerraum am Anfang", http_header_get(kopf, "Content-Encoding", 0, v, sizeof(v)));
    pruefe_text("gzip", "gzip  ", v);

    pruefe("fehlt", !http_header_get(kopf, "ETag", 0, v, sizeof(v)));
    pruefe_text("bei Fehlen leer", "", v);

    /* Die Statuszeile ist keine Kopfzeile. */
    pruefe("Statuszeile zaehlt nicht", !http_header_get("Server: x\r\n", "Server", 0, v, sizeof(v)));
    pruefe("nur Statuszeile", !http_header_get("HTTP/1.1 200 OK", "Server", 0, v, sizeof(v)));

    /* Gleiche Laenge, anderer Name: Der Doppelpunkt an der richtigen
     * Stelle genuegt nicht. */
    pruefe("gleich lang, anders benannt", !http_header_get("HTTP/1.1 200\r\nVia-ab: 1\r\n", "Server", 0, v, sizeof(v)));

    /* Ein Name, der nur Anfang eines anderen ist, trifft nicht. */
    pruefe("Teilname trifft nicht", !http_header_get("HTTP/1.1 200\r\nContent-Type-X: a\r\n", "Content-Type", 0, v, sizeof(v)));

    char klein[5];

    pruefe("zu kleiner Puffer", http_header_get(kopf, "Content-Type", 0, klein, sizeof(klein)));
    pruefe_text("abgeschnitten", "text", klein);

    pruefe_zahl("Status 200", 200, http_status_code("HTTP/1.1 200 OK\r\n"));
    pruefe_zahl("Status 301", 301, http_status_code("HTTP/1.0 301 Moved"));
    pruefe_zahl("Status Unsinn", 0, http_status_code("Hallo"));
    pruefe_zahl("Status ohne Ziffern", 0, http_status_code("HTTP/1.1 abc"));
    pruefe_zahl("Status zu kurz", 0, http_status_code("HTTP/1.1 20"));
    pruefe_zahl("Status ohne Leerzeichen", 0, http_status_code("HTTP/1.1"));
}

static void test_kodierung(void)
{
    printf("Kodierung\n");

    pruefe_zahl("gzip", ENC_GZIP, http_content_encoding("H\r\nContent-Encoding: gzip\r\n"));
    pruefe_zahl("x-gzip", ENC_GZIP, http_content_encoding("H\r\nContent-Encoding: x-gzip\r\n"));
    pruefe_zahl("GZIP gross", ENC_GZIP, http_content_encoding("H\r\ncontent-encoding: GZIP\r\n"));
    pruefe_zahl("deflate", ENC_DEFLATE, http_content_encoding("H\r\nContent-Encoding: deflate\r\n"));
    pruefe_zahl("identity", ENC_IDENTITY, http_content_encoding("H\r\nContent-Encoding: identity\r\n"));
    pruefe_zahl("ohne Angabe", ENC_IDENTITY, http_content_encoding("H\r\nServer: x\r\n"));
    pruefe_zahl("Brotli unbekannt", ENC_UNKNOWN, http_content_encoding("H\r\nContent-Encoding: br\r\n"));
    pruefe_zahl("zwei Kodierungen unbekannt", ENC_UNKNOWN, http_content_encoding("H\r\nContent-Encoding: gzip, br\r\n"));
}

static uint32_t entstuecken(const char *eingabe, char *aus)
{
    size_t n = strlen(eingabe);

    memcpy(aus, eingabe, n + 1);
    return http_decode_chunked(aus, (uint32_t)n);
}

static void test_stuecke(void)
{
    char b[256];
    uint32_t n;

    printf("Stueckelung\n");

    n = entstuecken("5\r\nHallo\r\n6\r\n Welt!\r\n0\r\n\r\n", b);
    pruefe_zahl("zwei Stuecke", 11, n);
    pruefe("Inhalt", memcmp(b, "Hallo Welt!", 11) == 0);

    n = entstuecken("A\r\n0123456789\r\n0\r\n\r\n", b);
    pruefe_zahl("Hex gross", 10, n);
    n = entstuecken("a\r\n0123456789\r\n0\r\n\r\n", b);
    pruefe_zahl("Hex klein", 10, n);

    n = entstuecken("5;name=wert\r\nHallo\r\n0\r\n\r\n", b);
    pruefe_zahl("Erweiterung", 5, n);

    n = entstuecken("5\r\nHallo\r\n0\r\nX-Trailer: 1\r\n\r\n", b);
    pruefe_zahl("Anhang nach dem Ende", 5, n);

    n = entstuecken("0\r\n\r\n", b);
    pruefe_zahl("leer", 0, n);

    n = entstuecken("", b);
    pruefe_zahl("gar nichts", 0, n);

    n = entstuecken("5\r\nHallo\r\n5\r\nWe", b);
    pruefe_zahl("abgeschnitten", 7, n);
    pruefe("Rest da", memcmp(b, "HalloWe", 7) == 0);

    n = entstuecken("5\r\nHallo\r\nZZ\r\nxxxx", b);
    pruefe_zahl("kaputte Groesse endet", 5, n);

    n = entstuecken("FFFFFFFF\r\nabc", b);
    pruefe("riesige Groesse bleibt in den Grenzen", n <= 3);

    n = entstuecken("123456789\r\nabc", b);
    pruefe("zu viele Stellen", n == 0);

    n = entstuecken("3\nabc\n0\n\n", b);
    pruefe_zahl("nur Zeilenvorschub", 3, n);

    /* Auch die ganz grosse Antwort mit vielen kleinen Stuecken. */
    char *gross = malloc(40000), *kopie = malloc(40000);
    size_t at = 0;

    for (int i = 0; i < 2000; i++)
        at += (size_t)sprintf(gross + at, "A\r\n0123456789\r\n");
    at += (size_t)sprintf(gross + at, "0\r\n\r\n");
    memcpy(kopie, gross, at + 1);
    pruefe_zahl("viele Stuecke", 20000, http_decode_chunked(kopie, (uint32_t)at));
    free(gross);
    free(kopie);
}

/* --- Cookies ------------------------------------------------------------ */

static struct cookie_jar J;

static void frisch(void) { cookie_jar_init(&J); }

static void sende(const char *host, const char *path, bool secure, char *out, size_t size)
{
    cookie_jar_header(&J, host, path, secure, 1000, out, size);
}

static void test_cookies(void)
{
    char h[512];

    printf("Cookies\n");

    /* So kommt es von Google: Domain mit Punkt, Secure, Strict. */
    frisch();
    pruefe("Google-Cookie angenommen", cookie_jar_store(&J, "www.google.com", "/",
        true, "__Secure-STRP=ABNhA6hA; expires=Wed, 25-Nov-2026 12:07:19 GMT; path=/; domain=.google.com; Secure; SameSite=strict", 0));
    sende("www.google.com", "/", true, h, sizeof(h));
    pruefe_text("zurueck an www", "__Secure-STRP=ABNhA6hA", h);
    sende("google.com", "/x", true, h, sizeof(h));
    pruefe_text("zurueck an die Hauptdomain", "__Secure-STRP=ABNhA6hA", h);
    sende("accounts.google.com", "/", true, h, sizeof(h));
    pruefe_text("an eine Unterdomain", "__Secure-STRP=ABNhA6hA", h);
    sende("www.google.com", "/", false, h, sizeof(h));
    pruefe_text("Secure nicht ueber http", "", h);
    sende("evil-google.com", "/", true, h, sizeof(h));
    pruefe_text("nicht an eine fremde Domain mit gleichem Ende", "", h);
    sende("notgoogle.com", "/", true, h, sizeof(h));
    pruefe_text("nicht an notgoogle.com", "", h);
    sende("google.com.evil.org", "/", true, h, sizeof(h));
    pruefe_text("nicht an google.com.evil.org", "", h);
    sende("GOOGLE.com", "/", true, h, sizeof(h));
    pruefe_text("Gross-/Kleinschreibung", "__Secure-STRP=ABNhA6hA", h);

    /* Ohne Domain: nur fuer genau diesen Rechner. */
    frisch();
    cookie_jar_store(&J, "www.example.com", "/", false, "k=v", 0);
    sende("www.example.com", "/", false, h, sizeof(h));
    pruefe_text("nur dieser Rechner", "k=v", h);
    sende("a.www.example.com", "/", false, h, sizeof(h));
    pruefe_text("nicht an die Unterdomain", "", h);
    sende("example.com", "/", false, h, sizeof(h));
    pruefe_text("nicht an die Oberdomain", "", h);

    /* Fremde Domain ablehnen. */
    frisch();
    pruefe("fremde Domain abgelehnt", !cookie_jar_store(&J, "www.a.de", "/", false, "k=v; Domain=b.de", 0));
    pruefe("Oberbegriff com abgelehnt", !cookie_jar_store(&J, "www.a.com", "/", false, "k=v; Domain=com", 0));
    pruefe("Domain ohne Punkt, die nicht der Rechner ist", !cookie_jar_store(&J, "x.a.de", "/", false, "k=v; Domain=de", 0));
    pruefe("einfacher Rechnername als Domain geht", cookie_jar_store(&J, "intranet", "/", false, "k=v; Domain=intranet", 0));
    pruefe("Unterdomain als Domain abgelehnt", !cookie_jar_store(&J, "a.de", "/", false, "k=v; Domain=www.a.de", 0));
    pruefe("Oberdomain angenommen", cookie_jar_store(&J, "www.a.de", "/", false, "k=v; Domain=a.de", 0));
    pruefe_zahl("zwei im Vorrat (Oberdomain und intranet)", 2, (long)cookie_jar_count(&J));

    /* Pfade. */
    frisch();
    cookie_jar_store(&J, "a.de", "/", false, "p=1; Path=/konto", 0);
    sende("a.de", "/konto", false, h, sizeof(h));
    pruefe_text("genau der Pfad", "p=1", h);
    sende("a.de", "/konto/seite", false, h, sizeof(h));
    pruefe_text("darunter", "p=1", h);
    sende("a.de", "/kontostand", false, h, sizeof(h));
    pruefe_text("nicht /kontostand", "", h);
    sende("a.de", "/", false, h, sizeof(h));
    pruefe_text("nicht darueber", "", h);
    sende("a.de", "/konto?x=1", false, h, sizeof(h));
    pruefe_text("mit Anfrage", "p=1", h);
    sende("a.de", "/abcde/x", false, h, sizeof(h));
    pruefe_text("anderer Pfad gleicher Laenge davor", "", h);

    frisch();
    cookie_jar_store(&J, "a.de", "/", false, "p=1; Path=/konto/", 0);
    sende("a.de", "/konto/x", false, h, sizeof(h));
    pruefe_text("Pfad mit Schraegstrich", "p=1", h);
    sende("a.de", "/konto", false, h, sizeof(h));
    pruefe_text("ohne den Schraegstrich nicht", "", h);

    /* Standardpfad: das Verzeichnis der Anfrage. */
    frisch();
    cookie_jar_store(&J, "a.de", "/blog/post/1?x=2", false, "d=1", 0);
    sende("a.de", "/blog/post/2", false, h, sizeof(h));
    pruefe_text("Standardpfad /blog/post", "d=1", h);
    sende("a.de", "/blog/anderes", false, h, sizeof(h));
    pruefe_text("nicht darueber", "", h);

    frisch();
    cookie_jar_store(&J, "a.de", "/seite", false, "d=1", 0);
    sende("a.de", "/", false, h, sizeof(h));
    pruefe_text("Standardpfad ist die Wurzel", "d=1", h);

    /* Secure von einer unverschluesselten Seite. */
    frisch();
    pruefe("Secure ueber http abgelehnt", !cookie_jar_store(&J, "a.de", "/", false, "s=1; Secure", 0));
    pruefe("Secure ueber https angenommen", cookie_jar_store(&J, "a.de", "/", true, "s=1; Secure", 0));

    /* Lebensdauer. */
    frisch();
    cookie_jar_store(&J, "a.de", "/", false, "m=1; Max-Age=60", 1000);
    cookie_jar_header(&J, "a.de", "/", false, 30000, h, sizeof(h));
    pruefe_text("vor Ablauf", "m=1", h);
    cookie_jar_header(&J, "a.de", "/", false, 61001, h, sizeof(h));
    pruefe_text("nach Ablauf", "", h);
    pruefe("Max-Age=0 loescht", !cookie_jar_store(&J, "a.de", "/", false, "m=1; Max-Age=0", 2000));
    pruefe_zahl("das abgelaufene ist weg", 0, (long)cookie_jar_count(&J));
    cookie_jar_store(&J, "a.de", "/", false, "m=1; Max-Age=60", 3000);
    cookie_jar_store(&J, "a.de", "/", false, "m=1; Max-Age=-5", 4000);
    cookie_jar_header(&J, "a.de", "/", false, 5000, h, sizeof(h));
    pruefe_text("negatives Alter loescht", "", h);

    frisch();
    cookie_jar_store(&J, "a.de", "/", false, "e=1", 0);
    pruefe("Expires 1970 loescht", !cookie_jar_store(&J, "a.de", "/", false, "e=; Expires=Thu, 01 Jan 1970 00:00:00 GMT", 0));
    pruefe_zahl("weg", 0, (long)cookie_jar_count(&J));

    /* Ersetzen statt Verdoppeln. */
    frisch();
    cookie_jar_store(&J, "a.de", "/", false, "x=1", 0);
    cookie_jar_store(&J, "a.de", "/", false, "x=2", 0);
    pruefe_zahl("ersetzt", 1, (long)cookie_jar_count(&J));
    sende("a.de", "/", false, h, sizeof(h));
    pruefe_text("neuer Wert", "x=2", h);

    /* Gleicher Name, anderer Pfad: zwei Cookies. */
    frisch();
    cookie_jar_store(&J, "a.de", "/", false, "x=wurzel; Path=/", 0);
    cookie_jar_store(&J, "a.de", "/", false, "x=tief; Path=/a/b", 0);
    sende("a.de", "/a/b/c", false, h, sizeof(h));
    pruefe_text("laengerer Pfad zuerst", "x=tief; x=wurzel", h);

    /* Reihenfolge bei gleichem Pfad: das aeltere zuerst. */
    frisch();
    cookie_jar_store(&J, "a.de", "/", false, "erst=1", 0);
    cookie_jar_store(&J, "a.de", "/", false, "zweit=2", 0);
    sende("a.de", "/", false, h, sizeof(h));
    pruefe_text("Reihenfolge", "erst=1; zweit=2", h);

    /* Syntax. */
    frisch();
    pruefe("ohne Gleichheitszeichen", !cookie_jar_store(&J, "a.de", "/", false, "kaputt", 0));
    pruefe("ohne Namen", !cookie_jar_store(&J, "a.de", "/", false, "=wert", 0));
    pruefe("leerer Wert ist erlaubt", cookie_jar_store(&J, "a.de", "/", false, "leer=", 0));
    pruefe("Leerzeichen gekappt", cookie_jar_store(&J, "a.de", "/", false, "  name  =  wert  ; Path = / ", 0));
    sende("a.de", "/", false, h, sizeof(h));
    pruefe_text("mit leerem Wert", "leer=; name=wert", h);

    frisch();
    cookie_jar_store(&J, "a.de", "/", false, "t=a=b=c", 0);
    sende("a.de", "/", false, h, sizeof(h));
    pruefe_text("Gleichheitszeichen im Wert", "t=a=b=c", h);

    char lang[600];

    memset(lang, 'x', sizeof(lang));
    memcpy(lang, "l=", 2);
    lang[sizeof(lang) - 1] = '\0';
    pruefe("zu langer Wert abgelehnt", !cookie_jar_store(&J, "a.de", "/", false, lang, 0));

    frisch();
    cookie_jar_store(&J, "a.de", "/", false, "ok=1; HttpOnly; SameSite=Lax; Priority=High", 0);
    sende("a.de", "/", false, h, sizeof(h));
    pruefe_text("unbekannte Merkmale stoeren nicht", "ok=1", h);

    /* IP-Adressen: Domain-Cookies gibt es nicht. */
    frisch();
    cookie_jar_store(&J, "10.0.2.2", "/", false, "ip=1; Domain=0.2.2", 0);
    pruefe_zahl("Domain bei IP abgelehnt", 0, (long)cookie_jar_count(&J));
    cookie_jar_store(&J, "10.0.2.2", "/", false, "ip=1", 0);
    sende("10.0.2.2", "/", false, h, sizeof(h));
    pruefe_text("bei IP: genau dieser Rechner", "ip=1", h);
    pruefe("Domain gleich der IP geht", cookie_jar_store(&J, "10.0.2.2", "/", false, "ip2=1; Domain=10.0.2.2", 0));

    /* Voll: Das aelteste weicht. */
    frisch();
    for (int i = 0; i < COOKIE_MAX + 5; i++) {
        char c[32];

        sprintf(c, "k%d=v", i);
        cookie_jar_store(&J, "a.de", "/", false, c, 0);
    }
    pruefe_zahl("Vorrat begrenzt", COOKIE_MAX, (long)cookie_jar_count(&J));
    sende("a.de", "/", false, h, sizeof(h));
    pruefe("das aelteste ist weg", strstr(h, "k0=v;") == NULL && strncmp(h, "k5=v", 4) == 0);
    pruefe("das neueste ist da", strstr(h, "k68=v") != NULL);

    /* Zu kleiner Ausgabepuffer: nur ganze Eintraege. */
    frisch();
    cookie_jar_store(&J, "a.de", "/", false, "aaaa=1111", 0);
    cookie_jar_store(&J, "a.de", "/", false, "bbbb=2222", 0);
    char klein[16];
    size_t n = cookie_jar_header(&J, "a.de", "/", false, 0, klein, sizeof(klein));

    pruefe_text("nur der erste passt", "aaaa=1111", klein);
    pruefe_zahl("Laenge", 9, (long)n);
    char winzig[4];

    n = cookie_jar_header(&J, "a.de", "/", false, 0, winzig, sizeof(winzig));
    pruefe_zahl("nichts passt", 0, (long)n);
}

/* --- Formulare ---------------------------------------------------------- */

static void test_formulare(void)
{
    char b[256];

    printf("Formulare\n");

    pruefe_zahl("Laenge", 10, (long)form_encode(b, sizeof(b), "Hallo Welt"));
    pruefe_text("Leerzeichen wird Plus", "Hallo+Welt", b);

    form_encode(b, sizeof(b), "Katz & Maus");
    pruefe_text("Und-Zeichen", "Katz+%26+Maus", b);

    form_encode(b, sizeof(b), "a=b&c=d");
    pruefe_text("Gleich und Und", "a%3Db%26c%3Dd", b);

    form_encode(b, sizeof(b), "a-b_c.d*e");
    pruefe_text("Unversehrte Zeichen", "a-b_c.d*e", b);

    form_encode(b, sizeof(b), "50% + 20%");
    pruefe_text("Prozent und Plus", "50%25+%2B+20%25", b);

    form_encode(b, sizeof(b), "\xC3\xA4\xC3\xB6");
    pruefe_text("UTF-8 Byte fuer Byte", "%C3%A4%C3%B6", b);

    form_encode(b, sizeof(b), "zeile1\r\nzeile2");
    pruefe_text("Zeilenumbruch", "zeile1%0D%0Azeile2", b);

    form_encode(b, sizeof(b), "a\nb");
    pruefe_text("einzelner Umbruch", "a%0Ab", b);

    form_encode(b, sizeof(b), "~/?#");
    pruefe_text("Sonderzeichen", "%7E%2F%3F%23", b);

    form_encode(b, sizeof(b), "");
    pruefe_text("leer", "", b);

    pruefe("zu wenig Platz", form_encode(b, 4, "abcdef") == (size_t)-1);
    pruefe("bei Fehler leer", b[0] == '\0');
    pruefe("genau passend", form_encode(b, 4, "abc") == 3);
    pruefe("Prozentfolge passt nicht", form_encode(b, 3, "&") == (size_t)-1);

    b[0] = '\0';
    pruefe("erstes Feld", form_append(b, sizeof(b), "q", "Katz & Maus"));
    pruefe("zweites Feld", form_append(b, sizeof(b), "hl", "de"));
    pruefe("leerer Wert", form_append(b, sizeof(b), "leer", ""));
    pruefe("fehlender Wert", form_append(b, sizeof(b), "nix", NULL));
    pruefe_text("zusammen", "q=Katz+%26+Maus&hl=de&leer=&nix=", b);

    b[0] = '\0';
    form_append(b, sizeof(b), "ae", "x");
    pruefe("Name wird kodiert", form_append(b, sizeof(b), "a&b", "c"));
    pruefe_text("kodierter Name", "ae=x&a%26b=c", b);

    char klein[12];

    klein[0] = '\0';
    pruefe("passt", form_append(klein, sizeof(klein), "a", "1"));
    pruefe("passt nicht", !form_append(klein, sizeof(klein), "langer", "wert"));
    pruefe_text("bei Fehler unveraendert", "a=1", klein);

    char u[256];

    pruefe("Adresse", form_get_url(u, sizeof(u), "http://a.de/suche", "q=1"));
    pruefe_text("mit Anfrage", "http://a.de/suche?q=1", u);
    form_get_url(u, sizeof(u), "http://a.de/suche?alt=1#m", "q=1");
    pruefe_text("alte Anfrage und Marke fallen weg", "http://a.de/suche?q=1", u);
    form_get_url(u, sizeof(u), "http://a.de/suche?alt=1", "");
    pruefe_text("ohne Felder", "http://a.de/suche", u);
    pruefe("zu klein", !form_get_url(u, 10, "http://a.de/suche", "q=1"));
}

int main(void)
{
    printf("=== Web ===\n");

    test_url_split();
    test_url_resolve();
    test_kopfzeilen();
    test_kodierung();
    test_stuecke();
    test_cookies();
    test_formulare();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
