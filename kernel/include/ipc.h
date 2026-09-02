/* ipc.h - Roehren und geteilter Speicher.
 *
 * Bisher konnten zwei Programme nur ueber das Netz miteinander reden -
 * ueber TCP, auch wenn sie auf demselben Rechner liefen. Das geht, ist
 * aber teuer und umstaendlich: Ein Elternteil, das sein Kind fuettern
 * will, braucht keinen Dreiwegehandschlag.
 *
 * Es gibt zwei Wege, und sie sind absichtlich verschieden:
 *
 *   Eine Roehre ist ein Strom von Bytes mit einem Schreiber und einem
 *   Leser. Sie kostet einen Ringpuffer im Kern und kopiert zweimal -
 *   dafuer muss sich niemand um Sperren kuemmern, und das Ende der
 *   Roehre sagt dem Leser von selbst, dass Schluss ist.
 *
 *   Geteilter Speicher ist derselbe Seitenrahmen in zwei Adressraeumen.
 *   Er kopiert gar nicht und ist damit die schnellste Art, grosse
 *   Mengen weiterzureichen - dafuer muessen sich beide Seiten selbst
 *   einigen, wer wann hineinschreibt.
 *
 * Beide ueberleben das Abspalten: Ein Kind erbt die offenen Roehren
 * seines Elternteils, und die Zaehlung merkt sich, wie viele Seiten
 * noch daran haengen.
 */
#ifndef IPC_H
#define IPC_H

#include "retro.h"
#include "vmm.h"

#define PIPE_BUFFER   4096
#define PIPE_MAX      16

#define SHM_MAX       8
#define SHM_NAME_MAX  23
#define SHM_PAGES_MAX 64          /* 256 KB je Bereich */

/* Wohin geteilte Bereiche im Adressraum eines Programms kommen. Weit
 * ueber dem Heap, damit sbrk() nie dagegen laeuft. */
#define SHM_BASE      0x0000000020000000ULL
#define SHM_STRIDE    (SHM_PAGES_MAX * PAGE_SIZE)

struct pipe;

/* --- Roehren ------------------------------------------------------- */

/* Legt eine Roehre mit je einem Leser und einem Schreiber an. */
struct pipe *pipe_create(void);
/* Meldet ein weiteres Ende an - beim Abspalten. */
void pipe_share(struct pipe *p, bool writer);
/* Gibt ein Ende zurueck. Beim letzten verschwindet die Roehre. */
void pipe_close(struct pipe *p, bool writer);

/* Schreibt, so viel Platz ist. Liefert die Anzahl; 0 heisst voll,
 * negativ, dass niemand mehr liest. */
int64_t pipe_write(struct pipe *p, const void *data, size_t length);
/* Liest, wartet aber hoechstens timeout_ms auf Nachschub. 0 heisst
 * "nichts da und die Zeit ist um", negativ heisst "Ende der Roehre":
 * kein Schreiber mehr und nichts mehr drin. */
int64_t pipe_read(struct pipe *p, void *out, size_t length,
                  uint32_t timeout_ms);

size_t pipe_pending(const struct pipe *p);
size_t pipe_count(void);

/* --- Geteilter Speicher -------------------------------------------- */

/* Sucht einen Bereich unter diesem Namen oder legt ihn an. Liefert die
 * Nummer oder -1. Die Groesse gilt nur beim Anlegen. */
int shm_open(const char *name, size_t bytes, bool create);
/* Blendet den Bereich in einen Adressraum ein. Liefert die Adresse
 * oder 0. Zweimaliges Einblenden liefert dieselbe Adresse. */
uint64_t shm_attach(struct address_space *space, int id);
/* Blendet ihn wieder aus und zaehlt herunter. */
void shm_detach(struct address_space *space, int id);
/* Entfernt den Namen. Der Bereich selbst bleibt, bis der letzte ihn
 * ausgeblendet hat - wie beim Loeschen einer offenen Datei. */
bool shm_unlink(const char *name);

size_t      shm_count(void);
const char *shm_name(int id);
size_t      shm_bytes(int id);
uint32_t    shm_users(int id);
uint32_t    shm_owner(int id);

void ipc_init(void);

#endif /* IPC_H */
