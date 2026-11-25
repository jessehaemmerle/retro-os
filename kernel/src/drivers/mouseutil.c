/* mouseutil.c - siehe mouseutil.h. */

#include "mouseutil.h"

bool ps2_decode_packet(const uint8_t *packet, size_t size,
                       struct ps2_motion *out)
{
    if (!packet || !out || size < 3)
        return false;

    uint8_t flags = packet[0];

    /* Bit 3 ist bei jedem gueltigen Paket im ersten Byte gesetzt. */
    if (!(flags & 0x08))
        return false;

    int32_t dx = packet[1];
    int32_t dy = packet[2];

    /* Das neunte Bit steht im ersten Byte. */
    if (flags & 0x10)
        dx -= 256;
    if (flags & 0x20)
        dy -= 256;

    out->dx = dx;
    out->dy = -dy;          /* die Maus zaehlt nach oben, der Schirm nach unten */
    out->scroll = 0;

    if (size >= 4) {
        /* Vier Bit mit Vorzeichen. Die oberen vier gehoeren bei Typ 4
         * zu den Tasten 4 und 5 und duerfen das Rad nicht
         * verfaelschen. */
        int32_t z = packet[3] & 0x0F;

        if (z & 0x08)
            z -= 16;

        /* Bei PS/2 ist "zum Benutzer hin" positiv, die Oberflaeche
         * rechnet andersherum. Der Betrag passt immer in ein int8_t:
         * -8 wird zu +8. */
        out->scroll = (int8_t)-z;
    }

    out->buttons = 0;
    if (flags & 0x01)
        out->buttons |= MOUSE_BTN_LEFT;
    if (flags & 0x02)
        out->buttons |= MOUSE_BTN_RIGHT;
    if (flags & 0x04)
        out->buttons |= MOUSE_BTN_MIDDLE;
    return true;
}

size_t mouse_button_steps(uint8_t prev, uint8_t now, uint8_t pressed,
                          uint8_t released, struct mouse_step *out)
{
    static const uint8_t buttons[3] = {
        MOUSE_BTN_LEFT, MOUSE_BTN_RIGHT, MOUSE_BTN_MIDDLE,
    };
    size_t n = 0;

    for (int i = 0; i < 3; i++) {
        uint8_t b = buttons[i];
        bool state = (prev & b) != 0;
        bool want_down = (pressed & b) != 0;
        bool want_up = (released & b) != 0;

        /* War die Taste schon unten und wurde losgelassen, ist das
         * zuerst geschehen - sonst koennte sie nicht erneut gedrueckt
         * worden sein. */
        if (state && want_up) {
            out[n++] = (struct mouse_step){ b, false };
            state = false;
            want_up = false;
        }

        if (want_down && !state) {
            out[n++] = (struct mouse_step){ b, true };
            state = true;
        }

        if (want_up && state) {
            out[n++] = (struct mouse_step){ b, false };
            state = false;
        }

        /* Hat der Treiber keine Flanke mitgezaehlt, stimmt aber der
         * Zustand nicht - etwa weil er den Wert nur gesetzt hat -,
         * zaehlt, was jetzt gilt. */
        if (state != ((now & b) != 0)) {
            out[n++] = (struct mouse_step){ b, (now & b) != 0 };
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Doppelklick                                                          */
/* ------------------------------------------------------------------ */

static int32_t distance(int32_t a, int32_t b)
{
    return a > b ? a - b : b - a;
}

bool mouse_is_double_click(uint64_t now_ms, uint64_t last_ms,
                           int32_t x, int32_t y, int32_t last_x, int32_t last_y,
                           uint32_t window_ms)
{
    if (last_ms == 0 || now_ms < last_ms)
        return false;

    return now_ms - last_ms < window_ms &&
           distance(x, last_x) < DOUBLE_CLICK_SLOP &&
           distance(y, last_y) < DOUBLE_CLICK_SLOP;
}

uint32_t mouse_double_click_sane(uint32_t ms)
{
    if (ms < DOUBLE_CLICK_MIN_MS || ms > DOUBLE_CLICK_MAX_MS)
        return DOUBLE_CLICK_DEFAULT_MS;
    return ms;
}

static const uint32_t click_steps[] = { 200, 300, 400, 500, 600, 800, 1000 };

uint32_t mouse_double_click_step(uint32_t ms, int delta)
{
    size_t count = sizeof(click_steps) / sizeof(click_steps[0]);

    ms = mouse_double_click_sane(ms);

    if (delta > 0) {
        for (size_t i = 0; i < count; i++) {
            if (click_steps[i] > ms)
                return click_steps[i];
        }
        return click_steps[0];                  /* im Kreis */
    }

    for (size_t i = count; i > 0; i--) {
        if (click_steps[i - 1] < ms)
            return click_steps[i - 1];
    }
    return click_steps[count - 1];
}
