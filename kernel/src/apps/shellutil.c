/* shellutil.c - Muster, Ausdruecke, Kalender und die Befehlstabelle.
 *
 * Alles hier rechnet nur und fasst nichts an. Das ist Absicht: Die
 * Stellen, an denen eine Konsole falsch liegt, sind selten die
 * Schleifen ueber den Dateibaum - es sind die Randfaelle beim
 * Mustervergleich, der Vorrang im Ausdruck und der Wochentag im
 * Februar eines Schaltjahres.
 */

#include "shellutil.h"
#include "kstring.h"

/* ------------------------------------------------------------------ */
/* Namensmuster                                                        */
/* ------------------------------------------------------------------ */

/* Ohne Rekursion: Beim Stern wird gemerkt, wo er stand und wie weit der
 * Text war. Passt es spaeter nicht, wird dorthin zurueckgesprungen und
 * der Stern frisst ein Zeichen mehr. Das ist derselbe Kniff, den
 * Kommandozeilen seit je benutzen, und er braucht keinen Stapel - was
 * im Kernel zaehlt, denn dessen Stapel ist klein. */
static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

bool sh_match(const char *pattern, const char *text)
{
    const char *star = NULL;
    const char *retry = NULL;

    if (!pattern || !text)
        return false;

    while (*text) {
        char p = *pattern;
        char t = *text;

        if (p == '?' || (p && lower(p) == lower(t))) {
            pattern++;
            text++;
            continue;
        }
        if (p == '*') {
            star = pattern++;
            retry = text;
            continue;
        }
        if (star) {
            pattern = star + 1;
            text = ++retry;
            continue;
        }
        return false;
    }

    /* Nachlaufende Sterne duerfen leer bleiben. */
    while (*pattern == '*')
        pattern++;
    return *pattern == '\0';
}

/* ------------------------------------------------------------------ */
/* Ausdruecke                                                          */
/* ------------------------------------------------------------------ */

/* Ein Absteiger mit zwei Ebenen: Summe ruft Produkt, Produkt ruft
 * Faktor, Faktor kann wieder eine Summe in Klammern sein. Mehr braucht
 * "Punkt vor Strich" nicht. */
struct parser {
    const char *p;
    bool        ok;
    const char *error;
};

static void skip_space(struct parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t')
        ps->p++;
}

static int64_t parse_sum(struct parser *ps);

static int64_t parse_factor(struct parser *ps)
{
    skip_space(ps);

    bool negate = false;

    /* Ein Vorzeichen darf mehrfach stehen: "--3" ist 3. */
    for (;;) {
        if (*ps->p == '-') { negate = !negate; ps->p++; }
        else if (*ps->p == '+') { ps->p++; }
        else break;
        skip_space(ps);
    }

    int64_t value = 0;

    if (*ps->p == '(') {
        ps->p++;
        value = parse_sum(ps);
        skip_space(ps);
        if (*ps->p != ')') {
            ps->ok = false;
            ps->error = "Es fehlt eine schliessende Klammer.";
            return 0;
        }
        ps->p++;
    } else if (*ps->p >= '0' && *ps->p <= '9') {
        while (*ps->p >= '0' && *ps->p <= '9') {
            int64_t digit = *ps->p++ - '0';

            /* Ueberlauf faellt auf, bevor er passiert - sonst kaeme
             * eine Zahl heraus, die niemand eingegeben hat. */
            if (value > (int64_t)922337203685477580LL) {
                ps->ok = false;
                ps->error = "Die Zahl ist zu gross.";
                return 0;
            }
            value = value * 10 + digit;
        }
    } else {
        ps->ok = false;
        ps->error = *ps->p ? "Da steht keine Zahl."
                           : "Der Ausdruck hoert zu frueh auf.";
        return 0;
    }

    return negate ? -value : value;
}

