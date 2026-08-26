/* acpi.c - so viel ACPI, wie zum Abschalten noetig ist.
 *
 * Die Firmware legt eine Kette von Tabellen ab: der RSDP nennt den XSDT,
 * dieser listet alle weiteren auf. Interessant ist die FADT ("FACP"): sie
 * enthaelt die Steuerregister der Energieverwaltung und den Zeiger auf die
 * DSDT.
 *
 * In der DSDT steht das Abschalten als kleines Programm in der Sprache
 * AML. Einen vollstaendigen AML-Interpreter zu schreiben waere ein
 * eigenes Projekt; fuer das Abschalten genuegt es, den Eintrag "_S5_" zu
 * suchen und die beiden Zahlen dahinter zu lesen. Genau diese Zahlen
 * gehoeren in die Register PM1a und PM1b, zusammen mit dem Bit, das den
 * Uebergang ausloest.
 */

#include "acpi.h"
#include "boot.h"
#include "io.h"
#include "kstring.h"

struct sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} PACKED;

struct rsdp_v1 {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
} PACKED;

struct rsdp_v2 {
    struct rsdp_v1 v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} PACKED;

struct generic_address {
    uint8_t  space;
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} PACKED;

struct fadt {
    struct sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved0;
    uint8_t  preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t  pm1_event_length;
    uint8_t  pm1_control_length;
    uint8_t  pm2_control_length;
    uint8_t  pm_timer_length;
    uint8_t  gpe0_length;
    uint8_t  gpe1_length;
    uint8_t  gpe1_base;
    uint8_t  cstate_control;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alarm;
    uint8_t  month_alarm;
    uint8_t  century;
    uint16_t boot_architecture_flags;
    uint8_t  reserved1;
    uint32_t flags;
    struct generic_address reset_register;
    uint8_t  reset_value;
    uint8_t  reserved2[3];
    uint64_t x_firmware_control;
    uint64_t x_dsdt;
} PACKED;

static struct acpi_info info;

/* Wurzel der Tabellenliste, damit spaetere Abfragen sie wiederfinden. */
static uint64_t table_root;
static bool     table_use_xsdt;

/* Je nach Fassung des Bootloader-Protokolls kommt die Adresse des RSDP
 * physisch oder bereits als Zeiger in der direkten Abbildung. Beides wird
 * hier auf dieselbe Form gebracht. */
static const void *as_virtual(uint64_t address)
{
    if (address >= g_hhdm_offset)
        return (const void *)address;
    return phys_to_virt(address);
}

static bool checksum_ok(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint8_t sum = 0;

    for (size_t i = 0; i < length; i++)
        sum = (uint8_t)(sum + bytes[i]);
    return sum == 0;
}

static const struct sdt_header *find_table(uint64_t root, bool use_xsdt,
                                           const char *signature)
{
    const struct sdt_header *header = as_virtual(root);

    if (!root || header->length < sizeof(*header) || header->length > 0x100000)
        return NULL;

    size_t entry_size = use_xsdt ? 8 : 4;
    size_t count = (header->length - sizeof(*header)) / entry_size;
    const uint8_t *entries = (const uint8_t *)(header + 1);

    for (size_t i = 0; i < count; i++) {
        uint64_t address = 0;

        /* Nicht ausgerichtet lesen - die Tabellen sind gepackt. */
        memcpy(&address, entries + i * entry_size, entry_size);
        if (!address)
            continue;

        const struct sdt_header *table = as_virtual(address);
        if (memcmp(table->signature, signature, 4) == 0)
            return table;
    }
    return NULL;
}

/* Sucht "_S5_" in der DSDT und liest die beiden Werte dahinter. */
static bool find_s5(const struct sdt_header *dsdt)
{
    const uint8_t *aml = (const uint8_t *)(dsdt + 1);
    size_t length = dsdt->length - sizeof(*dsdt);

    for (size_t i = 0; i + 8 < length; i++) {
        if (memcmp(aml + i, "_S5_", 4) != 0)
            continue;

        const uint8_t *p = aml + i + 4;

        /* Es folgt die Paket-Anweisung (0x12), davor evtl. ein Namensbyte. */
        if (*p == 0x12)
            p++;
        else if (p[0] == 0x08 && p[1] == 0x12)
            p += 2;
        else
            continue;

        /* Paketlaenge: die oberen zwei Bit sagen, wie viele Folgebytes. */
        uint8_t lead = *p++;
        p += (lead >> 6);
        p++;                       /* Anzahl der Elemente             */

        uint8_t values[2] = { 0, 0 };

        for (int k = 0; k < 2; k++) {
            if (*p == 0x0A)        /* Byte-Konstante                  */
                values[k] = *(++p);
            else if (*p == 0x00)   /* Null                            */
                values[k] = 0;
            else if (*p == 0x01)   /* Eins                            */
                values[k] = 1;
            else
                values[k] = *p;
            p++;
        }

        info.slp_typ_a = values[0];
        info.slp_typ_b = values[1];
        return true;
    }
    return false;
}

