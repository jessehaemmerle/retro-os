/* ohci.c - der OHCI-Controller (USB 1.1).
 *
 * Warum es ihn gibt: VirtualBox haengt das Zeigegeraet einer neuen VM
 * ab Werk an einen OHCI-Controller. Ohne diesen Treiber blieb der
 * Anschluss leer, und die Maus musste auf PS/2 umgestellt werden. Auch
 * viele aeltere Chipsaetze haben OHCI neben oder statt xHCI.
 *
 * Wie er arbeitet. Die Software baut im Speicher Listen: je Endpunkt
 * eine Beschreibung (ED), daran eine Kette von Transferbeschreibungen
 * (TD). Der Controller geht die Listen im Takt eines Millisekunden-
 * rahmens durch und arbeitet ab, was dort steht. Es gibt zwei Listen,
 * die hier gebraucht werden:
 *
 *   Die Steuerliste - ein einziger Endpunkt, der alle Anfragen an
 *   Endpunkt 0 bedient, eine nach der anderen.
 *   Die periodische Liste - die Unterbrechungsendpunkte der
 *   HID-Geraete. Alle 32 Plaetze der Rahmentabelle zeigen auf dieselbe
 *   Kette, es wird also in jedem Rahmen nachgefragt.
 *
 * Gewartet wird nicht auf Unterbrechungen, sondern durch Nachsehen: Die
 * Anfragen sind kurz, und die HID-Abfrage laeuft ohnehin in einem
 * eigenen Thread im Takt von vier Millisekunden.
 *
 * Alle Adressen, die der Controller sieht, sind 32 Bit breit. Der
 * Speicher dafuer kommt deshalb aus dem Bereich unter 4 GiB - auf einem
 * Rechner mit 16 GiB ist das nicht von selbst so.
 *
 * Nicht dabei, mit Absicht: Massenendpunkte (USB-Speicher) und
 * Verteiler hinter dem Wurzelverteiler. Beides braucht der Zeiger nicht,
 * und beides liesse sich ohne Gegenstueck zum Pruefen nur raten.
 */

#include "usb_hcd.h"
#include "ohcihw.h"
#include "usbconfig.h"
#include "arch.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"
#include "spinlock.h"

#define MAX_HC            4
#define MAX_DEV           16
#define MAX_PORTS         15
#define SLOT              16                /* ED und TD sind je 16 Byte */
#define POOL_SLOTS        (PAGE_SIZE / SLOT)

/* Aufbau der Datenseite. */
#define CTL_SETUP_OFF     0
#define CTL_DATA_OFF      64
#define CTL_DATA_MAX      2048
#define REPORT_OFF        (CTL_DATA_OFF + CTL_DATA_MAX)
#define REPORT_SIZE       64
#define REPORT_COUNT      ((PAGE_SIZE - REPORT_OFF) / REPORT_SIZE)

#define LIMIT_4G          0x100000000ull

struct ohci_ed {
    volatile uint32_t flags;
    volatile uint32_t tail;
    volatile uint32_t head;       /* Bit 0 angehalten, Bit 1 Datenwechsel */
    volatile uint32_t next;
};

struct ohci_td {
    volatile uint32_t flags;
    volatile uint32_t cbp;
    volatile uint32_t next;
    volatile uint32_t be;
};

/* Die Rahmentabelle und was der Controller dort hinterlaesst. */
struct hcca {
    volatile uint32_t interrupt_table[32];
    volatile uint16_t frame_number;
    volatile uint16_t pad;
    volatile uint32_t done_head;
    uint8_t           reserved[120];        /* zusammen 256 Byte */
} PACKED;

struct ohci_hc {
    bool              ok;
    volatile uint8_t *regs;

    struct hcca      *hcca;
    uint64_t          hcca_phys;
    uint8_t          *pool;           /* ED und TD, je 16 Byte           */
    uint64_t          pool_phys;
    uint8_t          *dma;            /* Puffer fuer die Datenstufen     */
    uint64_t          dma_phys;
    size_t            slots_used;
    size_t            reports_used;

    struct ohci_ed   *ctl_ed;
    uint64_t          ctl_ed_phys;
    struct ohci_td   *ctl_td[4];      /* SETUP, DATEN, STATUS, Ende      */
    uint64_t          ctl_td_phys[4];

