/* main.c - Einstiegspunkt von RetroOS.
 *
 * Die Reihenfolge ist bewusst starr: erst die CPU-Strukturen, dann der
 * Speicher, dann die Geraete, zuletzt die Oberflaeche. Jede Stufe darf
 * voraussetzen, dass die vorherige steht.
 */

#include "retro.h"
#include "acpi.h"
#include "apic.h"
#include "usb.h"
#include "arch.h"
#include "block.h"
#include "config.h"
#include "boot.h"
#include "cpu.h"
#include "fb.h"
#include "gui.h"
#include "input.h"
#include "io.h"
#include "mm.h"
#include "net.h"
#include "pki.h"
#include "pci.h"
#include "serial.h"
#include "process.h"
#include "syscall.h"
#include "thread.h"
#include "vmm.h"
#include "vfs.h"
#include "trash.h"
#include "lock.h"
#include "perm.h"
#include "user.h"

NORETURN void kmain(void)
{
    serial_init();
    kprintf("\n"
            "=====================================\n"
            "  RetroOS 1.0 - Systemstart\n"
            "=====================================\n");

    if (!boot_revision_ok())
        panic("Das Bootloader-Protokoll wird nicht unterstuetzt.");

    boot_collect();

    const struct boot_info *bi = boot_info();
    kprintf("Bildschirm  : %ux%u, %u Bit\n",
            (unsigned)bi->fb_width, (unsigned)bi->fb_height, bi->fb_bpp);

    /* CPU-Strukturen */
    cpu_init_bsp();
    gdt_init();
    idt_init();
    pic_init();

    /* Speicher */
    pmm_init();
    heap_init();
    vmm_init();

    /* Unterbrechungen: der APIC, wo es ihn gibt. Dafuer muessen die
     * ACPI-Tabellen schon gelesen sein - dort steht, wo er liegt. */
    acpi_init();
    if (apic_init()) {
        pic_disable();

        /* Der Bootkern hat jetzt seine endgueltige APIC-Nummer. */
        cpu_init_bsp();
        gdt_init();
    }

    /* Zeit und Eingabe */
    timer_init(1000);
    ps2_init();
    keyboard_init();
    sti();

    /* Busse und Datentraeger */
    pci_init();
    storage_init();
    xhci_init();

    /* Grafik */
    if (!fb_init())
        panic("Es wurde kein nutzbarer Framebuffer gefunden.");
    if (!gfx_init())
        panic("Der Bildpuffer laesst sich nicht anlegen.");

    mouse_init((int32_t)bi->fb_width, (int32_t)bi->fb_height);

    /* Daten und Oberflaeche */
    config_defaults();
    user_init();
    fs_init();
    fs_mount_disk();

    /* Erst jetzt gibt es die Festplatte - und damit die gespeicherten
     * Rechte, Benutzer und Einstellungen. Bis hierher galt die
     * Werkseinstellung, und alles lief als root. */
    perm_store_load();
    if (user_load())
        kprintf("Benutzer    : %u aus %s\n",
                (unsigned)user_count(), USER_PATH);

    trash_init();

    if (config_load())
        kprintf("Einstellung : aus %s uebernommen\n", CONFIG_PATH);
    config_apply();

    /* Ab hier laufen mehrere Threads nebeneinander. */
    thread_init();
    syscall_init();
    process_init();

    /* Und jetzt auch auf mehreren Kernen. */
    uint32_t cores = smp_start();

    if (cores > 1)
        kprintf("Kerne       : %u in Betrieb\n", (unsigned)cores);

    /* Die Eingabegeraete am USB-Bus brauchen einen eigenen Thread. */
    usb_start_polling();
    thread_current()->priority = PRIO_HIGH;   /* die Oberflaeche */

    /* Netzwerk (bringt seinen eigenen Thread mit) */
    trust_store_init();
    net_init();

    gui_init();

    /* Gibt es eine gespeicherte Benutzerdatenbank, wird angemeldet.
     * Ohne sie - vom Stick oder von der CD - laeuft RetroOS wie frueher
     * als root durch; einen Anmeldebildschirm vor ein System zu setzen,
     * das ohnehin jedem gehoert, der die Scheibe einlegt, waere Theater. */
    if (user_store_exists()) {
        lock_show(LOCK_START);
    } else {
        session_login(user_by_uid(UID_ROOT));
        kprintf("Benutzer    : root (keine Datenbank - alles erlaubt)\n");
    }

    kprintf("Oberflaeche : bereit\n\n");

    gui_run();
}
