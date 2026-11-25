/* ehci.c - der EHCI-Controller (USB 2.0, High Speed).
 *
 * Warum es ihn gibt: VirtualBox bietet mit dem Erweiterungspaket
 * "USB 2.0 (EHCI)" an, und aeltere Chipsaetze haben ihn neben ihren
 * USB-1.1-Controllern. Sticks sind dort High Speed - nur der EHCI spricht
 * mit ihnen.
 *
 * Was er mit allem Langsameren tut: Der EHCI kann nur High Speed. Ein
 * Geraet mit Full oder Low Speed uebergibt er an den "Begleiter", den
 * OHCI (oder UHCI) desselben Chipsatzes, indem er den Anschluss
 * freigibt. Deshalb muss ehci_init() VOR ohci_init() laufen: Erst
 * wenn alle Anschluesse sortiert sind, sieht der OHCI, was ihm gehoert.
 * Gibt es keinen Begleiter, bleiben solche Geraete unerreichbar - das
 * steht dann im Protokoll.
 *
 * Wie er arbeitet. Wie beim OHCI Listen im Speicher, aber dichter:
 *
 *   Die asynchrone Liste - ein Ring aus Warteschlangenkoepfen (QH), je
 *   einer fuer den Steuerkanal und je Massenendpunkt. Ein Kopf traegt
 *   einen Arbeitsbereich, in den der Controller die laufende
 *   Transferbeschreibung (qTD) kopiert; ein Auftrag heisst: qTDs
 *   bauen, dem Arbeitsbereich die erste nennen, fertig. Der Kopf
 *   bleibt dabei im Ring - er wird nur leer.
 *   Die periodische Liste - Unterbrechungsendpunkte der HID-Geraete,
 *   in jedem Rahmen abgefragt.
 *
 * Gewartet wird durch Nachsehen, wie bei den anderen Controllern.
 *
 * Nicht dabei, mit Absicht: Verteiler hinter dem Wurzelanschluss. Ein
 * High-Speed-Verteiler braucht fuer langsame Geraete dahinter gesplittete
 * Uebertragungen, und es liesse sich hier nichts davon pruefen - QEMU
 * hat keinen High-Speed-Verteiler. Gemeldet wird er, nicht abgesucht.
 */

#include "usb_hcd.h"
#include "ehcihw.h"
#include "usbconfig.h"
#include "usbhub.h"
#include "arch.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"
#include "spinlock.h"

#define MAX_HC            2
#define MAX_DEV           16
#define MAX_PORTS         15
#define FRAMES            1024

#define PAGES_FIXED       4            /* Rahmenliste, QH, qTD, Daten */
#define BOUNCE_BYTES      (16 * PAGE_SIZE)
#define BOUNCE_PAGES      (BOUNCE_BYTES / PAGE_SIZE)
#define QH_SLOTS          (PAGE_SIZE / sizeof(struct ehci_qh))
#define QTD_SLOTS         (PAGE_SIZE / sizeof(struct ehci_qtd))

/* Aufbau der Datenseite. */
#define CTL_SETUP_OFF     0
#define CTL_DATA_OFF      64
#define CTL_DATA_MAX      2048
#define REPORT_OFF        (CTL_DATA_OFF + CTL_DATA_MAX)
#define REPORT_SIZE       64
#define REPORT_COUNT      ((PAGE_SIZE - REPORT_OFF) / REPORT_SIZE)

#define BULK_QTDS         4            /* 4 mal 16 KiB = ein Stueck       */
#define BULK_TIMEOUT_MS   5000
#define LIMIT_4G          0x100000000ull

struct ehci_hc {
    bool              ok;
    volatile uint8_t *op;              /* Betriebsregister                */
    uint8_t           ports;
    uint8_t           companions;

    uint64_t          base_phys;
    volatile uint32_t *frames;
    struct ehci_qh   *qhs;
    uint64_t          qhs_phys;
    size_t            qh_used;
    struct ehci_qtd  *qtds;
    uint64_t          qtds_phys;
    size_t            qtd_used;
    uint8_t          *dma;
    uint64_t          dma_phys;
    size_t            reports_used;
    uint8_t          *bounce;
    uint64_t          bounce_phys;

    struct ehci_qh   *head;            /* Kopf der asynchronen Liste      */
    uint64_t          head_phys;
    struct ehci_qh   *ctl_qh;
    uint64_t          ctl_qh_phys;
    struct ehci_qtd  *ctl_qtd[3];
    uint64_t          ctl_qtd_phys[3];
    struct ehci_qtd  *bulk_qtd[BULK_QTDS];
    uint64_t          bulk_qtd_phys[BULK_QTDS];

    uint32_t          periodic_head;   /* erster Unterbrechungskopf       */
    uint8_t           next_address;
    uint8_t           last_error;

