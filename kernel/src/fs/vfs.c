/* vfs.c - der Dateibaum von RetroOS.
 *
 * Der Baum kennt zwei Sorten von Knoten. Knoten im Arbeitsspeicher halten
 * ihren Inhalt direkt im Heap; sie bilden die Wurzel und alles darunter, was
 * nach einem Neustart wieder im Auslieferungszustand sein soll. Knoten auf
 * einem Datentraeger spiegeln Eintraege eines FAT32-Dateisystems: sie werden
 * erst gelesen, wenn jemand hinsieht, und jede Aenderung geht sofort auf die
 * Platte.
 *
 * Nach aussen sieht man den Unterschied nicht - Dateimanager, Editor und
 * Konsole arbeiten mit denselben Funktionen, egal wo eine Datei liegt.
 */

#include "vfs.h"
#include "block.h"
#include "kstring.h"
#include "mm.h"
#include "rtc.h"

static struct fat_volume disk_volume;
static struct fs_node   *disk_mount_point;
static char              disk_label[32];

static void fat_load_children(struct fs_node *dir);

/* Stellt sicher, dass ein Ordner von der Platte eingelesen wurde. */
static void ensure_loaded(struct fs_node *dir)
{
    if (dir && dir->type == FS_DIR && dir->backend == FS_BACKEND_FAT &&
        !dir->children_loaded)
        fat_load_children(dir);
}

static struct fs_node *root;
static size_t node_count;

static void stamp(struct fs_node *node)
{
    struct datetime dt;

    rtc_read(&dt);
    node->mtime_hour  = dt.hour;
    node->mtime_min   = dt.minute;
    node->mtime_day   = dt.day;
    node->mtime_month = dt.month;
    node->mtime_year  = dt.year;
}

static struct fs_node *node_new(const char *name, enum fs_type type)
{
    struct fs_node *n = kzalloc(sizeof(struct fs_node));

    if (!n)
        return NULL;

    strlcpy(n->name, name, sizeof(n->name));
    n->type = (uint8_t)type;
    stamp(n);
    node_count++;
    return n;
}

static void link_child(struct fs_node *dir, struct fs_node *child)
{
    child->parent = dir;
    child->next_sibling = NULL;

    if (!dir->first_child) {
        dir->first_child = child;
        return;
    }

    struct fs_node *it = dir->first_child;
    while (it->next_sibling)
        it = it->next_sibling;
    it->next_sibling = child;
}

static void unlink_child(struct fs_node *node)
{
    struct fs_node *dir = node->parent;

    if (!dir)
        return;

    if (dir->first_child == node) {
        dir->first_child = node->next_sibling;
    } else {
        struct fs_node *it = dir->first_child;
        while (it && it->next_sibling != node)
            it = it->next_sibling;
        if (it)
            it->next_sibling = node->next_sibling;
    }
    node->next_sibling = NULL;
    node->parent = NULL;
}

struct fs_node *fs_find_child(struct fs_node *dir, const char *name)
{
    if (!dir || dir->type != FS_DIR)
        return NULL;

    ensure_loaded(dir);

    for (struct fs_node *it = dir->first_child; it; it = it->next_sibling) {
        if (strcasecmp(it->name, name) == 0)
            return it;
    }
    return NULL;
}

struct fs_node *fs_create(struct fs_node *dir, const char *name, enum fs_type type)
{
    if (!dir || dir->type != FS_DIR || !name || !name[0])
        return NULL;
    if (strchr(name, '/'))
        return NULL;
    if (fs_find_child(dir, name))
        return NULL;

    struct fs_node *n = node_new(name, type);
    if (!n)
        return NULL;

    if (dir->backend == FS_BACKEND_FAT) {
        struct fat_dirent entry;

        if (!fat_create(&disk_volume, dir->fat_cluster, name,
                        type == FS_DIR, &entry)) {
            kfree(n);
            return NULL;
        }

        n->backend         = FS_BACKEND_FAT;
        n->fat_cluster     = entry.first_cluster;
        n->fat_ref         = entry.ref;
        n->children_loaded = (type == FS_DIR);
    }

    link_child(dir, n);
    stamp(dir);
    return n;
}

struct fs_node *fs_create_path(struct fs_node *base, const char *path,
                               enum fs_type type)
{
    char parent[FS_PATH_MAX];
    const char *name = path;

    strlcpy(parent, path, sizeof(parent));

    char *slash = strrchr(parent, '/');
    struct fs_node *dir;

    if (slash) {
        *slash = '\0';
        name = path + (slash - parent) + 1;
        dir = fs_lookup(base, parent[0] ? parent : "/");
    } else {
        dir = base ? base : root;
    }

    if (!dir || dir->type != FS_DIR || !name[0])
        return NULL;

    return fs_create(dir, name, type);
}

