/* clipboard.c - eine Zwischenablage fuer das ganze System.
 *
 * Sie haelt schlicht einen Text. Wer kopiert, legt ihn hier ab; wer
 * einfuegt, holt ihn wieder. Editor, Konsole und Browser benutzen
 * dieselbe - deshalb sitzt sie beim Fenstersystem und nicht in einem
 * der Programme.
 */

#include "clipboard.h"
#include "kstring.h"
#include "mm.h"

static char  *content;
static size_t length;

bool clipboard_set(const char *text, size_t bytes)
{
    if (!text || bytes == 0) {
        clipboard_clear();
        return true;
    }

    char *copy = kmalloc(bytes + 1);

    if (!copy)
        return false;

    memcpy(copy, text, bytes);
    copy[bytes] = '\0';

    kfree(content);
    content = copy;
    length = bytes;
    return true;
}

const char *clipboard_get(size_t *bytes)
{
    if (bytes)
        *bytes = length;
    return content;
}

bool clipboard_empty(void)
{
    return content == NULL || length == 0;
}

void clipboard_clear(void)
{
    kfree(content);
    content = NULL;
    length = 0;
}
