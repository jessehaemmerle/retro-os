/* registry.c - die Liste der Programme, die das System kennt.
 *
 * RetroOS laedt keine ausfuehrbaren Dateien; die Programme sind Teil des
 * Kernels. Diese Tabelle ist die einzige Stelle, an der Startmenue und
 * Desktop erfahren, welche es gibt.
 */

#include "apps.h"
#include "trash.h"

/* Der Papierkorb zeigt an, ob etwas drinliegt. */
static enum icon_id trash_icon(void)
{
    return trash_count() ? ICON_TRASH_FULL : ICON_TRASH;
}

const struct app_entry app_list[] = {
    { "Dateimanager",      ICON_FOLDER_OPEN, app_filemanager, true,  NULL },
    { "Editor",            ICON_EDITOR,      app_editor,      true,  NULL },
    { "Browser",           ICON_BROWSER,     app_browser,     true,  NULL },
    { "Konsole",           ICON_TERMINAL,    app_terminal,    true,  NULL },
    { "Systeminformation", ICON_COMPUTER,    app_sysinfo,     true,  NULL },
    { "Systemmonitor",     ICON_MONITOR,     app_monitor,     true,  NULL },
    { "Protokoll",         ICON_LOG,         app_log,         true,  NULL },
    { "Aufgaben",          ICON_TASKS,       app_tasks,       true,  NULL },
    { "Rechner",           ICON_CALC,        app_calculator,  true,  NULL },
    { "Bilder",            ICON_IMAGE,       app_images,      true,  NULL },
    { "Tabelle",           ICON_TABLE,       app_sheet,       true,  NULL },
    { "Schreiben",         ICON_DOCUMENT,    app_write,       true,  NULL },
    { "Vortrag",           ICON_SLIDES,      app_slides,      true,  NULL },
    { "Programmieren",     ICON_CODE,        app_code,        true,  NULL },
    { "Einstellungen",     ICON_SETTINGS,    app_settings,    true,  NULL },
    { "Benutzer",          ICON_USERS,       app_users,       true,  NULL },
    { "Installieren",      ICON_DISK,        app_setup,       true,  NULL },
    { "Papierkorb",        ICON_TRASH,       app_trash,       true,  trash_icon },
    { "Bildschirmfoto",    ICON_CAMERA,      app_screenshot,  false, NULL },
    { "Ueber RetroOS",     ICON_INFO,        app_about,       false, NULL },
};

const size_t app_count = ARRAY_LEN(app_list);