    struct spinlock   lock;            /* ein Steuerauftrag zur Zeit      */
    struct spinlock   bulk_lock;       /* ein Massenauftrag zur Zeit      */
};

struct ehci_ep {
    bool              ready;
    uint8_t           number;
    uint16_t          packet;
    struct ehci_qh   *qh;
    uint64_t          qh_phys;
};

struct ehci_dev {
    bool              used;
    struct ehci_hc   *hc;
    uint8_t           address;
    uint16_t          mps0;
    struct usb_config config;

    struct ehci_ep    bulk_in, bulk_out;

    /* Unterbrechungsendpunkt */
    struct ehci_ep    intr;
    uint16_t          int_size;
    struct ehci_qtd  *int_qtd;
    uint64_t          int_qtd_phys;
    uint8_t          *report;
    uint64_t          report_phys;

    struct usb_device_info info;
};

static struct ehci_hc  controllers[MAX_HC];
static size_t          controller_count;
static struct ehci_dev devs[MAX_DEV];
static size_t          dev_count;

/* ------------------------------------------------------------------ */
/* Kleinkram                                                           */
/* ------------------------------------------------------------------ */

static inline uint32_t rd(struct ehci_hc *hc, uint32_t off)
{
    return *(volatile uint32_t *)(hc->op + off);
}

static inline void wr(struct ehci_hc *hc, uint32_t off, uint32_t value)
{
    *(volatile uint32_t *)(hc->op + off) = value;
}

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

static struct ehci_qh *qh_take(struct ehci_hc *hc, uint64_t *phys)
{
    *phys = 0;
    if (hc->qh_used >= QH_SLOTS)
        return NULL;

    size_t at = hc->qh_used++;

    *phys = hc->qhs_phys + at * sizeof(struct ehci_qh);
    return &hc->qhs[at];
}

static struct ehci_qtd *qtd_take(struct ehci_hc *hc, uint64_t *phys)
{
    *phys = 0;
    if (hc->qtd_used >= QTD_SLOTS)
        return NULL;

    size_t at = hc->qtd_used++;

    *phys = hc->qtds_phys + at * sizeof(struct ehci_qtd);
    return &hc->qtds[at];
}

/* Der PORTSC-Wert, mit dem man zurueckschreiben darf: die Aenderungsbits
 * sind ausgeblendet, sonst quittiert man Ereignisse mit. */
static uint32_t port_safe(struct ehci_hc *hc, uint32_t port)
{
    return rd(hc, EHCI_PORTSC(port)) & ~EHCI_PORT_CHANGES;
}

/* Setzt eine qTD zusammen. Der Controller sieht sie erst, wenn sie
 * jemand in einen Kopf haengt - die Reihenfolge der Felder zaehlt
 * hier nicht, nur dass das Aktivieren (Token) zuletzt kommt. */
static void qtd_set(struct ehci_qtd *q, uint8_t pid, bool toggle, bool ioc,
                    uint32_t phys, uint32_t length, uint64_t next_phys)
{
    uint32_t buf[EHCI_QTD_BUFFERS];

    q->next = next_phys ? (uint32_t)next_phys : EHCI_LINK_END;
    q->alt = EHCI_LINK_END;
    ehci_fill_buffers(buf, phys, length);
    for (int i = 0; i < EHCI_QTD_BUFFERS; i++) {
        q->buf[i] = buf[i];
        q->buf_hi[i] = 0;
    }
    barrier();
    q->token = ehci_qtd_token(pid, length, toggle, ioc);
}

/* Gibt dem Kopf einen Auftrag. Der Arbeitsbereich ist leer (der Kopf ist
 * untaetig), der Controller nimmt die erste qTD beim naechsten Besuch
 * auf. keep_toggle: Der Datenwechsel bleibt, wie ihn der Controller
 * hinterlassen hat. */
static void qh_start(struct ehci_qh *qh, uint64_t first_qtd, bool keep_toggle)
{
    uint32_t toggle = keep_toggle ? (qh->ov_token & EHCI_TOKEN_TOGGLE) : 0;

    qh->ov_alt = EHCI_LINK_END;
    qh->ov_token = toggle;
    barrier();
    qh->ov_next = (uint32_t)first_qtd;
    barrier();
}

/* Macht einen Kopf nach Fehler oder Zeitablauf wieder leer. "Inaktiv" (Bit
 * 7 in Wort 1) sagt dem Controller, den Kopf ohne Uebertragung zu
 * ueberspringen, solange wir ihn anfassen. */
static void qh_reset(struct ehci_qh *qh, bool clear_toggle)
{
    uint32_t chars = qh->chars;

    qh->chars = chars | (1u << 7);
    barrier();
    wait_ms(2);                              /* ein Mikrorahmenumlauf */
    qh->ov_next = EHCI_LINK_END;
    qh->ov_alt = EHCI_LINK_END;
    qh->ov_token = clear_toggle ? 0 : (qh->ov_token & EHCI_TOKEN_TOGGLE);
    barrier();
    qh->chars = chars;
    barrier();
}