static int64_t parse_product(struct parser *ps)
{
    int64_t left = parse_factor(ps);

    for (;;) {
        skip_space(ps);

        char op = *ps->p;

        if (op != '*' && op != '/' && op != '%')
            return left;
        ps->p++;

        int64_t right = parse_factor(ps);

        if (!ps->ok)
            return 0;
        if ((op == '/' || op == '%') && right == 0) {
            ps->ok = false;
            ps->error = "Durch null geht es nicht.";
            return 0;
        }

        if (op == '*')
            left = left * right;
        else if (op == '/')
            left = left / right;
        else
            left = left % right;
    }
}

static int64_t parse_sum(struct parser *ps)
{
    int64_t left = parse_product(ps);

    for (;;) {
        skip_space(ps);

        char op = *ps->p;

        if (op != '+' && op != '-')
            return left;
        ps->p++;

        int64_t right = parse_product(ps);

        if (!ps->ok)
            return 0;
        left = op == '+' ? left + right : left - right;
    }
}

bool sh_eval(const char *text, int64_t *out, char *error, size_t error_size)
{
    struct parser ps = { text, true, NULL };

    if (!text || !out)
        return false;

    int64_t value = parse_sum(&ps);

    skip_space(&ps);

    if (ps.ok && *ps.p) {
        ps.ok = false;
        ps.error = "Dahinter steht noch etwas.";
    }

    if (!ps.ok) {
        if (error && error_size)
            strlcpy(error, ps.error ? ps.error : "Das ist kein Ausdruck.",
                    error_size);
        return false;
    }

    *out = value;
    return true;
}

/* ------------------------------------------------------------------ */
/* Kalender                                                            */
/* ------------------------------------------------------------------ */

bool sh_leap_year(uint16_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint8_t sh_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[13] = {
        0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && sh_leap_year(year))
        return 29;
    return days[month];
}

const char *sh_month_name(uint8_t month)
{
    static const char *const names[13] = {
        "", "Januar", "Februar", "Maerz", "April", "Mai", "Juni", "Juli",
        "August", "September", "Oktober", "November", "Dezember"
    };

    return month >= 1 && month <= 12 ? names[month] : "";
}

int sh_weekday(uint16_t year, uint8_t month, uint8_t day)
{
    /* Zeller rechnet Januar und Februar als dreizehnten und
     * vierzehnten Monat des Vorjahres - dann faellt der Schalttag ans
     * Ende und stoert die Formel nicht. */
    int m = month;
    int y = year;

    if (m < 3) {
        m += 12;
        y -= 1;
    }

    int k = y % 100;
    int j = y / 100;
    int h = (day + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;

    /* Zeller liefert 0 = Samstag. Umgerechnet auf 0 = Montag. */
    return (h + 5) % 7;
}

size_t sh_calendar(uint16_t year, uint8_t month, uint8_t highlight,
                   char *out, size_t size)
{
    /* Jede Spalte ist vier Zeichen breit - nur so bleibt die Woche in
     * einer Reihe, auch wenn ein Tag in Klammern steht. */
    static const char *const heads = " Mo  Di  Mi  Do  Fr  Sa  So";
    size_t used = 0;

    if (!out || !size)
        return 0;
    out[0] = '\0';
    if (month < 1 || month > 12)
        return 0;

    #define ADD(...) do {                                        \
        if (used < size - 1) {                                   \
            ksnprintf(out + used, size - used, __VA_ARGS__);     \
            used += strlen(out + used);                          \
        }                                                        \
    } while (0)

    ADD("    %s %u\n%s\n", sh_month_name(month), (unsigned)year, heads);

    int first = sh_weekday(year, month, 1);
    uint8_t last = sh_days_in_month(year, month);

    /* Die Spalten vor dem Ersten bleiben leer. */
    for (int i = 0; i < first; i++)
        ADD("    ");

    int column = first;

    for (uint8_t day = 1; day <= last; day++) {
        if (day == highlight)
            ADD("[%2u]", (unsigned)day);
        else
            ADD(" %2u ", (unsigned)day);

        if (++column == 7) {
            ADD("\n");
            column = 0;
        }
    }
    if (column)
        ADD("\n");
    #undef ADD

    return used;
}

