/* rechte_test.c - prueft die Rechteverwaltung und die Benutzerdatenbank.
 *
 * Beides laesst sich ohne Kernel pruefen: Die Rechtepruefung kennt nur
 * Zahlen und Bits, die Datenbank nur Text. Der Dateibaum darunter ist
 * hier ein winziger Ersatz aus wenigen Knoten - genug, damit perm.c und
 * user.c dasselbe tun wie im laufenden System.
 *
 * Geprueft werden die drei Bloecke der Rechtebits samt Reihenfolge, das
 * Klebebit, die Textform in beide Richtungen, der Pruefwert des
 * Passworts und der Weg durch benutzer.conf und zurueck.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "perm.h"
#include "thread.h"
#include "user.h"

static int fehler;
static int geprueft;

static void pruefe(const char *was, bool bedingung)
{
    geprueft++;
    if (!bedingung) {
        printf("  FEHLER: %s\n", was);
        fehler++;
    }
}

static void pruefe_text(const char *was, const char *soll, const char *ist)
{
    geprueft++;
    if (strcmp(soll, ist) != 0) {
        printf("  FEHLER: %s - erwartet \"%s\", bekommen \"%s\"\n",
               was, soll, ist);
        fehler++;
    }
}

/* ------------------------------------------------------------------ */
/* Ersatz fuer den Kernel                                              */
/* ------------------------------------------------------------------ */

/* Der Dateibaum des Tests: ein Wurzelknoten und ein paar Kinder, alle
 * im Arbeitsspeicher. Mehr braucht keine der geprueften Funktionen. */
static struct fs_node knoten[16];
static size_t         knoten_anzahl;
static char           geschrieben[8192];
static size_t         geschrieben_laenge;
static bool           platte_da = true;

static struct fs_node *neu(const char *name, enum fs_type type,
                           uint32_t uid, uint32_t gid, uint16_t mode)
{
    struct fs_node *n = &knoten[knoten_anzahl++];

    memset(n, 0, sizeof(*n));
    strlcpy(n->name, name, sizeof(n->name));
    n->type = (uint8_t)type;
    n->uid  = uid;
    n->gid  = gid;
    n->mode = mode;
    return n;
}

/* Sitzung und Systemzaehler kommen aus user.c und perm.c selbst - der
 * Test setzt nur den Thread darunter. Damit wird gleich mitgeprueft,
 * was session_uid() aus einer Anmeldung macht. */
static struct thread der_thread;

struct thread *thread_current(void) { return &der_thread; }

/* Ring-3-Prozesse gibt es hier keine. */
uint32_t process_uid(struct process *proc) { UNUSED(proc); return UID_ROOT; }
uint32_t process_gid(struct process *proc) { UNUSED(proc); return GID_ROOT; }

/* Meldet einen Benutzer an, ohne sein Heim anzulegen - dafuer gibt es
 * hier keinen Dateibaum. */
static void als(const char *name)
{
    struct user *u = name ? user_by_name(name) : NULL;

    session_logout();
    if (u)
        session_login(u);
}

/* Die Dateisystemfunktionen, die user.c und perm.c anfassen. Sie tun
 * gerade so viel, dass der Weg durch benutzer.conf pruefbar wird. */
static struct fs_node *datei;

bool fs_disk_mounted(void) { return platte_da; }

struct fs_node *fs_lookup(struct fs_node *base, const char *path)
{
    (void)base;
    if (datei && strcmp(path, USER_PATH) == 0)
        return datei;
    return NULL;
}

struct fs_node *fs_create_path(struct fs_node *base, const char *path,
                               enum fs_type type)
{
    (void)base;
    if (strcmp(path, USER_PATH) != 0)
        return NULL;
    datei = neu("benutzer.conf", type, UID_ROOT, GID_ROOT, 0600);
    return datei;
}

bool fs_load(struct fs_node *file)
{
    if (!file)
        return false;
    file->data = (uint8_t *)geschrieben;
    file->size = geschrieben_laenge;
    return true;
}

