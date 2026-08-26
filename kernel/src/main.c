/* main.c - Einstiegspunkt von RetroOS.
 *
 * Die Reihenfolge ist bewusst starr: erst die CPU-Strukturen, dann der
 * Speicher, dann die Geraete, zuletzt die Oberflaeche. Jede Stufe darf
 * voraussetzen, dass die vorherige steht.
 */

#include "retro.h"
#include "arch.h"
#include "boot.h"
#include "fb.h"
#include "gui.h"
#include "input.h"
#include "io.h"
#include "mm.h"
#include "serial.h"
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

    /* Zeit und Eingabe */
    pit_init(1000);
    ps2_init();
    keyboard_init();
    sti();

    /* Grafik */
    if (!fb_init())
        panic("Es wurde kein nutzbarer Framebuffer gefunden.");
    if (!gfx_init())
        panic("Der Bildpuffer laesst sich nicht anlegen.");

    mouse_init((int32_t)bi->fb_width, (int32_t)bi->fb_height);

    /* Daten und Oberflaeche */
    fs_init();
    gui_init();

    kprintf("Oberflaeche : bereit\n\n");

    gui_run();
}
