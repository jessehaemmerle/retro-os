/* ps2.h - gemeinsame Grundfunktionen des 8042-Controllers.
 * Werden von keyboard.c und mouse.c benutzt. */
#ifndef PS2_H
#define PS2_H

#include "retro.h"

bool    ps2_wait_write(void);
bool    ps2_wait_read(void);
void    ps2_write_cmd(uint8_t cmd);
void    ps2_write_data(uint8_t data);
uint8_t ps2_read_data(void);

/* Ein Byte an das Geraet an Port 2 (Maus) schicken; liefert die Quittung. */
uint8_t ps2_mouse_command(uint8_t byte);

#endif /* PS2_H */
