/* mouse.c - PS/2-Maus an Port 2 des 8042.
 *
 * Die Maus sendet Pakete zu drei Byte (Tasten/Vorzeichen, dx, dy). Nach der
 * "Magic Sequence" mit den Abtastraten 200/100/80 meldet sie sich als Typ 3
 * und liefert ein viertes Byte fuer das Scrollrad.
 *
 * Drei Dinge halten den Treiber auch dort am Leben, wo der Anschluss
 * nicht so sauber emuliert wird wie in QEMU.
 *
 * Erstens wird nachgesehen, ob ueberhaupt jemand antwortet - aber die
 * Antwort entscheidet nichts. Sie steht in der Systeminformation und
 * im Protokoll, damit man weiss, woran man ist; eingeschaltet wird die
 * Maus in jedem Fall. Eine Erkennung, die im Zweifel abschaltet, macht
 * aus einem langsamen Geraet ein totes, und das ist der schlechtere
 * Fehler: Wer keine Maus hat, merkt nichts davon, dass sie trotzdem
 * eingeschaltet wurde.
 *
 * Zweitens ist der Interrupt nicht der einzige Weg herein. Bleibt IRQ 12
 * aus - eine Umlegung, die nicht stimmt, ein Emulator, der ihn nur
 * manchmal ausloest -, holt die Hauptschleife die Bytes selbst ab. Und
 * findet der Tastatur-Interrupt ein Byte, das der Maus gehoert, reicht
 * er es hierher weiter, statt es als Tastendruck zu verwerfen.
 *
 * Drittens wird das vierte Byte nur genommen, wenn die Maus sauber
 * gesagt hat, dass sie eines schickt. Wer hier falsch liegt, verschiebt
 * jedes Paket um ein Byte, und dann bewegt sich gar nichts mehr.
 *
 * Alle Wege herein gehen durch dieselbe Tuer, und die ist gesperrt.
 * Das ist keine Vorsicht, sondern noetig: Auf mehreren Kernen koennen
 * der Tastatur-Interrupt auf dem einen und die Hauptschleife auf dem
 * anderen gleichzeitig am selben Anschluss lesen. Wer sich dabei ein
 * Byte wegschnappt, verschiebt den Rest des Pakets - und ein
 * verschobenes Paket sieht man nicht als Fehler, sondern als Zeiger,
 * der ruckelt und danebenzeigt.
 *
 * Aus demselben Grund wird beim Lesen gleich leergeraeumt und nicht
 * ein Byte je Unterbrechung: Der 8042 hat genau ein Byte Platz. Wer zu
 * langsam abholt, verliert das naechste - und wieder ist das Paket
 * verschoben.
 *
 * Die Sperre gilt auch auf der anderen Seite: beim Abholen des Zustands.
 * mouse_poll() las frueher Position, Tasten und Aenderungsmarke einzeln
 * und setzte die Marke zuletzt zurueck. Schlug dazwischen ein Paket ein,
 * war die neue Position da, die Marke aber geloescht - der Zeiger blieb
 * einen Schritt hinter der Hand stehen, bis sich die Maus wieder
 * bewegte, und eine Taste, die genau dort gedrueckt oder losgelassen
 * wurde, ging verloren oder blieb haengen. Das passiert nicht bei jedem
 * Paket, aber bei jeder Sitzung, und es sieht aus wie ein Zeiger, der
 * "nicht richtig" funktioniert.
 *
 * Und Tasten werden als Flanken mitgezaehlt, nicht nur als Zustand: Wer
 * zwischen zwei Durchlaeufen der Hauptschleife drueckt und loslaesst,
 * hat geklickt, auch wenn der Zustand beide Male "oben" sagt.
 */

#include "input.h"
#include "arch.h"
#include "io.h"
#include "mouseutil.h"
#include "ps2.h"
#include "spinlock.h"

static uint8_t  packet[4];
static uint8_t  packet_index;
static uint8_t  packet_size = 3;

/* Der Zustand des Zeigers. Alles hier drin wird nur unter mouse_lock
 * angefasst - von den Eingaengen wie vom Abholen. */
static int32_t pos_x, pos_y;
static int32_t acc_dx, acc_dy;
static int8_t  acc_scroll;
static uint8_t btn_state;       /* was jetzt gedrueckt ist                */
static uint8_t btn_pressed;     /* Flanken seit dem letzten Abholen       */
static uint8_t btn_released;
static bool    dirty;

