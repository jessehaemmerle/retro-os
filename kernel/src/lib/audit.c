/* audit.c - die Pruefspur fuehren.
 *
 * Derselbe Ringaufbau wie beim Protokoll, aber ohne die Moeglichkeit,
 * ihn zu leeren: Wer eine Pruefspur loeschen kann, hat keine.
 *
 * Wenn eine Festplatte da ist, wird sie fortgeschrieben statt ersetzt.
 * Das kostet beim Sichern einmal Lesen, ist aber der ganze Unterschied
 * zwischen einer Spur und einer Momentaufnahme.
 */

#include "audit.h"

#include "arch.h"
#include "kstring.h"
#include "log.h"
#include "mm.h"
#include "spinlock.h"
#include "user.h"
#include "vfs.h"

#include <stdarg.h>

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

static struct audit_entry ring[AUDIT_ENTRIES];
static struct spinlock    lock = SPINLOCK_INIT("pruefspur");
static uint32_t           written;
static uint32_t           saved;      /* wie viele schon auf der Platte */

void audit(enum audit_kind kind, bool ok, const char *fmt, ...)
{
    char    object[AUDIT_OBJECT_MAX + 1];
    va_list ap;

    va_start(ap, fmt);
    kvsnprintf(object, sizeof(object), fmt, ap);
    va_end(ap);

    uint32_t uid = session_uid();
    uint64_t flags = spin_lock_irq(&lock);
    struct audit_entry *e = &ring[written % AUDIT_ENTRIES];

    e->seq  = written + 1;
    e->ms   = timer_ms();
    e->kind = (uint8_t)kind;
    e->ok   = ok;
    e->uid  = uid;
    strlcpy(e->object, object, sizeof(e->object));
    written++;

    spin_unlock_irq(&lock, flags);

    /* Was schiefging, faellt auch im Protokoll auf - dort sucht man
     * zuerst, wenn etwas nicht stimmt. */
    if (!ok)
        log_warn("pruefspur", "%s abgewiesen: %s (Benutzer %u)",
                 audit_kind_name(kind), object, (unsigned)uid);
}

size_t audit_count(void)
{
    uint32_t n = written;

    return n < AUDIT_ENTRIES ? n : AUDIT_ENTRIES;
}

uint32_t audit_lost(void)
{
    return written > AUDIT_ENTRIES ? written - AUDIT_ENTRIES : 0;
}

bool audit_get(size_t index, struct audit_entry *out)
{
    if (!out || index >= audit_count())
        return false;

    uint64_t flags = spin_lock_irq(&lock);
    uint32_t first = written > AUDIT_ENTRIES ? written - AUDIT_ENTRIES : 0;

    *out = ring[(first + index) % AUDIT_ENTRIES];
    spin_unlock_irq(&lock, flags);
    return true;
}

size_t audit_count_failed(void)
{
    size_t n = 0;
    size_t count = audit_count();
    struct audit_entry e;

    for (size_t i = 0; i < count; i++)
        if (audit_get(i, &e) && !e.ok)
            n++;
    return n;
}

const char *audit_kind_name(enum audit_kind kind)
{
    switch (kind) {
    case AUDIT_LOGIN:     return "Anmeldung";
    case AUDIT_LOGOUT:    return "Abmeldung";
    case AUDIT_DENIED:    return "Zugriff";
    case AUDIT_PRIVILEGE: return "Recht";
    case AUDIT_ACCOUNT:   return "Konto";
    case AUDIT_NETWORK:   return "Netz";
    default:              return "Programm";
    }
}

bool audit_readable(void)
{
    return session_can(CAP_LOG);
}

bool audit_save(void)
{
    if (!fs_disk_mounted())
        return false;

    /* Nur das Neue anhaengen - die Spur waechst, sie wird nicht
     * ersetzt. Wer sie kuerzen will, muss die Datei selbst anfassen,
     * und das steht dann in den Rechten. */
    size_t count = audit_count();
    uint32_t first_seq = written > AUDIT_ENTRIES
                       ? written - AUDIT_ENTRIES + 1 : 1;
    size_t cap = count * (AUDIT_OBJECT_MAX + 64) + 256;
    char  *text = kmalloc(cap);

    if (!text)
        return false;

    size_t used = 0;
    struct audit_entry e;

    #define ADD(...) do {                                        \
        if (used < cap - 1) {                                    \
            ksnprintf(text + used, cap - used, __VA_ARGS__);     \
            used += strlen(text + used);                         \
        }                                                        \
    } while (0)

    for (size_t i = 0; i < count; i++) {
        if (!audit_get(i, &e))
            break;
        if (e.seq <= saved)
            continue;
        ADD("%u\t%u.%03u\t%-10s\t%-9s\t%s\t%s\n",
            (unsigned)e.seq, (unsigned)(e.ms / 1000),
            (unsigned)(e.ms % 1000), audit_kind_name(e.kind),
            e.ok ? "erlaubt" : "abgewiesen", user_name_of(e.uid), e.object);
    }
    #undef ADD

    if (!used) {
        kfree(text);
        return true;                /* nichts Neues - auch ein Erfolg */
    }

    struct fs_node *file = fs_lookup(NULL, AUDIT_PATH);

    if (!file) {
        file = fs_create_path(NULL, AUDIT_PATH, FS_FILE);
        if (file) {
            file->uid  = UID_ROOT;
            file->gid  = GID_ROOT;
            file->mode = 0640;      /* lesen darf die Gruppe, aendern niemand */
            fs_write(file, "# Pruefspur von RetroOS - wird nur angehaengt.\n"
                           "# Nummer\tZeit\tArt\tAusgang\tBenutzer\tGegenstand\n",
                     94);
        }
    }

    bool ok = file && file->type == FS_FILE && fs_append(file, text, used);

    if (ok && count)
        saved = first_seq + (uint32_t)count - 1;

    kfree(text);
    return ok;
}
