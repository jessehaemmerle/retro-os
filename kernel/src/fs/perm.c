/* perm.c - die Rechtepruefung und die gespeicherten Rechte.
 *
 * Die Pruefung selbst ist ein Dreizeiler; das meiste hier drin ist die
 * Buchhaltung darum herum. Wichtig ist nur die Reihenfolge: Wer
 * Eigentuemer ist, wird nach den Eigentuemerbits beurteilt - und zwar
 * auch dann, wenn die Gruppenbits mehr erlauben wuerden. Sonst koennte
 * man sich selbst aussperren und ueber den Umweg der eigenen Gruppe
 * doch wieder hineinkommen, und die Bits meinten nicht mehr, was
 * dasteht.
 */

#include "perm.h"
#include "kstring.h"
#include "mm.h"
#include "thread.h"
#include "user.h"

/* ------------------------------------------------------------------ */
/* Pruefung                                                            */
/* ------------------------------------------------------------------ */

void perm_system_begin(void)
{
    struct thread *t = thread_current();

    if (t)
        t->perm_system++;
}

void perm_system_end(void)
{
    struct thread *t = thread_current();

    if (t && t->perm_system)
        t->perm_system--;
}

bool perm_system_active(void)
{
    struct thread *t = thread_current();

    return t && t->perm_system;
}

bool perm_check(const struct fs_node *node, uint32_t uid, uint32_t gid,
                uint8_t want)
{
    if (!node)
        return false;
    if (uid == UID_ROOT)
        return true;

    uint16_t mode = node->mode;
    uint8_t  bits;

    if (node->uid == uid)
        bits = (uint8_t)((mode >> 6) & 7);
    else if (node->gid == gid || user_in_group(uid, node->gid))
        bits = (uint8_t)((mode >> 3) & 7);
    else
        bits = (uint8_t)(mode & 7);

    return (bits & want) == want;
}

bool perm_may(const struct fs_node *node, uint8_t want)
{
    if (perm_system_active())
        return true;
    if (session_is_admin())
        return true;
    return perm_check(node, session_uid(), session_gid(), want);
}

bool perm_owns(const struct fs_node *node)
{
    if (!node)
        return false;
    if (perm_system_active() || session_is_admin())
        return true;
    return node->uid == session_uid();
}

bool perm_may_unlink(const struct fs_node *dir, const struct fs_node *node)
{
    if (!dir || !node)
        return false;
    if (!perm_may(dir, P_W | P_X))
        return false;

    /* Klebebit: in einem Ordner, in dem alle ablegen duerfen, nimmt nur
     * der Eigentuemer wieder weg - seinen eigenen Eintrag oder, als
     * Herr des Ordners, jeden. */
    if (dir->mode & MODE_STICKY) {
        if (perm_system_active() || session_is_admin())
            return true;

        uint32_t me = session_uid();

        if (node->uid != me && dir->uid != me)
            return false;
    }
    return true;
}

bool perm_set_mode(struct fs_node *node, uint16_t mode)
{
    if (!node || !perm_owns(node))
        return false;

    node->mode = (uint16_t)(mode & (MODE_MASK | MODE_STICKY));
    perm_store_record(node);
    return true;
}

