/* sandbox.c - Profile, Gruppen und die Pfadgrenze.
 *
 * Alles hier ist reine Rechnerei ohne Zustand ausser dem Kaefig selbst -
 * darum laesst es sich auf dem Entwicklungsrechner pruefen, und darum
 * ist die Pfadgrenze eine Textrechnung und keine Wanderung durch den
 * Dateibaum. Eine Grenze, die vom Zustand des Baums abhaengt, waere zu
 * dem Zeitpunkt falsch, an dem jemand einen Ordner umbenennt.
 */

#include "sandbox.h"
#include "kstring.h"
#include "syscall.h"

/* Die Profile. Vier genuegen: eines ohne Kaefig, eines fuer Programme
 * aus dem Netz, eines fuer Programme an eigenen Dateien und eines fuer
 * solche, die nur rechnen sollen. */
static const struct {
    const char *name;
    uint32_t    allow;
    bool        home_root;
    uint32_t    max_pages;
    uint8_t     penalty;
    const char *what;
} profiles[] = {
    { "offen",  SB_ALL, false, 0, SB_DENY,
      "alles - kein Kaefig" },

    /* Darf ins Netz und lesen, aber nichts anfassen und sich nicht
     * vermehren. Fuer alles, was Daten von draussen verarbeitet. */
    { "netz",   SB_CORE | SB_STDIO | SB_FILE_READ | SB_NET | SB_WIN | SB_IPC,
      false, 4096, SB_DENY,
      "lesen, Netz, Fenster - kein Schreiben, kein Abspalten" },

    /* Darf im eigenen Heim alles und sonst nichts. Fuer Programme, die
     * mit den Dateien des Benutzers arbeiten. */
    { "heim",   SB_CORE | SB_STDIO | SB_FILE_READ | SB_FILE_WRITE | SB_WIN |
                SB_PROC | SB_IPC,
      true, 4096, SB_DENY,
      "das eigene Heim - kein Netz" },

    /* Darf rechnen und reden, sonst nichts. Ein Verstoss beendet das
     * Programm: Wer hier anklopft, hat einen Fehler oder etwas vor. */
    { "streng", SB_CORE | SB_STDIO | SB_IPC, false, 1024, SB_KILL,
      "nur rechnen und ausgeben" },
};

size_t sandbox_profile_count(void) { return ARRAY_LEN(profiles); }

const char *sandbox_profile_name(size_t index)
{
    return index < ARRAY_LEN(profiles) ? profiles[index].name : "";
}

bool sandbox_apply(struct sandbox *box, const char *profile, const char *home)
{
    if (!box || !profile)
        return false;

    for (size_t i = 0; i < ARRAY_LEN(profiles); i++) {
        if (strcasecmp(profiles[i].name, profile) != 0)
            continue;

        uint32_t allow = profiles[i].allow;

        /* Enger geht immer, weiter nie. Sonst koennte ein Programm sich
         * selbst befreien, indem es "offen" waehlt - und der ganze
         * Kaefig waere eine Bitte. */
        if (box->active && (allow & ~box->allow))
            return false;

        box->allow = box->active ? (box->allow & allow) : allow;
        box->active = true;
        strlcpy(box->profile, profiles[i].name, sizeof(box->profile));
        box->penalty = profiles[i].penalty;

        if (profiles[i].max_pages &&
            (!box->max_pages || profiles[i].max_pages < box->max_pages))
            box->max_pages = profiles[i].max_pages;

        if (profiles[i].home_root && home && home[0])
            strlcpy(box->root, home, sizeof(box->root));

        /* "offen" ist kein Kaefig, sondern sein Fehlen. */
        if (allow == SB_ALL && !box->root[0] && !box->max_pages)
            box->active = false;
        return true;
    }
    return false;
}

bool sandbox_allows(const struct sandbox *box, uint32_t group)
{
    if (!box || !box->active || !group)
        return true;
    return (box->allow & group) == group;
}

/* ------------------------------------------------------------------ */
/* Die Pfadgrenze                                                      */
/* ------------------------------------------------------------------ */

/* Legt einen Pfad zusammen: Schraegstriche verdichten, "." wegwerfen,
 * ".." eine Ebene zurueckgehen lassen. Ohne das waere
 * "/Heim/../System" eine Umgehung, die aus zwei Punkten besteht. */
