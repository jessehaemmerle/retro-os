/* user.c - die Benutzer- und Gruppendatenbank samt Sitzung.
 *
 * Alles steht in zwei kleinen Feldern im Speicher; die Datei auf der
 * Platte ist nur ihr Abbild. Das reicht: Ein Rechner dieser Groesse hat
 * eine Handvoll Benutzer, keine Tausende, und eine Liste, die in einen
 * Blick passt, spart die halbe Verwaltung.
 */

#include "user.h"
#include "audit.h"
#include "crypto.h"
#include "kstring.h"
#include "log.h"
#include "mm.h"
#include "perm.h"
#include "process.h"
#include "thread.h"
#include "vfs.h"

static struct user  users[USER_MAX];
static struct group groups[GROUP_MAX];
static struct user *logged_in;
static bool         store_found;

/* ------------------------------------------------------------------ */
/* Pruefwert des Passworts                                             */
/* ------------------------------------------------------------------ */

/* Ein Durchlauf HMAC-SHA256 ueber das Passwort, dann viele weitere ueber
 * das jeweils vorige Ergebnis. Das ist PBKDF2 mit einem einzigen Block -
 * mehr braucht es nicht, weil genau 32 Bytes herauskommen sollen. Der
 * Sinn der Wiederholungen ist allein die Rechenzeit: Wer die Datei hat
 * und Passwoerter durchprobieren will, zahlt sie fuer jeden Versuch. */
static void derive(const char *password, const uint8_t *salt, uint32_t rounds,
                   uint8_t out[USER_HASH_SIZE])
{
    uint8_t block[USER_SALT_SIZE + 4];
    uint8_t work[USER_HASH_SIZE];
    size_t  len = password ? strlen(password) : 0;

    memcpy(block, salt, USER_SALT_SIZE);
    block[USER_SALT_SIZE + 0] = 0;
    block[USER_SALT_SIZE + 1] = 0;
    block[USER_SALT_SIZE + 2] = 0;
    block[USER_SALT_SIZE + 3] = 1;

    hmac_sha256((const uint8_t *)password, len, block, sizeof(block), work);
    memcpy(out, work, USER_HASH_SIZE);

    for (uint32_t i = 1; i < rounds; i++) {
        hmac_sha256((const uint8_t *)password, len, work, USER_HASH_SIZE, work);
        for (size_t b = 0; b < USER_HASH_SIZE; b++)
            out[b] ^= work[b];
    }
}

/* Vergleich ohne Abkuerzung: Die Laufzeit soll nicht verraten, wie viele
 * Bytes schon gestimmt haben. */