    uint32_t          periodic_head;
    uint8_t           next_address;
    uint8_t           ports;
    uint8_t           last_cc;        /* wie der letzte Auftrag endete */

    struct spinlock   lock;           /* ein Steuerauftrag zur Zeit      */
};

struct ohci_dev {
    bool              used;
    struct ohci_hc   *hc;
    uint8_t           address;
    uint8_t           speed;
    uint16_t          mps0;           /* Paketgroesse von Endpunkt 0     */

    uint8_t           int_ep;
    uint16_t          int_size;
    struct ohci_ed   *ed;
    uint64_t          ed_phys;
    struct ohci_td   *td[2];
    uint64_t          td_phys[2];
    uint8_t           cur;            /* welche der beiden die echte ist */
    uint8_t          *report;
    uint64_t          report_phys;

    struct usb_device_info info;
};

static struct ohci_hc  controllers[MAX_HC];
static size_t          controller_count;
static struct ohci_dev devs[MAX_DEV];
static size_t          dev_count;

/* ------------------------------------------------------------------ */
/* Kleinkram                                                           */
/* ------------------------------------------------------------------ */

static inline uint32_t rd(struct ohci_hc *hc, uint32_t off)
{
    return *(volatile uint32_t *)(hc->regs + off);
}

static inline void wr(struct ohci_hc *hc, uint32_t off, uint32_t value)
{
    *(volatile uint32_t *)(hc->regs + off) = value;
}

/* Der Compiler darf Schreibzugriffe auf Beschreibungen nicht
 * vertauschen: Die Reihenfolge entscheidet, ob der Controller ein
 * halbfertiges Ding sieht. Auf x86 reicht das, mehr als diese Schranke
 * braucht es nicht. */
static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static void wait_ms(uint32_t ms)
{
    uint64_t end = timer_ms() + ms;

    while (timer_ms() < end)
        __asm__ volatile("pause");
}

/* Zwei Plaetze aus dem Vorrat (16 Byte, auf 16 Byte ausgerichtet). */
static void *pool_take(struct ohci_hc *hc, size_t count, uint64_t *phys)
{
    *phys = 0;
    if (hc->slots_used + count > POOL_SLOTS)
        return NULL;

    size_t at = hc->slots_used;

    hc->slots_used += count;
    *phys = hc->pool_phys + at * SLOT;
    return hc->pool + at * SLOT;
}

/* ------------------------------------------------------------------ */
/* Steuerauftraege                                                     */
/* ------------------------------------------------------------------ */

static void td_set(struct ohci_td *td, uint32_t flags, uint32_t cbp,
                   uint32_t length, uint64_t next_phys)
{
    td->cbp = cbp;
    td->be = ohci_buffer_end(cbp, length);
    td->next = (uint32_t)next_phys;
    barrier();
    td->flags = flags;        /* zuletzt: Jetzt ist die Beschreibung gueltig */
}