bool perm_set_owner(struct fs_node *node, uint32_t uid, uint32_t gid)
{
    if (!node)
        return false;

    /* Eine Datei zu verschenken darf nur der Verwalter: sonst koennte
     * man sein Kontingent umgehen, indem man alles jemandem anhaengt. */
    if (!perm_system_active() && !session_is_admin())
        return false;

    node->uid = uid;
    node->gid = gid;
    perm_store_record(node);
    return true;
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

void perm_mode_text(uint16_t mode, uint8_t type, char out[11])
{
    static const char rwx[] = "rwx";

    out[0] = type == FS_DIR ? 'd' : '-';
    for (int block = 0; block < 3; block++) {
        uint8_t bits = (uint8_t)((mode >> (6 - block * 3)) & 7);

        for (int b = 0; b < 3; b++)
            out[1 + block * 3 + b] = (bits & (4 >> b)) ? rwx[b] : '-';
    }

    /* Das Klebebit sitzt auf dem x der uebrigen: klein, wenn dort auch
     * ausgefuehrt werden darf, sonst gross. So macht es Unix seit je. */
    if (mode & MODE_STICKY)
        out[9] = (mode & 1) ? 't' : 'T';
    out[10] = '\0';
}

bool perm_parse_mode(const char *text, uint16_t *out)
{
    if (!text || !text[0] || !out)
        return false;

    /* Erst die Zahlenform: "750" oder "0750". */
    bool octal = true;

    for (const char *p = text; *p; p++)
        if (*p < '0' || *p > '7')
            octal = false;

    if (octal) {
        size_t len = strlen(text);

        if (len < 1 || len > 4)
            return false;

        uint16_t value = 0;

        for (const char *p = text; *p; p++)
            value = (uint16_t)((value << 3) | (uint16_t)(*p - '0'));
        *out = (uint16_t)(value & (MODE_MASK | MODE_STICKY));
        return true;
    }

    /* Sonst die Buchstabenform "rwxr-x---", wahlweise mit der Art davor. */
    if (strlen(text) == 10 && (text[0] == 'd' || text[0] == '-'))
        text++;
    if (strlen(text) != 9)
        return false;

    uint16_t value = 0;
    static const char expect[] = "rwxrwxrwx";

    for (int i = 0; i < 9; i++) {
        char c = text[i];

        if (c == '-')
            continue;
        if (i == 8 && (c == 't' || c == 'T')) {
            value |= MODE_STICKY;
            if (c == 't')
                value |= 1;
            continue;
        }
        if (c != expect[i])
            return false;
        value |= (uint16_t)(1 << (8 - i));
    }
    *out = value;
    return true;
}

/* ------------------------------------------------------------------ */
/* Gespeicherte Rechte                                                 */
/* ------------------------------------------------------------------ */

/* FAT32 hat kein Feld fuer Eigentuemer oder Rechte. Statt das
 * Dateisystem zu erweitern - das koennte dann kein anderes System mehr
 * lesen - liegt daneben eine Liste. Sie ist klein, weil nur Eintraege
 * darin stehen, die vom Standard abweichen. */
#define PERM_ENTRIES 128
#define PERM_PATH_MAX 128

struct perm_entry {
    bool     used;
    char     path[PERM_PATH_MAX];
    uint32_t uid, gid;
    uint16_t mode;
};

static struct perm_entry entries[PERM_ENTRIES];
static bool dirty;

bool perm_store_dirty(void) { return dirty; }

static struct perm_entry *find_entry(const char *path)
{
    for (size_t i = 0; i < PERM_ENTRIES; i++)
        if (entries[i].used && strcasecmp(entries[i].path, path) == 0)
            return &entries[i];
    return NULL;
}

/* Nur Knoten auf der Platte lohnen einen Eintrag - alles im
 * Arbeitsspeicher entsteht bei jedem Start neu. */
static bool worth_saving(const struct fs_node *node)
{
    return node && node->backend == FS_BACKEND_FAT;
}

void perm_store_apply(struct fs_node *node)
{
    if (!worth_saving(node))
        return;

    char path[PERM_PATH_MAX];

    fs_path(node, path, sizeof(path));

    struct perm_entry *e = find_entry(path);

    if (!e)
        return;

    node->uid  = e->uid;
    node->gid  = e->gid;
    node->mode = e->mode;
}

void perm_store_record(struct fs_node *node)
{
    if (!worth_saving(node))
        return;

    char path[PERM_PATH_MAX];

    fs_path(node, path, sizeof(path));
    if (!path[0])
        return;

    struct perm_entry *e = find_entry(path);

    if (!e) {
        for (size_t i = 0; i < PERM_ENTRIES && !e; i++)
            if (!entries[i].used)
                e = &entries[i];
        if (!e)
            return;                 /* voll - dann bleibt es fluechtig */
        e->used = true;
        strlcpy(e->path, path, sizeof(e->path));
    }

    /* Nur was sich wirklich aendert, macht die Datei schmutzig - sonst
     * schriebe jeder Start sie einmal neu. */
    if (e->uid != node->uid || e->gid != node->gid || e->mode != node->mode) {
        e->uid  = node->uid;
        e->gid  = node->gid;
        e->mode = node->mode;
        dirty = true;
    }
}

void perm_store_forget(const struct fs_node *node)
{
    if (!worth_saving(node))
        return;

    char path[PERM_PATH_MAX];

    fs_path((struct fs_node *)node, path, sizeof(path));

    struct perm_entry *e = find_entry(path);

    if (e) {
        memset(e, 0, sizeof(*e));
        dirty = true;
    }
}

static uint32_t to_number(const char **text, char stop)
{
    uint32_t value = 0;

    while (**text >= '0' && **text <= '9')
        value = value * 10 + (uint32_t)(*(*text)++ - '0');
    if (**text == stop)
        (*text)++;
    return value;
}

void perm_store_load(void)
{
    memset(entries, 0, sizeof(entries));
    dirty = false;

    perm_system_begin();

    struct fs_node *file = fs_lookup(NULL, PERM_PATH);

    if (file && file->type == FS_FILE && fs_load(file) && file->data &&
        file->size > 0 && file->size <= 32768) {
        size_t size = file->size;
        char  *text = kmalloc(size + 1);

        if (text) {
            memcpy(text, file->data, size);
            text[size] = '\0';

            char *line = text;

            while (line && *line) {
                char *next = strchr(line, '\n');

                if (next)
                    *next++ = '\0';

                char *equals = strchr(line, '=');

                if (line[0] == '#' || !equals) {
                    line = next;
                    continue;
                }
                *equals = '\0';

                /* Der Pfad steht vorn und darf Leerzeichen enthalten -
                 * hinten wird nur der Rand abgeschnitten. */
                size_t len = strlen(line);

                while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
                    line[--len] = '\0';

                const char *v = equals + 1;

                while (*v == ' ' || *v == '\t')
                    v++;

                uint32_t uid = to_number(&v, ':');
                uint32_t gid = to_number(&v, ':');
                uint16_t mode = 0;

                while (*v >= '0' && *v <= '7')
                    mode = (uint16_t)((mode << 3) | (uint16_t)(*v++ - '0'));

                if (line[0] == '/' && len < PERM_PATH_MAX) {
                    for (size_t i = 0; i < PERM_ENTRIES; i++) {
                        if (entries[i].used)
                            continue;
                        entries[i].used = true;
                        strlcpy(entries[i].path, line, sizeof(entries[i].path));
                        entries[i].uid  = uid;
                        entries[i].gid  = gid;
                        entries[i].mode = (uint16_t)(mode &
                                          (MODE_MASK | MODE_STICKY));
                        break;
                    }
                }
                line = next;
            }
            kfree(text);
        }
    }

    perm_system_end();
}

bool perm_store_save(void)
{
    if (!fs_disk_mounted())
        return false;

    size_t cap = 16384;
    char  *text = kmalloc(cap);

    if (!text)
        return false;

    size_t used = 0;

    #define ADD(...) do {                                            \
        if (used < cap - 1) {                                        \
            ksnprintf(text + used, cap - used, __VA_ARGS__);         \
            used += strlen(text + used);                             \
        }                                                            \
    } while (0)

    ADD("# Eigentuemer und Rechte der Dateien auf der Platte\n"
        "#\n"
        "# FAT32 hat dafuer kein Feld, darum stehen sie hier daneben:\n"
        "# <pfad> = <uid>:<gid>:<rechte in Achtelschritten>\n"
        "#\n"
        "# Was hier fehlt, bekommt die Standardeinstellung.\n\n");

    for (size_t i = 0; i < PERM_ENTRIES; i++) {
        if (!entries[i].used)
            continue;
        ADD("%s = %u:%u:%04o\n", entries[i].path,
            (unsigned)entries[i].uid, (unsigned)entries[i].gid,
            (unsigned)entries[i].mode);
    }
    #undef ADD

    perm_system_begin();

    struct fs_node *file = fs_lookup(NULL, PERM_PATH);

    if (!file)
        file = fs_create_path(NULL, PERM_PATH, FS_FILE);

    bool ok = file && file->type == FS_FILE && fs_write(file, text, used);

    if (ok) {
        file->uid  = UID_ROOT;
        file->gid  = GID_ROOT;
        file->mode = 0644;
        dirty = false;
    }

    perm_system_end();
    kfree(text);
    return ok;
}
