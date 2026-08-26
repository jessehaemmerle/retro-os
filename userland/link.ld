/* Speicheraufteilung eines Benutzerprogramms.
 * Geladen wird ab 4 MiB - unterhalb liegt nichts, damit ein Zugriff auf
 * eine Null-Adresse zuverlaessig auffaellt. */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) }
}