static bool same_hash(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;

    for (size_t i = 0; i < USER_HASH_SIZE; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* ------------------------------------------------------------------ */
/* Hilfen                                                              */
/* ------------------------------------------------------------------ */

static void to_hex(const uint8_t *data, size_t length, char *out)
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < length; i++) {
        out[i * 2 + 0] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    out[length * 2] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool from_hex(const char *text, uint8_t *out, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        int hi = hex_value(text[i * 2]);
        int lo = hex_value(text[i * 2 + 1]);

        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static uint32_t to_number(const char *text)
{
    uint32_t value = 0;

    while (*text >= '0' && *text <= '9')
        value = value * 10 + (uint32_t)(*text++ - '0');
    return value;
}

/* Namen duerfen Buchstaben, Ziffern, Punkt, Strich und Unterstrich
 * enthalten - sonst gaebe es Aerger mit Pfaden und mit der Datei, in der
 * Doppelpunkt und Zeilenende die Felder trennen. */
static bool name_ok(const char *name)
{
    if (!name || !name[0] || strlen(name) > USER_NAME_MAX)
        return false;

    for (const char *p = name; *p; p++) {
        bool letter = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z');
        bool digit  = *p >= '0' && *p <= '9';

        if (!letter && !digit && *p != '.' && *p != '-' && *p != '_')
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Faehigkeiten und Rollen                                             */
/* ------------------------------------------------------------------ */

/* Vier Rollen genuegen fuer einen Rechner dieser Groesse. Wer eine
 * fuenfte braucht, setzt die Faehigkeiten einzeln - dann heisst die
 * Rolle "eigen", und das ist ehrlicher als ein Name, der nichts
 * bedeutet. */
static const struct {
    const char *name;
    uint32_t    caps;
} roles[] = {
    { "verwalter", CAP_ALL },
    { "netzwerk",  CAP_NET },
    { "wartung",   CAP_DISK | CAP_LOG | CAP_CONFIG },
    { "benutzer",  0 },
};

static const struct {
    const char *name;
    uint32_t    bit;
} capability_names[] = {
    { "konten",       CAP_USERS  },
    { "netz",         CAP_NET    },
    { "platte",       CAP_DISK   },
    { "protokoll",    CAP_LOG    },
    { "strom",        CAP_POWER  },
    { "einstellungen", CAP_CONFIG },
};

size_t role_count(void) { return ARRAY_LEN(roles); }

const char *role_name(size_t index)
{
    return index < ARRAY_LEN(roles) ? roles[index].name : "";
}

uint32_t role_caps(const char *name)
{
    for (size_t i = 0; i < ARRAY_LEN(roles); i++)
        if (strcasecmp(roles[i].name, name) == 0)
            return roles[i].caps;
    return 0;
}

const char *caps_role(uint32_t caps)
{
    for (size_t i = 0; i < ARRAY_LEN(roles); i++)
        if (roles[i].caps == caps)
            return roles[i].name;
    return "eigen";
}

void caps_text(uint32_t caps, char *out, size_t size)
{
    size_t used = 0;

    if (!out || !size)
        return;
    out[0] = '\0';

    if ((caps & CAP_ALL) == CAP_ALL) {
        strlcpy(out, "alles", size);
        return;
    }
    if (!caps) {
        strlcpy(out, "nichts", size);
        return;
    }

    for (size_t i = 0; i < ARRAY_LEN(capability_names); i++) {
        if (!(caps & capability_names[i].bit))
            continue;
        ksnprintf(out + used, size - used, "%s%s", used ? ", " : "",
                  capability_names[i].name);
        used += strlen(out + used);
    }
}

bool cap_parse(const char *text, uint32_t *out)
{
    if (!text || !out)
        return false;
    if (strcasecmp(text, "alles") == 0) { *out = CAP_ALL; return true; }
    if (strcasecmp(text, "nichts") == 0) { *out = 0; return true; }

    for (size_t i = 0; i < ARRAY_LEN(capability_names); i++) {
        if (strcasecmp(capability_names[i].name, text) != 0)
            continue;
        *out = capability_names[i].bit;
        return true;
    }
    return false;
}

bool user_set_role(struct user *u, const char *role)
{
    if (!u || !role)
        return false;

    for (size_t i = 0; i < ARRAY_LEN(roles); i++) {
        if (strcasecmp(roles[i].name, role) != 0)
            continue;
        u->caps = roles[i].caps;
        strlcpy(u->role, roles[i].name, sizeof(u->role));

        /* Wer wem welche Rechte gibt, ist die interessanteste Zeile,
         * die eine Pruefspur enthalten kann. */
        audit(AUDIT_ACCOUNT, true, "%s bekommt die Rolle %s", u->name,
              u->role);
        log_info("benutzer", "%s hat jetzt die Rolle %s", u->name, u->role);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Nachschlagen                                                        */
/* ------------------------------------------------------------------ */

size_t user_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < USER_MAX; i++)
        if (users[i].used)
            n++;
    return n;
}

struct user *user_at(size_t index)
{
    for (size_t i = 0; i < USER_MAX; i++) {
        if (!users[i].used)
            continue;
        if (index-- == 0)
            return &users[i];
    }
    return NULL;
}

struct user *user_by_name(const char *name)
{
    if (!name)
        return NULL;

    for (size_t i = 0; i < USER_MAX; i++)
        if (users[i].used && strcasecmp(users[i].name, name) == 0)
            return &users[i];
    return NULL;
}

struct user *user_by_uid(uint32_t uid)
{
    for (size_t i = 0; i < USER_MAX; i++)
        if (users[i].used && users[i].uid == uid)
            return &users[i];
    return NULL;
}

const char *user_name_of(uint32_t uid)
{
    struct user *u = user_by_uid(uid);

    return u ? u->name : "?";
}

size_t group_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < GROUP_MAX; i++)
        if (groups[i].used)
            n++;
    return n;
}

struct group *group_at(size_t index)
{
    for (size_t i = 0; i < GROUP_MAX; i++) {
        if (!groups[i].used)
            continue;
        if (index-- == 0)
            return &groups[i];
    }
    return NULL;
}

struct group *group_by_name(const char *name)
{
    if (!name)
        return NULL;

    for (size_t i = 0; i < GROUP_MAX; i++)
        if (groups[i].used && strcasecmp(groups[i].name, name) == 0)
            return &groups[i];
    return NULL;
}

struct group *group_by_gid(uint32_t gid)
{
    for (size_t i = 0; i < GROUP_MAX; i++)
        if (groups[i].used && groups[i].gid == gid)
            return &groups[i];
    return NULL;
}

const char *group_name_of(uint32_t gid)
{
    struct group *g = group_by_gid(gid);

    return g ? g->name : "?";
}

struct group *group_create(const char *name, uint32_t gid)
{
    if (!name_ok(name) || group_by_name(name) || group_by_gid(gid))
        return NULL;

    for (size_t i = 0; i < GROUP_MAX; i++) {
        if (groups[i].used)
            continue;
        memset(&groups[i], 0, sizeof(groups[i]));
        groups[i].used = true;
        groups[i].gid = gid;
        strlcpy(groups[i].name, name, sizeof(groups[i].name));
        return &groups[i];
    }
    return NULL;
}

bool group_add_member(struct group *g, uint32_t uid)
{
    if (!g)
        return false;

    for (size_t i = 0; i < g->members; i++)
        if (g->member[i] == uid)
            return true;
    if (g->members >= GROUP_MEMBER_MAX)
        return false;
    g->member[g->members++] = uid;
    return true;
}

bool group_remove_member(struct group *g, uint32_t uid)
{
    if (!g)
        return false;

    for (size_t i = 0; i < g->members; i++) {
        if (g->member[i] != uid)
            continue;
        for (size_t k = i; k + 1 < g->members; k++)
            g->member[k] = g->member[k + 1];
        g->members--;
        return true;
    }
    return false;
}

bool user_in_group(uint32_t uid, uint32_t gid)
{
    struct user *u = user_by_uid(uid);

    if (u && u->gid == gid)
        return true;

    struct group *g = group_by_gid(gid);

    if (!g)
        return false;
    for (size_t i = 0; i < g->members; i++)
        if (g->member[i] == uid)
            return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Anlegen und aendern                                                 */
/* ------------------------------------------------------------------ */

static void fail(char *error, size_t size, const char *text)
{
    if (error && size)
        strlcpy(error, text, size);
}

bool user_set_password(struct user *u, const char *password)
{
    if (!u)
        return false;

    crypto_random(u->salt, sizeof(u->salt));
    u->rounds = USER_ROUNDS;
    u->nopass = !password || !password[0];
    derive(password, u->salt, u->rounds, u->hash);

    if (u->nopass)
        log_warn("benutzer", "%s hat jetzt kein Passwort", u->name);
    else
        log_info("benutzer", "Passwort von %s gesetzt", u->name);
    audit(AUDIT_ACCOUNT, true, "Passwort von %s %s", u->name,
          u->nopass ? "geloescht" : "gesetzt");
    return true;
}

bool user_check_password(const struct user *u, const char *password)
{
    if (!u || !u->used || u->locked)
        return false;

    /* Ein Benutzer ohne Passwort kommt mit leerer Eingabe herein - aber
     * auch nur damit; sonst waere jede Eingabe richtig. */
    if (u->nopass)
        return !password || !password[0];
    if (!password || !password[0])
        return false;

    uint8_t got[USER_HASH_SIZE];

    derive(password, u->salt, u->rounds ? u->rounds : USER_ROUNDS, got);
    return same_hash(got, u->hash);
}

/* Die naechste freie Nummer. Menschen bekommen ab 1000 eine, damit sich
 * die kleinen Nummern fuer das System reservieren lassen. */
static uint32_t next_uid(void)
{
    uint32_t uid = 1000;

    while (user_by_uid(uid))
        uid++;
    return uid;
}

static void default_home(const char *name, char *out, size_t size)
{
    if (fs_disk_mounted())
        ksnprintf(out, size, "/Festplatte/Benutzer/%s", name);
    else
        ksnprintf(out, size, "/Benutzer/%s", name);
}

struct user *user_create(const char *name, const char *full,
                         const char *password, bool admin,
                         char *error, size_t error_size)
{
    if (!name_ok(name)) {
        fail(error, error_size,
             "Der Name darf nur Buchstaben, Ziffern, Punkt, Strich und "
             "Unterstrich enthalten.");
        return NULL;
    }
    if (user_by_name(name)) {
        fail(error, error_size, "Diesen Benutzer gibt es schon.");
        return NULL;
    }

    for (size_t i = 0; i < USER_MAX; i++) {
        if (users[i].used)
            continue;

        struct user *u = &users[i];

        memset(u, 0, sizeof(*u));
        u->used  = true;
        u->uid   = next_uid();
        u->gid   = GID_USERS;
        u->caps  = admin ? CAP_ALL : 0;
        strlcpy(u->role, admin ? "verwalter" : "benutzer", sizeof(u->role));
        strlcpy(u->name, name, sizeof(u->name));
        strlcpy(u->full, full && full[0] ? full : name, sizeof(u->full));
        default_home(name, u->home, sizeof(u->home));
        user_set_password(u, password);

        if (admin)
            group_add_member(group_by_gid(GID_ROOT), u->uid);
        group_add_member(group_by_gid(GID_USERS), u->uid);

        log_info("benutzer", "%s angelegt, Nummer %u%s", u->name,
                 (unsigned)u->uid, admin ? ", Verwalter" : "");
        audit(AUDIT_ACCOUNT, true, "%s angelegt%s", u->name,
              admin ? " (Verwalter)" : "");
        return u;
    }

    fail(error, error_size, "Es ist kein Platz fuer weitere Benutzer.");
    return NULL;
}

/* Wie viele Verwalter gibt es noch, wenn man einen wegdenkt? */
static size_t admins_besides(const struct user *skip)
{
    size_t n = 0;

    for (size_t i = 0; i < USER_MAX; i++) {
        if (!users[i].used || &users[i] == skip)
            continue;
        if (user_is_admin(&users[i]) && !users[i].locked)
            n++;
    }
    return n;
}

bool user_delete(struct user *u, char *error, size_t error_size)
{
    if (!u || !u->used) {
        fail(error, error_size, "Diesen Benutzer gibt es nicht.");
        return false;
    }
    if (u->uid == UID_ROOT) {
        fail(error, error_size, "root laesst sich nicht entfernen.");
        return false;
    }
    if (u == logged_in) {
        fail(error, error_size, "Wer angemeldet ist, kann sich nicht selbst "
                                "entfernen.");
        return false;
    }
    if (user_is_admin(u) && admins_besides(u) == 0) {
        fail(error, error_size, "Es muss ein Verwalter uebrig bleiben.");
        return false;
    }

    for (size_t i = 0; i < GROUP_MAX; i++)
        if (groups[i].used)
            group_remove_member(&groups[i], u->uid);

    log_warn("benutzer", "%s entfernt", u->name);
    audit(AUDIT_ACCOUNT, true, "%s entfernt", u->name);
    memset(u, 0, sizeof(*u));
    return true;
}

bool user_ensure_home(struct user *u)
{
    if (!u || !u->home[0])
        return false;

    perm_system_begin();

    /* Erst der Ordner, in dem die Heimatverzeichnisse liegen. Er gehoert
     * root: Wer sich zuerst anmeldet, soll ihn nicht mitbekommen, bloss
     * weil er ihn ausgeloest hat. */
    char parent[FS_PATH_MAX];

    strlcpy(parent, u->home, sizeof(parent));

    char *cut = strrchr(parent, '/');

    if (cut && cut != parent) {
        *cut = '\0';

        struct fs_node *dir = fs_lookup(NULL, parent);

        if (!dir)
            dir = fs_create_path(NULL, parent, FS_DIR);
        if (dir && dir->type == FS_DIR && dir->uid != UID_ROOT) {
            dir->uid  = UID_ROOT;
            dir->gid  = GID_ROOT;
            dir->mode = MODE_DIR_DEFAULT;
            perm_store_record(dir);
        }
    }

    struct fs_node *home = fs_lookup(NULL, u->home);

    if (!home)
        home = fs_create_path(NULL, u->home, FS_DIR);

    if (home && home->type == FS_DIR) {
        home->uid  = u->uid;
        home->gid  = u->gid;
        /* 0700 und nicht 0750: Alle Benutzer teilen sich die Gruppe
         * "benutzer", ein Gruppenrecht auf dem Heim waere darum ein
         * Recht fuer jeden. Wer sein Heim oeffnen will, setzt die Bits
         * selbst - dann ist es eine Entscheidung und kein Versehen. */
        home->mode = 0700;
        perm_store_record(home);
    }

    perm_system_end();

    if (perm_store_dirty())
        perm_store_save();
    return home != NULL;
}

void user_home_file(const char *name, const char *ersatz, char *out,
                    size_t size)
{
    struct user *u = logged_in;

    if (!u || !u->home[0] || !fs_lookup(NULL, u->home)) {
        strlcpy(out, ersatz, size);
        return;
    }

    /* Das Heim von root ist die Wurzel - dort stuenden sonst zwei
     * Schraegstriche hintereinander. */
    size_t len = strlen(u->home);

    ksnprintf(out, size, "%s%s%s", u->home,
              len && u->home[len - 1] == '/' ? "" : "/", name);
}

/* ------------------------------------------------------------------ */
/* Werkseinstellung, Lesen und Schreiben                               */
/* ------------------------------------------------------------------ */

void user_init(void)
{
    memset(users, 0, sizeof(users));
    memset(groups, 0, sizeof(groups));
    logged_in = NULL;
    store_found = false;

    group_create("verwalter", GID_ROOT);
    group_create("benutzer", GID_USERS);

    struct user *root = &users[0];

    memset(root, 0, sizeof(*root));
    root->used  = true;
    root->uid   = UID_ROOT;
    root->gid   = GID_ROOT;
    root->caps = CAP_ALL;
    strlcpy(root->role, "verwalter", sizeof(root->role));
    strlcpy(root->name, "root", sizeof(root->name));
    strlcpy(root->full, "Verwalter", sizeof(root->full));
    strlcpy(root->home, "/", sizeof(root->home));
    user_set_password(root, NULL);
}

bool user_store_exists(void) { return store_found; }

static void trim(char *text)
{
    char *start = text;

    while (*start == ' ' || *start == '\t')
        start++;
    if (start != text)
        memmove(text, start, strlen(start) + 1);

    size_t len = strlen(text);

    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                       text[len - 1] == '\r'))
        text[--len] = '\0';
}

/* Zerlegt "a:b:c" an den Doppelpunkten. Fehlende Felder werden zu
 * Zeigern auf ein leeres Wort, damit der Aufrufer nicht pruefen muss. */
static size_t split(char *text, char *out[], size_t max)
{
    static char empty[] = "";
    size_t n = 0;

    out[n++] = text;
    for (char *p = text; *p && n < max; p++) {
        if (*p != ':')
            continue;
        *p = '\0';
        out[n++] = p + 1;
    }
    for (size_t i = n; i < max; i++)
        out[i] = empty;
    return n;
}

static void read_group(char *value)
{
    char *f[3];

    split(value, f, 3);
    if (!f[0][0])
        return;

    uint32_t gid = to_number(f[1]);
    struct group *g = group_by_gid(gid);

    if (!g)
        g = group_create(f[0], gid);
    if (!g)
        return;
    strlcpy(g->name, f[0], sizeof(g->name));

    g->members = 0;
    for (char *p = f[2]; *p; ) {
        char *comma = strchr(p, ',');

        if (comma)
            *comma = '\0';
        if (*p)
            group_add_member(g, to_number(p));
        if (!comma)
            break;
        p = comma + 1;
    }
}

static void read_user(char *value)
{
    char *f[10];

    split(value, f, 10);
    if (!name_ok(f[0]))
        return;

    struct user *u = user_by_name(f[0]);

    if (!u) {
        for (size_t i = 0; i < USER_MAX && !u; i++)
            if (!users[i].used)
                u = &users[i];
    }
    if (!u)
        return;

    memset(u, 0, sizeof(*u));
    u->used = true;
    strlcpy(u->name, f[0], sizeof(u->name));
    u->uid = to_number(f[1]);
    u->gid = to_number(f[2]);
    strlcpy(u->full, f[3][0] ? f[3] : f[0], sizeof(u->full));
    if (f[4][0])
        strlcpy(u->home, f[4], sizeof(u->home));
    else
        default_home(u->name, u->home, sizeof(u->home));

    for (const char *p = f[5]; *p; p++) {
        if (*p == 'v') u->caps = CAP_ALL;
        if (*p == 'g') u->locked = true;
        if (*p == 'o') u->nopass = true;
    }

    u->rounds = to_number(f[6]);
    if (!u->rounds)
        u->rounds = USER_ROUNDS;

    /* Steht dort Unsinn, bleibt der Pruefwert auf null - dann passt kein
     * Passwort, und der Eintrag ist wirkungslos statt weit offen. */
    if (strlen(f[7]) != USER_SALT_SIZE * 2 ||
        !from_hex(f[7], u->salt, USER_SALT_SIZE))
        memset(u->salt, 0, sizeof(u->salt));
    if (strlen(f[8]) != USER_HASH_SIZE * 2 ||
        !from_hex(f[8], u->hash, USER_HASH_SIZE)) {
        memset(u->hash, 0, sizeof(u->hash));
        u->nopass = false;
    }

    /* Das zehnte Feld gibt es erst seit den Rollen. Aeltere Dateien
     * haben es nicht - dann entscheidet weiter das "v" aus den
     * Merkmalen, und niemand muss seine Datei anfassen. */
    if (f[9][0] && user_set_role(u, f[9]))
        return;
    strlcpy(u->role, caps_role(u->caps), sizeof(u->role));
}

bool user_load(void)
{
    perm_system_begin();

    struct fs_node *file = fs_lookup(NULL, USER_PATH);
    bool ok = false;

    if (file && file->type == FS_FILE && fs_load(file) && file->data &&
        file->size > 0 && file->size <= 16384) {
        size_t size = file->size;
        char *text = kmalloc(size + 1);

        if (text) {
            memcpy(text, file->data, size);
            text[size] = '\0';

            /* Bevor die Datei gelesen wird, alles leeren - sonst bliebe
             * die Werkseinstellung neben ihr stehen. */
            memset(users, 0, sizeof(users));
            memset(groups, 0, sizeof(groups));
            group_create("verwalter", GID_ROOT);
            group_create("benutzer", GID_USERS);

            char *line = text;

            while (line && *line) {
                char *next = strchr(line, '\n');

                if (next)
                    *next++ = '\0';

                char *hash = strchr(line, '#');

                if (hash)
                    *hash = '\0';

                char *equals = strchr(line, '=');

                if (equals) {
                    *equals = '\0';
                    trim(line);
                    trim(equals + 1);
                    if (strcasecmp(line, "gruppe") == 0)
                        read_group(equals + 1);
                    else if (strcasecmp(line, "benutzer") == 0)
                        read_user(equals + 1);
                }
                line = next;
            }
            kfree(text);

            /* Ohne root waere niemand mehr Verwalter. Lieber die
             * Werkseinstellung als ein System, an das keiner kommt. */
            if (!user_by_uid(UID_ROOT) || user_count() == 0)
                user_init();
            else
                ok = true;

            /* Bei jedem Start neu durchgesetzt: Die Pruefwerte gehen
             * niemanden ausser dem Verwalter etwas an, und FAT32 selbst
             * merkt sich das nicht. Geht die Liste daneben verloren,
             * sitzt die Sperre trotzdem wieder. */
            file->uid  = UID_ROOT;
            file->gid  = GID_ROOT;
            file->mode = 0600;
            perm_store_record(file);
        }
    }

    store_found = ok;
    perm_system_end();

    if (ok && perm_store_dirty())
        perm_store_save();
    return ok;
}

bool user_save(void)
{
    if (!fs_disk_mounted())
        return false;

    size_t cap = 8192;
    char  *text = kmalloc(cap);

    if (!text)
        return false;

    size_t used = 0;

    /* ksnprintf meldet, wie lang es geworden waere - dazuzaehlen wuerde
     * ueber das Ende hinauslaufen. Darum jedes Mal begrenzen. */
    #define ADD(...) do {                                            \
        if (used < cap - 1) {                                        \
            ksnprintf(text + used, cap - used, __VA_ARGS__);         \
            used += strlen(text + used);                             \
        }                                                            \
    } while (0)

    ADD("# Benutzer und Gruppen von RetroOS\n"
        "#\n"
        "# gruppe   = <name>:<gid>:<mitglied>,<mitglied>\n"
        "# benutzer = <name>:<uid>:<gid>:<voller Name>:<heim>:<merkmale>:"
        "<runden>:<salz>:<pruefwert>:<rolle>\n"
        "#\n"
        "# Merkmale: v = Verwalter, g = Anmeldung gesperrt, o = ohne Passwort.\n"
        "# Rollen  : verwalter, netzwerk, wartung, benutzer.\n"
        "# Das Passwort selbst steht hier nicht - nur ein Wert, aus dem es\n"
        "# sich nicht zurueckrechnen laesst.\n\n");

    for (size_t i = 0; i < GROUP_MAX; i++) {
        if (!groups[i].used)
            continue;
        ADD("gruppe   = %s:%u:", groups[i].name, (unsigned)groups[i].gid);
        for (size_t m = 0; m < groups[i].members; m++)
            ADD("%s%u", m ? "," : "", (unsigned)groups[i].member[m]);
        ADD("\n");
    }
    ADD("\n");

    char salt[USER_SALT_SIZE * 2 + 1];
    char hash[USER_HASH_SIZE * 2 + 1];

    for (size_t i = 0; i < USER_MAX; i++) {
        if (!users[i].used)
            continue;

        to_hex(users[i].salt, USER_SALT_SIZE, salt);
        to_hex(users[i].hash, USER_HASH_SIZE, hash);
        ADD("benutzer = %s:%u:%u:%s:%s:%s%s%s:%u:%s:%s:%s\n",
            users[i].name, (unsigned)users[i].uid, (unsigned)users[i].gid,
            users[i].full, users[i].home,
            user_is_admin(&users[i]) ? "v" : "", users[i].locked ? "g" : "",
            users[i].nopass ? "o" : "",
            (unsigned)users[i].rounds, salt, hash,
            users[i].role[0] ? users[i].role : caps_role(users[i].caps));
    }
    #undef ADD

    perm_system_begin();

    struct fs_node *file = fs_lookup(NULL, USER_PATH);

    if (!file)
        file = fs_create_path(NULL, USER_PATH, FS_FILE);

    bool ok = file && file->type == FS_FILE && fs_write(file, text, used);

    if (ok) {
        /* Der Pruefwert geht niemanden ausser dem Verwalter etwas an. */
        file->uid  = UID_ROOT;
        file->gid  = GID_ROOT;
        file->mode = 0600;
        perm_store_record(file);
        store_found = true;
    }

    perm_system_end();

    /* Ohne die Liste daneben waeren die 0600 nach dem Neustart wieder
     * weg - FAT32 merkt sie sich nicht. Beides gehoert zusammen. */
    if (ok && perm_store_dirty())
        perm_store_save();
    kfree(text);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Sitzung                                                             */
/* ------------------------------------------------------------------ */

void session_login(struct user *u)
{
    logged_in = u;
    if (u) {
        user_ensure_home(u);
        log_info("sitzung", "%s angemeldet (Nummer %u%s)", u->name,
                 (unsigned)u->uid, user_is_admin(u) ? ", Verwalter" : "");
        audit(AUDIT_LOGIN, true, "%s (Rolle %s)", u->name,
              u->role[0] ? u->role : caps_role(u->caps));
    }
}

void session_logout(void)
{
    if (logged_in) {
        log_info("sitzung", "%s abgemeldet", logged_in->name);
        audit(AUDIT_LOGOUT, true, "%s", logged_in->name);
    }
    logged_in = NULL;
}

struct user *session_user(void) { return logged_in; }

uint32_t session_uid(void)
{
    /* Ein Ring-3-Programm handelt unter seiner eigenen Nummer, auch wenn
     * inzwischen jemand anders vor dem Bildschirm sitzt. */
    struct thread *t = thread_current();

    if (t && t->process)
        return process_uid(t->process);
    return logged_in ? logged_in->uid : UID_ROOT;
}

uint32_t session_gid(void)
{
    struct thread *t = thread_current();

    if (t && t->process)
        return process_gid(t->process);
    return logged_in ? logged_in->gid : GID_ROOT;
}

bool session_is_admin(void)
{
    uint32_t uid = session_uid();

    if (uid == UID_ROOT)
        return true;

    struct user *u = user_by_uid(uid);

    return user_is_admin(u);
}

uint32_t session_caps(void)
{
    uint32_t uid = session_uid();

    if (uid == UID_ROOT)
        return CAP_ALL;

    struct user *u = user_by_uid(uid);

    return u ? u->caps : 0;
}

bool session_can(uint32_t cap)
{
    bool ok = (session_caps() & cap) == cap;

    /* Gebrauchte und verweigerte Rechte stehen beide in der Spur - die
     * verweigerten, weil sie auffallen sollen, die gebrauchten, weil
     * man hinterher wissen will, wer wann Verwalter war. */
    if (session_uid() != UID_ROOT) {
        char text[64];

        caps_text(cap, text, sizeof(text));
        audit(AUDIT_PRIVILEGE, ok, "%s", text);
    }
    return ok;
}