bool fs_write(struct fs_node *file, const void *data, size_t size)
{
    if (!file || size >= sizeof(geschrieben))
        return false;
    memcpy(geschrieben, data, size);
    geschrieben[size] = '\0';
    geschrieben_laenge = size;
    file->data = (uint8_t *)geschrieben;
    file->size = size;
    return true;
}

void fs_path(struct fs_node *node, char *buf, size_t size)
{
    ksnprintf(buf, size, "/%s", node ? node->name : "");
}

/* Zufall: fuer den Test genuegt ein Zaehler - er muss nur bei jedem
 * Aufruf etwas anderes liefern, damit zwei Benutzer verschiedene Salze
 * bekommen. */
void crypto_random(void *out, size_t length)
{
    static uint8_t zaehler;
    uint8_t *p = out;

    for (size_t i = 0; i < length; i++)
        p[i] = (uint8_t)(++zaehler * 31u + i);
}

/* ------------------------------------------------------------------ */

static void test_bits(void)
{
    printf("Rechtebits\n");

    struct fs_node *f = neu("bericht.txt", FS_FILE, 1000, 100, 0640);

    /* Der Eigentuemer wird nach den Eigentuemerbits beurteilt. */
    pruefe("Eigentuemer darf lesen",  perm_check(f, 1000, 100, P_R));
    pruefe("Eigentuemer darf schreiben", perm_check(f, 1000, 100, P_W));
    pruefe("Eigentuemer darf nicht ausfuehren",
           !perm_check(f, 1000, 100, P_X));

    /* Die Gruppe darf lesen, aber nicht schreiben. */
    pruefe("Gruppe darf lesen", perm_check(f, 1001, 100, P_R));
    pruefe("Gruppe darf nicht schreiben", !perm_check(f, 1001, 100, P_W));

    /* Alle uebrigen duerfen gar nichts. */
    pruefe("Fremder darf nicht lesen", !perm_check(f, 1001, 200, P_R));

    /* Und root immer alles. */
    pruefe("root darf lesen",     perm_check(f, UID_ROOT, GID_ROOT, P_R));
    pruefe("root darf schreiben", perm_check(f, UID_ROOT, GID_ROOT, P_W));

    /* Der Eigentuemer wird auch dann nach seinen Bits beurteilt, wenn
     * die Gruppe mehr erlaubt - sonst kaeme man ueber die eigene Gruppe
     * an dem vorbei, was man sich selbst verboten hat. */
    struct fs_node *g = neu("gesperrt.txt", FS_FILE, 1000, 100, 0060);

    pruefe("Eigentuemer bleibt bei seinen Bits",
           !perm_check(g, 1000, 100, P_R));
    pruefe("Die Gruppe darf trotzdem", perm_check(g, 1001, 100, P_R));

    /* Mehrere Rechte auf einmal muessen alle da sein. */
    struct fs_node *d = neu("ordner", FS_DIR, 1000, 100, 0755);

    pruefe("Eigentuemer darf hinein und schreiben",
           perm_check(d, 1000, 100, P_W | P_X));
    pruefe("Fremder darf hinein, aber nicht schreiben",
           perm_check(d, 1002, 200, P_X) &&
           !perm_check(d, 1002, 200, P_W | P_X));

    pruefe("Ohne Knoten kein Recht", !perm_check(NULL, 1000, 100, P_R));
}

/* jesse bekommt 1000, anna 1001 - die beiden Nummern, mit denen die
 * Knoten in diesen Tests angelegt sind. */
static void zwei_benutzer(void)
{
    char text[96];

    user_init();
    user_create("jesse", "Jesse", "geheim", false, text, sizeof(text));
    user_create("anna", "Anna", "apfel", false, text, sizeof(text));
}

