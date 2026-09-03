/* trig.c - siehe trig.h. */

#include "trig.h"

/* Sinus von 0 bis 90 Grad, mit 10000 vervielfacht. Erzeugt mit einem
 * Zeilenskript und danach gegen die Identitaeten geprueft, die im
 * Testlauf stehen. */
static const int16_t table[91] = {
        0,   175,   349,   523,   698,   872,  1045,  1219,  1392,  1564,
     1736,  1908,  2079,  2250,  2419,  2588,  2756,  2924,  3090,  3256,
     3420,  3584,  3746,  3907,  4067,  4226,  4384,  4540,  4695,  4848,
     5000,  5150,  5299,  5446,  5592,  5736,  5878,  6018,  6157,  6293,
     6428,  6561,  6691,  6820,  6947,  7071,  7193,  7314,  7431,  7547,
     7660,  7771,  7880,  7986,  8090,  8192,  8290,  8387,  8480,  8572,
     8660,  8746,  8829,  8910,  8988,  9063,  9135,  9205,  9272,  9336,
     9397,  9455,  9511,  9563,  9613,  9659,  9703,  9744,  9781,  9816,
     9848,  9877,  9903,  9925,  9945,  9962,  9976,  9986,  9994,  9998,
    10000,
};

int32_t sin_deg(int32_t degrees)
{
    /* Erst in den Bereich 0 bis 359 holen - auch fuer negative
     * Winkel, denn der Rest einer negativen Division ist in C
     * negativ. */
    int32_t d = degrees % 360;

    if (d < 0)
        d += 360;

    if (d <= 90)
        return table[d];
    if (d <= 180)
        return table[180 - d];
    if (d <= 270)
        return -table[d - 180];
    return -table[360 - d];
}

int32_t cos_deg(int32_t degrees)
{
    /* Der Kosinus ist der um ein Viertel verschobene Sinus. */
    return sin_deg(degrees + 90);
}
