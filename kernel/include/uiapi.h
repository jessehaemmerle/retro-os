/* uiapi.h - Fenster fuer Programme in Ring 3.
 *
 * Der Kernel besitzt das Fenstersystem; ein Benutzerprogramm bekommt
 * deshalb kein Fenster in die Hand, sondern eine Nummer. Dahinter
 * steht hier eine Leinwand, in die seine Zeichenbefehle malen, und eine
 * Warteschlange, in der die Ereignisse liegen, bis das Programm sie
 * abholt.
 *
 * Stuerzt das Programm ab, raeumt der Kernel das Fenster weg. Schliesst
 * der Benutzer es, bekommt das Programm ein Ereignis - beenden muss es
 * sich selbst.
 */
#ifndef UIAPI_H
#define UIAPI_H

#include "retro.h"
#include "syscall.h"

struct process;
struct user_window;

/* Legt ein Fenster an und traegt es beim Prozess ein. Gibt die Nummer
 * zurueck, unter der das Programm es anspricht. */
int64_t uiapi_open(struct process *proc, const char *title,
                   int32_t width, int32_t height);

int64_t uiapi_draw(struct process *proc, int32_t handle,
                   const struct user_draw_cmd *cmds, size_t count);

/* Holt ein Ereignis; 1 = eines da, 0 = keines. */
int64_t uiapi_event(struct process *proc, int32_t handle,
                    struct user_event *out, uint32_t timeout_ms);

int64_t uiapi_close(struct process *proc, int32_t handle);

/* Schliesst alle Fenster eines Prozesses - beim Beenden. */
void uiapi_release(struct process *proc);

#endif /* UIAPI_H */