static void free_subtree(struct fs_node *node)
{
    struct fs_node *child = node->first_child;

    while (child) {
        struct fs_node *next = child->next_sibling;
        free_subtree(child);
        child = next;
    }

    if (node->data)
        kfree(node->data);
    kfree(node);
    node_count--;
}

/* Loescht einen Plattenknoten samt Inhalt - Ordner von innen nach aussen. */
static bool fat_remove_recursive(struct fs_node *node)
{
    if (node->type == FS_DIR) {
        ensure_loaded(node);

        struct fs_node *child = node->first_child;
        while (child) {
            struct fs_node *next = child->next_sibling;

            if (!fat_remove_recursive(child))
                return false;
            child = next;
        }
    }
    return fat_delete(&disk_volume, &node->fat_ref, node->fat_cluster,
                      node->type == FS_DIR);
}

bool fs_remove(struct fs_node *node)
{
    if (!node || node == root || node->readonly)
        return false;
    if (node == disk_mount_point)
        return false;

    if (node->backend == FS_BACKEND_FAT && !fat_remove_recursive(node))
        return false;

    struct fs_node *parent = node->parent;
    unlink_child(node);
    free_subtree(node);
    if (parent)
        stamp(parent);
    return true;
}

bool fs_rename(struct fs_node *node, const char *name)
{
    if (!node || node == root || !name || !name[0] || strchr(name, '/'))
        return false;

    struct fs_node *clash = fs_find_child(node->parent, name);
    if (clash && clash != node)
        return false;

    if (node->backend == FS_BACKEND_FAT) {
        struct fat_dirent entry;

        memset(&entry, 0, sizeof(entry));
        strlcpy(entry.name, node->name, sizeof(entry.name));
        entry.is_dir        = node->type == FS_DIR;
        entry.first_cluster = node->fat_cluster;
        entry.size          = (uint32_t)node->size;
        entry.ref           = node->fat_ref;

        if (!fat_rename(&disk_volume, &entry, name))
            return false;
        node->fat_ref = entry.ref;
    }

    strlcpy(node->name, name, sizeof(node->name));
    stamp(node);
    return true;
}

bool fs_move(struct fs_node *node, struct fs_node *new_parent)
{
    if (!node || !new_parent || new_parent->type != FS_DIR || node == root)
        return false;
    if (node == new_parent)
        return false;
    if (fs_find_child(new_parent, node->name))
        return false;

    /* Ein Ordner darf nicht in sich selbst verschoben werden. */
    for (struct fs_node *p = new_parent; p; p = p->parent) {
        if (p == node)
            return false;
    }

    unlink_child(node);
    link_child(new_parent, node);
    stamp(new_parent);
    return true;
}

static bool ensure_capacity(struct fs_node *file, size_t need)
{
    if (file->capacity >= need)
        return true;

    size_t cap = file->capacity ? file->capacity : 64;
    while (cap < need)
        cap *= 2;

    uint8_t *buf = krealloc(file->data, cap);
    if (!buf)
        return false;

    file->data = buf;
    file->capacity = cap;
    return true;
}

bool fs_write(struct fs_node *file, const void *data, size_t size)
{
    if (!file || file->type != FS_FILE || file->readonly)
        return false;

    if (file->backend == FS_BACKEND_FAT) {
        uint32_t cluster = file->fat_cluster;

        if (!fat_write_file(&disk_volume, &file->fat_ref, &cluster,
                            data, (uint32_t)size))
            return false;
        file->fat_cluster = cluster;
    }

    if (!ensure_capacity(file, size + 1))
        return false;

    memcpy(file->data, data, size);
    file->data[size] = '\0';
    file->size = size;
    stamp(file);
    /* Geaenderte Sektoren gleich hinausschreiben - ein Rechner wird
     * abgeschaltet, ohne zu fragen. */
    block_flush(disk_volume.dev);
    return true;
}

bool fs_append(struct fs_node *file, const void *data, size_t size)
{
    if (!file || file->type != FS_FILE || file->readonly)
        return false;
    if (!fs_load(file))
        return false;
    if (!ensure_capacity(file, file->size + size + 1))
        return false;

    memcpy(file->data + file->size, data, size);
    file->size += size;
    file->data[file->size] = '\0';

    if (file->backend == FS_BACKEND_FAT) {
        uint32_t cluster = file->fat_cluster;

        if (!fat_write_file(&disk_volume, &file->fat_ref, &cluster,
                            file->data, (uint32_t)file->size))
            return false;
        file->fat_cluster = cluster;
    }

    stamp(file);
    return true;
}

struct fs_node *fs_lookup(struct fs_node *base, const char *path)
{
    if (!path)
        return NULL;

    struct fs_node *cur = (path[0] == '/') ? root : (base ? base : root);

    char part[FS_NAME_MAX + 1];
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;