/* Ein Auftrag an Endpunkt 0: SETUP, auf Wunsch Daten, STATUS. */
static bool ctl_transfer(struct ohci_hc *hc, uint8_t address, uint8_t speed,
                         uint16_t mps, const struct usb_setup *setup,
                         void *buffer)
{
    uint16_t length = setup->length;
    bool in = (setup->request_type & USB_DIR_IN) != 0;

    if (length > CTL_DATA_MAX)
        return false;

    spin_lock(&hc->lock);

    uint32_t setup_phys = (uint32_t)(hc->dma_phys + CTL_SETUP_OFF);
    uint32_t data_phys = (uint32_t)(hc->dma_phys + CTL_DATA_OFF);

    memcpy(hc->dma + CTL_SETUP_OFF, setup, 8);
    if (length) {
        /* Beim Lesen vorher leeren: Antwortet das Geraet mit weniger,
         * als verlangt war, soll der Rest nicht aus dem letzten
         * Auftrag stammen. */
        if (in)
            memset(hc->dma + CTL_DATA_OFF, 0, length);
        else if (buffer)
            memcpy(hc->dma + CTL_DATA_OFF, buffer, length);
    }

    /* Die Kette: SETUP [DATEN] STATUS ENDE. Das Ende ist eine leere
     * Beschreibung, bei der der Controller haelt. */
    struct ohci_td *chain[3];
    uint64_t        where[3];
    size_t          n = 0;

    chain[n] = hc->ctl_td[0]; where[n++] = hc->ctl_td_phys[0];
    if (length) {
        chain[n] = hc->ctl_td[1]; where[n++] = hc->ctl_td_phys[1];
    }
    chain[n] = hc->ctl_td[2]; where[n++] = hc->ctl_td_phys[2];

    struct ohci_td *tail = hc->ctl_td[3];
    uint64_t tail_phys = hc->ctl_td_phys[3];

    tail->flags = 0;
    tail->cbp = 0;
    tail->be = 0;
    tail->next = 0;

    /* Von hinten nach vorn, damit jede Beschreibung ihren Nachfolger
     * schon kennt, wenn sie gueltig wird. */

    /* STATUS: Richtung gegen die Daten, immer DATA1, ohne Nutzdaten. Ohne
     * Datenstufe heisst das: Der Controller liest eine leere Quittung. */
    td_set(chain[n - 1],
           ohci_td_word0((length && in) ? OHCI_PID_OUT : OHCI_PID_IN,
                         OHCI_TOGGLE_DATA1, false),
           0, 0, tail_phys);

    if (length)
        td_set(chain[1],
               ohci_td_word0(in ? OHCI_PID_IN : OHCI_PID_OUT,
                             OHCI_TOGGLE_DATA1, in),
               data_phys, length, where[2]);

    td_set(chain[0], ohci_td_word0(OHCI_PID_SETUP, OHCI_TOGGLE_DATA0, false),
           setup_phys, 8, where[1]);

    /* Der Endpunkt. Beim Einrichten wird er uebersprungen, damit der
     * Controller nie ein halbes Bild sieht. */
    struct ohci_ed *ed = hc->ctl_ed;
    bool low = (speed == 2);

    ed->flags = ohci_ed_word0(address, 0, OHCI_DIR_FROM_TD, low, true, mps);
    ed->tail = (uint32_t)tail_phys;
    ed->head = (uint32_t)where[0];
    barrier();
    ed->flags = ohci_ed_word0(address, 0, OHCI_DIR_FROM_TD, low, false, mps);
    barrier();

    wr(hc, OHCI_CMDSTATUS, OHCI_CMD_CLF);

    /* Warten, bis der Controller durch ist - oder einen Fehler meldet. */
    struct ohci_td *last = chain[n - 1];
    uint64_t deadline = timer_ms() + 500;
    uint8_t failed = OHCI_CC_OK;
    bool done = false;

    while (!done) {
        for (size_t t = 0; t < n; t++) {
            uint32_t w = chain[t]->flags;

            if (ohci_td_done(w) && ohci_td_cc(w) != OHCI_CC_OK) {
                failed = ohci_td_cc(w);
                done = true;
            }
        }
        if (ohci_td_done(last->flags))
            done = true;
        if (ed->head & 1u)             /* angehalten */
            done = true;
        if (done)
            break;
        if (timer_ms() > deadline) {
            failed = OHCI_CC_NOT_ACCESSED;
            break;
        }
        __asm__ volatile("pause");
    }

    bool ok = done && failed == OHCI_CC_OK && ohci_td_done(last->flags);

    hc->last_cc = ok ? OHCI_CC_OK : (failed != OHCI_CC_OK ? failed
                                                         : OHCI_CC_NOT_ACCESSED);

    /* Aufraeumen: Der Endpunkt wird uebersprungen und leer gemacht. Nach
     * einem Fehler kann der Controller noch mittendrin sein - dann einen
     * Rahmen abwarten. */
    ed->flags |= (1u << 14);
    if (!ok)
        wait_ms(2);
    ed->head = (uint32_t)tail_phys;
    barrier();

    if (ok && in && length && buffer)
        memcpy(buffer, hc->dma + CTL_DATA_OFF, length);

    spin_unlock(&hc->lock);
    return ok;
}

bool ohci_control(struct usb_device *dev, const struct usb_setup *setup,
                  void *buffer)
{
    struct ohci_dev *d = (struct ohci_dev *)dev;

    if (!d || !d->used || !setup)
        return false;
    return ctl_transfer(d->hc, d->address, d->speed, d->mps0, setup, buffer);
}

