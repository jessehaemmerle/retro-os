/* acpi.h - Energieverwaltung ueber die ACPI-Tabellen. */
#ifndef ACPI_H
#define ACPI_H

#include "retro.h"

struct acpi_info {
    bool     available;
    bool     can_shutdown;
    bool     can_reset;

    uint32_t pm1a_control;
    uint32_t pm1b_control;
    uint32_t smi_command;
    uint8_t  acpi_enable;

    uint8_t  slp_typ_a;
    uint8_t  slp_typ_b;

    uint64_t reset_register;
    uint8_t  reset_space;
    uint8_t  reset_value;

    /* Aus den Boot-Kennzeichen der FADT: sagt, welche Altgeraete der
     * Rechner ueberhaupt noch hat. */
    uint16_t boot_flags;
    bool     boot_flags_valid;
};

/* Bit 1 der Boot-Kennzeichen: es gibt einen 8042-Controller. */
#define ACPI_BOOT_8042 (1u << 1)

/* Gibt es an diesem Rechner noch einen PS/2-Anschluss? Ohne ACPI-Angabe
 * lautet die Antwort "vermutlich ja". */
bool acpi_has_ps2(void);

void acpi_init(void);

/* Liefert eine Tabelle anhand ihrer Kennung, etwa "APIC" oder "HPET". */
const void *acpi_find_table(const char *signature);
const struct acpi_info *acpi_get(void);

bool acpi_enable_mode(void);
/* Beide kehren nur zurueck, wenn es nicht geklappt hat. */
bool acpi_shutdown(void);
bool acpi_reset(void);

#endif /* ACPI_H */