/* ------------------------------------------------------------------ */
/* Steuerauftraege                                                     */
/* ------------------------------------------------------------------ */

static bool ctl_transfer(struct ehci_hc *hc, uint8_t address, uint16_t mps,
                         const struct usb_setup *setup, void *buffer)
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
        if (in)
            memset(hc->dma + CTL_DATA_OFF, 0, length);
        else if (buffer)
            memcpy(hc->dma + CTL_DATA_OFF, buffer, length);
    }

    struct ehci_qtd *chain[3];
    uint64_t         where[3];
    size_t           n = 0;

    chain[n] = hc->ctl_qtd[0]; where[n++] = hc->ctl_qtd_phys[0];
    if (length) {
        chain[n] = hc->ctl_qtd[1]; where[n++] = hc->ctl_qtd_phys[1];
    }
    chain[n] = hc->ctl_qtd[2]; where[n++] = hc->ctl_qtd_phys[2];

    /* STATUS: gegen die Daten gerichtet, immer DATA1, ohne Nutzdaten - ohne
     * Datenstufe heisst das: eine leere Quittung vom Geraet. */
    qtd_set(chain[n - 1],
            (length && in) ? EHCI_PID_OUT : EHCI_PID_IN, true, true,
            0, 0, 0);

    if (length)
        qtd_set(chain[1], in ? EHCI_PID_IN : EHCI_PID_OUT, true, false,
                data_phys, length, where[2]);

    qtd_set(chain[0], EHCI_PID_SETUP, false, false, setup_phys, 8,
            where[1]);

    /* Den Steuerkopf auf dieses Geraet einstellen - er ist untaetig,
     * wird dabei aber uebersprungen. Der Datenwechsel kommt aus den qTDs
     * (Bit 14). */
    struct ehci_qh *qh = hc->ctl_qh;

    qh->chars = ehci_qh_chars(address, 0, 3, true, false, mps, false) | (1u << 7);
    qh->caps = ehci_qh_caps(0, 0, 0, 0, 1);
    barrier();
    qh_start(qh, where[0], false);
    qh->chars = ehci_qh_chars(address, 0, 3, true, false, mps, false);
    barrier();

    struct ehci_qtd *last = chain[n - 1];
    uint64_t deadline = timer_ms() + 500;
    bool done = false;
    uint32_t failed = 0;

    for (;;) {
        for (size_t t = 0; t < n; t++) {
            uint32_t w = chain[t]->token;

            if (!ehci_token_active(w) && ehci_token_failed(w)) {
                failed = w;
                done = true;
            }
        }
        if (!ehci_token_active(last->token))
            done = true;
        if (done)
            break;
        if (timer_ms() > deadline) {
            failed = EHCI_TOKEN_ERRORS;          /* nichts kam an */
            break;
        }
        __asm__ volatile("pause");
    }

    bool ok = done && !failed && !ehci_token_active(last->token);

    hc->last_error = (uint8_t)(failed & 0xFF);

    if (!ok)
        qh_reset(qh, true);
    else
        qh_reset(qh, true);              /* leer machen, Wechsel egal: der
                                          * naechste Auftrag beginnt neu */

    if (ok && in && length && buffer)
        memcpy(buffer, hc->dma + CTL_DATA_OFF, length);

    spin_unlock(&hc->lock);
    return ok;
}

bool ehci_control(struct usb_device *dev, const struct usb_setup *setup,
                  void *buffer)
{
    struct ehci_dev *d = (struct ehci_dev *)dev;

    if (!d || !d->used || !setup)
        return false;
    return ctl_transfer(d->hc, d->address, d->mps0, setup, buffer);
}

/* ------------------------------------------------------------------ */
/* Unterbrechungsendpunkte                                             */
/* ------------------------------------------------------------------ */

static void int_arm(struct ehci_dev *d)
{
    struct ehci_qtd *q = d->int_qtd;

    qtd_set(q, EHCI_PID_IN, false, false, (uint32_t)d->report_phys,
            d->int_size, 0);

    /* Der Kopf ist untaetig (die letzte qTD ist durch), wir geben ihm
     * die naechste. Der Datenwechsel bleibt im Kopf. */
    qh_start(d->intr.qh, d->int_qtd_phys, true);
}

