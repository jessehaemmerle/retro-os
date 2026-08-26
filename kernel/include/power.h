/* power.h - Neustart und Abschalten. */
#ifndef POWER_H
#define POWER_H

#include "retro.h"

NORETURN void power_reboot(void);
NORETURN void power_shutdown(void);

#endif /* POWER_H */