static void test_klebebit(void)
{
    printf("Klebebit\n");

    zwei_benutzer();

    struct fs_node *korb = neu("Papierkorb", FS_DIR, UID_ROOT, GID_ROOT,
                               0777 | MODE_STICKY);
    struct fs_node *meins = neu("meins.txt", FS_FILE, 1000, 100, 0644);
    struct fs_node *deins = neu("deins.txt", FS_FILE, 1001, 100, 0644);

    als("jesse");
    pruefe("Eigenes darf weg", perm_may_unlink(korb, meins));
    pruefe("Fremdes bleibt liegen", !perm_may_unlink(korb, deins));

    als("root");
    pruefe("Der Verwalter darf beides",
           perm_may_unlink(korb, meins) && perm_may_unlink(korb, deins));

    /* Ohne Klebebit entscheidet allein der Ordner. */
    struct fs_node *offen = neu("offen", FS_DIR, UID_ROOT, GID_ROOT, 0777);

    als("jesse");
    pruefe("Ohne Klebebit darf jeder wegnehmen",
           perm_may_unlink(offen, deins));

    /* Ein Ordner, in den man nicht schreiben darf, gibt nichts her. */
    struct fs_node *zu = neu("zu", FS_DIR, UID_ROOT, GID_ROOT, 0755);

    pruefe("Ohne Schreibrecht am Ordner geht nichts",
           !perm_may_unlink(zu, meins));
}

static void test_text(void)
{
    printf("Rechte als Text\n");

    char text[11];

    perm_mode_text(0755, FS_DIR, text);
    pruefe_text("0755 als Ordner", "drwxr-xr-x", text);

    perm_mode_text(0644, FS_FILE, text);
    pruefe_text("0644 als Datei", "-rw-r--r--", text);

    perm_mode_text(0600, FS_FILE, text);
    pruefe_text("0600", "-rw-------", text);

    perm_mode_text(0777 | MODE_STICKY, FS_DIR, text);
    pruefe_text("Klebebit mit x", "drwxrwxrwt", text);

    perm_mode_text(0776 | MODE_STICKY, FS_DIR, text);
    pruefe_text("Klebebit ohne x", "drwxrwxrwT", text);

    uint16_t mode = 0;

    pruefe("750 wird gelesen", perm_parse_mode("750", &mode) && mode == 0750);
    pruefe("0750 wird gelesen", perm_parse_mode("0750", &mode) && mode == 0750);
    pruefe("1777 wird gelesen",
           perm_parse_mode("1777", &mode) && mode == (0777 | MODE_STICKY));
    pruefe("rwxr-x--- wird gelesen",
           perm_parse_mode("rwxr-x---", &mode) && mode == 0750);
    pruefe("Mit Art davor geht es auch",
           perm_parse_mode("drwxr-x---", &mode) && mode == 0750);
    pruefe("rwxrwxrwt wird gelesen",
           perm_parse_mode("rwxrwxrwt", &mode) && mode == (0777 | MODE_STICKY));

    pruefe("Ziffer 8 ist keine",       !perm_parse_mode("758", &mode));
    pruefe("Zu kurz faellt durch",     !perm_parse_mode("rwx", &mode));
    pruefe("Falscher Buchstabe",       !perm_parse_mode("rwxr-w---", &mode));
    pruefe("Leer faellt durch",        !perm_parse_mode("", &mode));

    /* Hin und zurueck muss dasselbe ergeben. */
    for (uint16_t m = 0; m <= 0777; m++) {
        char  form[11];
        uint16_t zurueck = 0;

        perm_mode_text(m, FS_FILE, form);
        if (!perm_parse_mode(form + 1, &zurueck) || zurueck != m) {
            printf("  FEHLER: %04o ergibt \"%s\" und daraus %04o\n",
                   m, form, zurueck);
            fehler++;
            break;
        }
    }
    geprueft++;
}

static void test_setzen(void)
{
    printf("Rechte aendern\n");

    zwei_benutzer();

    struct fs_node *f = neu("meins.txt", FS_FILE, 1000, 100, 0644);

    als("jesse");
    pruefe("Der Eigentuemer darf",  perm_set_mode(f, 0600) && f->mode == 0600);

    als("anna");
    pruefe("Ein anderer darf nicht", !perm_set_mode(f, 0666));
    pruefe("Und die Bits bleiben",   f->mode == 0600);
    pruefe("Verschenken darf er auch nicht",
           !perm_set_owner(f, 1001, 100));

    als("root");
    pruefe("Der Verwalter darf verschenken",
           perm_set_owner(f, 1001, 200) && f->uid == 1001 && f->gid == 200);

    /* Ueber die neun Bits und das Klebebit hinaus wird nichts gemerkt. */
    pruefe("Fremde Bits fallen weg",
           perm_set_mode(f, 07777) && f->mode == (0777 | MODE_STICKY));
}