static int32_t limit_x = 1023, limit_y = 767;

/* Haengt eine Maus am Anschluss, und kommen ihre Bytes per Interrupt?
 * Beides wird gebraucht, um ehrlich sagen zu koennen, woran es liegt,
 * wenn sich der Zeiger nicht bewegt. */
static bool     attached;
static uint32_t irq_count;
static uint32_t polled_count;
static uint32_t resync_count;
static uint64_t last_byte_ms;

/* Zwei Wege lesen am selben Anschluss - der Interrupt und die
 * Hauptschleife. Ohne Sperre koennten beide dasselbe Byte holen, und
 * das eine Paket waere dann um ein Byte verschoben. */
static struct spinlock mouse_lock = SPINLOCK_INIT("maus");

/* Alles Folgende nur unter der Sperre. */

static void set_buttons(uint8_t now)
{
    btn_pressed  |= (uint8_t)(now & ~btn_state);
    btn_released |= (uint8_t)(btn_state & ~now);
    btn_state = now;
}

static void move_relative(int32_t dx, int32_t dy, int8_t scroll)
{
    pos_x = CLAMP(pos_x + dx, 0, limit_x);
    pos_y = CLAMP(pos_y + dy, 0, limit_y);

    acc_dx += dx;
    acc_dy += dy;
    acc_scroll = (int8_t)(acc_scroll + scroll);
}

static void handle_packet(void)
{
    struct ps2_motion m;

    /* Aus dem Takt: Das Byte gehoert zu einem Paket, dessen Anfang
     * verlorenging. */
    if (!ps2_decode_packet(packet, packet_size, &m)) {
        packet_index = 0;
        resync_count++;
        return;
    }

    move_relative(m.dx, m.dy, m.scroll);
    set_buttons(m.buttons);
    dirty = true;
}

/* Bewegung und Tasten von aussen uebernehmen - der Weg der USB-Maus.
 * Die Zaehlweise ist dieselbe wie beim PS/2-Paket: nach rechts und nach
 * unten positiv. */
void mouse_inject(int32_t dx, int32_t dy, int8_t scroll,
                  bool left, bool right, bool middle)
{
    uint64_t flags = spin_lock_irq(&mouse_lock);

    move_relative(dx, dy, scroll);
    set_buttons((uint8_t)((left ? MOUSE_BTN_LEFT : 0) |
                          (right ? MOUSE_BTN_RIGHT : 0) |
                          (middle ? MOUSE_BTN_MIDDLE : 0)));
    dirty = true;
    spin_unlock_irq(&mouse_lock, flags);
}

/* Eine absolute Zeigerposition, wie sie ein Tablett meldet.
 *
 * x und y sind Anteile von 0 bis 65535 der Breite und Hoehe, nicht
 * Punkte: Das Tablett weiss nicht, wie gross die Oberflaeche ist - und
 * die kann sich aendern, sobald jemand die Aufloesung oder die
 * Vergroesserung umstellt. Umgerechnet wird erst hier, mit den
 * Grenzen von jetzt. */
void mouse_inject_unit(int32_t x, int32_t y, int8_t scroll,
                       bool left, bool right, bool middle)
{
    uint64_t flags = spin_lock_irq(&mouse_lock);

    x = CLAMP(x, 0, 65535);
    y = CLAMP(y, 0, 65535);

    int32_t nx = (int32_t)((int64_t)x * limit_x / 65535);
    int32_t ny = (int32_t)((int64_t)y * limit_y / 65535);

    acc_dx += nx - pos_x;
    acc_dy += ny - pos_y;
    pos_x = nx;
    pos_y = ny;
    acc_scroll = (int8_t)(acc_scroll + scroll);

    set_buttons((uint8_t)((left ? MOUSE_BTN_LEFT : 0) |
                          (right ? MOUSE_BTN_RIGHT : 0) |
                          (middle ? MOUSE_BTN_MIDDLE : 0)));
    dirty = true;
    spin_unlock_irq(&mouse_lock, flags);
}

