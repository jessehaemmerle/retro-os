/* registry.c - die Liste der Programme, die das System kennt.
 *
 * RetroOS laedt keine ausfuehrbaren Dateien; die Programme sind Teil des
 * Kernels. Diese Tabelle ist die einzige Stelle, an der Startmenue und
 * Desktop erfahren, welche es gibt.
 */

#include "apps.h"

const struct app_entry app_list[] = {
    { "Dateimanager",      ICON_FOLDER_OPEN, app_filemanager, true  },
    { "Editor",            ICON_EDITOR,      app_editor,      true  },
    { "Browser",           ICON_BROWSER,     app_browser,     true  },
    { "Konsole",           ICON_TERMINAL,    app_terminal,    true  },
    { "Systeminformation", ICON_COMPUTER,    app_sysinfo,     true  },
    { "Installieren",      ICON_DISK,        app_setup,       true  },
    { "Ueber RetroOS",     ICON_INFO,        app_about,       false },
};

const size_t app_count = ARRAY_LEN(app_list);