static void normalise(const char *path, char *out, size_t size)
{
    char parts[24][FS_NAME_MAX + 1];
    size_t count = 0;
    const char *p = path;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;

        char name[FS_NAME_MAX + 1];
        size_t n = 0;

        while (*p && *p != '/' && n < FS_NAME_MAX)
            name[n++] = *p++;
        name[n] = '\0';
        while (*p && *p != '/')
            p++;

        if (strcmp(name, ".") == 0)
            continue;
        if (strcmp(name, "..") == 0) {
            if (count)
                count--;
            continue;
        }
        if (count < ARRAY_LEN(parts))
            strlcpy(parts[count++], name, sizeof(parts[0]));
    }

    size_t used = 0;

    out[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        ksnprintf(out + used, size - used, "/%s", parts[i]);
        used += strlen(out + used);
    }
    if (!used)
        strlcpy(out, "/", size);
}

bool sandbox_path_ok(const struct sandbox *box, const char *path)
{
    if (!box || !box->active || !path)
        return true;

    /* Ohne das Recht, Dateien zu sehen, gibt es keinen erlaubten Pfad. */
    if (!(box->allow & (SB_FILE_READ | SB_FILE_WRITE)))
        return false;
    if (!box->root[0])
        return true;

    char clean[FS_PATH_MAX];
    char root[FS_PATH_MAX];

    normalise(path, clean, sizeof(clean));
    normalise(box->root, root, sizeof(root));

    size_t len = strlen(root);

    if (strcmp(root, "/") == 0)
        return true;
    if (strncasecmp(clean, root, len) != 0)
        return false;

    /* "/Heim" darf nicht auf "/Heimlich" passen - hinter der Wurzel muss
     * ein Schraegstrich oder das Ende kommen. */
    return clean[len] == '\0' || clean[len] == '/';
}

/* ------------------------------------------------------------------ */
/* Gruppen                                                             */
/* ------------------------------------------------------------------ */

uint32_t sandbox_group_of(uint64_t number)
{
    switch (number) {
    case SYS_EXIT:
    case SYS_SBRK:
    case SYS_SLEEP:
    case SYS_YIELD:
    case SYS_GETPID:
    case SYS_UPTIME:
    case SYS_ARGS:
        return SB_CORE;

    case SYS_OPEN:
    case SYS_CLOSE:
    case SYS_SEEK:
    case SYS_FILESIZE:
    case SYS_READDIR:
        return SB_FILE_READ;

    case SYS_CONNECT:
    case SYS_SEND:
    case SYS_RECV:
    case SYS_DISCONNECT:
    case SYS_LISTEN:
    case SYS_ACCEPT:
        return SB_NET;

    case SYS_WIN_OPEN:
    case SYS_WIN_DRAW:
    case SYS_WIN_EVENT:
    case SYS_WIN_CLOSE:
        return SB_WIN;

    case SYS_FORK:
    case SYS_WAIT:
        return SB_PROC;

    case SYS_PIPE:
    case SYS_SHM_OPEN:
    case SYS_SHM_MAP:
    case SYS_SHM_UNLINK:
        return SB_IPC;

    /* Lesen und Schreiben haengen an der Nummer, nicht am Aufruf -
     * dieselbe Nummer bedient die Konsole, eine Datei und eine Roehre.
     * Darum entscheidet der Aufruf selbst, und hier steht 0. */
    default:
        return 0;
    }
}

const char *sandbox_group_name(uint32_t group)
{
    switch (group) {
    case SB_CORE:       return "Grundlagen";
    case SB_STDIO:      return "Ein-/Ausgabe";
    case SB_FILE_READ:  return "Dateien lesen";
    case SB_FILE_WRITE: return "Dateien schreiben";
    case SB_NET:        return "Netz";
    case SB_WIN:        return "Fenster";
    case SB_PROC:       return "Abspalten";
    case SB_IPC:        return "Roehren";
    default:            return "Unbekanntes";
    }
}

void sandbox_text(const struct sandbox *box, char *out, size_t size)
{
    static const uint32_t all[] = {
        SB_CORE, SB_STDIO, SB_FILE_READ, SB_FILE_WRITE, SB_NET, SB_WIN,
        SB_PROC, SB_IPC
    };
    size_t used = 0;

    if (!out || !size)
        return;
    out[0] = '\0';

    if (!box || !box->active) {
        strlcpy(out, "kein Kaefig", size);
        return;
    }

    for (size_t i = 0; i < ARRAY_LEN(all); i++) {
        if (!(box->allow & all[i]))
            continue;
        ksnprintf(out + used, size - used, "%s%s", used ? ", " : "",
                  sandbox_group_name(all[i]));
        used += strlen(out + used);
    }
    if (!used)
        strlcpy(out, "nichts", size);
}