/* Ein Byte in die Zustandsmaschine geben. Nur unter der Sperre. */
static void feed_byte(uint8_t byte)
{
    uint64_t now = timer_ms();

    /* Ein angefangenes Paket, das haengengeblieben ist, wird
     * weggeworfen. Fehlt einmal ein Byte, waere sonst jedes weitere
     * Paket um eines verschoben - und das bleibt so, bis jemand neu
     * startet. */
    if (packet_index > 0 && now - last_byte_ms > 100) {
        packet_index = 0;
        resync_count++;
    }
    last_byte_ms = now;

    packet[packet_index] = byte;

    /* Bit 3 ist im ersten Byte jedes gueltigen Pakets gesetzt. Steht es
     * nicht, sind wir nicht am Anfang - das Byte gehoert zu einem
     * Paket, dessen Anfang verlorenging. */
    if (packet_index == 0 && !(packet[0] & 0x08)) {
        resync_count++;
        return;
    }

    if (++packet_index >= packet_size) {
        packet_index = 0;
        handle_packet();
    }
}

/* Holt ein Byte, das der Maus gehoert. false heisst: da liegt keines -
 * entweder ist der Puffer leer, oder das Byte gehoert der Tastatur.
 * Nur unter der Sperre aufrufen. */
static bool take_byte(uint8_t *out)
{
    uint8_t status = inb(0x64);

    if (status == 0xFF || !(status & 0x01) || !(status & 0x20))
        return false;
    *out = inb(0x60);
    return true;
}

/* Raeumt alles ab, was gerade an Mausbytes bereitliegt. Die einzige
 * Stelle, an der 0x60 fuer die Maus gelesen wird. */
static void drain(bool from_irq)
{
    uint64_t flags = spin_lock_irq(&mouse_lock);

    /* Mit Obergrenze: Ein Anschluss, der pausenlos nachliefert, soll
     * weder die Unterbrechung noch die Oberflaeche festhalten. */
    for (int i = 0; i < 64; i++) {
        uint8_t byte;

        if (!take_byte(&byte))
            break;

        if (from_irq)
            irq_count++;
        else
            polled_count++;
        feed_byte(byte);
    }
    spin_unlock_irq(&mouse_lock, flags);
}

/* Der Tastatur-Interrupt hat ein Byte gefunden, das der Maus gehoert.
 * Er reicht es nicht selbst weiter, sondern laesst hier abraeumen -
 * sonst schriebe er ohne Sperre in dieselben Felder. */
void mouse_drain(void)
{
    drain(true);
}

static void mouse_irq(struct registers *regs)
{
    UNUSED(regs);

    drain(true);
}

/* Nachsehen, ob ein Mausbyte im Controller liegt, das kein Interrupt
 * abgeholt hat. Kostet ein inb je Bilddurchlauf und rettet den Zeiger
 * ueberall dort, wo IRQ 12 nicht ankommt.
 *
 * Unterbrechungen bleiben dabei aus: Sonst koennte der eigene Interrupt
 * mitten hinein platzen und dasselbe Byte ein zweites Mal lesen. */
void mouse_pump(void)
{
    /* Nicht an "attached" gebunden: Gerade wenn die Erkennung nichts
     * gefunden hat, ist das hier der Weg, auf dem doch etwas
     * hereinkommt. */
    if (!ps2_present())
        return;

    drain(false);
}

bool mouse_attached(void) { return attached; }

uint32_t mouse_irq_count(void)    { return irq_count; }
uint32_t mouse_polled_count(void) { return polled_count; }
uint32_t mouse_resync_count(void) { return resync_count; }
uint8_t  mouse_packet_size(void)  { return packet_size; }

/* Ein Zusatz fuer die Systeminformation, und zwar nur dann, wenn es
 * etwas zu sagen gibt: Der Normalfall braucht keine Anmerkung. */
const char *mouse_note(void)
{
    if (attached && irq_count == 0 && polled_count > 0)
        return " (abgefragt)";
    return "";
}

static void set_sample_rate(uint8_t rate)
{
    ps2_mouse_command(0xF3);
    ps2_mouse_command(rate);
}

