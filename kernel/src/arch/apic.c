/* apic.c - lokaler APIC und IOAPIC.
 *
 * Die Tabelle MADT sagt, wo die Bausteine liegen und welche
 * Altgeraete-Leitung auf welchen Eingang des IOAPIC gelegt wurde. Diese
 * Umlegungen sind wichtig: Auf fast jedem Rechner haengt der Systemtakt
 * nicht an Eingang null, sondern an zwei.
 */

#include "apic.h"
#include "acpi.h"
#include "arch.h"
#include "io.h"
#include "kstring.h"

/* --- Register des lokalen APIC, Abstand in Bytes --- */
#define LAPIC_ID          0x020
#define LAPIC_VERSION     0x030
#define LAPIC_TPR         0x080
#define LAPIC_EOI         0x0B0
#define LAPIC_SPURIOUS    0x0F0
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_LVT_LINT0   0x350
#define LAPIC_LVT_LINT1   0x360
#define LAPIC_LVT_ERROR   0x370
#define LAPIC_TIMER_INIT  0x380
#define LAPIC_TIMER_CUR   0x390
#define LAPIC_TIMER_DIV   0x3E0

#define LVT_MASKED        0x10000u
#define TIMER_PERIODIC    0x20000u

#define IA32_APIC_BASE    0x1B

/* --- IOAPIC --- */
#define IOAPIC_REGSEL     0x00
#define IOAPIC_WINDOW     0x10
#define IOAPIC_ID         0x00
#define IOAPIC_VERSION    0x01
#define IOAPIC_REDIR      0x10

#define MAX_IOAPICS       4
#define MAX_OVERRIDES     24

struct ioapic {
    volatile uint32_t *base;
    uint32_t           gsi_base;
    uint32_t           entries;
};

struct override {
    uint8_t  irq;
    uint32_t gsi;
    uint16_t flags;
};

static volatile uint32_t *lapic;
static struct ioapic      ioapics[MAX_IOAPICS];
static uint32_t           ioapic_count;
static struct override    overrides[MAX_OVERRIDES];
static uint32_t           override_count;
static uint32_t           cpu_count;
static bool               ready;

/* ------------------------------------------------------------------ */
/* Zugriff                                                             */
/* ------------------------------------------------------------------ */

static uint32_t lapic_read(uint32_t reg)
{
    return lapic[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t value)
{
    lapic[reg / 4] = value;
    (void)lapic[LAPIC_ID / 4];      /* erzwingt die Schreibreihenfolge */
}

static uint32_t ioapic_read(struct ioapic *io, uint32_t reg)
{
    io->base[IOAPIC_REGSEL / 4] = reg;
    return io->base[IOAPIC_WINDOW / 4];
}

static void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t value)
{
    io->base[IOAPIC_REGSEL / 4] = reg;
    io->base[IOAPIC_WINDOW / 4] = value;
}

static uint64_t read_msr(uint32_t msr)
{
    uint32_t low, high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" ::
                     "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}

static bool cpu_has_apic(void)
{
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    return (edx & (1u << 9)) != 0;
}

/* ------------------------------------------------------------------ */
/* MADT lesen                                                          */
/* ------------------------------------------------------------------ */

struct madt {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    uint32_t lapic_address;
    uint32_t flags;
} PACKED;

static void read_madt(const struct madt *madt)
{
    const uint8_t *p = (const uint8_t *)madt + sizeof(*madt);
    const uint8_t *end = (const uint8_t *)madt + madt->length;

    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t length = p[1];

        if (length < 2 || p + length > end)
            break;

        switch (type) {
        case 0:                     /* Kern mit lokalem APIC */
            if (length >= 8 && (p[4] & 1))
                cpu_count++;
            break;

        case 1:                     /* IOAPIC */
            if (length >= 12 && ioapic_count < MAX_IOAPICS) {
                struct ioapic *io = &ioapics[ioapic_count++];
                uint32_t address = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                                   ((uint32_t)p[6] << 16) |
                                   ((uint32_t)p[7] << 24);

                io->base = phys_to_virt(address);
                io->gsi_base = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                               ((uint32_t)p[10] << 16) |
                               ((uint32_t)p[11] << 24);
                io->entries = ((ioapic_read(io, IOAPIC_VERSION) >> 16) &
                               0xFF) + 1;
            }
            break;

        case 2:                     /* Umlegung einer Altgeraete-Leitung */
            if (length >= 10 && override_count < MAX_OVERRIDES) {
                struct override *o = &overrides[override_count++];

                o->irq = p[3];
                o->gsi = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                         ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
                o->flags = (uint16_t)(p[8] | (p[9] << 8));
            }
            break;

        case 5:                     /* andere Adresse des lokalen APIC */
            if (length >= 12) {
                uint64_t address = 0;

                for (int i = 0; i < 8; i++)
                    address |= (uint64_t)p[4 + i] << (i * 8);
                lapic = phys_to_virt(address);
            }
            break;

        case 9:                     /* Kern mit x2APIC */
            if (length >= 16 && (p[8] & 1))
                cpu_count++;
            break;

        default:
            break;
        }
        p += length;
    }
}