static bool setup_interrupt(struct ehci_dev *d, uint8_t address,
                            uint16_t packet)
{
    struct ehci_hc *hc = d->hc;

    if (!(address & 0x80) || (address & 0x0F) == 0)
        return false;
    if (hc->reports_used >= REPORT_COUNT)
        return false;

    if (packet == 0 || packet > REPORT_SIZE)
        packet = REPORT_SIZE;

    d->intr.qh = qh_take(hc, &d->intr.qh_phys);
    d->int_qtd = qtd_take(hc, &d->int_qtd_phys);
    if (!d->intr.qh || !d->int_qtd)
        return false;

    size_t slot = hc->reports_used++;

    d->report = hc->dma + REPORT_OFF + slot * REPORT_SIZE;
    d->report_phys = hc->dma_phys + REPORT_OFF + slot * REPORT_SIZE;
    memset(d->report, 0, REPORT_SIZE);
    d->intr.number = address & 0x0F;
    d->int_size = packet;

    struct ehci_qh *qh = d->intr.qh;

    memset(qh, 0, sizeof(*qh));
    qh->chars = ehci_qh_chars(d->address, d->intr.number, 3, false, false,
                              packet, false);
    /* Im ersten Mikrorahmen jedes Rahmens fragen. */
    qh->caps = ehci_qh_caps(0x01, 0, 0, 0, 1);
    qh->ov_next = EHCI_LINK_END;
    qh->ov_alt = EHCI_LINK_END;
    int_arm(d);

    /* Vorn in die Kette einhaengen; alle Rahmenplaetze zeigen dorthin. */
    qh->horiz = hc->periodic_head;
    barrier();
    hc->periodic_head = (uint32_t)d->intr.qh_phys | EHCI_LINK_QH;
    for (int i = 0; i < FRAMES; i++)
        hc->frames[i] = hc->periodic_head;

    d->intr.ready = true;
    return true;
}

size_t ehci_interrupt_poll(struct usb_device *dev, void *buffer, size_t size)
{
    struct ehci_dev *d = (struct ehci_dev *)dev;

    if (!d || !d->used || !d->intr.ready)
        return 0;

    uint32_t w = d->int_qtd->token;

    if (ehci_token_active(w))
        return 0;                              /* noch nichts da */

    size_t got = 0;

    if (!ehci_token_failed(w)) {
        got = ehci_transferred(d->int_size, w);
        if (got > size)
            got = size;
        if (buffer && got)
            memcpy(buffer, d->report, got);
    } else {
        /* Ein Fehler haelt den Kopf an - mit leerem Arbeitsbereich geht es
         * weiter. */
        qh_reset(d->intr.qh, false);
    }

    int_arm(d);
    return got;
}

/* ------------------------------------------------------------------ */
/* Massenendpunkte                                                     */
/* ------------------------------------------------------------------ */

static bool bulk_endpoint_init(struct ehci_dev *d, struct ehci_ep *ep,
                               uint8_t number, uint16_t packet)
{
    struct ehci_hc *hc = d->hc;

    if (!number || !packet)
        return false;

    ep->qh = qh_take(hc, &ep->qh_phys);
    if (!ep->qh)
        return false;

    ep->number = number;
    ep->packet = packet;

    struct ehci_qh *qh = ep->qh;

    memset(qh, 0, sizeof(*qh));
    qh->chars = ehci_qh_chars(d->address, number, 3, false, false, packet,
                              false);
    qh->caps = ehci_qh_caps(0, 0, 0, 0, 1);
    qh->ov_next = EHCI_LINK_END;
    qh->ov_alt = EHCI_LINK_END;

    /* Hinter den Kopf der asynchronen Liste. */
    qh->horiz = hc->head->horiz;
    barrier();
    hc->head->horiz = (uint32_t)ep->qh_phys | EHCI_LINK_QH;
    barrier();

    ep->ready = true;
    return true;
}

bool ehci_setup_bulk_from_config(struct usb_device *dev)
{
    struct ehci_dev *d = (struct ehci_dev *)dev;

    if (!d || !d->used)
        return false;
    if (d->bulk_in.ready && d->bulk_out.ready)
        return true;
    if (!d->config.bulk_in || !d->config.bulk_out)
        return false;

    return bulk_endpoint_init(d, &d->bulk_in, d->config.bulk_in,
                              d->config.bulk_in_packet) &&
           bulk_endpoint_init(d, &d->bulk_out, d->config.bulk_out,
                              d->config.bulk_out_packet);
}