void mouse_init(int32_t screen_w, int32_t screen_h)
{
    limit_x = screen_w - 1;
    limit_y = screen_h - 1;
    pos_x = screen_w / 2;
    pos_y = screen_h / 2;
    btn_state = btn_pressed = btn_released = 0;

    attached = false;
    irq_count = 0;
    polled_count = 0;

    /* Ohne 8042 gibt es hier nichts zu tun; dann kommt die Maus ueber
     * USB oder gar nicht. */
    if (!ps2_present()) {
        kprintf("Maus        : kein PS/2-Controller\n");
        return;
    }

    /* Zuruecksetzen und nachsehen, ob jemand antwortet. Eine Maus
     * quittiert mit 0xFA, meldet danach 0xAA (Selbsttest bestanden)
     * und ihre Kennung. Das Zuruecksetzen darf dauern - eine Maus
     * prueft sich dabei selbst. */
    bool answered = ps2_mouse_command_ok(0xFF, NULL);

    if (answered) {
        uint8_t selftest = 0;
        uint8_t ignored = 0;

        answered = ps2_read_byte_ms(&selftest, 800) && selftest == 0xAA;
        ps2_read_byte_ms(&ignored, 200);   /* Kennung, hier 0x00 */
    }

    attached = answered;
    packet_size = 3;

    ps2_mouse_command(0xF6);      /* Standardwerte                        */

    /* Scrollrad freischalten und Typ abfragen. Das vierte Byte wird
     * nur genommen, wenn beides sauber durchgeht - eine geratene
     * Paketlaenge verschiebt jedes folgende Paket. */
    set_sample_rate(200);
    set_sample_rate(100);
    set_sample_rate(80);

    uint8_t id = 0;

    if (ps2_mouse_command_ok(0xF2, NULL) && ps2_read_byte(&id) &&
        (id == 3 || id == 4))
        packet_size = 4;

    set_sample_rate(100);         /* 100 Pakete je Sekunde                */
    ps2_mouse_command(0xE8);      /* Aufloesung ...                        */
    ps2_mouse_command(0x03);      /* ... 8 Zaehler je Millimeter          */
    ps2_mouse_command(0xF4);      /* Datenuebertragung starten            */

    packet_index = 0;
    irq_install(12, mouse_irq);

    /* Auch wenn niemand geantwortet hat, wird eingeschaltet und der
     * Interrupt angemeldet: Es kostet nichts, und ein Anschluss, der
     * nur beim Erkennen geschwiegen hat, funktioniert danach oft
     * trotzdem. */
    if (attached)
        kprintf("Maus        : PS/2, Typ %u, %u-Byte-Pakete\n", id,
                packet_size);
    else
        kprintf("Maus        : PS/2 meldet sich nicht - trotzdem "
                "eingeschaltet\n");
}

bool mouse_poll(struct mouse_state *out)
{
    uint64_t flags = spin_lock_irq(&mouse_lock);

    /* Ein Griff: Was hier gelesen wird, gehoert zusammen, und was
     * zurueckgesetzt wird, ist genau das, was gelesen wurde. */
    bool changed = dirty;

    out->x       = pos_x;
    out->y       = pos_y;
    out->dx      = acc_dx;
    out->dy      = acc_dy;
    out->scroll  = acc_scroll;
    out->buttons = btn_state;
    out->pressed = btn_pressed;
    out->released = btn_released;
    out->left    = (btn_state & MOUSE_BTN_LEFT) != 0;
    out->right   = (btn_state & MOUSE_BTN_RIGHT) != 0;
    out->middle  = (btn_state & MOUSE_BTN_MIDDLE) != 0;

    acc_dx = acc_dy = 0;
    acc_scroll = 0;
    btn_pressed = btn_released = 0;
    dirty = false;

    spin_unlock_irq(&mouse_lock, flags);
    return changed;
}

void mouse_position(int32_t *x, int32_t *y)
{
    uint64_t flags = spin_lock_irq(&mouse_lock);

    if (x)
        *x = pos_x;
    if (y)
        *y = pos_y;
    spin_unlock_irq(&mouse_lock, flags);
}

/* Der Bildschirm ist ein anderer geworden - der Zeiger darf nicht
 * ausserhalb stehenbleiben. */
void mouse_set_limits(int32_t screen_w, int32_t screen_h)
{
    uint64_t flags = spin_lock_irq(&mouse_lock);

    limit_x = screen_w - 1;
    limit_y = screen_h - 1;
    pos_x = CLAMP(pos_x, 0, limit_x);
    pos_y = CLAMP(pos_y, 0, limit_y);
    dirty = true;
    spin_unlock_irq(&mouse_lock, flags);
}

void mouse_set_position(int32_t x, int32_t y)
{
    uint64_t flags = spin_lock_irq(&mouse_lock);

    pos_x = CLAMP(x, 0, limit_x);
    pos_y = CLAMP(y, 0, limit_y);
    dirty = true;
    spin_unlock_irq(&mouse_lock, flags);
}
