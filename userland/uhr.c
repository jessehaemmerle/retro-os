/* uhr.c - eine Uhr mit eigenem Fenster.
 *
 * Das erste Programm, das in Ring 3 laeuft und trotzdem etwas auf dem
 * Bildschirm anstellt. Es besitzt kein einziges Pixel selbst: Es
 * schickt Zeichenbefehle hinueber, und der Kernel malt sie in seine
 * Leinwand.
 *
 * Beenden: Fenster schliessen oder Esc druecken.
 */

#include "retroos.h"
#include "retroui.h"

#define W 320
#define H 180

/* Zeigt die Laufzeit als hh:mm:ss - eine echte Uhrzeit gibt es fuer
 * Programme (noch) nicht, die Betriebsdauer schon. */
static void format_uptime(char *out, unsigned long ms)
{
    unsigned long total = ms / 1000;
    unsigned h = (unsigned)(total / 3600);
    unsigned m = (unsigned)((total / 60) % 60);
    unsigned s = (unsigned)(total % 60);

    out[0] = (char)('0' + (h / 10) % 10);
    out[1] = (char)('0' + h % 10);
    out[2] = ':';
    out[3] = (char)('0' + m / 10);
    out[4] = (char)('0' + m % 10);
    out[5] = ':';
    out[6] = (char)('0' + s / 10);
    out[7] = (char)('0' + s % 10);
    out[8] = '\0';
}

int main(void)
{
    int win = ui_open("Uhr", W, H);

    if (win < 0) {
        print("Das Fenster liess sich nicht oeffnen.\n");
        return 1;
    }

    unsigned int ticks = 0;

    for (;;) {
        struct ui_event ev;

        while (ui_event(win, &ev, 0) == 1) {
            if (ev.type == UI_EV_CLOSE)
                return 0;
            if (ev.type == UI_EV_KEY && ev.key == 27)   /* Esc */
                goto done;
        }

        char text[16];

        format_uptime(text, (unsigned long)sys_uptime());

        struct ui_cmd cmds[4];

        memset(cmds, 0, sizeof(cmds));

        cmds[0].op = UI_CLEAR;
        cmds[0].color = ui_rgb(0x10, 0x18, 0x28);

        /* Ein Rahmen, der im Takt der Sekunde die Farbe wechselt -
         * damit man sieht, dass wirklich neu gezeichnet wird. */
        cmds[1].op = UI_RECT;
        cmds[1].x = 8; cmds[1].y = 8;
        cmds[1].w = W - 16; cmds[1].h = H - 16;
        cmds[1].color = (ticks / 20) % 2 ? ui_rgb(0x40, 0x90, 0xC0)
                                         : ui_rgb(0x20, 0x50, 0x70);

        cmds[2].op = UI_TEXT;
        cmds[2].x = 24; cmds[2].y = 40;
        cmds[2].color = ui_rgb(0xC0, 0xD0, 0xE0);
        strcpy(cmds[2].text, "Seit dem Start:");

        cmds[3].op = UI_TEXT;
        cmds[3].x = 24; cmds[3].y = 70;
        cmds[3].color = ui_rgb(0xF0, 0xF0, 0xF0);
        strcpy(cmds[3].text, text);

        ui_draw(win, cmds, 4);

        ticks++;
        sys_sleep(50);
    }

done:
    ui_close(win);
    return 0;
}
