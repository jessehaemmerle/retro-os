/* apic.h - Unterbrechungen so, wie moderne Rechner sie zustellen.
 *
 * Der 8259A stammt aus dem Jahr 1976 und kennt 15 Leitungen. Ein
 * heutiger Rechner hat einen lokalen APIC je Kern und mindestens einen
 * IOAPIC im Chipsatz; PCIe-Geraete schreiben ihre Unterbrechung sogar
 * gleich als Speicherzugriff (MSI). RetroOS benutzt den APIC, wenn er
 * da ist, und faellt sonst auf den 8259A zurueck.
 */
#ifndef APIC_H
#define APIC_H

#include "retro.h"

/* Sucht die MADT und richtet lokalen APIC und IOAPIC ein.
 * Gibt false zurueck, wenn der Rechner keinen APIC hat. */
bool apic_init(void);
/* Schaltet den lokalen APIC eines weiteren Kerns ein. */
void apic_init_ap(void);
bool apic_available(void);

/* Kennung des lokalen APIC dieses Kerns. */
uint32_t apic_id(void);

void apic_send_eoi(void);

/* Legt eine Altgeraete-Leitung im IOAPIC auf einen Vektor. */
bool ioapic_route(uint8_t irq, uint8_t vector);
void ioapic_mask(uint8_t irq, bool masked);

/* Der Zeitgeber des lokalen APIC ersetzt den PIT. */
bool apic_timer_start(uint32_t frequency_hz, uint8_t vector);

/* Adresse und Datenwort, die ein Geraet fuer MSI schreiben muss. */
uint64_t apic_msi_address(void);
uint32_t apic_msi_data(uint8_t vector);

/* Wie viele Kerne die MADT auffuehrt - RetroOS benutzt bislang einen. */
uint32_t apic_cpu_count(void);

#endif /* APIC_H */