/* ------------------------------------------------------------------ */
/* Die Befehlstabelle                                                  */
/* ------------------------------------------------------------------ */

static const char *const groups[] = {
    "Dateien", "Text", "Programme", "System", "Netz", "Sicherheit"
};

static const struct sh_command commands[] = {
    /* --- Dateien --- */
    { "ls", "dir", "Dateien", "ls [-l] [pfad]",
      "Ordnerinhalt anzeigen",
      "Mit -l stehen Rechte, Eigentuemer und Gruppe davor." },
    { "cd", NULL, "Dateien", "cd <pfad>", "Ordner wechseln", NULL },
    { "pwd", NULL, "Dateien", "pwd", "aktuellen Pfad anzeigen", NULL },
    { "baum", "tree", "Dateien", "baum [pfad] [tiefe]",
      "Ordner als Baum zeichnen",
      "Ohne Tiefe geht es drei Ebenen hinunter." },
    { "mkdir", NULL, "Dateien", "mkdir <name>", "Ordner anlegen", NULL },
    { "touch", NULL, "Dateien", "touch <name>", "leere Datei anlegen", NULL },
    { "kopiere", "cp", "Dateien", "kopiere <quelle> <ziel>",
      "Datei oder Ordner kopieren",
      "Ist das Ziel ein Ordner, landet die Kopie darin unter demselben\n"
      "Namen. Ordner werden mit allem Inhalt kopiert." },
    { "verschiebe", "mv", "Dateien", "verschiebe <quelle> <ziel>",
      "verschieben oder umbenennen",
      "Innerhalb eines Dateisystems wird nur umgehaengt - es wird kein\n"
      "Byte angefasst. Ueber die Grenze hinweg wird kopiert und geloescht." },
    { "rm", "del", "Dateien", "rm <name>",
      "in den Papierkorb legen",
      "Was schon im Papierkorb liegt, wird endgueltig geloescht." },
    { "finde", "find", "Dateien", "finde [pfad] <muster>",
      "Dateien nach Namensmuster suchen",
      "Im Muster steht * fuer beliebig viele Zeichen und ? fuer eines." },
    { "groesse", "du", "Dateien", "groesse [pfad]",
      "Platzbedarf eines Ordners",
      "Zaehlt alles darunter zusammen und zeigt die groessten zuerst." },
    { "info", "stat", "Dateien", "info <pfad>",
      "alles ueber einen Eintrag",
      "Art, Groesse, Eigentuemer, Rechte, Zeitpunkt und wo er liegt." },
    { "papierkorb", NULL, "Dateien", "papierkorb [zurueck <n>|leeren]",
      "Geloeschtes ansehen und zurueckholen", NULL },
    { "platte", NULL, "Dateien", "platte",
      "Laufwerke und eingehaengtes Dateisystem", NULL },
    { "formatieren", NULL, "Dateien", "formatieren wirklich [name]",
      "Datentraeger neu mit FAT32 formatieren", NULL },

    /* --- Text --- */
    { "cat", "type", "Text", "cat <datei>", "Datei ausgeben", NULL },
    { "kopf", "head", "Text", "kopf [-n] <datei>",
      "die ersten Zeilen", "Ohne Angabe sind es zehn." },
    { "ende", "tail", "Text", "ende [-n] <datei>",
      "die letzten Zeilen", "Ohne Angabe sind es zehn." },
    { "zaehle", "wc", "Text", "zaehle <datei>",
      "Zeilen, Woerter und Zeichen zaehlen", NULL },
    { "suche", "grep", "Text", "suche <text> <pfad>",
      "Text in Dateien suchen",
      "Gross- und Kleinschreibung sind gleich. Ist der Pfad ein Ordner,\n"
      "wird alles darunter durchsucht." },
    { "sortiere", "sort", "Text", "sortiere <datei>",
      "Zeilen sortiert ausgeben", NULL },
    { "vergleiche", "diff", "Text", "vergleiche <a> <b>",
      "zwei Dateien vergleichen",
      "Zeigt die erste Zeile, in der sie sich unterscheiden." },
    { "hex", NULL, "Text", "hex <datei> [anzahl]",
      "Datei als Zahlen und Zeichen",
      "Ohne Angabe die ersten 256 Bytes." },
    { "schreib", NULL, "Text", "schreib <datei> <text>",
      "Text an eine Datei anhaengen", NULL },
    { "echo", NULL, "Text", "echo <text>", "Text ausgeben", NULL },
    { "edit", NULL, "Text", "edit <datei>", "Datei im Editor oeffnen", NULL },

    /* --- Programme --- */
    { "starte", "run", "Programme", "starte <programm> [text]",
      "ein Programm in Ring 3 ausfuehren", NULL },
    { "kaefig", NULL, "Programme", "kaefig [<profil> <programm> [text]]",
      "ein Programm eingesperrt starten",
      "Ohne Angabe stehen die Profile mit ihren Rechten da." },
    { "programme", NULL, "Programme", "programme",
      "die mitgelieferten Programme auflisten", NULL },
    { "prozesse", NULL, "Programme", "prozesse",
      "laufende Programme mit Verwandtschaft", NULL },
    { "threads", "ps", "Programme", "threads",
      "laufende Threads mit Zustand und Rechenzeit", NULL },
    { "beende", "kill", "Programme", "beende <nummer>",
      "ein laufendes Programm beenden",
      "Fremde Programme beendet nur ein Verwalter." },
    { "wo", "which", "Programme", "wo <name>",
      "wo ein Programm liegt", NULL },

    /* --- System --- */
    { "hilfe", "help", "System", "hilfe [bereich]",
      "diese Uebersicht",
      "Mit einem Bereich nur dessen Befehle: Dateien, Text, Programme,\n"
      "System, Netz, Sicherheit." },
    { "man", NULL, "System", "man <befehl>",
      "alles zu einem einzelnen Befehl", NULL },
    { "verlauf", "history", "System", "verlauf",
      "die zuletzt eingegebenen Zeilen",
      "Mit den Pfeiltasten nach oben und unten holt man sie zurueck." },
    { "rechne", "expr", "System", "rechne <ausdruck>",
      "ganze Zahlen ausrechnen",
      "Es gibt + - * / % und Klammern, Punkt vor Strich." },
    { "kalender", "cal", "System", "kalender [monat] [jahr]",
      "einen Monat als Kalender",
      "Ohne Angabe der laufende Monat, der heutige Tag in Klammern." },
    { "datum", "date", "System", "datum", "Datum und Uhrzeit", NULL },
    { "laufzeit", "uptime", "System", "laufzeit",
      "Zeit seit dem Einschalten", NULL },
    { "speicher", "mem", "System", "speicher", "Speicherbelegung", NULL },
    { "warte", "sleep", "System", "warte <millisekunden>",
      "eine Weile nichts tun", NULL },
    { "version", "ver", "System", "version", "Systemversion", NULL },
    { "maus", "mouse", "System", "maus",
      "Zeigegeraet und woran es haengt",
      "Bewegt sich der Zeiger nicht, steht hier, ob ueberhaupt Bytes\n"
      "ankommen und ueber welchen Weg." },
    { "bildschirm", "display", "System", "bildschirm [1280x800|2x|auto]",
      "Aufloesung und Vergroesserung",
      "Ohne Angabe stehen die moeglichen Aufloesungen da. Die\n"
      "Vergroesserung blaest jeden Punkt zum Quadrat auf - auf einem\n"
      "sehr feinen Bildschirm ist die Schrift damit wieder lesbar." },
    { "sprache", "lang", "System", "sprache [de|en]",
      "Sprache zeigen oder umschalten",
      "Ohne Angabe stehen die verfuegbaren Sprachen da. Die Tastatur\n"
      "wandert mit, solange sie nicht von Hand gesetzt wurde." },
    { "schrift", NULL, "System", "schrift [name]",
      "Schriftarten zeigen oder waehlen", NULL },
    { "usb", NULL, "System", "usb",
      "Geraete am USB-Bus", NULL },
    { "installieren", "setup", "System", "installieren [ziel]",
      "RetroOS auf eine Platte bringen", NULL },
    { "leeren", "clear", "System", "leeren", "Bildschirm loeschen", NULL },
    { "neustart", "reboot", "System", "neustart", "Rechner neu starten", NULL },

    /* --- Netz --- */
    { "netz", "ipconfig", "Netz", "netz",
      "Adresse, Gateway, Namensserver, Zaehler", NULL },
    { "ping", NULL, "Netz", "ping <ziel>", "Erreichbarkeit pruefen", NULL },
    { "aufloesen", "nslookup", "Netz", "aufloesen <name>",
      "Namen in eine Adresse wandeln", NULL },
    { "holen", "wget", "Netz", "holen <adresse> [datei]",
      "Seite abrufen und wahlweise speichern", NULL },
    { "firewall", NULL, "Netz", "firewall [an|aus|standard|regel|weg|leeren]",
      "Paketfilter zeigen und regeln",
      "Regeln werden von oben nach unten geprueft, die erste passende\n"
      "entscheidet. Passt keine, gilt die Grundeinstellung." },

    /* --- Sicherheit --- */
    { "wer", "whoami", "Sicherheit", "wer",
      "angemeldeter Benutzer, Rolle und Gruppen", NULL },
    { "benutzer", NULL, "Sicherheit",
      "benutzer [neu|loeschen|passwort|rolle|verwalter <name> [wert]]",
      "Konten zeigen und verwalten", NULL },
    { "gruppen", NULL, "Sicherheit", "gruppen",
      "Gruppen und ihre Mitglieder", NULL },
    { "rechte", NULL, "Sicherheit", "rechte <datei> [modus]",
      "Rechte zeigen oder setzen",
      "Der Modus ist \"750\" oder \"rwxr-x---\"." },
    { "besitzer", NULL, "Sicherheit", "besitzer <datei> [name[:gruppe]]",
      "Eigentuemer zeigen oder setzen", NULL },
    { "pruefsumme", "sha256", "Sicherheit", "pruefsumme <datei>",
      "Pruefsumme einer Datei",
      "SHA-256, dieselbe, die auch anderswo gerechnet wird." },
    { "sperren", NULL, "Sicherheit", "sperren",
      "Bildschirm sperren", NULL },
    { "protokoll", NULL, "Sicherheit",
      "protokoll [alle|warnung|fehler|speichern|leeren]",
      "Systemprotokoll zeigen und sichern", NULL },
    { "pruefspur", NULL, "Sicherheit", "pruefspur [alle|abgewiesen|speichern]",
      "Sicherheitsereignisse ansehen", NULL },
    { "aufgaben", NULL, "System",
      "aufgaben [neu|fertig|weg|wichtig|termin ...]",
      "Aufgabenliste fuehren", NULL },
};

size_t sh_command_count(void) { return ARRAY_LEN(commands); }

const struct sh_command *sh_command_at(size_t index)
{
    return index < ARRAY_LEN(commands) ? &commands[index] : NULL;
}

const struct sh_command *sh_command_find(const char *name)
{
    if (!name || !name[0])
        return NULL;

    for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
        if (strcasecmp(commands[i].name, name) == 0)
            return &commands[i];
        if (commands[i].alias && strcasecmp(commands[i].alias, name) == 0)
            return &commands[i];
    }
    return NULL;
}

size_t sh_group_count(void) { return ARRAY_LEN(groups); }

const char *sh_group_at(size_t index)
{
    return index < ARRAY_LEN(groups) ? groups[index] : "";
}