/* ------------------------------------------------------------------ */
/* Unterbrechungsendpunkte                                             */
/* ------------------------------------------------------------------ */

/* Der Controller legt erledigte Beschreibungen in einer Liste ab und
 * meldet das in HcInterruptStatus. Solange das Bit steht, schreibt er
 * keine neue Liste. Wir brauchen die Liste nicht - wir sehen in den
 * Beschreibungen selbst nach -, muessen den Empfang aber quittieren,
 * sonst gibt der Controller auf. */
static void ack_done(struct ohci_hc *hc)
{
    if (rd(hc, OHCI_INTSTATUS) & OHCI_INT_WDH) {
        hc->hcca->done_head = 0;
        wr(hc, OHCI_INTSTATUS, OHCI_INT_WDH);
    }
}

static bool setup_interrupt(struct ohci_dev *d, uint8_t address,
                            uint16_t packet)
{
    struct ohci_hc *hc = d->hc;

    if (!(address & 0x80) || (address & 0x0F) == 0)
        return false;
    if (hc->reports_used >= REPORT_COUNT)
        return false;

    if (packet == 0 || packet > REPORT_SIZE)
        packet = REPORT_SIZE;

    uint64_t ed_phys, td_phys;
    struct ohci_ed *ed = pool_take(hc, 1, &ed_phys);
    struct ohci_td *td = pool_take(hc, 2, &td_phys);

    if (!ed || !td)
        return false;

    d->ed = ed;
    d->ed_phys = ed_phys;
    d->td[0] = &td[0];
    d->td[1] = &td[1];
    d->td_phys[0] = td_phys;
    d->td_phys[1] = td_phys + SLOT;
    d->cur = 0;

    size_t slot = hc->reports_used++;

    d->report = hc->dma + REPORT_OFF + slot * REPORT_SIZE;
    d->report_phys = hc->dma_phys + REPORT_OFF + slot * REPORT_SIZE;
    memset(d->report, 0, REPORT_SIZE);
    d->int_ep = address & 0x0F;
    d->int_size = packet;

    /* Die erste Beschreibung ist echt, die zweite steht am Ende und
     * haelt den Controller an. */
    td_set(d->td[1], 0, 0, 0, 0);
    td_set(d->td[0], ohci_td_word0(OHCI_PID_IN, OHCI_TOGGLE_CARRY, true),
           (uint32_t)d->report_phys, packet, d->td_phys[1]);

    bool low = (d->speed == 2);

    ed->flags = ohci_ed_word0(d->address, d->int_ep, OHCI_DIR_FROM_TD, low,
                              true, packet);
    ed->tail = (uint32_t)d->td_phys[1];
    ed->head = (uint32_t)d->td_phys[0];
    ed->next = hc->periodic_head;
    barrier();
    ed->flags = ohci_ed_word0(d->address, d->int_ep, OHCI_DIR_FROM_TD, low,
                              false, packet);
    barrier();

    /* In die Kette einhaengen: vorn, und alle 32 Rahmenplaetze zeigen
     * dorthin. Dass mehrere Geraete jedes Mal durchlaufen werden, ist
     * Absicht - es sind wenige, und so fragt jedes in jedem Rahmen. */
    hc->periodic_head = (uint32_t)ed_phys;
    for (int i = 0; i < 32; i++)
        hc->hcca->interrupt_table[i] = hc->periodic_head;
    return true;
}

