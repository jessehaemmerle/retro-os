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
/* Wie ps2_read_data, aber false bei Zeitueberschreitung. */
bool    ps2_read_byte(uint8_t *out);
/* Mit eigener Frist in Millisekunden. */
bool    ps2_read_byte_ms(uint8_t *out, uint32_t ms);

/* Ein Byte an das Geraet an Port 2 (Maus) schicken; liefert die Quittung. */
uint8_t ps2_mouse_command(uint8_t byte);
/* Dasselbe mit Auskunft: true = mit 0xFA quittiert. answer darf NULL sein. */
bool    ps2_mouse_command_ok(uint8_t byte, uint8_t *answer);

/* Hat der Selbsttest des zweiten Ports geklappt? */
bool    ps2_port2_present(void);


#endif /* PS2_H */
