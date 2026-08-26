/* rtc.h - Echtzeituhr des Rechners (CMOS). */
#ifndef RTC_H
#define RTC_H

#include "retro.h"

struct datetime {
    uint16_t year;
    uint8_t  month, day;
    uint8_t  hour, minute, second;
};

void rtc_read(struct datetime *out);

/* Kurzform "HH:MM" bzw. "TT.MM.JJJJ". */
void rtc_format_time(char *buf, size_t size);
void rtc_format_date(char *buf, size_t size);

#endif /* RTC_H */