size_t ohci_interrupt_poll(struct usb_device *dev, void *buffer, size_t size)
{
    struct ohci_dev *d = (struct ohci_dev *)dev;

    if (!d || !d->used || !d->int_ep)
        return 0;

    struct ohci_hc *hc = d->hc;

    ack_done(hc);

    struct ohci_td *old = d->td[d->cur];
    uint32_t w = old->flags;

    if (!ohci_td_done(w))
        return 0;

    uint8_t cc = ohci_td_cc(w);
    size_t got = 0;

    if (cc == OHCI_CC_OK) {
        got = ohci_transferred(old->cbp, (uint32_t)d->report_phys, d->int_size);
        if (got > size)
            got = size;
        if (buffer && got)
            memcpy(buffer, d->report, got);
    } else if (d->ed->head & 1u) {
        /* Ein Fehler haelt den Endpunkt an. Weiterlaufen laesst ihn nur,
         * wer das Bit loescht. */
        d->ed->head &= ~1u;
    }

    /* Nachlegen. Die zweite Beschreibung - die am Ende, bei der der
     * Controller wartet - wird zur echten, und die abgearbeitete rueckt
     * als neues Ende nach. Den Endpunkt selbst fasst nur der Controller
     * an; wir aendern allein das Ende. */
    struct ohci_td *fresh = d->td[d->cur ^ 1];

    td_set(old, 0, 0, 0, 0);
    fresh->cbp = (uint32_t)d->report_phys;
    fresh->be = ohci_buffer_end((uint32_t)d->report_phys, d->int_size);
    fresh->next = (uint32_t)d->td_phys[d->cur];
    barrier();
    fresh->flags = ohci_td_word0(OHCI_PID_IN, OHCI_TOGGLE_CARRY, true);
    barrier();
    d->ed->tail = (uint32_t)d->td_phys[d->cur];
    d->cur ^= 1;

    return got;
}

/* ------------------------------------------------------------------ */
/* Geraete aufzaehlen                                                  */
/* ------------------------------------------------------------------ */

static bool get_descriptor(struct ohci_dev *d, uint8_t type, uint8_t index,
                           void *buffer, uint16_t length)
{
    struct usb_setup setup = {
        .request_type = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (uint16_t)((type << 8) | index),
        .index = 0,
        .length = length,
    };

    return ctl_transfer(d->hc, d->address, d->speed, d->mps0, &setup, buffer);
}

static bool reset_port(struct ohci_hc *hc, uint32_t port)
{
    wr(hc, OHCI_RH_PORT(port), OHCI_PORT_PRS);

    uint64_t deadline = timer_ms() + 200;

    while (!(rd(hc, OHCI_RH_PORT(port)) & OHCI_PORT_PRSC)) {
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }

    wr(hc, OHCI_RH_PORT(port), OHCI_PORT_PRSC);     /* Meldung loeschen */
    wait_ms(10);                                    /* Erholzeit        */

    uint32_t status = rd(hc, OHCI_RH_PORT(port));

    if (!(status & OHCI_PORT_PES)) {
        /* Nach dem Reset ist der Anschluss freigegeben; wenn nicht,
         * hilft ein ausdrueckliches Freigeben. */
        wr(hc, OHCI_RH_PORT(port), OHCI_PORT_PES);
        wait_ms(5);
        status = rd(hc, OHCI_RH_PORT(port));
    }
    return (status & OHCI_PORT_PES) != 0;
}

/* Ein Abbruch bei der Aufzaehlung wird gemeldet - mit dem Schritt und
 * dem Ergebnis des Controllers. Frueher endeten mehrere dieser Wege
 * stumm, und ein Geraet, das einfach nicht auftauchte, liess sich von
 * aussen nicht von einem leeren Anschluss unterscheiden. */
static void enum_fail(struct ohci_hc *hc, uint32_t port, const char *step)
{
    kprintf("USB         : OHCI-Anschluss %u: %s (%s)\n", (unsigned)port + 1,
            step, ohci_cc_name(hc->last_cc));
}

