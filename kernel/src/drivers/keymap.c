/* keymap.c - die Tabellen der Tastaturbelegungen.
 *
 * Drei Belegungen: die deutsche (QWERTZ), die amerikanische (QWERTY)
 * und die schweizerische. Sie unterscheiden sich in drei Ebenen -
 * ungedrueckt, mit Umschalt und mit AltGr - und in beiden
 * Zaehlweisen, die die Hardware benutzt: Scancodes am PS/2-Anschluss,
 * Verwendungsnummern am USB-Bus.
 */

#include "keymap.h"
#include "input.h"
#include "kstring.h"

/* ------------------------------------------------------------------ */
/* Deutsch                                                             */
/* ------------------------------------------------------------------ */

static const uint16_t de_ps2_base[KEYMAP_PS2_CODES] = {
    [0x01] = KEY_ESCAPE,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = 0xDF /* ss */, [0x0D] = '\'',
    [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'z', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = 0xFC /* ue */, [0x1B] = '+',
    [0x1C] = KEY_ENTER, [0x1D] = KEY_LCTRL,
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = 0xF6 /* oe */, [0x28] = 0xE4 /* ae */, [0x29] = '^',
    [0x2A] = KEY_LSHIFT, [0x2B] = '#',
    [0x2C] = 'y', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '-',
    [0x36] = KEY_RSHIFT, [0x37] = '*', [0x38] = KEY_LALT, [0x39] = ' ',
    [0x3A] = KEY_CAPSLOCK,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGEUP,
    [0x4A] = '-', [0x4B] = KEY_LEFT, [0x4D] = KEY_RIGHT, [0x4E] = '+',
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGEDOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x56] = '<', [0x57] = KEY_F11, [0x58] = KEY_F12,
};

static const uint16_t de_ps2_shift[KEYMAP_PS2_CODES] = {
    [0x02] = '!', [0x03] = '"', [0x04] = 0xA7, [0x05] = '$',
    [0x06] = '%', [0x07] = '&', [0x08] = '/', [0x09] = '(', [0x0A] = ')',
    [0x0B] = '=', [0x0C] = '?', [0x0D] = '`',
    [0x1B] = '*', [0x29] = 0xB0, [0x2B] = '\'',
    [0x33] = ';', [0x34] = ':', [0x35] = '_', [0x56] = '>',
};

static const uint16_t de_ps2_altgr[KEYMAP_PS2_CODES] = {
    [0x03] = 0xB2, [0x04] = 0xB3,
    [0x08] = '{', [0x09] = '[', [0x0A] = ']', [0x0B] = '}', [0x0C] = '\\',
    [0x10] = '@', [0x11] = 0x80 /* Euro */, [0x32] = 0xB5 /* Mikro */,
    [0x1B] = '~', [0x2B] = '|', [0x38] = KEY_RALT, [0x56] = '|',
};

static const uint16_t de_hid_base[KEYMAP_HID_CODES] = {
    /* 04 */ 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
    /* 0e */ 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
    /* 18 */ 'u', 'v', 'w', 'x', 'z', 'y',
    /* 1e */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    /* 28 */ KEY_ENTER, KEY_ESCAPE, KEY_BACKSPACE, KEY_TAB, ' ',
    /* 2d */ 0xDF, 0xB4, 0xFC, '+',
    /* 31 */ '#', '#', 0xF6, 0xE4, '^',
    /* 36 */ ',', '.', '-',
    /* 39 */ KEY_CAPSLOCK,
    /* 3a */ KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    /* 40 */ KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* 46 */ 0, 0, 0,
    /* 49 */ KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END,
    /* 4e */ KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    /* 53 */ 0, '/', '*', '-', '+', KEY_ENTER,
    /* 59 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', ',',
    /* 64 */ '<',
};

