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
};

void acpi_init(void);
const struct acpi_info *acpi_get(void);

bool acpi_enable_mode(void);
/* Beide kehren nur zurueck, wenn es nicht geklappt hat. */
bool acpi_shutdown(void);
bool acpi_reset(void);

#endif /* ACPI_H */