static void enumerate_port(struct ohci_hc *hc, uint32_t port)
{
    uint32_t seen = rd(hc, OHCI_RH_PORT(port));
    bool connected = ohci_port_connected(seen);

    /* Die Firmware (SeaBIOS) hat manche Anschluesse schon zurueckgesetzt
     * und dabei das Bit "angeschlossen" verloren, obwohl das Geraet
     * dasteht - nur die Aenderungsbits verraten, dass hier etwas war.
     * Ein Reset entscheidet: Wird der Anschluss danach freigegeben, ist
     * jemand da; ein leerer bleibt gesperrt. */
    if (!connected && !(seen & (OHCI_PORT_CSC | OHCI_PORT_PRSC)))
        return;
    if (!reset_port(hc, port)) {
        if (!connected)
            return;
        kprintf("USB         : OHCI-Anschluss %u laesst sich nicht freigeben\n",
                (unsigned)port + 1);
        return;
    }

    struct ohci_dev *d = NULL;

    for (size_t i = 0; i < MAX_DEV; i++) {
        if (!devs[i].used) {
            d = &devs[i];
            break;
        }
    }
    if (!d || hc->next_address >= 127)
        return;

    memset(d, 0, sizeof(*d));
    d->hc = hc;
    d->speed = ohci_port_speed(rd(hc, OHCI_RH_PORT(port)));
    d->address = 0;
    d->mps0 = 8;

    uint8_t head[8];

    if (!get_descriptor(d, USB_DESC_DEVICE, 0, head, sizeof(head))) {
        enum_fail(hc, port, "antwortet nicht");
        return;
    }

    uint16_t mps = head[7];

    if (mps != 8 && mps != 16 && mps != 32 && mps != 64)
        mps = 8;
    d->mps0 = mps;

    /* Eine Adresse vergeben. Ab jetzt hoert das Geraet auf sie und nicht
     * mehr auf null. */
    uint8_t address = (uint8_t)(++hc->next_address);
    struct usb_setup set_address = {
        .request_type = USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = 0x05,
        .value = address,
        .index = 0,
        .length = 0,
    };

    if (!ctl_transfer(hc, 0, d->speed, d->mps0, &set_address, NULL)) {
        enum_fail(hc, port, "nimmt keine Adresse");
        return;
    }
    wait_ms(3);                       /* das Geraet braucht einen Moment */
    d->address = address;

    uint8_t descriptor[18];

    if (!get_descriptor(d, USB_DESC_DEVICE, 0, descriptor, sizeof(descriptor))) {
        enum_fail(hc, port, "Geraetebeschreibung nicht lesbar");
        return;
    }

    d->info.device_class = descriptor[4];
    d->info.vendor_id = (uint16_t)(descriptor[8] | (descriptor[9] << 8));
    d->info.product_id = (uint16_t)(descriptor[10] | (descriptor[11] << 8));
    d->info.max_packet = d->mps0;
    d->info.port = (uint8_t)(port + 1);
    d->info.speed = d->speed;

    uint8_t header[9];

    if (!get_descriptor(d, USB_DESC_CONFIG, 0, header, sizeof(header))) {
        enum_fail(hc, port, "Konfiguration nicht lesbar");
        return;
    }

    uint16_t total = (uint16_t)(header[2] | (header[3] << 8));

    if (total < 9 || total > 512) {
        kprintf("USB         : OHCI-Anschluss %u: Konfiguration ungueltig (%u Byte)\n",
                (unsigned)port + 1, (unsigned)total);
        return;
    }

    uint8_t full[512];

    if (!get_descriptor(d, USB_DESC_CONFIG, 0, full, total)) {
        enum_fail(hc, port, "Konfiguration nicht vollstaendig lesbar");
        return;
    }

    struct usb_config config;

    if (!usb_parse_config(full, total, &config)) {
        kprintf("USB         : OHCI-Anschluss %u: keine Schnittstelle gefunden\n",
                (unsigned)port + 1);
        return;
    }

    d->info.interface_number = config.interface_number;
    d->info.interface_class = config.interface_class;
    d->info.interface_subclass = config.interface_subclass;
    d->info.interface_protocol = config.interface_protocol;
    d->info.hid_report_length = config.hid_report_length;

    struct usb_setup select = {
        .request_type = USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = USB_REQ_SET_CONFIG,
        .value = config.configuration,
        .index = 0,
        .length = 0,
    };

    if (!ctl_transfer(hc, d->address, d->speed, d->mps0, &select, NULL)) {
        enum_fail(hc, port, "Konfiguration laesst sich nicht waehlen");
        return;
    }

    if (config.int_address)
        setup_interrupt(d, config.int_address, config.int_packet);

    d->used = true;
    dev_count++;

    kprintf("USB         : OHCI-Anschluss %u, %04x:%04x, Klasse %u.%u.%u, %s\n",
            (unsigned)port + 1, d->info.vendor_id, d->info.product_id,
            d->info.interface_class, d->info.interface_subclass,
            d->info.interface_protocol, usb_speed_name(d->speed));

    /* Verteiler am OHCI: nicht unterstuetzt. */
    if (d->info.device_class == 0x09 || d->info.interface_class == 0x09) {
        kprintf("USB         : Verteiler am OHCI werden nicht abgesucht\n");
        return;
    }

    usb_hid_attach((struct usb_device *)d);
    usb_storage_attach((struct usb_device *)d);
}