static const uint16_t de_hid_shift[KEYMAP_HID_CODES] = {
    /* 04 */ 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
    /* 0e */ 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
    /* 18 */ 'U', 'V', 'W', 'X', 'Z', 'Y',
    /* 1e */ '!', '"', 0xA7, '$', '%', '&', '/', '(', ')', '=',
    /* 28 */ KEY_ENTER, KEY_ESCAPE, KEY_BACKSPACE, KEY_TAB, ' ',
    /* 2d */ '?', '`', 0xDC, '*',
    /* 31 */ '\'', '\'', 0xD6, 0xC4, 0xB0,
    /* 36 */ ';', ':', '_',
    /* 39 */ KEY_CAPSLOCK,
    /* 3a */ KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    /* 40 */ KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* 46 */ 0, 0, 0,
    /* 49 */ KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END,
    /* 4e */ KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    /* 53 */ 0, '/', '*', '-', '+', KEY_ENTER,
    /* 59 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', ',',
    /* 64 */ '>',
};

static const uint16_t de_hid_altgr[KEYMAP_HID_CODES] = {
    /* 04 */ 0, 0, 0, 0, 0x80, 0, 0, 0, 0, 0,
    /* 0e */ 0, 0, 0xB5, 0, 0, 0, '@', 0, 0, 0,
    /* 18 */ 0, 0, 0, 0, 0, 0,
    /* 1e */ 0, 0xB2, 0xB3, 0, 0, 0, '{', '[', ']', '}',
    /* 28 */ 0, 0, 0, 0, 0,
    /* 2d */ '\\', 0, 0, '~',
    /* 31 */ 0, 0, 0, 0, 0,
    /* 36 */ 0, 0, 0,
    /* 39 */ 0,
    /* 3a */ 0, 0, 0, 0, 0, 0,
    /* 40 */ 0, 0, 0, 0, 0, 0,
    /* 46 */ 0, 0, 0,
    /* 49 */ 0, 0, 0, 0, 0,
    /* 4e */ 0, 0, 0, 0, 0,
    /* 53 */ 0, 0, 0, 0, 0, 0,
    /* 59 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 64 */ '|',
};

/* ------------------------------------------------------------------ */
/* Vereinigte Staaten                                                  */
/* ------------------------------------------------------------------ */

static const uint16_t us_ps2_base[KEYMAP_PS2_CODES] = {
    [0x01] = KEY_ESCAPE,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=',
    [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']',
    [0x1C] = KEY_ENTER, [0x1D] = KEY_LCTRL,
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2A] = KEY_LSHIFT, [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x36] = KEY_RSHIFT, [0x37] = '*', [0x38] = KEY_LALT, [0x39] = ' ',
    [0x3A] = KEY_CAPSLOCK,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGEUP,
    [0x4A] = '-', [0x4B] = KEY_LEFT, [0x4D] = KEY_RIGHT, [0x4E] = '+',
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGEDOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x56] = '\\', [0x57] = KEY_F11, [0x58] = KEY_F12,
};

static const uint16_t us_ps2_shift[KEYMAP_PS2_CODES] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+',
    [0x1A] = '{', [0x1B] = '}',
    [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x56] = '|',
};

/* Die amerikanische Belegung kennt keine dritte Ebene; die rechte
 * Alt-Taste bleibt trotzdem eine Alt-Taste. */
static const uint16_t us_ps2_altgr[KEYMAP_PS2_CODES] = {
    [0x38] = KEY_RALT,
};

static const uint16_t us_hid_base[KEYMAP_HID_CODES] = {
    /* 04 */ 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
    /* 0e */ 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
    /* 18 */ 'u', 'v', 'w', 'x', 'y', 'z',
    /* 1e */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    /* 28 */ KEY_ENTER, KEY_ESCAPE, KEY_BACKSPACE, KEY_TAB, ' ',
    /* 2d */ '-', '=', '[', ']',
    /* 31 */ '\\', '\\', ';', '\'', '`',
    /* 36 */ ',', '.', '/',
    /* 39 */ KEY_CAPSLOCK,
    /* 3a */ KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    /* 40 */ KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* 46 */ 0, 0, 0,
    /* 49 */ KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END,
    /* 4e */ KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    /* 53 */ 0, '/', '*', '-', '+', KEY_ENTER,
    /* 59 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', ',',
    /* 64 */ '\\',
};

