/* user.h - Benutzer, Gruppen und die angemeldete Sitzung.
 *
 * RetroOS war lange ein System fuer genau einen Menschen: Wer davor
 * sass, durfte alles. Das reicht, solange nur ein Stick im Rechner
 * steckt - sobald das System auf einer Festplatte liegt und mehrere
 * Leute daran arbeiten, nicht mehr.
 *
 * Ein Benutzer hat eine Nummer (uid), eine Hauptgruppe (gid), ein
 * Heimatverzeichnis und einen Pruefwert seines Passworts. Das Passwort
 * selbst wird nirgends abgelegt: Gespeichert ist ein Salz und der
 * vieltausendfach wiederholte HMAC darueber. Wer die Datei liest,
 * bekommt daraus das Passwort nicht zurueck, und zwei Leute mit
 * demselben Passwort haben trotzdem verschiedene Eintraege.
 *
 * Benutzer 0 ("root") ist der Verwalter. Er darf alles, auch dort, wo
 * die Rechtebits nein sagen - genau wie anderswo. Weitere Verwalter
 * gibt es ueber das Kennzeichen "admin".
 *
 * Abgelegt wird alles als Text unter /Festplatte/benutzer.conf. Ohne
 * Festplatte gibt es keine gespeicherten Benutzer; dann laeuft RetroOS
 * wie frueher als root weiter, und die Anmeldung entfaellt.
 */
#ifndef USER_H
#define USER_H

#include "retro.h"
#include "vfs.h"

#define USER_PATH        "/Festplatte/benutzer.conf"

#define USER_NAME_MAX    23
#define USER_FULL_MAX    31
#define USER_MAX         16
#define GROUP_MAX        8
#define GROUP_MEMBER_MAX 16

#define USER_SALT_SIZE   16
#define USER_HASH_SIZE   32
/* So oft wird der HMAC wiederholt. Auf dieser Maschine sind das ein
 * paar Millisekunden - beim Anmelden nicht zu spueren, beim Durchprobieren
 * von Passwortlisten sehr wohl. */
#define USER_ROUNDS      4096

/* --- Faehigkeiten und Rollen ----------------------------------------
 *
 * "Verwalter ja/nein" ist zu grob. Wer den Paketfilter pflegen soll,
 * braucht keinen Zugriff auf die Passwoerter, und wer die Platte
 * formatiert, muss nicht das Protokoll leeren duerfen. Darum haengt an
 * jedem Benutzer eine Menge von Faehigkeiten, und eine Rolle ist nichts
 * weiter als ein Name fuer eine solche Menge - das ist RBAC in seiner
 * schlichtesten brauchbaren Form.
 *
 * Wer alle Faehigkeiten hat, ist Verwalter; das alte Kennzeichen ist
 * damit nicht verschwunden, sondern nur zum Sonderfall geworden. */
#define CAP_USERS    (1u << 0)   /* Konten anlegen, sperren, Passwoerter */
#define CAP_NET      (1u << 1)   /* Netz und Paketfilter                 */
#define CAP_DISK     (1u << 2)   /* Formatieren, Einhaengen, Installieren */
#define CAP_LOG      (1u << 3)   /* Protokoll leeren, Pruefspur lesen     */
#define CAP_POWER    (1u << 4)   /* Neu starten und abschalten            */
#define CAP_CONFIG   (1u << 5)   /* Systemweite Einstellungen             */
#define CAP_ALL      0x3Fu

#define UID_ROOT         0u
#define GID_ROOT         0u
#define GID_USERS        100u
#define UID_NONE         0xFFFFFFFFu

struct user {
    bool     used;
    uint32_t uid;
    uint32_t gid;
    char     name[USER_NAME_MAX + 1];
    char     full[USER_FULL_MAX + 1];
    char     home[FS_PATH_MAX];
    uint32_t caps;               /* was er darf - siehe CAP_*    */
    char     role[USER_NAME_MAX + 1];
    bool     locked;             /* Anmeldung gesperrt           */
    bool     nopass;             /* kein Passwort gesetzt        */
    uint32_t rounds;
    uint8_t  salt[USER_SALT_SIZE];
    uint8_t  hash[USER_HASH_SIZE];
};

