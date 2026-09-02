/* trash.c - der Papierkorb.
 *
 * Der Korb liegt unter /Papierkorb im Arbeitsspeicher. Das hat einen
 * Grund: Geloescht wird auch auf der Festplatte, und ein Eintrag von
 * dort muesste sonst quer ueber die Dateisystemgrenze verschoben
 * werden. Stattdessen wird er in den Korb kopiert und auf der Platte
 * geloescht - eine Bewegung, die jedes der beiden Dateisysteme fuer
 * sich versteht.
 *
 * Der Preis steht ehrlich hier: Was im Korb liegt, belegt Hauptspeicher
 * und ist nach einem Neustart weg. Dafuer ist ein Fehlgriff bis dahin
 * ruecknehmbar, und genau darum geht es.
 *
 * Woher ein Eintrag stammt, steht in einer Liste daneben. Sie ist an
 * den Namen im Korb gebunden, nicht an den Knoten: Ein Knoten kann
 * unter den Fingern wegsterben, ein Name nicht.
 */

#include "trash.h"

#include "kstring.h"
#include "mm.h"
#include "perm.h"
#include "user.h"

#define TRASH_MAX 64

struct entry {
    bool used;
    char name[FS_NAME_MAX + 1];
    char origin[FS_PATH_MAX];
};

static struct fs_node *korb;
static struct entry    entries[TRASH_MAX];

void trash_init(void)
{
    memset(entries, 0, sizeof(entries));

    korb = fs_lookup(fs_root(), TRASH_PATH);
    if (!korb) {
        /* Jeder wirft hier hinein, aber das Klebebit sorgt dafuer, dass
         * niemand fremde Sachen wieder herausholt oder endgueltig
         * loescht. Ohne das waere der Korb ein Loch in der Rechteverwaltung:
         * geloeschte Dateien liegen darin ja weiter vollstaendig da. */
        korb = fs_create_as(fs_root(), TRASH_PATH + 1, FS_DIR,
                            UID_ROOT, GID_ROOT, 0777 | MODE_STICKY);
    }

    /* Der Korb selbst laesst sich nicht wegwerfen. */
    if (korb)
        korb->readonly = true;
}

struct fs_node *trash_dir(void)
{
    return korb;
}

bool trash_contains(const struct fs_node *node)
{
    for (const struct fs_node *p = node; p; p = p->parent) {
        if (p == korb)
            return true;
    }
    return false;
}

/* --- Herkunftsliste ------------------------------------------------- */

static struct entry *find_entry(const char *name)
{
    for (size_t i = 0; i < TRASH_MAX; i++) {
        if (entries[i].used && !strcmp(entries[i].name, name))
            return &entries[i];
    }
    return NULL;
}

static void remember(const char *name, const char *origin)
{
    struct entry *slot = find_entry(name);

    if (!slot) {
        for (size_t i = 0; i < TRASH_MAX; i++) {
            if (!entries[i].used) {
                slot = &entries[i];
                break;
            }
        }
    }
    if (!slot)
        return;             /* Die Liste ist voll - dann eben ohne Herkunft. */

    slot->used = true;
    strlcpy(slot->name, name, sizeof(slot->name));
    strlcpy(slot->origin, origin, sizeof(slot->origin));
}

static void forget(const char *name)
{
    struct entry *slot = find_entry(name);

    if (slot)
        slot->used = false;
}

const char *trash_origin(const struct fs_node *node)
{
    if (!node || node->parent != korb)
        return "";

    struct entry *slot = find_entry(node->name);

    return slot ? slot->origin : "";
}

/* --- Kopieren ueber die Dateisystemgrenze --------------------------- */

/* Die Kopie im Korb behaelt Eigentuemer und Rechte des Originals -
 * sonst kaeme etwas anderes zurueck, als weggeworfen wurde. */
static bool copy_into(struct fs_node *src, struct fs_node *dest_parent,
                      const char *name)
{
    if (src->type == FS_DIR) {
        struct fs_node *copy = fs_create_as(dest_parent, name, FS_DIR,
                                            src->uid, src->gid, src->mode);

        if (!copy)
            return false;

        for (struct fs_node *child = src->first_child; child;
             child = child->next_sibling) {
            if (!copy_into(child, copy, child->name))
                return false;
        }
        return true;
    }

    if (!fs_load(src))
        return false;

    struct fs_node *copy = fs_create_as(dest_parent, name, FS_FILE,
                                        src->uid, src->gid, src->mode);

    if (!copy)
        return false;
    if (src->size == 0)
        return true;

    /* Schreiben darf hier das System: Rechte sind schon beim Wegwerfen
     * geprueft worden, und die Kopie gehoert unter Umstaenden jemand
     * anderem als dem, der geloescht hat. */
    perm_system_begin();

    bool ok = fs_write(copy, src->data, src->size);

    perm_system_end();
    return ok;
}