/* Ein Stueck bis 64 KiB; die Daten liegen im Zwischenpuffer. */
static bool bulk_chunk(struct ehci_hc *hc, struct ehci_ep *ep, bool in,
                       uint32_t length, uint32_t *moved)
{
    uint32_t count = (length + EHCI_QTD_MAX - 1) / EHCI_QTD_MAX;
    uint32_t sizes[BULK_QTDS];

    *moved = 0;
    if (count > BULK_QTDS)
        return false;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = i * EHCI_QTD_MAX;

        sizes[i] = MIN(length - off, (uint32_t)EHCI_QTD_MAX);
    }

    for (uint32_t i = count; i > 0; i--) {
        uint32_t k = i - 1;

        qtd_set(hc->bulk_qtd[k], in ? EHCI_PID_IN : EHCI_PID_OUT, false,
                k == count - 1,
                (uint32_t)(hc->bounce_phys + (uint64_t)k * EHCI_QTD_MAX),
                sizes[k],
                k + 1 < count ? hc->bulk_qtd_phys[k + 1] : 0);
    }

    barrier();
    qh_start(ep->qh, hc->bulk_qtd_phys[0], true);

    uint64_t deadline = timer_ms() + BULK_TIMEOUT_MS;
    bool failed = false, stuck = false, finished = false;

    /* Ob der Auftrag fertig ist, sagen allein die qTDs - nicht der
     * Arbeitsbereich im Kopf. Der Controller leert ihn, bevor er das
     * Ergebnis in die qTD zurueckschreibt; wer dort schon hinsieht,
     * liest ein Ergebnis, das noch gar nicht da ist, und bekommt
     * Nullen als Daten. */
    for (;;) {
        uint32_t i;

        for (i = 0; i < count; i++) {
            uint32_t w = hc->bulk_qtd[i]->token;

            if (ehci_token_active(w))
                break;                      /* laeuft noch */
            if (ehci_token_failed(w)) {
                failed = true;
                break;
            }
            if (ehci_token_remaining(w) != 0) {
                finished = true;            /* kurzes Paket: Ende */
                break;
            }
        }
        if (failed || finished)
            break;
        if (i == count) {
            finished = true;                /* alle durch */
            break;
        }
        if (timer_ms() > deadline) {
            stuck = true;
            break;
        }
        __asm__ volatile("pause");
    }

    /* Gezaehlt wird, was die qTDs sagen. */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t w = hc->bulk_qtd[i]->token;

        if (ehci_token_active(w))
            break;
        *moved += ehci_transferred(sizes[i], w);
        if (ehci_token_remaining(w) != 0 || ehci_token_failed(w))
            break;
    }

    bool ok = finished && !failed && !stuck;

    hc->last_error = failed ? 1 : (stuck ? 2 : 0);

    if (!ok)
        qh_reset(ep->qh, false);
    return ok;
}

bool ehci_bulk(struct usb_device *dev, uint8_t endpoint, void *buffer,
               uint32_t length, bool in, uint32_t *transferred)
{
    struct ehci_dev *d = (struct ehci_dev *)dev;

    UNUSED(endpoint);

    if (transferred)
        *transferred = 0;
    if (!d || !d->used)
        return false;

    struct ehci_ep *ep = in ? &d->bulk_in : &d->bulk_out;

    if (!ep->ready)
        return false;
    if (length == 0)
        return true;

    struct ehci_hc *hc = d->hc;
    uint8_t *data = buffer;
    uint32_t total = 0;
    bool ok = true;

    spin_lock(&hc->bulk_lock);

    while (length > 0) {
        uint32_t chunk = MIN(length, (uint32_t)BOUNCE_BYTES);
        uint32_t moved = 0;

        if (!in && data)
            memcpy(hc->bounce, data, chunk);

        bool pass = bulk_chunk(hc, ep, in, chunk, &moved);

        if (in && data && moved)
            memcpy(data, hc->bounce, moved);

        total += moved;
        if (!pass) {
            ok = false;
            break;
        }
        if (moved < chunk)
            break;

        data += chunk;
        length -= chunk;
    }

    spin_unlock(&hc->bulk_lock);

    if (transferred)
        *transferred = total;
    return ok;
}