/* Findet den IOAPIC, der fuer diese Systemleitung zustaendig ist. */
static struct ioapic *ioapic_for(uint32_t gsi, uint32_t *index)
{
    for (uint32_t i = 0; i < ioapic_count; i++) {
        struct ioapic *io = &ioapics[i];

        if (gsi >= io->gsi_base && gsi < io->gsi_base + io->entries) {
            *index = gsi - io->gsi_base;
            return io;
        }
    }
    return NULL;
}

/* Rechnet eine Altgeraete-Leitung in die Systemleitung um. */
static uint32_t gsi_for_irq(uint8_t irq, uint16_t *flags)
{
    for (uint32_t i = 0; i < override_count; i++) {
        if (overrides[i].irq != irq)
            continue;
        if (flags)
            *flags = overrides[i].flags;
        return overrides[i].gsi;
    }
    if (flags)
        *flags = 0;
    return irq;
}

/* ------------------------------------------------------------------ */
/* Einrichten                                                          */
/* ------------------------------------------------------------------ */

bool ioapic_route(uint8_t irq, uint8_t vector)
{
    uint16_t flags = 0;
    uint32_t gsi = gsi_for_irq(irq, &flags);
    uint32_t index;
    struct ioapic *io = ioapic_for(gsi, &index);

    if (!io)
        return false;

    /* Bit 13 kehrt die Polaritaet um, Bit 15 macht die Leitung
     * pegelgesteuert - beides steht in den Kennzeichen der MADT. */
    uint32_t low = vector;

    if ((flags & 0x3) == 3)
        low |= 1u << 13;
    if ((flags & 0xC) == 0xC)
        low |= 1u << 15;

    ioapic_write(io, IOAPIC_REDIR + index * 2 + 1, apic_id() << 24);
    ioapic_write(io, IOAPIC_REDIR + index * 2, low);
    return true;
}

void ioapic_mask(uint8_t irq, bool masked)
{
    uint16_t flags = 0;
    uint32_t gsi = gsi_for_irq(irq, &flags);
    uint32_t index;
    struct ioapic *io = ioapic_for(gsi, &index);

    if (!io)
        return;

    uint32_t low = ioapic_read(io, IOAPIC_REDIR + index * 2);

    if (masked)
        low |= LVT_MASKED;
    else
        low &= ~LVT_MASKED;
    ioapic_write(io, IOAPIC_REDIR + index * 2, low);
}

uint32_t apic_id(void)
{
    return ready ? (lapic_read(LAPIC_ID) >> 24) : 0;
}

void apic_send_eoi(void)
{
    if (ready)
        lapic_write(LAPIC_EOI, 0);
}

bool apic_available(void) { return ready; }
uint32_t apic_cpu_count(void) { return cpu_count ? cpu_count : 1; }

uint64_t apic_msi_address(void)
{
    /* Die Adresse traegt die Zielkennung; feste Zustellung an diesen Kern. */
    return 0xFEE00000ull | ((uint64_t)apic_id() << 12);
}

