/* rtc.c - Uhrzeit aus dem CMOS lesen.
 *
 * Die Uhr aktualisiert sich einmal pro Sekunde; waehrend dieser Zeit sind
 * die Register instabil. Deshalb wird zweimal gelesen und nur ein Ergebnis
 * akzeptiert, das sich nicht veraendert hat.
 */

#include "config.h"
#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static bool update_in_progress(void)
{
    return cmos_read(0x0A) & 0x80;
}

static uint8_t from_bcd(uint8_t value)
{
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

static void read_raw(struct datetime *dt)
{
    dt->second = cmos_read(0x00);
    dt->minute = cmos_read(0x02);
    dt->hour   = cmos_read(0x04);
    dt->day    = cmos_read(0x07);
    dt->month  = cmos_read(0x08);
    dt->year   = cmos_read(0x09);
}

/* Wie viele Tage hat dieser Monat? */
static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[13] = { 0, 31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31 };

    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return month >= 1 && month <= 12 ? days[month] : 30;
}

/* Verschiebt eine Zeitangabe um Minuten - auch ueber Tages-, Monats-
 * und Jahresgrenzen hinweg. */
static void shift_minutes(struct datetime *dt, int32_t minutes)
{
    int32_t total = dt->hour * 60 + dt->minute + minutes;

    while (total < 0) {
        total += 24 * 60;
        if (--dt->day == 0) {
            if (--dt->month == 0) {
                dt->month = 12;
                dt->year--;
            }
            dt->day = days_in_month(dt->year, dt->month);
        }
    }
    while (total >= 24 * 60) {
        total -= 24 * 60;
        if (++dt->day > days_in_month(dt->year, dt->month)) {
            dt->day = 1;
            if (++dt->month > 12) {
                dt->month = 1;
                dt->year++;
            }
        }
    }

    dt->hour   = (uint8_t)(total / 60);
    dt->minute = (uint8_t)(total % 60);
}

void rtc_read(struct datetime *out)
{
    struct datetime a, b;

    for (int guard = 0; guard < 100; guard++) {
        while (update_in_progress())
            ;
        read_raw(&a);
        while (update_in_progress())
            ;
        read_raw(&b);

        if (a.second == b.second && a.minute == b.minute &&
            a.hour == b.hour && a.day == b.day &&
            a.month == b.month && a.year == b.year)
            break;
    }

    uint8_t status_b = cmos_read(0x0B);
    bool    bcd      = !(status_b & 0x04);
    bool    hour12   = !(status_b & 0x02);
    bool    pm       = (a.hour & 0x80) != 0;

    if (bcd) {
        a.second = from_bcd(a.second);
        a.minute = from_bcd(a.minute);
        a.hour   = from_bcd((uint8_t)(a.hour & 0x7F));
        a.day    = from_bcd(a.day);
        a.month  = from_bcd(a.month);
        a.year   = from_bcd((uint8_t)a.year);
    } else {
        a.hour &= 0x7F;
    }

    if (hour12 && pm && a.hour < 12)
        a.hour = (uint8_t)(a.hour + 12);
    if (hour12 && !pm && a.hour == 12)
        a.hour = 0;

    a.year = (uint16_t)(a.year + 2000);

    /* Steht die Batterieuhr auf UTC, kommt die Zeitzone dazu. Auf
     * Ortszeit gestellt (so machen es Rechner, auf denen auch Windows
     * laeuft) bleibt sie, wie sie ist. */
    if (config_current()->clock == CLOCK_UTC)
        shift_minutes(&a, config_current()->timezone);

    *out = a;
}

void rtc_format_time(char *buf, size_t size)
{
    struct datetime dt;
    rtc_read(&dt);
    ksnprintf(buf, size, "%02u:%02u", dt.hour, dt.minute);
}

void rtc_format_date(char *buf, size_t size)
{
    struct datetime dt;
    rtc_read(&dt);
    ksnprintf(buf, size, "%02u.%02u.%04u", dt.day, dt.month, dt.year);
}
