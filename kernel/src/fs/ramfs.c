/* ramfs.c - Dateisystem im Arbeitsspeicher.
 *
 * Jeder Knoten ist ein Ordner oder eine Datei; Ordner verketten ihre Kinder
 * einfach hintereinander. Dateiinhalte liegen als Heap-Puffer vor, der beim
 * Schreiben verdoppelt wird. Fuer ein System, dessen gesamter Datenbestand in
 * den RAM passt, ist das voellig ausreichend - und es kommt ohne Blockgeraet,
 * Cache oder Journal aus.
 */

#include "vfs.h"
#include "kstring.h"
#include "mm.h"
#include "rtc.h"

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

    link_child(dir, n);
    stamp(dir);
    return n;
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

bool fs_remove(struct fs_node *node)
{
    if (!node || node == root || node->readonly)
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
    if (!ensure_capacity(file, size + 1))
        return false;

    memcpy(file->data, data, size);
    file->data[size] = '\0';
    file->size = size;
    stamp(file);
    return true;
}

bool fs_append(struct fs_node *file, const void *data, size_t size)
{
    if (!file || file->type != FS_FILE || file->readonly)
        return false;
    if (!ensure_capacity(file, file->size + size + 1))
        return false;

    memcpy(file->data + file->size, data, size);
    file->size += size;
    file->data[file->size] = '\0';
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
                                      ".c", ".h", ".sh", ".ini", ".list" };
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
