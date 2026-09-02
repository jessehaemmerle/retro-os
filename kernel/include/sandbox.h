/* sandbox.h - der Kaefig um ein Programm.
 *
 * Rechte sagen, was ein Benutzer darf. Ein Kaefig sagt, was ein
 * einzelnes Programm darf - und das ist etwas anderes. Ein Bildbetrachter
 * laeuft unter meinem Namen und darf damit alles, was ich darf; er
 * braucht davon aber nichts ausser der einen Datei, die ich ihm
 * hinhalte. Genau diese Luecke schliesst der Kaefig: Er nimmt einem
 * Programm Faehigkeiten weg, die sein Benutzer sehr wohl haette.
 *
 * Vier Dinge werden beschraenkt:
 *
 *   Systemaufrufe   Nur die erlaubten Gruppen kommen durch. Alles
 *                   andere endet je nach Profil mit einem Fehler oder
 *                   damit, dass das Programm beendet wird.
 *   Dateibaum       Ein Wurzelpfad, unter dem alles liegen muss. Wer
 *                   darueber hinausgreift, bekommt "nicht gefunden" -
 *                   nicht "verboten", denn die blosse Auskunft, dass es
 *                   eine Datei gibt, ist schon eine Auskunft.
 *   Speicher        Eine Obergrenze fuer den Heap.
 *   Kinder          Wer sich nicht abspalten darf, kann auch keine
 *                   Kindeskinder erzeugen.
 *
 * Ein Kaefig laesst sich nur enger machen, nie weiter - weder von aussen
 * noch vom Programm selbst. Deshalb darf ein Programm sich getrost
 * selbst einsperren, sobald es alles beisammen hat, was es braucht:
 * genau der Zug, den seccomp in Linux moeglich macht.
 *
 * Vererbt wird er beim Abspalten. Ein Kind kommt nicht dadurch frei,
 * dass es ein Kind ist.
 */
#ifndef SANDBOX_H
#define SANDBOX_H

#include "retro.h"
#include "vfs.h"

#define SB_NAME_MAX 15

/* Die Gruppen von Systemaufrufen. Einzelne Nummern zu erlauben waere
 * genauer und in der Bedienung unbrauchbar - niemand stellt sich
 * dreissig Schalter richtig ein. */
#define SB_CORE       (1u << 0)   /* beenden, schlafen, eigene Nummer   */
#define SB_STDIO      (1u << 1)   /* lesen und schreiben auf 0, 1, 2    */
#define SB_FILE_READ  (1u << 2)   /* oeffnen, lesen, auflisten          */
#define SB_FILE_WRITE (1u << 3)   /* schreiben und anlegen              */
#define SB_NET        (1u << 4)   /* verbinden, senden, zuhoeren        */
#define SB_WIN        (1u << 5)   /* eigene Fenster                     */
#define SB_PROC       (1u << 6)   /* abspalten und warten               */
#define SB_IPC        (1u << 7)   /* Roehren und geteilter Speicher     */
#define SB_ALL        0xFFu

/* Was bei einem Verstoss geschieht. */
enum sb_penalty {
    SB_DENY,        /* Fehler zurueckgeben - das Programm darf reagieren */
    SB_KILL         /* sofort beenden - fuer Profile ohne Spielraum      */
};

struct sandbox {
    bool     active;
    uint32_t allow;
    char     profile[SB_NAME_MAX + 1];
    char     root[FS_PATH_MAX];    /* leer = ganzer Baum */
    uint32_t max_pages;            /* 0 = unbegrenzt     */
    uint8_t  penalty;
    uint32_t denials;
};

/* Setzt den Kaefig auf ein bekanntes Profil. Enger geht immer, weiter
 * nie - der Versuch scheitert dann. */
bool sandbox_apply(struct sandbox *box, const char *profile,
                   const char *home);

/* Ist diese Gruppe erlaubt? Ohne Kaefig immer. */
bool sandbox_allows(const struct sandbox *box, uint32_t group);

/* Liegt der Pfad im erlaubten Teil des Baums? Prueft rein auf dem Text
 * und behandelt ".." dabei richtig - sonst waere die Wurzel eine
 * Empfehlung und keine Grenze. */
bool sandbox_path_ok(const struct sandbox *box, const char *path);

size_t      sandbox_profile_count(void);
const char *sandbox_profile_name(size_t index);
/* "Dateien lesen, Fenster" - fuer die Anzeige. */
void        sandbox_text(const struct sandbox *box, char *out, size_t size);

/* Welche Gruppe braucht dieser Systemaufruf? 0 heisst "immer erlaubt". */
uint32_t sandbox_group_of(uint64_t syscall_number);
const char *sandbox_group_name(uint32_t group);

#endif /* SANDBOX_H */