struct group {
    bool     used;
    uint32_t gid;
    char     name[USER_NAME_MAX + 1];
    uint32_t member[GROUP_MEMBER_MAX];
    size_t   members;
};

/* Legt die Werkseinstellung an: die Gruppen "verwalter" und "benutzer"
 * sowie root ohne Passwort. */
void user_init(void);

/* Liest /Festplatte/benutzer.conf. false heisst "keine Datei" - das ist
 * kein Fehler, dann gilt die Werkseinstellung. */
bool user_load(void);
bool user_save(void);
/* Gibt es eine gespeicherte Datenbank? Nur dann wird angemeldet. */
bool user_store_exists(void);

size_t        user_count(void);
struct user  *user_at(size_t index);
struct user  *user_by_name(const char *name);
struct user  *user_by_uid(uint32_t uid);
const char   *user_name_of(uint32_t uid);

/* Die vorgegebenen Rollen. Eine unbekannte Rolle gilt als "benutzer". */
size_t      role_count(void);
const char *role_name(size_t index);
uint32_t    role_caps(const char *name);
/* Der Name zur Menge - "verwalter", "netzwerk", ... oder "eigen". */
const char *caps_role(uint32_t caps);
/* "Konten, Netz, Protokoll" fuer die Anzeige. */
void        caps_text(uint32_t caps, char *out, size_t size);
bool        cap_parse(const char *text, uint32_t *out);

size_t        group_count(void);
struct group *group_at(size_t index);
struct group *group_by_name(const char *name);
struct group *group_by_gid(uint32_t gid);
const char   *group_name_of(uint32_t gid);
struct group *group_create(const char *name, uint32_t gid);
bool          group_add_member(struct group *g, uint32_t uid);
bool          group_remove_member(struct group *g, uint32_t uid);
/* Gehoert uid zu gid - als Hauptgruppe oder als eingetragenes Mitglied? */
bool          user_in_group(uint32_t uid, uint32_t gid);

/* Legt einen Benutzer an. Der Name muss frei und brauchbar sein; ein
 * leeres Passwort ist erlaubt, wird aber als solches vermerkt. */
struct user *user_create(const char *name, const char *full,
                         const char *password, bool admin,
                         char *error, size_t error_size);
/* Setzt Rolle und damit Faehigkeiten. Unbekannte Namen scheitern. */
bool user_set_role(struct user *u, const char *role);
static inline bool user_is_admin(const struct user *u)
{
    return u && (u->uid == UID_ROOT || (u->caps & CAP_ALL) == CAP_ALL);
}
/* Entfernt einen Benutzer. Der letzte Verwalter bleibt stehen - sonst
 * kaeme niemand mehr an die Verwaltung heran. */
bool user_delete(struct user *u, char *error, size_t error_size);
bool user_set_password(struct user *u, const char *password);
bool user_check_password(const struct user *u, const char *password);
/* Legt das Heimatverzeichnis an, falls es fehlt, und setzt die Rechte. */
bool user_ensure_home(struct user *u);

/* Setzt name in das Heimatverzeichnis des angemeldeten Benutzers. Gibt
 * es keines - niemand angemeldet, oder root, dem die Wurzel gehoert -,
 * kommt ersatz zum Zug. Die Funktion gibt es, damit nicht vier
 * Programme dieselbe Schraegstrich-Rechnung anstellen. */
void user_home_file(const char *name, const char *ersatz, char *out,
                    size_t size);

/* --- Sitzung ------------------------------------------------------- */

/* Meldet einen Benutzer an der Oberflaeche an. */
void         session_login(struct user *u);
void         session_logout(void);
struct user *session_user(void);        /* NULL = niemand angemeldet */

/* Wer handelt gerade? Ein Ring-3-Programm gilt unter seiner eigenen
 * Nummer, alles andere unter der des angemeldeten Benutzers. Solange
 * niemand angemeldet ist - beim Hochfahren etwa - ist das root. */
uint32_t session_uid(void);
uint32_t session_gid(void);
bool     session_is_admin(void);
/* Darf der Handelnde das? Ein Ring-3-Programm nie mehr als sein
 * Benutzer, und ein Fehlschlag steht in der Pruefspur. */
bool     session_can(uint32_t cap);
uint32_t session_caps(void);

#endif /* USER_H */
