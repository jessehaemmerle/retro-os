/* zeiger_test.c - prueft, was zwischen Maus und Mauszeiger liegt.
 *
 * Der Zeiger selbst laesst sich hier nicht pruefen - dafuer braucht es
 * einen Bildschirm. Was sich pruefen laesst, ist das Rechnen davor:
 * wie aus den Bytes einer PS/2-Maus Bewegung wird, wie aus der
 * Beschreibung eines Tabletts die Lage seiner Felder, und wie aus
 * Zustaenden und Flanken die Folge der Klicks. Genau dort stecken die
 * Fehler, die man spaeter nur als "der Zeiger tut nicht richtig"
 * wahrnimmt: ein Bit, das als ungueltig galt und mit dem Paket auch das
 * Loslassen einer Taste verschluckte; ein Feld, das um acht Bit
 * verschoben liegt, weil vorn eine Berichtsnummer steht; ein Klick, der
 * kuerzer war als ein Bild.
 */

#include <stdio.h>
#include <string.h>

#include "hidparse.h"
#include "mouseutil.h"

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

/* --- Beschreibungen, wie echte Geraete sie liefern ------------------- */

/* Das Tablett von QEMU (usb-tablet) - und, bis auf Kleinigkeiten, das
 * von VirtualBox: drei Tasten, X und Y absolut in 0..32767, ein Rad. */
static const uint8_t tablett[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x26, 0xFF, 0x7F, 0x35, 0x00, 0x46, 0xFF, 0x7F,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F,
    0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
};

/* Eine gewoehnliche Maus mit Rad: X, Y und Rad relativ, je ein Byte. */
static const uint8_t maus[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,
};

/* Dieselbe Maus, aber mit Berichtsnummer 1 vorn: Alles liegt um acht
 * Bit weiter hinten. */
static const uint8_t maus_mit_nummer[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,
};

/* Eine Tastatur: Sie hat weder X noch Y und darf nie als Zeiger
 * gelten. */