static const uint16_t us_hid_shift[KEYMAP_HID_CODES] = {
    /* 04 */ 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
    /* 0e */ 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
    /* 18 */ 'U', 'V', 'W', 'X', 'Y', 'Z',
    /* 1e */ '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    /* 28 */ KEY_ENTER, KEY_ESCAPE, KEY_BACKSPACE, KEY_TAB, ' ',
    /* 2d */ '_', '+', '{', '}',
    /* 31 */ '|', '|', ':', '"', '~',
    /* 36 */ '<', '>', '?',
    /* 39 */ KEY_CAPSLOCK,
    /* 3a */ KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    /* 40 */ KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* 46 */ 0, 0, 0,
    /* 49 */ KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END,
    /* 4e */ KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    /* 53 */ 0, '/', '*', '-', '+', KEY_ENTER,
    /* 59 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', ',',
    /* 64 */ '|',
};

static const uint16_t us_hid_altgr[KEYMAP_HID_CODES] = { 0 };

/* ------------------------------------------------------------------ */
/* Schweiz                                                             */
/* ------------------------------------------------------------------ */

static const uint16_t ch_ps2_base[KEYMAP_PS2_CODES] = {
    [0x01] = KEY_ESCAPE,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '\'', [0x0D] = '^',
    [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'z', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = 0xFC /* ue */, [0x1B] = '"',
    [0x1C] = KEY_ENTER, [0x1D] = KEY_LCTRL,
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = 0xF6 /* oe */, [0x28] = 0xE4 /* ae */, [0x29] = 0xA7,
    [0x2A] = KEY_LSHIFT, [0x2B] = '$',
    [0x2C] = 'y', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '-',
    [0x36] = KEY_RSHIFT, [0x37] = '*', [0x38] = KEY_LALT, [0x39] = ' ',
    [0x3A] = KEY_CAPSLOCK,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGEUP,
    [0x4A] = '-', [0x4B] = KEY_LEFT, [0x4D] = KEY_RIGHT, [0x4E] = '+',
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGEDOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x56] = '<', [0x57] = KEY_F11, [0x58] = KEY_F12,
};

static const uint16_t ch_ps2_shift[KEYMAP_PS2_CODES] = {
    [0x02] = '+', [0x03] = '"', [0x04] = '*', [0x05] = 0xE7 /* c-Zedille */,
    [0x06] = '%', [0x07] = '&', [0x08] = '/', [0x09] = '(', [0x0A] = ')',
    [0x0B] = '=', [0x0C] = '?', [0x0D] = '`',
    [0x1A] = 0xE8 /* e-gravis */, [0x1B] = '!',
    [0x27] = 0xE9 /* e-akut */, [0x28] = 0xE0 /* a-gravis */, [0x29] = 0xB0,
    [0x2B] = 0xA3 /* Pfund */,
    [0x33] = ';', [0x34] = ':', [0x35] = '_', [0x56] = '>',
};

static const uint16_t ch_ps2_altgr[KEYMAP_PS2_CODES] = {
    [0x02] = 0xA6, [0x03] = '@', [0x04] = '#', [0x06] = '~',
    [0x07] = 0xAC, [0x08] = '|', [0x09] = 0xA2,
    [0x0B] = '}', [0x0C] = 0xB4,
    [0x11] = 0x80 /* Euro */,
    [0x1A] = '[', [0x1B] = ']',
    [0x28] = '{', [0x2B] = '}',
    [0x38] = KEY_RALT, [0x56] = '\\',
};