        size_t n = 0;
        while (*path && *path != '/' && n < FS_NAME_MAX)
            part[n++] = *path++;
        part[n] = '\0';

        if (strcmp(part, ".") == 0)
            continue;
        if (strcmp(part, "..") == 0) {
            cur = cur->parent ? cur->parent : root;
            continue;
        }

        cur = fs_find_child(cur, part);
        if (!cur)
            return NULL;
    }
    return cur;
}

size_t fs_child_count(struct fs_node *dir)
{
    size_t n = 0;

    if (!dir || dir->type != FS_DIR)
        return 0;

    ensure_loaded(dir);
    for (struct fs_node *it = dir->first_child; it; it = it->next_sibling)
        n++;
    return n;
}

/* Ordner vor Dateien, danach alphabetisch ohne Ruecksicht auf Gross/Klein. */
static bool sorts_before(const struct fs_node *a, const struct fs_node *b)
{
    if (a->type != b->type)
        return a->type == FS_DIR;
    return strcasecmp(a->name, b->name) < 0;
}

size_t fs_list(struct fs_node *dir, struct fs_node **out, size_t max)
{
    size_t n = 0;

    if (!dir || dir->type != FS_DIR)
        return 0;

    ensure_loaded(dir);

    for (struct fs_node *it = dir->first_child; it && n < max; it = it->next_sibling)
        out[n++] = it;

    /* Einfaches Insertion-Sort - Ordner haben selten hunderte Eintraege. */
    for (size_t i = 1; i < n; i++) {
        struct fs_node *key = out[i];
        size_t j = i;

        while (j > 0 && sorts_before(key, out[j - 1])) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return n;
}

void fs_path(struct fs_node *node, char *buf, size_t size)
{
    if (!node || !size)
        return;

    if (node == root || !node->parent) {
        strlcpy(buf, "/", size);
        return;
    }

    /* Von unten nach oben sammeln, dann in richtiger Reihenfolge ausgeben. */
    const char *parts[32];
    int depth = 0;

    for (struct fs_node *it = node; it && it != root && depth < 32; it = it->parent)
        parts[depth++] = it->name;

    size_t pos = 0;
    for (int i = depth - 1; i >= 0 && pos + 1 < size; i--) {
        buf[pos++] = '/';
        size_t len = strlen(parts[i]);
        for (size_t k = 0; k < len && pos + 1 < size; k++)
            buf[pos++] = parts[i][k];
    }
    buf[pos] = '\0';
}

bool fs_node_alive(const struct fs_node *node)
{
    if (!node)
        return false;

    for (const struct fs_node *it = node; it; it = it->parent) {
        if (it == root)
            return true;
    }
    return false;
}

size_t fs_total_size(struct fs_node *node)
{
    if (!node)
        return 0;
    if (node->type == FS_FILE)
        return node->size;

    /* Nicht eingelesene Plattenordner bleiben aussen vor - sonst wuerde
     * eine Statuszeile die ganze Platte durchsuchen. */
    if (node->backend == FS_BACKEND_FAT && !node->children_loaded)
        return 0;

    size_t total = 0;
    for (struct fs_node *it = node->first_child; it; it = it->next_sibling)
        total += fs_total_size(it);
    return total;
}

void fs_format_size(char *buf, size_t bufsize, size_t bytes)
{
    if (bytes < 1024)
        ksnprintf(buf, bufsize, "%u B", (unsigned)bytes);
    else if (bytes < 1024 * 1024)
        ksnprintf(buf, bufsize, "%u,%u KB",
                  (unsigned)(bytes / 1024), (unsigned)((bytes % 1024) * 10 / 1024));
    else
        ksnprintf(buf, bufsize, "%u,%u MB",
                  (unsigned)(bytes / (1024 * 1024)),
                  (unsigned)((bytes % (1024 * 1024)) * 10 / (1024 * 1024)));
}

size_t fs_node_count(void) { return node_count; }
size_t fs_bytes_used(void) { return fs_total_size(root); }

bool fs_is_text(const struct fs_node *node)
{
    if (!node || node->type != FS_FILE)
        return false;

    const char *dot = strrchr(node->name, '.');
    if (!dot)
        return true;

    static const char *text_ext[] = { ".txt", ".md", ".cfg", ".conf", ".log",
                                      ".c", ".h", ".sh", ".ini", ".list",
                                      ".js", ".json", ".css" };
    for (size_t i = 0; i < ARRAY_LEN(text_ext); i++) {
        if (strcasecmp(dot, text_ext[i]) == 0)
            return true;
    }
    return false;
}

struct fs_node *fs_root(void)
{
    return root;
}

void initfs_populate(struct fs_node *root_dir);

void fs_init(void)
{
    root = node_new("/", FS_DIR);
    if (!root)
        panic("Dateisystem laesst sich nicht anlegen");

    root->readonly = true;
    initfs_populate(root);

    kprintf("Dateisystem : %u Eintraege, %u Bytes\n",
            (unsigned)node_count, (unsigned)fs_bytes_used());
}

/* ------------------------------------------------------------------ */
/* Datentraeger einhaengen                                             */
/* ------------------------------------------------------------------ */

static void load_child_cb(void *user, const struct fat_dirent *entry)
{
    struct fs_node *dir = user;
    struct fs_node *n = kzalloc(sizeof(struct fs_node));

    if (!n)
        return;

    strlcpy(n->name, entry->name, sizeof(n->name));
    n->type        = entry->is_dir ? FS_DIR : FS_FILE;
    n->readonly    = entry->read_only;
    n->size        = entry->size;
    n->backend     = FS_BACKEND_FAT;
    n->fat_cluster = entry->first_cluster;
    n->fat_ref     = entry->ref;

    n->mtime_day   = entry->day;
    n->mtime_month = entry->month;
    n->mtime_year  = entry->year;
    n->mtime_hour  = entry->hour;
    n->mtime_min   = entry->minute;

    node_count++;
    link_child(dir, n);
}

static void fat_load_children(struct fs_node *dir)
{
    dir->children_loaded = true;
    fat_list_dir(&disk_volume, dir->fat_cluster, load_child_cb, dir);
}

bool fs_load(struct fs_node *file)
{
    if (!file || file->type != FS_FILE)
        return false;
    if (file->backend != FS_BACKEND_FAT)
        return true;
    if (file->data)
        return true;

    uint8_t *buf = kmalloc(file->size + 1);
    if (!buf)
        return false;

    if (file->size > 0 &&
        !fat_read_file(&disk_volume, file->fat_cluster,
                       (uint32_t)file->size, buf)) {
        kfree(buf);
        return false;
    }

    buf[file->size] = '\0';
    file->data     = buf;
    file->capacity = file->size + 1;
    return true;
}

static bool attach_mount_point(void)
{
    /* Einen eventuell vorhandenen alten Einhaengepunkt entfernen. */
    struct fs_node *old = fs_find_child(root, "Festplatte");
    if (old) {
        old->readonly = false;
        struct fs_node *parent_backup = old->parent;
        UNUSED(parent_backup);

        /* Nur den Baum im Speicher loesen, nichts auf der Platte anfassen. */
        old->backend = FS_BACKEND_RAM;
        disk_mount_point = NULL;
        fs_remove(old);
    }

    struct fs_node *mount = fs_create(root, "Festplatte", FS_DIR);
    if (!mount)
        return false;

    mount->backend         = FS_BACKEND_FAT;
    mount->fat_cluster     = disk_volume.root_cluster;
    mount->children_loaded = false;
    mount->readonly        = true;   /* der Einhaengepunkt selbst bleibt */

    disk_mount_point = mount;
    return true;
}

bool fs_mount_disk(void)
{
    struct block_device *dev = block_primary();

    if (!dev) {
        kprintf("Datentraeger: keiner gefunden\n");
        return false;
    }

    if (!fat_mount(dev, &disk_volume)) {
        kprintf("Datentraeger: kein FAT32 gefunden - \"formatieren\" hilft\n");
        return false;
    }

    if (!attach_mount_point())
        return false;

    ksnprintf(disk_label, sizeof(disk_label), "%s",
              disk_volume.label[0] ? disk_volume.label : dev->name);

    kprintf("Datentraeger: %s eingehaengt unter /Festplatte (%u MiB frei)\n",
            disk_label,
            (unsigned)(fat_free_bytes(&disk_volume) / (1024 * 1024)));
    return true;
}

/* Vergisst alles, was von der Platte im Baum haengt. Noetig, bevor der
 * Traeger neu beschrieben wird - danach zeigen die gemerkten Cluster ins
 * Leere. */
void fs_detach_disk(void)
{
    if (disk_mount_point) {
        struct fs_node *child = disk_mount_point->first_child;

        while (child) {
            struct fs_node *next = child->next_sibling;

            child->backend = FS_BACKEND_RAM;
            child->readonly = false;
            fs_remove(child);
            child = next;
        }
        disk_mount_point->children_loaded = false;
    }
    disk_volume.mounted = false;
}

bool fs_format_disk(const char *label)
{
    struct block_device *dev = block_primary();

    if (!dev)
        return false;

    fs_detach_disk();

    if (!fat_format(dev, label && label[0] ? label : "RETROOS"))
        return false;

    return fs_mount_disk();
}

bool fs_disk_mounted(void)          { return disk_volume.mounted; }
struct fat_volume *fs_disk_volume(void) { return &disk_volume; }
struct fs_node *fs_disk_root(void)  { return disk_mount_point; }
const char *fs_disk_name(void)      { return disk_label; }