/* ------------------------------------------------------------------ */
/* Controller einrichten                                               */
/* ------------------------------------------------------------------ */

static bool setup_controller(struct ohci_hc *hc, const struct pci_device *pci)
{
    if (pci->bar[0] == 0 || pci->bar_is_io[0])
        return false;

    hc->regs = phys_to_virt(pci->bar[0]);
    pci_enable_bus_master(pci);

    /* Revision 1.0 ist die einzige, die es gibt. */
    if ((rd(hc, OHCI_REVISION) & 0xFF) != 0x10)
        return false;

    /* Speicher fuer Rahmentabelle, Beschreibungen und Puffer: drei
     * Seiten, zusammenhaengend, alle unter 4 GiB. */
    uint64_t base = pmm_alloc_pages_below(3, LIMIT_4G);

    if (!base) {
        kprintf("USB         : OHCI braucht Speicher unter 4 GiB\n");
        return false;
    }

    hc->hcca_phys = base;
    hc->pool_phys = base + PAGE_SIZE;
    hc->dma_phys = base + 2 * PAGE_SIZE;
    hc->hcca = phys_to_virt(hc->hcca_phys);
    hc->pool = phys_to_virt(hc->pool_phys);
    hc->dma = phys_to_virt(hc->dma_phys);
    memset(hc->hcca, 0, PAGE_SIZE);
    memset(hc->pool, 0, PAGE_SIZE);
    memset(hc->dma, 0, PAGE_SIZE);
    spin_init(&hc->lock, "ohci");

    /* Der Endpunkt der Steuerliste und die Beschreibungen, die er
     * braucht. Er wird uebersprungen, bis ein Auftrag da ist. */
    uint64_t phys;

    hc->ctl_ed = pool_take(hc, 1, &phys);
    hc->ctl_ed_phys = phys;

    for (int i = 0; i < 4; i++) {
        hc->ctl_td[i] = pool_take(hc, 1, &hc->ctl_td_phys[i]);
        if (!hc->ctl_td[i])
            return false;
    }

    hc->ctl_ed->flags = ohci_ed_word0(0, 0, OHCI_DIR_FROM_TD, false, true, 8);
    hc->ctl_ed->tail = (uint32_t)hc->ctl_td_phys[3];
    hc->ctl_ed->head = (uint32_t)hc->ctl_td_phys[3];
    hc->ctl_ed->next = 0;

    /* Gehoert der Controller noch dem BIOS (System-Verwaltungsmodus),
     * muss er ihn herausgeben - sonst lassen sich seine Register nicht
     * verlaesslich benutzen. */
    if (rd(hc, OHCI_CONTROL) & OHCI_CTRL_IR) {
        wr(hc, OHCI_CMDSTATUS, OHCI_CMD_OCR);

        uint64_t deadline = timer_ms() + 500;

        while ((rd(hc, OHCI_CONTROL) & OHCI_CTRL_IR) && timer_ms() < deadline)
            __asm__ volatile("pause");
    }

    uint32_t saved_interval = rd(hc, OHCI_FMINTERVAL);

    /* Hat die Firmware den Controller schon in Betrieb genommen
     * (SeaBIOS tut es fuer die Tastatur), stehen die Anschluesse in
     * einem Zwischenzustand: zurueckgesetzt, aber ohne "angeschlossen"
     * - ein Tablett daneben bliebe unsichtbar. Ein ausdrueckliches
     * Zuruecksetzen des ganzen Busses holt alle Geraete neu herein. */
    if (rd(hc, OHCI_CONTROL) & OHCI_CTRL_HCFS_MASK) {
        wr(hc, OHCI_CONTROL, 0);
        wait_ms(50);
    }

    wr(hc, OHCI_INTDISABLE, 0xFFFFFFFFu);

    /* Zuruecksetzen. Danach hat der Controller zwei Millisekunden Zeit
     * bis zum Betrieb - alles Folgende muss zuegig geschehen. */
    wr(hc, OHCI_CMDSTATUS, OHCI_CMD_HCR);

    uint64_t deadline = timer_ms() + 30;

    while (rd(hc, OHCI_CMDSTATUS) & OHCI_CMD_HCR) {
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }

    /* Die Rahmenlaenge, wie sie vorher war - das Umschaltbit aber muss
     * gegenueber dem Stand wechseln, den der Controller jetzt hat, sonst
     * uebernimmt er den Wert nicht. */
    uint32_t interval = ohci_frame_interval(
        (saved_interval & 0x3FFFu) |
        (rd(hc, OHCI_FMINTERVAL) & (1u << 31)));

    wr(hc, OHCI_HCCA, (uint32_t)hc->hcca_phys);
    wr(hc, OHCI_CONTROL_HEAD, (uint32_t)hc->ctl_ed_phys);
    wr(hc, OHCI_BULK_HEAD, 0);
    wr(hc, OHCI_FMINTERVAL, interval);
    wr(hc, OHCI_PERIODIC_START, ohci_periodic_start(interval));
    wr(hc, OHCI_LS_THRESHOLD, 0x628);
    wr(hc, OHCI_INTSTATUS, 0xFFFFFFFFu);
    wr(hc, OHCI_CONTROL, OHCI_CTRL_CBSR_4_1 | OHCI_CTRL_PLE |
                         OHCI_CTRL_CLE | OHCI_CTRL_HCFS_OPER);

    if ((rd(hc, OHCI_CONTROL) & OHCI_CTRL_HCFS_MASK) != OHCI_CTRL_HCFS_OPER) {
        kprintf("USB         : OHCI geht nicht in Betrieb\n");
        return false;
    }

    /* Der Wurzelverteiler: Strom ein und warten, bis die Geraete bereit
     * sind. */
    uint32_t desca = rd(hc, OHCI_RH_DESCA);

    hc->ports = (uint8_t)(desca & 0xFF);
    if (hc->ports > MAX_PORTS)
        hc->ports = MAX_PORTS;

    wr(hc, OHCI_RH_STATUS, OHCI_RHS_LPSC);
    for (uint32_t p = 0; p < hc->ports; p++)
        wr(hc, OHCI_RH_PORT(p), OHCI_PORT_PPS);

    uint32_t power_good = ((desca >> 24) & 0xFF) * 2;

    wait_ms(power_good > 20 ? power_good : 20);
    wait_ms(100);                     /* so lange braucht ein Geraet */

    hc->ok = true;
    return true;
}