static void test_system(void)
{
    printf("Arbeiten des Systems\n");

    zwei_benutzer();

    struct fs_node *f = neu("fremd.txt", FS_FILE, UID_ROOT, GID_ROOT, 0600);

    als("jesse");
    pruefe("Ohne Recht geht nichts", !perm_may(f, P_R));

    perm_system_begin();
    pruefe("Das System darf",          perm_may(f, P_W));
    pruefe("Und gilt als Eigentuemer", perm_owns(f));

    /* Verschachtelt: erst das aeussere Ende gibt die Rechte zurueck. */
    perm_system_begin();
    perm_system_end();
    pruefe("Ein Ende reicht nicht", perm_may(f, P_R));

    perm_system_end();
    pruefe("Danach wieder nicht", !perm_may(f, P_R));

    /* Ein Verwalter kommt auch ohne Systemrecht ueberall hin. */
    als("root");
    pruefe("Der Verwalter darf", perm_may(f, P_W));
}

static void test_passwort(void)
{
    printf("Passwoerter\n");

    user_init();

    struct user *root = user_by_uid(UID_ROOT);

    pruefe("root ist da",             root != NULL);
    pruefe("root ist Verwalter",      root && root->admin);
    pruefe("root hat kein Passwort",  root && root->nopass);
    pruefe("Leer kommt herein",       user_check_password(root, ""));
    pruefe("Alles andere nicht",      !user_check_password(root, "x"));

    char fehlertext[96] = "";
    struct user *u = user_create("jesse", "Jesse", "geheim", false,
                                 fehlertext, sizeof(fehlertext));

    pruefe("Anlegen geht",            u != NULL);
    pruefe("Nummer ab 1000",          u && u->uid >= 1000);
    pruefe("Gruppe benutzer",         u && u->gid == GID_USERS);
    pruefe("Kein Verwalter",          u && !u->admin);
    pruefe("Passwort stimmt",         user_check_password(u, "geheim"));
    pruefe("Falsches nicht",          !user_check_password(u, "Geheim"));
    pruefe("Leeres auch nicht",       !user_check_password(u, ""));

    /* Zwei Benutzer mit demselben Passwort bekommen verschiedene
     * Pruefwerte - dafuer ist das Salz da. */
    struct user *v = user_create("anna", "Anna", "geheim", false,
                                 fehlertext, sizeof(fehlertext));

    pruefe("Zweiter angelegt",  v != NULL);
    pruefe("Andere Nummer",     u && v && u->uid != v->uid);
    pruefe("Anderes Salz",      u && v &&
           memcmp(u->salt, v->salt, USER_SALT_SIZE) != 0);
    pruefe("Anderer Pruefwert", u && v &&
           memcmp(u->hash, v->hash, USER_HASH_SIZE) != 0);
    pruefe("Beide kommen herein",
           user_check_password(u, "geheim") && user_check_password(v, "geheim"));

    /* Ein neues Passwort ersetzt das alte. */
    user_set_password(u, "anders");
    pruefe("Neues gilt",  user_check_password(u, "anders"));
    pruefe("Altes nicht", !user_check_password(u, "geheim"));

    /* Gesperrt kommt niemand herein, auch mit richtigem Passwort nicht. */
    u->locked = true;
    pruefe("Gesperrt bleibt draussen", !user_check_password(u, "anders"));
    u->locked = false;

    /* Namen, die im Dateipfad oder in der Datei Aerger machen wuerden. */
    pruefe("Kein Doppelpunkt im Namen",
           !user_create("a:b", "", "x", false, fehlertext, sizeof(fehlertext)));
    pruefe("Kein Schraegstrich",
           !user_create("a/b", "", "x", false, fehlertext, sizeof(fehlertext)));
    pruefe("Kein Leerzeichen",
           !user_create("a b", "", "x", false, fehlertext, sizeof(fehlertext)));
    pruefe("Nicht zweimal derselbe",
           !user_create("jesse", "", "x", false, fehlertext, sizeof(fehlertext)));
    pruefe("Und der Grund steht da", fehlertext[0] != '\0');
}