void acpi_init(void)
{
    const struct boot_info *bi = boot_info();

    if (!bi->rsdp) {
        kprintf("ACPI        : keine Tabellen gefunden\n");
        return;
    }

    const struct rsdp_v2 *rsdp = as_virtual(bi->rsdp);

    if (!rsdp || memcmp(rsdp->v1.signature, "RSD PTR ", 8) != 0) {
        kprintf("ACPI        : RSDP nicht lesbar\n");
        return;
    }
    if (!checksum_ok(&rsdp->v1, sizeof(rsdp->v1)))
        return;

    uint64_t root;
    bool use_xsdt = rsdp->v1.revision >= 2 && rsdp->xsdt_address != 0;

    root = use_xsdt ? rsdp->xsdt_address : rsdp->v1.rsdt_address;
    table_root = root;
    table_use_xsdt = use_xsdt;

    const struct fadt *fadt = (const struct fadt *)find_table(root, use_xsdt,
                                                              "FACP");
    if (!fadt) {
        kprintf("ACPI        : keine FADT gefunden\n");
        return;
    }

    /* Ab Fassung 2 sind die Boot-Kennzeichen verbindlich. */
    if (fadt->header.revision >= 2 ||
        fadt->header.length >= sizeof(struct fadt)) {
        info.boot_flags = fadt->boot_architecture_flags;
        info.boot_flags_valid = true;
    }

    info.pm1a_control = fadt->pm1a_control_block;
    info.pm1b_control = fadt->pm1b_control_block;
    info.smi_command  = fadt->smi_command;
    info.acpi_enable  = fadt->acpi_enable;

    if (fadt->flags & (1u << 10)) {     /* RESET_REG unterstuetzt */
        info.reset_register = fadt->reset_register.address;
        info.reset_space    = fadt->reset_register.space;
        info.reset_value    = fadt->reset_value;
        info.can_reset      = info.reset_register != 0;
    }

    uint64_t dsdt_address = fadt->x_dsdt ? fadt->x_dsdt : fadt->dsdt;
    const struct sdt_header *dsdt = dsdt_address ? as_virtual(dsdt_address)
                                                 : NULL;

    if (dsdt && memcmp(dsdt->signature, "DSDT", 4) == 0 && find_s5(dsdt))
        info.can_shutdown = info.pm1a_control != 0;

    info.available = true;

    kprintf("ACPI        : PM1a 0x%x, S5 = %u/%u%s\n",
            (unsigned)info.pm1a_control, info.slp_typ_a, info.slp_typ_b,
            info.can_shutdown ? "" : " (Abschalten nicht moeglich)");
}

const struct acpi_info *acpi_get(void)
{
    return &info;
}

bool acpi_enable_mode(void)
{
    if (!info.available || !info.pm1a_control)
        return false;

    /* Ist SCI_EN schon gesetzt, laeuft ACPI bereits. */
    if (inw((uint16_t)info.pm1a_control) & 1)
        return true;
    if (!info.smi_command || !info.acpi_enable)
        return false;

    outb((uint16_t)info.smi_command, info.acpi_enable);

    for (int i = 0; i < 300; i++) {
        if (inw((uint16_t)info.pm1a_control) & 1)
            return true;
        io_wait();
    }
    return false;
}

bool acpi_shutdown(void)
{
    if (!info.can_shutdown)
        return false;

    acpi_enable_mode();

    /* SLP_TYP in Bit 10-12, SLP_EN in Bit 13. */
    outw((uint16_t)info.pm1a_control,
         (uint16_t)((info.slp_typ_a << 10) | (1 << 13)));

    if (info.pm1b_control)
        outw((uint16_t)info.pm1b_control,
             (uint16_t)((info.slp_typ_b << 10) | (1 << 13)));

    /* Wenn es geklappt hat, kommen wir hier nicht mehr an. */
    for (int i = 0; i < 100000; i++)
        io_wait();

    return false;
}

bool acpi_reset(void)
{
    if (!info.can_reset)
        return false;

    switch (info.reset_space) {
    case 0:    /* Speicher */
        *(volatile uint8_t *)phys_to_virt(info.reset_register) = info.reset_value;
        break;
    case 1:    /* I/O-Port */
        outb((uint16_t)info.reset_register, info.reset_value);
        break;
    default:
        return false;
    }

    for (int i = 0; i < 100000; i++)
        io_wait();

    return false;
}

/* Sucht eine Tabelle anhand ihrer vierstelligen Kennung, etwa "APIC"
 * fuer die MADT oder "HPET" fuer den Hochleistungszeitgeber. */
const void *acpi_find_table(const char *signature)
{
    if (!table_root || !signature)
        return NULL;
    return find_table(table_root, table_use_xsdt, signature);
}

bool acpi_has_ps2(void)
{
    if (!info.boot_flags_valid)
        return true;            /* keine Angabe - dann eben probieren */
    return (info.boot_flags & ACPI_BOOT_8042) != 0;
}