void ohci_init(void)
{
    memset(controllers, 0, sizeof(controllers));
    memset(devs, 0, sizeof(devs));
    controller_count = 0;
    dev_count = 0;

    for (size_t i = 0; i < pci_device_count() && controller_count < MAX_HC; i++) {
        const struct pci_device *pci = pci_device_at(i);

        /* Klasse 12, Unterklasse 3, Schnittstelle 0x10 ist OHCI. */
        if (!pci || pci->class_code != 0x0C || pci->subclass != 0x03)
            continue;
        if (pci->prog_if != 0x10)
            continue;

        struct ohci_hc *hc = &controllers[controller_count++];

        if (!setup_controller(hc, pci)) {
            kprintf("USB         : OHCI %02x:%02x.%u antwortet nicht\n",
                    pci->bus, pci->slot, pci->func);
            continue;
        }

        kprintf("USB         : OHCI mit %u Anschluessen\n", (unsigned)hc->ports);

        for (uint32_t p = 0; p < hc->ports; p++)
            enumerate_port(hc, p);
    }
}

/* ------------------------------------------------------------------ */
/* Auskunft                                                            */
/* ------------------------------------------------------------------ */

bool ohci_owns(const struct usb_device *dev)
{
    const struct ohci_dev *d = (const struct ohci_dev *)dev;

    return d >= devs && d < devs + MAX_DEV;
}

const struct usb_device_info *ohci_device_details(struct usb_device *dev)
{
    struct ohci_dev *d = (struct ohci_dev *)dev;

    return d ? &d->info : NULL;
}

size_t ohci_device_count(void) { return dev_count; }

struct usb_device *ohci_device_at(size_t index)
{
    size_t seen = 0;

    for (size_t i = 0; i < MAX_DEV; i++) {
        if (!devs[i].used)
            continue;
        if (seen == index)
            return (struct usb_device *)&devs[i];
        seen++;
    }
    return NULL;
}
