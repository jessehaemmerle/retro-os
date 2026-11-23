/* hidparse.h - Zeigergeraete aus der HID-Berichtsbeschreibung lesen.
 *
 * Bisher sprach RetroOS mit USB-Mäusen nur im Boot-Protokoll: drei
 * feste Bytes, jedes BIOS versteht sie. Das reicht fuer eine Maus -
 * aber nicht fuer ein Tablett. Und genau das ist in VirtualBox (und
 * in QEMU mit -device usb-tablet) das Zeigergeraet ab Werk: Es meldet
 * keine Bewegung, sondern die Stelle, an der der Zeiger des Gastgebers
 * steht, und braucht darum kein Einfangen der Maus und keine
 * Beschleunigung, die nicht passt.
 *
 * Ein solches Geraet hat kein Boot-Protokoll. Wie seine Berichte
 * aufgebaut sind, steht in der Berichtsbeschreibung, die es auf
 * Anfrage herausgibt: eine kleine Bytecode-Sprache aus Elementen mit
 * einem Praefixbyte. Hier wird sie so weit gelesen, wie ein Zeiger sie
 * braucht - Lage und Breite von X, Y, Rad und Tasten.
 *
 * Getrennt vom Treiber, weil sich genau hier die Fehler verstecken: ein
 * Bericht mit vorangestellter Berichtsnummer, der alles um acht Bit
 * verschiebt; ein Maximum, das als 0xFF in einem Byte steht und ohne
 * Vorsicht -1 ergibt; ein Feld von 16 Bit, das ueber eine Bytegrenze
 * laeuft. Das laesst sich auf dem Entwicklungsrechner pruefen, das
 * Tablett selbst nicht.
 */
#ifndef HIDPARSE_H
#define HIDPARSE_H

#include "retro.h"

#define HID_MAX_BUTTONS 8

struct hid_field {
    bool     present;
    uint16_t bit;           /* Bitversatz im Bericht, Berichtsnummer eingerechnet */
    uint8_t  size;          /* Breite in Bit                                      */
    bool     is_signed;
    bool     relative;      /* false = absolute Lage                              */
    int32_t  min, max;      /* der Wertebereich, den das Geraet meldet            */
};

struct hid_pointer {
    struct hid_field x, y, wheel;

    /* Tasten stehen einzeln: Manche Geraete legen sie nicht
     * hintereinander. -1 heisst, diese Taste gibt es nicht. */
    int16_t button_bit[HID_MAX_BUTTONS];

    bool    has_id;         /* Berichte tragen vorn eine Nummer */
    uint8_t id;

    /* Wie viele Bit ein Bericht mindestens haben muss, damit alles
     * Gelesene darin liegt. Ein kuerzerer wird verworfen, statt aus
     * dem Puffer hinaus zu lesen. */
    uint16_t report_bits;
};

/* Liest eine Berichtsbeschreibung. true heisst: Es ist ein Zeiger mit
 * X und Y gefunden. Ohne diese beiden ist es keiner - eine Tastatur
 * etwa. */
bool hid_parse_pointer(const uint8_t *desc, size_t length,
                       struct hid_pointer *out);

/* Holt ein Feld aus einem Bericht. Little endian, Bit 0 ist das
 * niedrigste Bit des ersten Bytes. Liegt das Feld nicht ganz im
 * Bericht, kommt 0 zurueck. */
int32_t hid_get_bits(const uint8_t *report, size_t bytes, uint16_t bit,
                     uint8_t size, bool is_signed);

/* Rechnet einen Wert aus [min, max] auf [0, span] um. Was ausserhalb
 * liegt, wird auf den Rand gesetzt; ein leerer Bereich ergibt 0. */
int32_t hid_scale(int32_t value, int32_t min, int32_t max, int32_t span);

#endif /* HIDPARSE_H */
