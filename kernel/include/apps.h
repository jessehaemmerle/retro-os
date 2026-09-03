/* apps.h - die mitgelieferten Programme. */
#ifndef APPS_H
#define APPS_H

#include "gui.h"
#include "vfs.h"

struct app_entry {
    const char  *name;
    enum icon_id icon;
    void       (*launch)(void);
    bool         on_desktop;
    /* Manche Symbole haengen vom Zustand ab - der Papierkorb sieht
     * voll anders aus als leer. Ohne diesen Haken gilt icon. */
    enum icon_id (*icon_now)(void);
};

extern const struct app_entry app_list[];
extern const size_t app_count;

void app_filemanager(void);
void app_editor(void);
void app_terminal(void);
void app_sysinfo(void);
void app_about(void);
void app_browser(void);
void app_setup(void);
void app_settings(void);
void app_trash(void);
void app_code(void);
void app_sheet(void);
void app_write(void);
void app_slides(void);
void app_users(void);
void app_monitor(void);
void app_log(void);
void app_tasks(void);
void app_images(void);
void app_calculator(void);
void app_screenshot(void);
void app_archive(void);
void app_calendar(void);
void app_clock(void);

/* --- Archive --- */
/* Oeffnet ein ZIP-Archiv in seinem Fenster. */
void archive_open(struct fs_node *file);
/* Packt einen Eintrag in ein Archiv daneben; out_path bekommt dessen
 * Pfad. */
bool archive_pack(struct fs_node *node, char *out_path, size_t out_size,
                  char *error, size_t error_size);
/* Packt ein Archiv in einen Ordner daneben aus. error traegt in beiden
 * Faellen eine Meldung. */
bool archive_unpack(struct fs_node *file, char *out_path, size_t out_size,
                    char *error, size_t error_size);

/* --- Bildschirmfoto ---
 * Aufgenommen wird erst im naechsten fertigen Bild; sonst waere das
 * Menue mit darauf, aus dem heraus es angestossen wurde. */
void screenshot_request(void);
bool screenshot_pending(void);
bool screenshot_take(char *path_out, size_t path_size,
                     char *error, size_t error_size);

/* Der Dateimanager laesst sich auch in einem bestimmten Ordner
 * oeffnen - der Papierkorb auf dem Desktop tut genau das. */
struct fs_node;
void filemanager_open(struct fs_node *dir);
/* Oeffnet eine Quelltextdatei im Programmierfenster. */
void code_open(struct fs_node *file);
/* Dasselbe fuer die Bueroprogramme. */
void sheet_open(struct fs_node *file);
void write_open(struct fs_node *file);
void slides_open(struct fs_node *file);

/* Oeffnet eine Adresse im Browser (auch aus anderen Programmen heraus). */
void browser_open(const char *url);

/* Oeffnet eine Datei im Editor (auch aus dem Dateimanager heraus). */
void editor_open(struct fs_node *file);
/* Zeigt ein Bild an. */
void image_open(struct fs_node *file);

/* Kleine Standarddialoge, von mehreren Programmen genutzt. */
typedef void (*dialog_text_fn)(const char *text, void *user);
typedef void (*dialog_confirm_fn)(bool yes, void *user);

void dialog_input(const char *title, const char *prompt, const char *preset,
                  dialog_text_fn on_ok, void *user);
void dialog_confirm(const char *title, const char *message,
                    dialog_confirm_fn on_answer, void *user);
/* Wie dialog_input, zeigt aber nur Sternchen. */
void dialog_password(const char *title, const char *prompt,
                     dialog_text_fn on_ok, void *user);
void dialog_message(const char *title, const char *message);

#endif /* APPS_H */