static void test_gruppen(void)
{
    printf("Gruppen\n");

    user_init();

    char fehlertext[96];
    struct user *u = user_create("jesse", "Jesse", "geheim", false,
                                 fehlertext, sizeof(fehlertext));
    struct user *chef = user_create("chef", "Chefin", "x", true,
                                    fehlertext, sizeof(fehlertext));

    pruefe("Zwei Gruppen von Anfang an", group_count() == 2);
    pruefe("Verwalter hat gid 0",  group_by_name("verwalter") &&
           group_by_name("verwalter")->gid == GID_ROOT);

    pruefe("jesse ist in benutzer", u && user_in_group(u->uid, GID_USERS));
    pruefe("jesse ist nicht Verwalter",
           u && !user_in_group(u->uid, GID_ROOT));
    pruefe("chef schon", chef && user_in_group(chef->uid, GID_ROOT));

    pruefe("Name zur Nummer",
           strcmp(group_name_of(GID_USERS), "benutzer") == 0);
    pruefe("Unbekannte Nummer",  strcmp(group_name_of(4711), "?") == 0);
    pruefe("Unbekannter Benutzer", strcmp(user_name_of(4711), "?") == 0);

    struct group *g = group_by_gid(GID_USERS);

    pruefe("Austragen geht",  u && group_remove_member(g, u->uid));
    pruefe("Aber die Hauptgruppe bleibt",
           u && user_in_group(u->uid, GID_USERS));
    pruefe("Zweimal austragen geht nicht",
           u && !group_remove_member(g, u->uid));
}

static void test_loeschen(void)
{
    printf("Benutzer entfernen\n");

    user_init();

    char fehlertext[96] = "";
    struct user *u = user_create("jesse", "Jesse", "geheim", false,
                                 fehlertext, sizeof(fehlertext));

    pruefe("root bleibt",
           !user_delete(user_by_uid(UID_ROOT), fehlertext, sizeof(fehlertext)));
    pruefe("Und sagt warum", fehlertext[0] != '\0');

    pruefe("Ein gewoehnlicher geht",
           user_delete(u, fehlertext, sizeof(fehlertext)));
    pruefe("Danach ist er weg", user_by_name("jesse") == NULL);
    pruefe("Nur noch einer",    user_count() == 1);

    /* Der letzte Verwalter bleibt stehen, sonst kaeme niemand mehr an
     * die Verwaltung heran. */
    struct user *a = user_create("a", "A", "x", true,
                                 fehlertext, sizeof(fehlertext));
    struct user *b = user_create("b", "B", "x", true,
                                 fehlertext, sizeof(fehlertext));

    pruefe("Einer von zweien darf gehen",
           user_delete(a, fehlertext, sizeof(fehlertext)));
    pruefe("Solange root noch da ist, geht auch der zweite",
           user_delete(b, fehlertext, sizeof(fehlertext)));
}

