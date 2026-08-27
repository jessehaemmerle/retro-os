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
    if (apic_init())
        pic_disable();

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
    fs_init();
    fs_mount_disk();

    /* Erst jetzt gibt es die Festplatte - und damit die gespeicherten
     * Einstellungen. Bis hierher galt die Werkseinstellung. */
    if (config_load())
        kprintf("Einstellung : aus %s uebernommen\n", CONFIG_PATH);
    config_apply();

    /* Ab hier laufen mehrere Threads nebeneinander. */
    thread_init();
    syscall_init();
    process_init();

    /* Die Eingabegeraete am USB-Bus brauchen einen eigenen Thread. */
    usb_start_polling();
    thread_current()->priority = PRIO_HIGH;   /* die Oberflaeche */

    /* Netzwerk (bringt seinen eigenen Thread mit) */
    trust_store_init();
    net_init();

    gui_init();

    kprintf("Oberflaeche : bereit\n\n");

    gui_run();
}
