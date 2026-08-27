/* e1000e.c - Intel-Netzwerkkarten der neueren Reihen.
 *
 * Vom 82574L ueber die Chipsatz-Karten ICH8 bis ICH10 bis zu I217, I218
 * und I219: In fast jedem Notebook der letzten fuenfzehn Jahre steckt
 * eine davon. Der Deskriptoraufbau ist derselbe wie beim alten 8254x,
 * deshalb benutzt dieser Treiber dessen Maschinerie mit.
 *
 * Der Unterschied liegt woanders: Die neueren Karten starten ihre
 * Warteschlangen nicht mehr von selbst, sondern erst, wenn man das
 * Freigabebit in RXDCTL und TXDCTL setzt. Und wo der alte Chip seine
 * MAC-Adresse beim Einschalten in den Adressfilter legt, muss man sie
 * bei manchen aus dem Flash-Baustein nachlesen.
 */

#include "net.h"
#include "nic.h"
#include "kstring.h"
#include "pci.h"

bool e1000_bring_up(const struct pci_device *dev, struct nic *nic,
                    bool queue_enable, const char *name);

/* Die Kennungen sind ueber viele Jahre gewachsen. Statt jede einzeln
 * aufzuzaehlen, decken Bereiche ganze Baureihen ab; die bekanntesten
 * bekommen ihren richtigen Namen. */
static const struct {
    uint16_t    id;
    const char *name;
} named[] = {
    { 0x10D3, "Intel 82574L" },
    { 0x10F5, "Intel 82567LM" },
    { 0x10EA, "Intel 82577LM" },
    { 0x10EB, "Intel 82577LC" },
    { 0x1502, "Intel 82579LM" },
    { 0x1503, "Intel 82579V" },
    { 0x153A, "Intel I217-LM" },
    { 0x153B, "Intel I217-V" },
    { 0x155A, "Intel I218-LM" },
    { 0x1559, "Intel I218-V" },
    { 0x15A0, "Intel I218-LM" },
    { 0x15A1, "Intel I218-V" },
    { 0x15A2, "Intel I218-LM" },
    { 0x15A3, "Intel I218-V" },
    { 0x156F, "Intel I219-LM" },
    { 0x1570, "Intel I219-V" },
    { 0x15B7, "Intel I219-LM" },
    { 0x15B8, "Intel I219-V" },
    { 0x15B9, "Intel I219-LM" },
    { 0x15BB, "Intel I219-LM" },
    { 0x15BD, "Intel I219-LM" },
    { 0x15BE, "Intel I219-V" },
    { 0x15D7, "Intel I219-LM" },
    { 0x15D8, "Intel I219-V" },
    { 0x15E3, "Intel I219-LM" },
    { 0x0D4E, "Intel I219-LM" },
    { 0x0D4F, "Intel I219-V" },
    { 0x0D53, "Intel I219-LM" },
    { 0x0D55, "Intel I219-V" },
    { 0x1A1C, "Intel I219-LM" },
    { 0x1A1D, "Intel I219-V" },
    { 0x15F2, "Intel I225-LM" },
    { 0x15F3, "Intel I225-V" },
    { 0x125B, "Intel I226-LM" },
    { 0x125C, "Intel I226-V" },
};

/* Diese Bereiche gehoeren zu den Reihen, die dieser Treiber bedient. */
static bool id_in_family(uint16_t id)
{
    static const struct { uint16_t from, to; } ranges[] = {
        { 0x1049, 0x104D },   /* ICH8 und ICH9        */
        { 0x10BD, 0x10BF },
        { 0x10C0, 0x10C3 },
        { 0x10CC, 0x10CF },
        { 0x10D3, 0x10D3 },   /* 82574L               */
        { 0x10DE, 0x10DF },
        { 0x10E5, 0x10E5 },
        { 0x10EA, 0x10EF },   /* 82577 und 82578      */
        { 0x10F5, 0x10F6 },
        { 0x1501, 0x1503 },   /* 82579                */
        { 0x1525, 0x1525 },
        { 0x153A, 0x153B },   /* I217                 */
        { 0x1559, 0x155A },   /* I218                 */
        { 0x156F, 0x1570 },   /* I219                 */
        { 0x15A0, 0x15A3 },
        { 0x15B7, 0x15BE },
        { 0x15D6, 0x15E3 },
        { 0x15F2, 0x15F3 },   /* I225                 */
        { 0x0D4C, 0x0D55 },   /* I219 in neueren Chipsaetzen */
        { 0x1A1C, 0x1A1F },
        { 0x125B, 0x125C },   /* I226                 */
        { 0x550A, 0x550B },
    };

    for (size_t i = 0; i < ARRAY_LEN(ranges); i++)
        if (id >= ranges[i].from && id <= ranges[i].to)
            return true;
    return false;
}

static bool e1000e_probe(const struct pci_device *pci)
{
    if (pci->vendor_id != 0x8086)
        return false;
    if (id_in_family(pci->device_id))
        return true;

    for (size_t i = 0; i < ARRAY_LEN(named); i++)
        if (named[i].id == pci->device_id)
            return true;

    /* Die Reihe 82575 bis I350 sieht von aussen genauso aus, hat aber
     * einen anderen Aufbau - dafuer gibt es igb.c. */
    if (igb_owns(pci->device_id))
        return false;

    /* Bleibt eine Intel-Ethernetkarte uebrig, die niemand kennt, ist
     * dieser Treiber die bessere Wette als der ganz alte: Seit dem
     * 82571 haben alle das Freigabebit. */
    return pci->device_id >= 0x1049;
}

static bool e1000e_attach(const struct pci_device *pci, struct nic *nic)
{
    const char *name = "Intel-Netzwerkkarte";

    for (size_t i = 0; i < ARRAY_LEN(named); i++)
        if (named[i].id == pci->device_id)
            name = named[i].name;

    return e1000_bring_up(pci, nic, true, name);
}

const struct nic_driver e1000e_driver = {
    .family = "Intel e1000e",
    .probe  = e1000e_probe,
    .attach = e1000e_attach,
};
