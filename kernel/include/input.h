/* input.h - Tastatur- und Mausereignisse. */
#ifndef INPUT_H
#define INPUT_H

#include "retro.h"

/* Sondertasten liegen oberhalb des ASCII-Bereichs. */
enum {
    KEY_NONE = 0,
    KEY_BACKSPACE = 8,
    KEY_TAB       = 9,
    KEY_ENTER     = 10,
    KEY_ESCAPE    = 27,

    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN,
    KEY_INSERT, KEY_DELETE,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT,
    KEY_CAPSLOCK, KEY_SUPER,
};

enum {
    MOD_SHIFT = 1 << 0,
    MOD_CTRL  = 1 << 1,
    MOD_ALT   = 1 << 2,
    MOD_CAPS  = 1 << 3,
};

struct key_event {
    uint16_t key;      /* ASCII oder KEY_*                        */
    char     ascii;    /* 0, wenn die Taste kein Zeichen erzeugt  */
    uint8_t  mods;
    bool     pressed;
};

struct mouse_state {
    int32_t x, y;
    int32_t dx, dy;
    int8_t  scroll;
    bool    left, right, middle;
};

/* PS/2-Controller und die beiden Geraete einrichten. Gibt false zurueck,
 * wenn der Rechner gar keinen mehr hat - dann uebernimmt USB. */
bool ps2_init(void);
bool ps2_present(void);

void keyboard_init(void);
bool keyboard_poll(struct key_event *out);
uint8_t keyboard_mods(void);

/* Eine Taste von einem anderen Treiber - etwa der USB-Tastatur. */
void keyboard_inject(uint16_t key, bool pressed);

void mouse_init(int32_t screen_w, int32_t screen_h);
/* Liefert true, wenn sich seit dem letzten Aufruf etwas geaendert hat. */
bool mouse_poll(struct mouse_state *out);
/* Ein Byte, das der Tastatur-Interrupt aufgesammelt hat, aber der Maus
 * gehoert - beide haengen am selben Datenport. */
void mouse_feed_byte(uint8_t byte);
/* Nachsehen, ob im Controller ein Mausbyte liegt, das kein Interrupt
 * abgeholt hat. Wird aus der Hauptschleife der Oberflaeche gerufen. */
void mouse_pump(void);
/* Haengt eine Maus am PS/2-Anschluss? */
bool mouse_attached(void);
/* Leer, solange alles gewoehnlich laeuft; sonst eine kurze Anmerkung
 * fuer die Systeminformation. */
const char *mouse_note(void);
void mouse_set_position(int32_t x, int32_t y);

/* Bewegung von einem anderen Treiber - etwa der USB-Maus. */
void mouse_inject(int32_t dx, int32_t dy, int8_t scroll,
                  bool left, bool right, bool middle);
void mouse_inject_absolute(int32_t x, int32_t y, int8_t scroll,
                           bool left, bool right, bool middle);

#endif /* INPUT_H */
