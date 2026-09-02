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

/* Kleine Standarddialoge, von mehreren Programmen genutzt. */
typedef void (*dialog_text_fn)(const char *text, void *user);
typedef void (*dialog_confirm_fn)(bool yes, void *user);

void dialog_input(const char *title, const char *prompt, const char *preset,
                  dialog_text_fn on_ok, void *user);
void dialog_confirm(const char *title, const char *message,
                    dialog_confirm_fn on_answer, void *user);
void dialog_message(const char *title, const char *message);

#endif /* APPS_H */