static const uint16_t ch_hid_base[KEYMAP_HID_CODES] = {
    /* 04 */ 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
    /* 0e */ 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
    /* 18 */ 'u', 'v', 'w', 'x', 'z', 'y',
    /* 1e */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    /* 28 */ KEY_ENTER, KEY_ESCAPE, KEY_BACKSPACE, KEY_TAB, ' ',
    /* 2d */ '\'', '^', 0xFC, '"',
    /* 31 */ '$', '$', 0xF6, 0xE4, 0xA7,
    /* 36 */ ',', '.', '-',
    /* 39 */ KEY_CAPSLOCK,
    /* 3a */ KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    /* 40 */ KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* 46 */ 0, 0, 0,
    /* 49 */ KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END,
    /* 4e */ KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    /* 53 */ 0, '/', '*', '-', '+', KEY_ENTER,
    /* 59 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', ',',
    /* 64 */ '<',
};

static const uint16_t ch_hid_shift[KEYMAP_HID_CODES] = {
    /* 04 */ 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
    /* 0e */ 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
    /* 18 */ 'U', 'V', 'W', 'X', 'Z', 'Y',
    /* 1e */ '+', '"', '*', 0xE7, '%', '&', '/', '(', ')', '=',
    /* 28 */ KEY_ENTER, KEY_ESCAPE, KEY_BACKSPACE, KEY_TAB, ' ',
    /* 2d */ '?', '`', 0xE8, '!',
    /* 31 */ 0xA3, 0xA3, 0xE9, 0xE0, 0xB0,
    /* 36 */ ';', ':', '_',
    /* 39 */ KEY_CAPSLOCK,
    /* 3a */ KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    /* 40 */ KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* 46 */ 0, 0, 0,
    /* 49 */ KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END,
    /* 4e */ KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    /* 53 */ 0, '/', '*', '-', '+', KEY_ENTER,
    /* 59 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', ',',
    /* 64 */ '>',
};

static const uint16_t ch_hid_altgr[KEYMAP_HID_CODES] = {
    /* 04 */ 0, 0, 0, 0, 0x80, 0, 0, 0, 0, 0,
    /* 0e */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 18 */ 0, 0, 0, 0, 0, 0,
    /* 1e */ 0xA6, '@', '#', 0, 0, '~', 0xAC, '|', 0xA2, 0,
    /* 28 */ 0, 0, 0, 0, 0,
    /* 2d */ 0xB4, 0, '[', ']',
    /* 31 */ '}', '}', 0, '{', 0,
    /* 36 */ 0, 0, 0,
    /* 39 */ 0,
    /* 3a */ 0, 0, 0, 0, 0, 0,
    /* 40 */ 0, 0, 0, 0, 0, 0,
    /* 46 */ 0, 0, 0,
    /* 49 */ 0, 0, 0, 0, 0,
    /* 4e */ 0, 0, 0, 0, 0,
    /* 53 */ 0, 0, 0, 0, 0, 0,
    /* 59 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 64 */ '\\',
};

/* ------------------------------------------------------------------ */

static const struct keymap maps[] = {
    { "de", "Deutsch (QWERTZ)",
      de_ps2_base, de_ps2_shift, de_ps2_altgr,
      de_hid_base, de_hid_shift, de_hid_altgr },
    { "us", "Englisch (QWERTY)",
      us_ps2_base, us_ps2_shift, us_ps2_altgr,
      us_hid_base, us_hid_shift, us_hid_altgr },
    { "ch", "Schweiz (QWERTZ)",
      ch_ps2_base, ch_ps2_shift, ch_ps2_altgr,
      ch_hid_base, ch_hid_shift, ch_hid_altgr },
};

static size_t current;

size_t keymap_count(void) { return ARRAY_LEN(maps); }

const struct keymap *keymap_at(size_t index)
{
    return index < ARRAY_LEN(maps) ? &maps[index] : &maps[0];
}

const struct keymap *keymap_current(void) { return &maps[current]; }

size_t keymap_current_index(void) { return current; }

void keymap_select_index(size_t index)
{
    if (index < ARRAY_LEN(maps))
        current = index;
}

bool keymap_select(const char *code)
{
    if (!code)
        return false;

    for (size_t i = 0; i < ARRAY_LEN(maps); i++) {
        if (strcasecmp(maps[i].code, code) == 0) {
            current = i;
            return true;
        }
    }
    return false;
}