/* Ein freier Name im Korb: "bericht.txt", dann "bericht.txt (2)". */
static void free_name(const char *wanted, char *out, size_t size)
{
    strlcpy(out, wanted, size);
    if (!fs_find_child(korb, out))
        return;

    for (unsigned n = 2; n < 1000; n++) {
        ksnprintf(out, size, "%s (%u)", wanted, n);
        if (!fs_find_child(korb, out))
            return;
    }
}

/* --- Wegwerfen ------------------------------------------------------ */

bool trash_delete(struct fs_node *node)
{
    if (!korb || !node || node == korb || node->readonly)
        return false;
    if (trash_contains(node))
        return false;           /* schon drin - von hier geht es nur raus */

    char origin[FS_PATH_MAX];
    char name[FS_NAME_MAX + 1];

    fs_path(node, origin, sizeof(origin));
    free_name(node->name, name, sizeof(name));

    /* Liegt der Eintrag ohnehin im Arbeitsspeicher, genuegt das
     * Umhaengen - kein Byte wird angefasst. */
    if (node->backend == FS_BACKEND_RAM) {
        if (strcmp(node->name, name) != 0 && !fs_rename(node, name))
            return false;
        if (!fs_move(node, korb))
            return false;
        remember(name, origin);
        return true;
    }

    if (!copy_into(node, korb, name))
        return false;
    if (!fs_remove(node)) {
        /* Die Platte hat sich gewehrt - dann bleibt auch die Kopie
         * nicht stehen. */
        struct fs_node *copy = fs_find_child(korb, name);

        if (copy)
            fs_remove(copy);
        return false;
    }

    remember(name, origin);
    return true;
}

/* --- Zurueckholen --------------------------------------------------- */

/* Legt den Ordner an, in den ein Eintrag zurueck soll. */
static struct fs_node *ensure_dir(const char *path)
{
    if (!path[0] || !strcmp(path, "/"))
        return fs_root();

    struct fs_node *found = fs_lookup(fs_root(), path);

    if (found)
        return found->type == FS_DIR ? found : NULL;

    return fs_create_path(fs_root(), path, FS_DIR);
}

bool trash_restore(struct fs_node *node)
{
    if (!korb || !node || node->parent != korb)
        return false;

    struct entry *slot = find_entry(node->name);
    char target[FS_PATH_MAX];

    /* Ohne Herkunft geht es in die Wurzel - besser dort als nirgends. */
    strlcpy(target, slot ? slot->origin : "/", sizeof(target));

    char *cut = strrchr(target, '/');
    const char *wanted = cut ? cut + 1 : node->name;
    char folder[FS_PATH_MAX];

    strlcpy(folder, target, sizeof(folder));
    if (cut) {
        size_t at = (size_t)(cut - target);

        folder[at ? at : 1] = '\0';
    } else {
        strlcpy(folder, "/", sizeof(folder));
    }

    struct fs_node *dest = ensure_dir(folder);

    if (!dest)
        return false;

    char name[FS_NAME_MAX + 1];

    strlcpy(name, wanted[0] ? wanted : node->name, sizeof(name));

    /* Am alten Platz steht schon wieder etwas: nicht ueberschreiben. */
    for (unsigned n = 2; fs_find_child(dest, name) && n < 1000; n++)
        ksnprintf(name, sizeof(name), "%s (%u)", wanted, n);

    if (fs_find_child(dest, name))
        return false;

    char in_trash[FS_NAME_MAX + 1];

    strlcpy(in_trash, node->name, sizeof(in_trash));

    if (dest->backend == FS_BACKEND_RAM && node->backend == FS_BACKEND_RAM) {
        if (strcmp(node->name, name) != 0 && !fs_rename(node, name))
            return false;
        if (!fs_move(node, dest))
            return false;
    } else {
        if (!copy_into(node, dest, name))
            return false;
        fs_remove(node);
    }

    forget(in_trash);
    return true;
}

/* --- Endgueltig ----------------------------------------------------- */

bool trash_purge(struct fs_node *node)
{
    if (!korb || !node || node->parent != korb)
        return false;

    char name[FS_NAME_MAX + 1];

    strlcpy(name, node->name, sizeof(name));
    if (!fs_remove(node))
        return false;

    forget(name);
    return true;
}

size_t trash_empty(void)
{
    size_t gone = 0;

    if (!korb)
        return 0;

    /* Von vorn, bis nichts mehr da ist: Jeder Durchgang haengt den
     * ersten Eintrag aus, der naechste rueckt nach. */
    while (korb->first_child) {
        if (!trash_purge(korb->first_child))
            break;
        gone++;
    }
    return gone;
}

size_t trash_count(void)
{
    return korb ? fs_child_count(korb) : 0;
}

size_t trash_bytes(void)
{
    return korb ? fs_total_size(korb) : 0;
}