bool ehci_clear_halt(struct usb_device *dev, uint8_t endpoint)
{
    struct ehci_dev *d = (struct ehci_dev *)dev;

    if (!d || !d->used)
        return false;

    bool in = (endpoint & 0x80) != 0;
    struct ehci_ep *ep = in ? &d->bulk_in : &d->bulk_out;

    if (!ep->ready)
        return false;

    struct usb_setup setup = {
        .request_type = 0x02,
        .request = 0x01,
        .value = 0,
        .index = (uint16_t)(ep->number | (in ? 0x80 : 0)),
        .length = 0,
    };
    bool ok = ctl_transfer(d->hc, d->address, d->mps0, &setup, NULL);

    spin_lock(&d->hc->bulk_lock);
    qh_reset(ep->qh, true);              /* nicht angehalten, DATA0 */
    spin_unlock(&d->hc->bulk_lock);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Geraete aufzaehlen                                                  */
/* ------------------------------------------------------------------ */

static bool get_descriptor(struct ehci_dev *d, uint8_t type, uint8_t index,
                           void *buffer, uint16_t length)
{
    struct usb_setup setup = {
        .request_type = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (uint16_t)((type << 8) | index),
        .index = 0,
        .length = length,
    };

    return ctl_transfer(d->hc, d->address, d->mps0, &setup, buffer);
}

static void enum_fail(struct ehci_hc *hc, uint32_t port, const char *step)
{
    kprintf("USB         : EHCI-Anschluss %u: %s (Status %02x)\n",
            (unsigned)port + 1, step, hc->last_error);
}

/* Der Wurzelanschluss: Was nicht High Speed ist, geht an den Begleiter. */
static void release_port(struct ehci_hc *hc, uint32_t port, const char *why)
{
    wr(hc, EHCI_PORTSC(port), port_safe(hc, port) | EHCI_PORT_OWNER);

    if (hc->companions)
        kprintf("USB         : EHCI-Anschluss %u: %s - an den Begleiter uebergeben\n",
                (unsigned)port + 1, why);
    else
        kprintf("USB         : EHCI-Anschluss %u: %s - es gibt keinen Begleiter\n",
                (unsigned)port + 1, why);
}

static bool reset_port(struct ehci_hc *hc, uint32_t port)
{
    uint32_t v = port_safe(hc, port);

    /* Reset: Freigabe aus, Reset an, fuenfzig Millisekunden, Reset aus. */
    wr(hc, EHCI_PORTSC(port), (v & ~EHCI_PORT_PED) | EHCI_PORT_RESET);
    wait_ms(50);
    wr(hc, EHCI_PORTSC(port), port_safe(hc, port) & ~EHCI_PORT_RESET);

    uint64_t deadline = timer_ms() + 200;

    while (rd(hc, EHCI_PORTSC(port)) & EHCI_PORT_RESET) {
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }
    wait_ms(10);                       /* Erholzeit nach dem Reset */
    return true;
}

static void enumerate_port(struct ehci_hc *hc, uint32_t port)
{
    uint32_t seen = rd(hc, EHCI_PORTSC(port));

    if (!(seen & EHCI_PORT_CCS)) {
        /* Nichts da: hoechstens alte Aenderungsmeldungen quittieren. */
        wr(hc, EHCI_PORTSC(port), seen & ~EHCI_PORT_PED);
        return;
    }

    /* Leitungszustand K: ein Geraet mit niedriger Geschwindigkeit - gar
     * nicht erst zuruecksetzen. */
    if (((seen & EHCI_PORT_LS) >> 10) == EHCI_LINE_K) {
        release_port(hc, port, "langsames Geraet");
        return;
    }

    if (!reset_port(hc, port)) {
        kprintf("USB         : EHCI-Anschluss %u laesst sich nicht zuruecksetzen\n",
                (unsigned)port + 1);
        return;
    }

    if (!(rd(hc, EHCI_PORTSC(port)) & EHCI_PORT_PED)) {
        /* Nach dem Reset nicht freigegeben: kein High Speed. */
        release_port(hc, port, "Full Speed");
        return;
    }

    struct ehci_dev *d = NULL;

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
    d->address = 0;
    d->mps0 = 64;

    uint8_t head[8];

    if (!get_descriptor(d, USB_DESC_DEVICE, 0, head, sizeof(head))) {
        enum_fail(hc, port, "antwortet nicht");
        return;
    }

    uint16_t mps = head[7];

    if (mps != 8 && mps != 16 && mps != 32 && mps != 64)
        mps = 64;
    d->mps0 = mps;

    uint8_t address = (uint8_t)(++hc->next_address);
    struct usb_setup set_address = {
        .request_type = USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = 0x05,
        .value = address,
        .index = 0,
        .length = 0,
    };

    if (!ctl_transfer(hc, 0, d->mps0, &set_address, NULL)) {
        enum_fail(hc, port, "nimmt keine Adresse");
        return;
    }
    wait_ms(3);
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
    d->info.speed = 3;

    uint8_t header[9];

    if (!get_descriptor(d, USB_DESC_CONFIG, 0, header, sizeof(header))) {
        enum_fail(hc, port, "Konfiguration nicht lesbar");
        return;
    }

    uint16_t total = (uint16_t)(header[2] | (header[3] << 8));

    if (total < 9 || total > 512) {
        kprintf("USB         : EHCI-Anschluss %u: Konfiguration ungueltig (%u Byte)\n",
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
        kprintf("USB         : EHCI-Anschluss %u: keine Schnittstelle gefunden\n",
                (unsigned)port + 1);
        return;
    }

    d->config = config;
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

    if (!ctl_transfer(hc, d->address, d->mps0, &select, NULL)) {
        enum_fail(hc, port, "Konfiguration laesst sich nicht waehlen");
        return;
    }

    bool hub = d->info.device_class == USB_CLASS_HUB ||
               d->info.interface_class == USB_CLASS_HUB;

    if (!hub && config.int_address)
        setup_interrupt(d, config.int_address, config.int_packet);

    d->used = true;
    dev_count++;

    kprintf("USB         : EHCI-Anschluss %u, %04x:%04x, Klasse %u.%u.%u, %s\n",
            (unsigned)port + 1, d->info.vendor_id, d->info.product_id,
            d->info.interface_class, d->info.interface_subclass,
            d->info.interface_protocol, usb_speed_name(3));

    if (hub) {
        kprintf("USB         : Verteiler am EHCI werden nicht abgesucht\n");
        return;
    }

    usb_hid_attach((struct usb_device *)d);
    usb_storage_attach((struct usb_device *)d);
}

/* ------------------------------------------------------------------ */
/* Controller einrichten                                               */
/* ------------------------------------------------------------------ */

/* Gehoert der Controller noch dem BIOS, muss es ihn herausgeben - ueber
 * die erweiterte Faehigkeit "USB Legacy Support". */
static void take_from_bios(struct ehci_hc *hc, const struct pci_device *pci,
                           volatile uint8_t *cap)
{
    uint32_t hcc = *(volatile uint32_t *)(cap + EHCI_HCCPARAMS);
    uint32_t at = (hcc >> 8) & 0xFF;

    for (int guard = 0; at >= 0x40 && guard < 16; guard++) {
        uint32_t word = pci_read32(pci, (uint8_t)at);

        if ((word & 0xFF) == 1) {
            if (word & (1u << 16)) {               /* BIOS besitzt ihn */
                pci_write32(pci, (uint8_t)at, word | (1u << 24));

                uint64_t deadline = timer_ms() + 1000;

                while ((pci_read32(pci, (uint8_t)at) & (1u << 16)) &&
                       timer_ms() < deadline)
                    __asm__ volatile("pause");
            }
            /* Systemverwaltungs-Unterbrechungen aus. */
            pci_write32(pci, (uint8_t)(at + 4), 0);
            return;
        }
        at = (word >> 8) & 0xFF;
    }
}

static bool setup_controller(struct ehci_hc *hc, const struct pci_device *pci)
{
    if (pci->bar[0] == 0 || pci->bar_is_io[0])
        return false;

    volatile uint8_t *cap = phys_to_virt(pci->bar[0]);

    pci_enable_bus_master(pci);

    uint8_t caplen = *cap;

    if (caplen < 0x10)
        return false;

    hc->op = cap + caplen;

    uint32_t sparams = *(volatile uint32_t *)(cap + EHCI_HCSPARAMS);

    hc->ports = (uint8_t)(sparams & 0x0F);
    hc->companions = (uint8_t)((sparams >> 12) & 0x0F);
    if (hc->ports == 0 || hc->ports > MAX_PORTS)
        return false;

    take_from_bios(hc, pci, cap);

    /* Anhalten und zuruecksetzen. */
    wr(hc, EHCI_USBCMD, rd(hc, EHCI_USBCMD) & ~EHCI_CMD_RUN);

    uint64_t deadline = timer_ms() + 200;

    while (!(rd(hc, EHCI_USBSTS) & EHCI_STS_HALTED)) {
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }

    wr(hc, EHCI_USBCMD, EHCI_CMD_RESET);
    deadline = timer_ms() + 500;
    while (rd(hc, EHCI_USBCMD) & EHCI_CMD_RESET) {
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }

    /* Speicher: Rahmenliste, Koepfe, qTDs, Daten, Zwischenpuffer -
     * zusammenhaengend und unter 4 GiB. */
    uint64_t base = pmm_alloc_pages_below(PAGES_FIXED + BOUNCE_PAGES, LIMIT_4G);

    if (!base) {
        kprintf("USB         : EHCI braucht Speicher unter 4 GiB\n");
        return false;
    }

    hc->base_phys = base;
    hc->frames = phys_to_virt(base);
    hc->qhs_phys = base + PAGE_SIZE;
    hc->qtds_phys = base + 2 * PAGE_SIZE;
    hc->dma_phys = base + 3 * PAGE_SIZE;
    hc->bounce_phys = base + 4 * PAGE_SIZE;
    hc->qhs = phys_to_virt(hc->qhs_phys);
    hc->qtds = phys_to_virt(hc->qtds_phys);
    hc->dma = phys_to_virt(hc->dma_phys);
    hc->bounce = phys_to_virt(hc->bounce_phys);
    memset(phys_to_virt(base), 0, (size_t)(PAGES_FIXED + BOUNCE_PAGES) * PAGE_SIZE);
    spin_init(&hc->lock, "ehci");
    spin_init(&hc->bulk_lock, "ehci-bulk");

    for (int i = 0; i < FRAMES; i++)
        hc->frames[i] = EHCI_LINK_END;
    hc->periodic_head = EHCI_LINK_END;

    /* Kopf der asynchronen Liste: zeigt auf sich selbst, ist als
     * "Kopf der Rueckgewinnung" markiert und angehalten. An ihm haengen
     * die echten Koepfe. */
    hc->head = qh_take(hc, &hc->head_phys);
    hc->head->horiz = (uint32_t)hc->head_phys | EHCI_LINK_QH;
    hc->head->chars = ehci_qh_chars(0, 0, 3, false, true, 64, false);
    hc->head->caps = ehci_qh_caps(0, 0, 0, 0, 1);
    hc->head->ov_next = EHCI_LINK_END;
    hc->head->ov_alt = EHCI_LINK_END;
    hc->head->ov_token = EHCI_TOKEN_HALTED;

    /* Steuerkopf hinter den Kopf. */
    hc->ctl_qh = qh_take(hc, &hc->ctl_qh_phys);
    hc->ctl_qh->horiz = hc->head->horiz;
    hc->ctl_qh->chars = ehci_qh_chars(0, 0, 3, true, false, 64, false);
    hc->ctl_qh->caps = ehci_qh_caps(0, 0, 0, 0, 1);
    hc->ctl_qh->ov_next = EHCI_LINK_END;
    hc->ctl_qh->ov_alt = EHCI_LINK_END;
    hc->head->horiz = (uint32_t)hc->ctl_qh_phys | EHCI_LINK_QH;

    for (int i = 0; i < 3; i++)
        hc->ctl_qtd[i] = qtd_take(hc, &hc->ctl_qtd_phys[i]);
    for (int i = 0; i < BULK_QTDS; i++)
        hc->bulk_qtd[i] = qtd_take(hc, &hc->bulk_qtd_phys[i]);

    wr(hc, EHCI_CTRLDSSEG, 0);
    wr(hc, EHCI_USBINTR, 0);
    wr(hc, EHCI_PERIODICBASE, (uint32_t)base);
    wr(hc, EHCI_ASYNCLIST, (uint32_t)hc->head_phys);
    wr(hc, EHCI_USBSTS, 0x3Fu);
    wr(hc, EHCI_USBCMD, EHCI_CMD_ITC8 | EHCI_CMD_PSE | EHCI_CMD_ASE |
                        EHCI_CMD_RUN);

    deadline = timer_ms() + 200;
    while (rd(hc, EHCI_USBSTS) & EHCI_STS_HALTED) {
        if (timer_ms() > deadline) {
            kprintf("USB         : EHCI geht nicht in Betrieb\n");
            return false;
        }
        __asm__ volatile("pause");
    }

    /* Alle Anschluesse gehoeren ab jetzt dem EHCI; was nicht High Speed
     * ist, geben wir einzeln weiter. */
    wr(hc, EHCI_CONFIGFLAG, 1);

    for (uint32_t p = 0; p < hc->ports; p++) {
        uint32_t v = rd(hc, EHCI_PORTSC(p));

        if (!(v & EHCI_PORT_POWER))
            wr(hc, EHCI_PORTSC(p), (v & ~EHCI_PORT_CHANGES) | EHCI_PORT_POWER);
    }

    wait_ms(120);                      /* Strom, dann Erkennung */

    hc->ok = true;
    return true;
}

void ehci_init(void)
{
    memset(controllers, 0, sizeof(controllers));
    memset(devs, 0, sizeof(devs));
    controller_count = 0;
    dev_count = 0;

    for (size_t i = 0; i < pci_device_count() && controller_count < MAX_HC; i++) {
        const struct pci_device *pci = pci_device_at(i);

        /* Klasse 12, Unterklasse 3, Schnittstelle 0x20 ist EHCI. */
        if (!pci || pci->class_code != 0x0C || pci->subclass != 0x03)
            continue;
        if (pci->prog_if != 0x20)
            continue;

        struct ehci_hc *hc = &controllers[controller_count++];

        if (!setup_controller(hc, pci)) {
            kprintf("USB         : EHCI %02x:%02x.%u antwortet nicht\n",
                    pci->bus, pci->slot, pci->func);
            continue;
        }

        kprintf("USB         : EHCI mit %u Anschluessen, %u Begleiter\n",
                (unsigned)hc->ports, (unsigned)hc->companions);

        for (uint32_t p = 0; p < hc->ports; p++)
            enumerate_port(hc, p);
    }
}

/* ------------------------------------------------------------------ */
/* Auskunft                                                            */
/* ------------------------------------------------------------------ */

bool ehci_owns(const struct usb_device *dev)
{
    const struct ehci_dev *d = (const struct ehci_dev *)dev;

    return d >= devs && d < devs + MAX_DEV;
}

const struct usb_device_info *ehci_device_details(struct usb_device *dev)
{
    struct ehci_dev *d = (struct ehci_dev *)dev;

    return d ? &d->info : NULL;
}

size_t ehci_device_count(void) { return dev_count; }

struct usb_device *ehci_device_at(size_t index)
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