uint32_t apic_msi_data(uint8_t vector)
{
    return vector;              /* flankengesteuert, feste Prioritaet */
}

bool apic_init(void)
{
    ready = false;
    ioapic_count = 0;
    override_count = 0;
    cpu_count = 0;

    if (!cpu_has_apic())
        return false;

    const struct madt *madt = acpi_find_table("APIC");

    if (!madt || madt->length < sizeof(*madt))
        return false;

    lapic = phys_to_virt(madt->lapic_address);
    read_madt(madt);

    if (ioapic_count == 0)
        return false;

    /* Den lokalen APIC ueber sein eigenes MSR einschalten. */
    uint64_t base = read_msr(IA32_APIC_BASE);

    write_msr(IA32_APIC_BASE, base | (1ull << 11));

    /* Alle Prioritaeten zulassen und den Baustein scharf schalten. */
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SPURIOUS, IRQ_VECTOR_SPURIOUS | 0x100);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);

    ready = true;

    /* Alle Eingaenge der IOAPICs erst einmal stilllegen. */
    for (uint32_t i = 0; i < ioapic_count; i++) {
        struct ioapic *io = &ioapics[i];

        for (uint32_t k = 0; k < io->entries; k++) {
            ioapic_write(io, IOAPIC_REDIR + k * 2, LVT_MASKED);
            ioapic_write(io, IOAPIC_REDIR + k * 2 + 1, 0);
        }
    }

    kprintf("APIC        : %u Kern%s, %u IOAPIC mit %u Eingaengen\n",
            (unsigned)apic_cpu_count(), apic_cpu_count() == 1 ? "" : "e",
            (unsigned)ioapic_count, (unsigned)ioapics[0].entries);
    return true;
}

/* ------------------------------------------------------------------ */
/* Zeitgeber des lokalen APIC                                          */
/* ------------------------------------------------------------------ */

/* Der Takt des APIC-Zeitgebers ist nicht festgelegt. Er wird deshalb
 * einmal gegen den PIT ausgezaehlt: Wir lassen ihn frei laufen und
 * sehen nach, wie weit er in einer bekannten Zeitspanne kommt. */
static uint32_t measure_ticks_per_ms(void)
{
    lapic_write(LAPIC_TIMER_DIV, 0x3);        /* Teiler 16 */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);

    /* Kanal 2 des PIT laeuft ohne Unterbrechung - ideal zum Messen. */
    outb(0x61, (uint8_t)((inb(0x61) & ~0x02) | 0x01));
    outb(0x43, 0xB2);                          /* Kanal 2, Modus 0 */

    uint16_t count = 1193182u / 100;           /* zehn Millisekunden */

    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));

    /* Zaehler durch Aus- und Einschalten des Tors starten. */
    uint8_t port = inb(0x61);

    outb(0x61, (uint8_t)(port & ~0x01));
    outb(0x61, (uint8_t)(port | 0x01));

    uint32_t start = lapic_read(LAPIC_TIMER_CUR);

    while (!(inb(0x61) & 0x20))
        __asm__ volatile("pause");

    uint32_t end = lapic_read(LAPIC_TIMER_CUR);

    outb(0x61, (uint8_t)(inb(0x61) & ~0x01));
    lapic_write(LAPIC_TIMER_INIT, 0);

    uint32_t elapsed = start - end;

    return elapsed / 10;                       /* je Millisekunde */
}

bool apic_timer_start(uint32_t frequency_hz, uint8_t vector)
{
    if (!ready || frequency_hz == 0)
        return false;

    uint32_t per_ms = measure_ticks_per_ms();

    if (per_ms == 0)
        return false;

    uint32_t initial = (per_ms * 1000u) / frequency_hz;

    if (initial == 0)
        initial = 1;

    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, vector | TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, initial);

    kprintf("APIC-Timer  : %u Hz, %u Takte je Millisekunde\n",
            (unsigned)frequency_hz, (unsigned)per_ms);
    return true;
}