static const uint8_t tastatur[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

/* Ein Maximum, das als 0xFF in einem einzigen Byte steht. */
static const uint8_t ein_byte_maximum[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x25, 0xFF, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xC0,
};

/* --- Lesen ------------------------------------------------------------ */

static void test_tablett(void)
{
    printf("Tablett lesen\n");

    struct hid_pointer p;

    pruefe("als Zeiger erkannt", hid_parse_pointer(tablett, sizeof(tablett), &p));

    pruefe("X da", p.x.present);
    pruefe("Y da", p.y.present);
    pruefe("Rad da", p.wheel.present);

    /* Drei Tasten, dann fuenf Bit Fuellung, dann erst X. */
    pruefe_zahl("Taste 1", 0, p.button_bit[0]);
    pruefe_zahl("Taste 2", 1, p.button_bit[1]);
    pruefe_zahl("Taste 3", 2, p.button_bit[2]);
    pruefe_zahl("keine vierte", -1, p.button_bit[3]);

    pruefe_zahl("X liegt hinter dem Byte der Tasten", 8, p.x.bit);
    pruefe_zahl("X ist 16 Bit breit", 16, p.x.size);
    pruefe_zahl("Y gleich dahinter", 24, p.y.bit);
    pruefe_zahl("Y ist 16 Bit breit", 16, p.y.size);
    pruefe_zahl("Rad dahinter", 40, p.wheel.bit);
    pruefe_zahl("Rad ist acht Bit breit", 8, p.wheel.size);

    pruefe("X ist absolut", !p.x.relative);
    pruefe("Y ist absolut", !p.y.relative);
    pruefe("das Rad ist relativ", p.wheel.relative);

    /* Der Wertebereich: ohne ihn weiss man nicht, wo "ganz rechts" ist. */
    pruefe_zahl("kleinster X", 0, p.x.min);
    pruefe_zahl("groesster X", 32767, p.x.max);
    pruefe_zahl("groesster Y", 32767, p.y.max);
    pruefe("X ohne Vorzeichen", !p.x.is_signed);
    pruefe("das Rad hat eines", p.wheel.is_signed);

    pruefe("keine Berichtsnummer", !p.has_id);
    pruefe_zahl("sechs Byte lang", 48, p.report_bits);
}

static void test_maus(void)
{
    printf("Gewoehnliche Maus lesen\n");

    struct hid_pointer p;

    pruefe("erkannt", hid_parse_pointer(maus, sizeof(maus), &p));
    pruefe("X relativ", p.x.relative);
    pruefe("Y relativ", p.y.relative);
    pruefe_zahl("X im zweiten Byte", 8, p.x.bit);
    pruefe_zahl("Y im dritten", 16, p.y.bit);
    pruefe_zahl("Rad im vierten", 24, p.wheel.bit);
    pruefe("mit Vorzeichen", p.x.is_signed && p.y.is_signed);
    pruefe_zahl("kleinster Wert", -127, p.x.min);
    pruefe_zahl("groesster Wert", 127, p.x.max);
}

static void test_nummer(void)
{
    printf("Berichtsnummer\n");

    struct hid_pointer p;

    pruefe("erkannt", hid_parse_pointer(maus_mit_nummer,
                                        sizeof(maus_mit_nummer), &p));
    pruefe("Nummer vorhanden", p.has_id);
    pruefe_zahl("Nummer eins", 1, p.id);

    /* Alles rueckt um ein Byte nach hinten. */
    pruefe_zahl("Taste 1", 8, p.button_bit[0]);
    pruefe_zahl("X", 16, p.x.bit);
    pruefe_zahl("Y", 24, p.y.bit);
    pruefe_zahl("Rad", 32, p.wheel.bit);
}

static void test_keine_zeiger(void)
{
    printf("Was kein Zeiger ist\n");

    struct hid_pointer p;

    pruefe("Tastatur ist keiner", !hid_parse_pointer(tastatur,
                                                     sizeof(tastatur), &p));
    pruefe("nichts ist keiner", !hid_parse_pointer(NULL, 0, &p));
    pruefe("leer ist keiner", !hid_parse_pointer(tablett, 0, &p));
    pruefe("kein Ziel", !hid_parse_pointer(tablett, sizeof(tablett), NULL));

    /* Nach einem Fehlschlag steht nichts Halbes in der Antwort. */
    pruefe("X fehlt", !p.x.present);
    pruefe("Taste fehlt", p.button_bit[0] == -1);
}

static void test_maximum(void)
{
    printf("Maximum in einem Byte\n");

    struct hid_pointer p;

    pruefe("erkannt", hid_parse_pointer(ein_byte_maximum,
                                        sizeof(ein_byte_maximum), &p));

    /* 0xFF in einem Byte ist 255 - als -1 gelesen waere der Bereich
     * leer, und ein Tablett stuende ganz links oben still. */
    pruefe_zahl("groesster X", 255, p.x.max);
    pruefe_zahl("groesster Y", 255, p.y.max);
    pruefe("ohne Vorzeichen", !p.x.is_signed);
    pruefe_zahl("X nach Umrechnung ganz rechts", 1279,
                hid_scale(255, p.x.min, p.x.max, 1279));
}

/* Ein Strom von Zufall: Nichts darf abstuerzen, nichts aus dem Puffer
 * lesen. Gelaufen wird das auch unter AddressSanitizer. */
static void test_zufall(void)
{
    printf("Zufall und abgeschnittene Beschreibungen\n");

    /* Jede Abschneidung der echten Beschreibung. */
    for (size_t n = 0; n <= sizeof(tablett); n++) {
        struct hid_pointer p;
        bool ok = hid_parse_pointer(tablett, n, &p);

        /* Was gefunden wird, muss in den Bericht passen - sonst liest
         * der Treiber hinterher ins Leere. */
        if (ok && !(p.x.present && p.y.present)) {
            pruefe("Zeiger ohne X und Y", false);
            break;
        }
    }
    pruefe("alle Abschneidungen ueberlebt", true);

    unsigned long long z = 0x9E3779B97F4A7C15ull;
    int unstimmig = 0;

    for (int runde = 0; runde < 20000; runde++) {
        uint8_t d[96];
        size_t n = 0;

        z = z * 6364136223846793005ull + 1442695040888963407ull;
        n = (size_t)((z >> 33) % sizeof(d));

        for (size_t i = 0; i < n; i++) {
            z = z * 6364136223846793005ull + 1442695040888963407ull;
            d[i] = (uint8_t)(z >> 56);
        }

        struct hid_pointer p;
        bool ok = hid_parse_pointer(d, n, &p);

        if (ok && !(p.x.present && p.y.present))
            unstimmig++;

        /* Und was sie meldet, muss sich lesen lassen. */
        uint8_t bericht[16] = { 0 };

        if (p.x.present)
            (void)hid_get_bits(bericht, sizeof(bericht), p.x.bit, p.x.size,
                               p.x.is_signed);
    }
    pruefe_zahl("Zufall ohne Unstimmigkeiten", 0, unstimmig);
}

/* --- Berichte lesen --------------------------------------------------- */

static void test_bits(void)
{
    printf("Felder aus einem Bericht\n");

    /* Das Tablett in der Mitte des Feldes, linke Taste gedrueckt. */
    const uint8_t mitte[] = { 0x01, 0x00, 0x40, 0x00, 0x40, 0x00 };

    pruefe_zahl("Taste", 1, hid_get_bits(mitte, 6, 0, 1, false));
    pruefe_zahl("X", 0x4000, hid_get_bits(mitte, 6, 8, 16, false));
    pruefe_zahl("Y", 0x4000, hid_get_bits(mitte, 6, 24, 16, false));

    /* Ein Feld, das nicht an einer Bytegrenze beginnt: 12 Bit ab Bit 4. */
    const uint8_t schief[] = { 0xA0, 0xBC, 0x00 };

    pruefe_zahl("zwoelf Bit ab Bit 4", 0xBCA, hid_get_bits(schief, 3, 4, 12, false));

    /* Vorzeichen. */
    const uint8_t minus[] = { 0xFF, 0x80, 0x7F };

    pruefe_zahl("-1 in acht Bit", -1, hid_get_bits(minus, 3, 0, 8, true));
    pruefe_zahl("-128 in acht Bit", -128, hid_get_bits(minus, 3, 8, 8, true));
    pruefe_zahl("+127 in acht Bit", 127, hid_get_bits(minus, 3, 16, 8, true));
    pruefe_zahl("255 ohne Vorzeichen", 255, hid_get_bits(minus, 3, 0, 8, false));
    /* 0xFF, 0x80 gelesen als 16 Bit: 0x80FF, mit Vorzeichen negativ. */
    pruefe_zahl("16 Bit mit Vorzeichen", -32513, hid_get_bits(minus, 3, 0, 16, true));
    pruefe_zahl("16 Bit ohne Vorzeichen", 0x80FF, hid_get_bits(minus, 3, 0, 16, false));

    /* Grenzen. */
    pruefe_zahl("Breite null", 0, hid_get_bits(minus, 3, 0, 0, false));
    pruefe_zahl("Breite 33", 0, hid_get_bits(minus, 3, 0, 33, false));
    pruefe_zahl("ueber das Ende", 0, hid_get_bits(minus, 3, 20, 8, false));
    pruefe_zahl("genau bis zum Ende", 0x7F, hid_get_bits(minus, 3, 16, 8, false));
    pruefe_zahl("kein Bericht", 0, hid_get_bits(NULL, 3, 0, 8, false));
}

static void test_umrechnen(void)
{
    printf("Auf den Bildschirm umrechnen\n");

    /* Das Tablett meldet 0..32767; der Schirm ist 1280 breit, also
     * Punkte 0..1279. */
    pruefe_zahl("ganz links", 0, hid_scale(0, 0, 32767, 1279));
    pruefe_zahl("ganz rechts", 1279, hid_scale(32767, 0, 32767, 1279));
    pruefe_zahl("Mitte", 639, hid_scale(16383, 0, 32767, 1279));

    /* Der letzte Punkt muss erreichbar sein - sonst bleibt am Rand ein
     * Streifen, an den der Zeiger nie kommt. */
    pruefe_zahl("kurz vor dem Rand liegt der Punkt davor", 1278,
                hid_scale(32767 - 20, 0, 32767, 1279));

    /* Nichts laeuft ueber den Rand. */
    pruefe_zahl("unter dem Bereich", 0, hid_scale(-50, 0, 32767, 1279));
    pruefe_zahl("ueber dem Bereich", 1279, hid_scale(99999, 0, 32767, 1279));

    /* Bereiche mit Vorzeichen. */
    pruefe_zahl("-100..100, Mitte", 50, hid_scale(0, -100, 100, 100));
    pruefe_zahl("-100..100, links", 0, hid_scale(-100, -100, 100, 100));

    /* Unsinn. */
    pruefe_zahl("leerer Bereich", 0, hid_scale(5, 7, 7, 100));
    pruefe_zahl("verkehrter Bereich", 0, hid_scale(5, 10, 0, 100));
    pruefe_zahl("keine Breite", 0, hid_scale(5, 0, 10, 0));

    /* Monoton: Ein Tablett, das nach rechts geht, darf den Zeiger nie
     * nach links springen lassen. */
    int32_t vorher = -1;
    bool steigend = true;

    for (int32_t v = 0; v <= 32767; v += 7) {
        int32_t x = hid_scale(v, 0, 32767, 1279);

        if (x < vorher)
            steigend = false;
        vorher = x;
    }
    pruefe("steigt nie rueckwaerts", steigend);
}

/* --- PS/2-Pakete ------------------------------------------------------ */

static struct ps2_motion paket(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                               size_t n, bool *ok)
{
    uint8_t bytes[4] = { a, b, c, d };
    struct ps2_motion m;

    memset(&m, 0xAA, sizeof(m));
    *ok = ps2_decode_packet(bytes, n, &m);
    return m;
}

static void test_ps2(void)
{
    printf("PS/2-Pakete\n");

    bool ok;
    struct ps2_motion m;

    m = paket(0x08, 5, 3, 0, 3, &ok);
    pruefe("gueltig", ok);
    pruefe_zahl("nach rechts", 5, m.dx);
    pruefe_zahl("nach oben wird nach unten gezaehlt", -3, m.dy);
    pruefe_zahl("keine Taste", 0, m.buttons);
    pruefe_zahl("kein Rad bei drei Byte", 0, m.scroll);

    /* Vorzeichen stehen im ersten Byte. */
    m = paket(0x18, 0xFB, 0, 0, 3, &ok);      /* X: 251 - 256 */
    pruefe_zahl("nach links", -5, m.dx);
    m = paket(0x28, 0, 0xFD, 0, 3, &ok);      /* Y: 253 - 256 = -3, dann gedreht */
    pruefe_zahl("nach unten", 3, m.dy);

    /* Tasten. */
    m = paket(0x0B, 0, 0, 0, 3, &ok);
    pruefe_zahl("links und rechts", MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT, m.buttons);
    m = paket(0x0C, 0, 0, 0, 3, &ok);
    pruefe_zahl("Mitte", MOUSE_BTN_MIDDLE, m.buttons);

    /* Aus dem Takt: Bit 3 fehlt, das ist kein Paketanfang. */
    paket(0x00, 5, 3, 0, 3, &ok);
    pruefe("Bit 3 fehlt: ungueltig", !ok);
    paket(0x07, 5, 3, 0, 3, &ok);
    pruefe("Bit 3 fehlt, Tasten gesetzt: auch", !ok);

    /* Zu kurz oder ohne Ziel. */
    paket(0x08, 5, 3, 0, 2, &ok);
    pruefe("zwei Byte sind kein Paket", !ok);
    pruefe("kein Paket", !ps2_decode_packet(NULL, 3, &m));
    uint8_t b[3] = { 0x08, 0, 0 };
    pruefe("kein Ziel", !ps2_decode_packet(b, 3, NULL));
}

static void test_ueberlauf(void)
{
    printf("Pakete mit Ueberlauf\n");

    bool ok;
    struct ps2_motion m;

    /* Eine schnelle Bewegung setzt das Ueberlaufbit. Frueher wurde so
     * ein Paket ganz verworfen - und mit ihm der Zustand der Tasten. */
    m = paket(0x48 | 0x01, 0xFF, 0x10, 0, 3, &ok);   /* X-Ueberlauf, links */
    pruefe("gilt", ok);
    pruefe_zahl("die Bewegung kommt an", 255, m.dx);
    pruefe_zahl("die Taste auch", MOUSE_BTN_LEFT, m.buttons);

    /* Vor allem das Loslassen: Es kommt in einem Paket, das gerade
     * wegen einer schnellen Bewegung Ueberlauf meldet. */
    m = paket(0x88, 0x20, 0x00, 0, 3, &ok);          /* Y-Ueberlauf, nichts gedrueckt */
    pruefe("gilt auch", ok);
    pruefe_zahl("Taste ist wieder oben", 0, m.buttons);

    m = paket(0xC8, 0x00, 0x00, 0, 3, &ok);          /* beide Ueberlaeufe */
    pruefe("beide Bits: gilt", ok);
}

static void test_rad(void)
{
    printf("Das Rad\n");

    bool ok;
    struct ps2_motion m;

    /* Bei PS/2 ist "zum Benutzer hin" positiv. Die Oberflaeche rechnet
     * mit "nach oben positiv" (wie HID) - das Vorzeichen wird hier
     * gedreht. Ein Rad, das nach unten gedreht wird, muss einen
     * negativen Wert liefern, sonst scrollt es verkehrt herum. */
    m = paket(0x08, 0, 0, 0x01, 4, &ok);
    pruefe_zahl("ein Schritt zum Benutzer hin = nach unten", -1, m.scroll);
    m = paket(0x08, 0, 0, 0x0F, 4, &ok);
    pruefe_zahl("ein Schritt vom Benutzer weg = nach oben", 1, m.scroll);
    m = paket(0x08, 0, 0, 0x08, 4, &ok);
    pruefe_zahl("groesster Schritt nach oben", 8, m.scroll);
    m = paket(0x08, 0, 0, 0x07, 4, &ok);
    pruefe_zahl("groesster Schritt nach unten", -7, m.scroll);
    m = paket(0x08, 0, 0, 0x00, 4, &ok);
    pruefe_zahl("kein Schritt", 0, m.scroll);

    /* Typ 4 (IntelliMouse Explorer) legt die Tasten 4 und 5 in die
     * oberen vier Bit. Sie duerfen das Rad nicht veraendern. */
    m = paket(0x08, 0, 0, 0x31, 4, &ok);
    pruefe_zahl("obere Bit gehoeren nicht dazu", -1, m.scroll);
    m = paket(0x08, 0, 0, 0xFF, 4, &ok);
    pruefe_zahl("alle gesetzt", 1, m.scroll);
}

/* --- Tastenflanken ---------------------------------------------------- */

static void test_flanken(void)
{
    printf("Klicks, die kuerzer sind als ein Bild\n");

    struct mouse_step st[MOUSE_MAX_STEPS];
    size_t n;

    n = mouse_button_steps(0, 0, 0, 0, st);
    pruefe_zahl("nichts geschieht nichts", 0, (long)n);

    n = mouse_button_steps(0, MOUSE_BTN_LEFT, MOUSE_BTN_LEFT, 0, st);
    pruefe_zahl("Druck: ein Schritt", 1, (long)n);
    pruefe("Druck: unten", n == 1 && st[0].down && st[0].button == MOUSE_BTN_LEFT);

    n = mouse_button_steps(MOUSE_BTN_LEFT, 0, 0, MOUSE_BTN_LEFT, st);
    pruefe("Loslassen: oben", n == 1 && !st[0].down);

    /* Der Kern der Sache: gedrueckt und losgelassen, noch bevor jemand
     * nachgesehen hat. Der Zustand sagt beide Male "oben". */
    n = mouse_button_steps(0, 0, MOUSE_BTN_LEFT, MOUSE_BTN_LEFT, st);
    pruefe_zahl("schneller Klick: zwei Schritte", 2, (long)n);
    pruefe("erst unten", n == 2 && st[0].down);
    pruefe("dann oben", n == 2 && !st[1].down);

    /* Umgekehrt: losgelassen und gleich wieder gedrueckt. */
    n = mouse_button_steps(MOUSE_BTN_LEFT, MOUSE_BTN_LEFT, MOUSE_BTN_LEFT,
                           MOUSE_BTN_LEFT, st);
    pruefe_zahl("Loslassen und Druck: zwei Schritte", 2, (long)n);
    pruefe("erst oben", n == 2 && !st[0].down);
    pruefe("dann unten", n == 2 && st[1].down);

    /* Ohne Flanken, nur mit Zustand - etwa bei einem Treiber, der sie
     * nicht mitzaehlt. */
    n = mouse_button_steps(0, MOUSE_BTN_RIGHT, 0, 0, st);
    pruefe("nur Zustand: Druck", n == 1 && st[0].down && st[0].button == MOUSE_BTN_RIGHT);
    n = mouse_button_steps(MOUSE_BTN_RIGHT, 0, 0, 0, st);
    pruefe("nur Zustand: Loslassen", n == 1 && !st[0].down);

    /* Zwei Tasten, jede fuer sich. */
    n = mouse_button_steps(MOUSE_BTN_LEFT, MOUSE_BTN_LEFT, 0, 0, st);
    pruefe_zahl("gehaltene Taste: nichts", 0, (long)n);
    n = mouse_button_steps(MOUSE_BTN_LEFT, MOUSE_BTN_LEFT, MOUSE_BTN_RIGHT,
                           MOUSE_BTN_RIGHT, st);
    pruefe("rechts geklickt, links gehalten", n == 2 && st[0].button == MOUSE_BTN_RIGHT);
}

/* Fuer jede Eingabe, die es gibt: Die Folge ist in sich stimmig und
 * endet im Zustand, den der Treiber meldet. Das ist die eigentliche
 * Zusicherung - sie gilt auch fuer Eingaben, an die niemand gedacht
 * hat. */
static void test_alle_flanken(void)
{
    printf("Jede Kombination\n");

    int unstimmig = 0;
    int zu_lang = 0;
    int endet_falsch = 0;

    for (int prev = 0; prev < 8; prev++) {
        for (int now = 0; now < 8; now++) {
            for (int p = 0; p < 8; p++) {
                for (int r = 0; r < 8; r++) {
                    struct mouse_step st[MOUSE_MAX_STEPS];
                    size_t n = mouse_button_steps((uint8_t)prev, (uint8_t)now,
                                                  (uint8_t)p, (uint8_t)r, st);
                    int state = prev;

                    if (n > MOUSE_MAX_STEPS) {
                        zu_lang++;
                        continue;
                    }

                    for (size_t i = 0; i < n; i++) {
                        uint8_t b = st[i].button;

                        /* Eine Taste, die oben ist, kann nicht noch
                         * einmal losgelassen werden - und umgekehrt. */
                        if (st[i].down == ((state & b) != 0))
                            unstimmig++;

                        if (st[i].down)
                            state |= b;
                        else
                            state &= ~b;
                    }

                    if (state != now)
                        endet_falsch++;
                }
            }
        }
    }

    pruefe_zahl("nie eine doppelte Flanke", 0, unstimmig);
    pruefe_zahl("nie mehr Schritte als Platz", 0, zu_lang);
    pruefe_zahl("endet immer im gemeldeten Zustand", 0, endet_falsch);
}

/* --- Doppelklick ------------------------------------------------------ */

static void test_doppelklick(void)
{
    printf("Doppelklick\n");

    pruefe("kein erster Klick", !mouse_is_double_click(100, 0, 5, 5, 5, 5, 400));
    pruefe("innerhalb der Zeit", mouse_is_double_click(1300, 1000, 5, 5, 5, 5, 400));
    pruefe("genau an der Grenze nicht", !mouse_is_double_click(1400, 1000, 5, 5, 5, 5, 400));
    pruefe("zu spaet", !mouse_is_double_click(2000, 1000, 5, 5, 5, 5, 400));
    pruefe("Uhr zurueck", !mouse_is_double_click(900, 1000, 5, 5, 5, 5, 400));
    pruefe("fuenf Punkte daneben", mouse_is_double_click(1100, 1000, 10, 5, 5, 5, 400));
    pruefe("sechs Punkte daneben", !mouse_is_double_click(1100, 1000, 11, 5, 5, 5, 400));
    pruefe("sechs nach links", !mouse_is_double_click(1100, 1000, 0, 5, 6, 5, 400));
    pruefe("sechs nach oben", !mouse_is_double_click(1100, 1000, 5, 0, 5, 6, 400));
    pruefe("laengere Zeit gilt", mouse_is_double_click(1900, 1000, 5, 5, 5, 5, 1000));
    pruefe("kuerzere Zeit gilt", !mouse_is_double_click(1300, 1000, 5, 5, 5, 5, 200));

    pruefe_zahl("Unsinn null", 400, mouse_double_click_sane(0));
    pruefe_zahl("Unsinn riesig", 400, mouse_double_click_sane(1000000));
    pruefe_zahl("zu kurz", 400, mouse_double_click_sane(149));
    pruefe_zahl("Untergrenze", 150, mouse_double_click_sane(150));
    pruefe_zahl("Obergrenze", 1500, mouse_double_click_sane(1500));
    pruefe_zahl("zu lang", 400, mouse_double_click_sane(1501));

    pruefe_zahl("weiter von 400", 500, mouse_double_click_step(400, +1));
    pruefe_zahl("zurueck von 400", 300, mouse_double_click_step(400, -1));
    pruefe_zahl("Kreis oben", 200, mouse_double_click_step(1000, +1));
    pruefe_zahl("Kreis unten", 1000, mouse_double_click_step(200, -1));
    pruefe_zahl("zwischen den Stufen weiter", 500, mouse_double_click_step(450, +1));
    pruefe_zahl("zwischen den Stufen zurueck", 400, mouse_double_click_step(450, -1));
    pruefe_zahl("Unsinn startet bei 400", 500, mouse_double_click_step(0, +1));

    /* Jeder Wert im Kreis kommt nach sieben Schritten wieder an. */
    uint32_t v = 400;

    for (int i = 0; i < 7; i++)
        v = mouse_double_click_step(v, +1);
    pruefe_zahl("Kreis schliesst sich", 400, v);
}

int main(void)
{
    printf("=== Zeiger ===\n");

    test_tablett();
    test_maus();
    test_nummer();
    test_keine_zeiger();
    test_maximum();
    test_zufall();
    test_bits();
    test_umrechnen();
    test_ps2();
    test_ueberlauf();
    test_rad();
    test_flanken();
    test_alle_flanken();
    test_doppelklick();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
