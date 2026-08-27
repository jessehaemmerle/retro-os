/* Speicheraufteilung eines Benutzerprogramms.
 * Geladen wird ab 4 MiB - unterhalb liegt nichts, damit ein Zugriff auf
 * eine Null-Adresse zuverlaessig auffaellt.
 *
 * Code, Nur-Lese-Daten und veraenderliche Daten liegen in getrennten
 * Segmenten. So kann der Lader jedem die Rechte geben, die ihm
 * zustehen: Code wird ausgefuehrt, aber nicht beschrieben, Daten
 * beschrieben, aber nicht ausgefuehrt. Ein einziges Segment mit allen
 * drei Rechten waere bequemer - und genau die Bequemlichkeit, die
 * Angreifer sich wuenschen. */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* lesen und ausfuehren */
    rodata PT_LOAD FLAGS(4);   /* nur lesen            */
    data   PT_LOAD FLAGS(6);   /* lesen und schreiben  */
}

SECTIONS
{
    . = 0x400000;

    .text : { *(.text .text.*) } :text

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    .bss  : { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) }
}