static void test_datei(void)
{
    printf("benutzer.conf schreiben und lesen\n");

    knoten_anzahl = 0;
    datei = NULL;
    geschrieben_laenge = 0;
    platte_da = true;

    user_init();
    als("root");

    char fehlertext[96];
    struct user *u = user_create("jesse", "Jesse Beispiel", "geheim", false,
                                 fehlertext, sizeof(fehlertext));
    struct user *c = user_create("chef", "Chefin", "leitung", true,
                                 fehlertext, sizeof(fehlertext));

    pruefe("Es gibt noch keine Datei", !user_store_exists());
    pruefe("Schreiben geht", user_save());
    pruefe("Jetzt gibt es sie", user_store_exists());
    pruefe("Und sie enthaelt beide Zeilen",
           strstr(geschrieben, "benutzer = jesse:") != NULL &&
           strstr(geschrieben, "benutzer = chef:") != NULL);
    pruefe("Das Passwort steht nicht drin",
           strstr(geschrieben, "geheim") == NULL &&
           strstr(geschrieben, "leitung") == NULL);

    uint32_t uid_vorher = u->uid;
    uint32_t cid_vorher = c->uid;

    /* Alles vergessen und wieder einlesen. */
    user_init();
    pruefe("Nach dem Zuruecksetzen nur root", user_count() == 1);
    pruefe("Lesen geht", user_load());

    struct user *w = user_by_name("jesse");
    struct user *d = user_by_name("chef");

    pruefe("jesse ist wieder da",  w != NULL);
    pruefe("Mit derselben Nummer", w && w->uid == uid_vorher);
    pruefe("Und dem vollen Namen",
           w && strcmp(w->full, "Jesse Beispiel") == 0);
    pruefe("Und dem Heim",         w && w->home[0] == '/');
    pruefe("Das Passwort passt noch", user_check_password(w, "geheim"));
    pruefe("Ein falsches nicht",      !user_check_password(w, "Geheim"));

    pruefe("chef ist Verwalter",   d && d->admin);
    pruefe("Mit derselben Nummer", d && d->uid == cid_vorher);
    pruefe("Und in der Gruppe",    d && user_in_group(d->uid, GID_ROOT));

    pruefe("root ist auch wieder da", user_by_uid(UID_ROOT) != NULL);
    pruefe("Drei insgesamt",          user_count() == 3);

    /* Merkmale ueberstehen den Weg. */
    d->locked = true;
    pruefe("Nochmal schreiben", user_save());
    user_init();
    pruefe("Nochmal lesen", user_load());
    pruefe("Gesperrt bleibt gesperrt",
           user_by_name("chef") && user_by_name("chef")->locked);

    /* Ohne Platte laesst sich nichts sichern - und das sagt es auch. */
    platte_da = false;
    pruefe("Ohne Platte kein Speichern", !user_save());
    platte_da = true;
}

static void test_kaputte_datei(void)
{
    printf("Beschaedigte Datei\n");

    knoten_anzahl = 0;
    user_init();
    als("root");
    datei = neu("benutzer.conf", FS_FILE, UID_ROOT, GID_ROOT, 0600);

    /* Eine Datei ohne root darf nicht gelten - sonst haette das System
     * keinen Verwalter mehr. */
    const char *ohne_root = "benutzer = jesse:1000:100:Jesse:/:...:4096::\n";

    strcpy(geschrieben, ohne_root);
    geschrieben_laenge = strlen(ohne_root);
    pruefe("Ohne root faellt die Datei durch", !user_load());
    pruefe("Und es gilt die Werkseinstellung",
           user_by_uid(UID_ROOT) != NULL);

    /* Unsinn im Pruefwert darf nicht dazu fuehren, dass jedes Passwort
     * passt - dann waere ein kaputter Eintrag eine offene Tuer. */
    const char *kaputt =
        "benutzer = root:0:0:Verwalter:/:v:4096:zzz:zzz\n";

    strcpy(geschrieben, kaputt);
    geschrieben_laenge = strlen(kaputt);
    user_load();

    struct user *r = user_by_uid(UID_ROOT);

    pruefe("root ist da",            r != NULL);
    pruefe("Aber kein Passwort passt",
           r && !user_check_password(r, "") && !user_check_password(r, "x"));

    /* Leere und nur kommentierte Dateien aendern nichts. */
    const char *nur_kommentar = "# nichts weiter\n\n";

    strcpy(geschrieben, nur_kommentar);
    geschrieben_laenge = strlen(nur_kommentar);
    pruefe("Nur Kommentare faellt durch", !user_load());
    pruefe("Und root steht wieder da",    user_by_uid(UID_ROOT) != NULL);
}

int main(void)
{
    printf("=== Rechte und Benutzer ===\n");

    test_bits();
    test_klebebit();
    test_text();
    test_setzen();
    test_system();
    test_passwort();
    test_gruppen();
    test_loeschen();
    test_datei();
    test_kaputte_datei();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
