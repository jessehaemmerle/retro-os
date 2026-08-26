/* xhci.c - der USB-Controller moderner Rechner.
 *
 * xHCI arbeitet mit Ringen im Arbeitsspeicher. Jeder Ring besteht aus
 * Bausteinen zu 16 Byte (TRB); der letzte Eintrag zeigt mit einem
 * Verbindungsbaustein zurueck auf den Anfang. Damit Controller und Kern
 * unterscheiden koennen, welche Eintraege neu sind, traegt jeder
 * Baustein ein Umlaufbit, das sich bei jeder Runde umdreht.
 *
 * Drei Sorten Ringe kommen vor:
 *   Befehlsring    - der Kern schickt Verwaltungsbefehle
 *   Ereignisring   - der Controller meldet, was geschehen ist
 *   Uebertragung   - je Endpunkt eines Geraets
 *
 * Angestossen wird alles ueber Tuerklingeln: ein Schreibzugriff sagt
 * dem Controller, dass in einem Ring etwas Neues liegt.
 */

#include "usb.h"
#include "arch.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"
#include "thread.h"

/* Die Aufzaehlung laeuft vor dem Scheduler - gewartet wird deshalb mit
 * timer_sleep(), das nur auf den Systemtakt horcht. */

/* --- Faehigkeitsregister --- */
#define CAP_LENGTH      0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* --- Betriebsregister --- */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTS        0x400

#define CMD_RUN         (1u << 0)
#define CMD_RESET       (1u << 1)
#define CMD_INTE        (1u << 2)
#define CMD_HSEE        (1u << 3)

#define STS_HALTED      (1u << 0)
#define STS_EVENT_INT   (1u << 3)
#define STS_PORT_CHANGE (1u << 4)
#define STS_CNR         (1u << 11)

#define PORT_CCS        (1u << 0)   /* Geraet angeschlossen */
#define PORT_PED        (1u << 1)   /* Anschluss freigegeben */
#define PORT_RESET      (1u << 4)
#define PORT_POWER      (1u << 9)
#define PORT_CSC        (1u << 17)
#define PORT_PRC        (1u << 21)

/* --- Laufzeitregister --- */
#define RT_IMAN         0x20
#define RT_IMOD         0x24
#define RT_ERSTSZ       0x28
#define RT_ERSTBA       0x30
#define RT_ERDP         0x38

/* --- Bausteinarten --- */
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIG_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT 13
#define TRB_NOOP_COMMAND    23
#define TRB_TRANSFER_EVENT  32
#define TRB_COMMAND_EVENT   33
#define TRB_PORT_EVENT      34

#define RING_SIZE       64          /* Bausteine je Ring */
#define MAX_DEVICES     8
#define MAX_PORTS       32

struct trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} PACKED;

struct ring {
    struct trb *trb;
    uint64_t    phys;
    uint32_t    index;
    uint8_t     cycle;
};

/* Ein Geraet und sein Steuer- bzw. Unterbrechungsendpunkt. */
struct usb_device {
    bool     used;
    uint8_t  slot;
    uint8_t  port;
    uint8_t  speed;

    struct ring control_ring;
    struct ring interrupt_ring;

    uint64_t input_phys;
    uint8_t *input_context;
    uint64_t device_phys;
    uint8_t *device_context;

    uint8_t  interrupt_endpoint;    /* Endpunktnummer, 0 = keiner */
    uint16_t interrupt_size;
    uint8_t *report;
    uint64_t report_phys;
    bool     report_pending;

    struct usb_device_info info;
};

static volatile uint8_t  *cap_base;
static volatile uint8_t  *op_base;
static volatile uint32_t *doorbells;
static volatile uint8_t  *runtime;

static uint32_t context_size = 32;
static uint32_t max_slots;
static uint32_t max_ports;

static uint64_t *dcbaa;
static uint64_t  dcbaa_phys;

static struct ring command_ring;
static struct ring event_ring;
static uint64_t    erst_phys;

static struct usb_device devices[MAX_DEVICES];
static size_t            device_count;

/* Das Ergebnis des zuletzt abgeschickten Befehls. */
static volatile bool     command_done;
static volatile uint8_t  command_code;
static volatile uint8_t  command_slot;

/* ------------------------------------------------------------------ */
/* Register                                                            */
/* ------------------------------------------------------------------ */

static uint32_t op_read(uint32_t offset)
{
    return *(volatile uint32_t *)(op_base + offset);
}

static void op_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(op_base + offset) = value;
}

static void op_write64(uint32_t offset, uint64_t value)
{
    *(volatile uint32_t *)(op_base + offset) = (uint32_t)value;
    *(volatile uint32_t *)(op_base + offset + 4) = (uint32_t)(value >> 32);
}

static uint32_t rt_read(uint32_t offset)
{
    return *(volatile uint32_t *)(runtime + offset);
}

static void rt_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(runtime + offset) = value;
}

static void rt_write64(uint32_t offset, uint64_t value)
{
    *(volatile uint32_t *)(runtime + offset) = (uint32_t)value;
    *(volatile uint32_t *)(runtime + offset + 4) = (uint32_t)(value >> 32);
}

static uint32_t port_read(uint32_t port)
{
    return op_read(OP_PORTS + (port - 1) * 0x10);
}

static void port_write(uint32_t port, uint32_t value)
{
    op_write(OP_PORTS + (port - 1) * 0x10, value);
}

/* Die Zustandsbits werden durch Schreiben einer Eins geloescht - beim
 * Aendern anderer Bits muessen sie deshalb ausmaskiert werden. */
static uint32_t port_keep(uint32_t value)
{
    return value & ~((1u << 17) | (1u << 18) | (1u << 19) | (1u << 20) |
                     (1u << 21) | (1u << 22) | (1u << 23) | PORT_PED);
}

/* ------------------------------------------------------------------ */
/* Ringe                                                               */
/* ------------------------------------------------------------------ */

static bool ring_create(struct ring *r, bool link_back)
{
    uint64_t phys = pmm_alloc_page();

    if (!phys)
        return false;

    r->trb = phys_to_virt(phys);
    r->phys = phys;
    r->index = 0;
    r->cycle = 1;
    memset(r->trb, 0, PAGE_SIZE);

    if (link_back) {
        /* Der letzte Baustein verweist auf den Anfang und dreht dabei
         * das Umlaufbit um. */
        struct trb *last = &r->trb[RING_SIZE - 1];

        last->parameter = phys;
        last->status = 0;
        last->control = (TRB_LINK << 10) | (1u << 1) | 1;
    }
    return true;
}

static void ring_push(struct ring *r, uint64_t parameter, uint32_t status,
                      uint32_t control)
{
    struct trb *slot = &r->trb[r->index];

    slot->parameter = parameter;
    slot->status = status;
    slot->control = (control & ~1u) | r->cycle;

    r->index++;
    if (r->index == RING_SIZE - 1) {
        /* Den Verbindungsbaustein mitnehmen und umlaufen. */
        struct trb *link = &r->trb[RING_SIZE - 1];

        link->control = (link->control & ~1u) | r->cycle;
        r->index = 0;
        r->cycle ^= 1;
    }
}

static void doorbell_ring(uint8_t slot, uint32_t value)
{
    doorbells[slot] = value;
}

/* ------------------------------------------------------------------ */
/* Ereignisse                                                          */
/* ------------------------------------------------------------------ */

static void handle_transfer_event(const struct trb *event)
{
    uint8_t slot = (uint8_t)(event->control >> 24);

    for (size_t i = 0; i < MAX_DEVICES; i++) {
        struct usb_device *dev = &devices[i];

        if (!dev->used || dev->slot != slot)
            continue;
        dev->report_pending = true;
        break;
    }
}

/* Arbeitet den Ereignisring ab. Laeuft im Unterbrechungskontext oder
 * beim Warten auf einen Befehl. */
static void drain_events(void)
{
    for (int guard = 0; guard < 256; guard++) {
        struct trb *event = &event_ring.trb[event_ring.index];

        if ((event->control & 1) != event_ring.cycle)
            break;

        uint32_t type = (event->control >> 10) & 0x3F;

        if (type == TRB_COMMAND_EVENT) {
            command_code = (uint8_t)(event->status >> 24);
            command_slot = (uint8_t)(event->control >> 24);
            command_done = true;
        } else if (type == TRB_TRANSFER_EVENT) {
            handle_transfer_event(event);
        }

        event_ring.index++;
        if (event_ring.index == RING_SIZE) {
            event_ring.index = 0;
            event_ring.cycle ^= 1;
        }
    }

    /* Dem Controller sagen, bis wohin gelesen wurde. Bit 3 quittiert. */
    rt_write64(RT_ERDP, (event_ring.phys +
                         event_ring.index * sizeof(struct trb)) | (1u << 3));
}

static void xhci_irq(struct registers *regs)
{
    UNUSED(regs);

    uint32_t status = op_read(OP_USBSTS);

    op_write(OP_USBSTS, status & (STS_EVENT_INT | STS_PORT_CHANGE));

    uint32_t iman = rt_read(RT_IMAN);

    rt_write(RT_IMAN, iman | 1);        /* Unterbrechung quittieren */

    drain_events();
}

/* Schickt einen Befehl ab und wartet auf das Ergebnis. */
static bool command_run(uint64_t parameter, uint32_t control, uint8_t *slot_out)
{
    command_done = false;
    command_code = 0;

    ring_push(&command_ring, parameter, 0, control);
    doorbell_ring(0, 0);

    uint64_t deadline = timer_ms() + 1000;

    while (!command_done) {
        drain_events();
        if (command_done)
            break;
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }

    if (slot_out)
        *slot_out = command_slot;
    return command_code == 1;           /* 1 = erfolgreich */
}

/* ------------------------------------------------------------------ */
/* Kontexte                                                            */
/* ------------------------------------------------------------------ */

/* Ein Kontext beschreibt dem Controller, was er ueber ein Geraet wissen
 * muss. Die Groesse der Eintraege haengt vom Controller ab - deshalb
 * wird hier mit Byteversatz gerechnet. */
static uint32_t *slot_context(uint8_t *base, bool input)
{
    return (uint32_t *)(base + (input ? context_size : 0));
}

static uint32_t *endpoint_context(uint8_t *base, uint32_t endpoint,
                                  bool input)
{
    /* Endpunkt 0 liegt an Stelle 1, danach zaehlt man in halben
     * Schritten: Ausgang und Eingang teilen sich eine Nummer. */
    uint32_t index = endpoint + 1;

    return (uint32_t *)(base + (input ? context_size : 0) +
                        index * context_size);
}

/* ------------------------------------------------------------------ */
/* Steuerkanal                                                         */
/* ------------------------------------------------------------------ */

bool usb_control(struct usb_device *dev, const struct usb_setup *setup,
                 void *buffer)
{
    if (!dev || !dev->used)
        return false;

    uint64_t data_phys = 0;
    uint8_t *scratch = NULL;

    if (setup->length) {
        data_phys = dev->report_phys;   /* der Puffer des Geraets */
        scratch = dev->report;
        if (setup->length > PAGE_SIZE)
            return false;
        if (!(setup->request_type & USB_DIR_IN) && buffer)
            memcpy(scratch, buffer, setup->length);
    }

    uint64_t packed;

    memcpy(&packed, setup, sizeof(packed));

    /* Der Anfragebaustein traegt die acht Bytes unmittelbar. */
    uint32_t transfer_type = setup->length
                             ? ((setup->request_type & USB_DIR_IN) ? 3 : 2)
                             : 0;

    ring_push(&dev->control_ring, packed, 8,
              (TRB_SETUP << 10) | (1u << 6) | (transfer_type << 16));

    if (setup->length)
        ring_push(&dev->control_ring, data_phys, setup->length,
                  (TRB_DATA << 10) |
                  ((setup->request_type & USB_DIR_IN) ? (1u << 16) : 0));

    /* Der Abschluss laeuft in die Gegenrichtung zur Datenphase. */
    ring_push(&dev->control_ring, 0, 0,
              (TRB_STATUS << 10) | (1u << 5) |
              ((setup->length && (setup->request_type & USB_DIR_IN))
               ? 0 : (1u << 16)));

    dev->report_pending = false;
    doorbell_ring(dev->slot, 1);        /* Endpunkt 0 */

    uint64_t deadline = timer_ms() + 500;

    while (!dev->report_pending) {
        drain_events();
        if (dev->report_pending)
            break;
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }
    dev->report_pending = false;

    if (setup->length && (setup->request_type & USB_DIR_IN) && buffer)
        memcpy(buffer, scratch, setup->length);
    return true;
}

size_t usb_interrupt_poll(struct usb_device *dev, void *buffer, size_t size)
{
    if (!dev || !dev->used || !dev->interrupt_endpoint)
        return 0;

    drain_events();
    if (!dev->report_pending)
        return 0;

    dev->report_pending = false;

    size_t length = MIN(size, (size_t)dev->interrupt_size);

    memcpy(buffer, dev->report, length);

    /* Gleich den naechsten Auftrag einstellen - sonst meldet das Geraet
     * nichts mehr. */
    ring_push(&dev->interrupt_ring, dev->report_phys, dev->interrupt_size,
              (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
    doorbell_ring(dev->slot, (uint32_t)(dev->interrupt_endpoint * 2 + 1));
    return length;
}

const struct usb_device_info *usb_device_details(struct usb_device *dev)
{
    return dev ? &dev->info : NULL;
}

size_t usb_device_count(void) { return device_count; }

struct usb_device *usb_device_at(size_t index)
{
    size_t seen = 0;

    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].used)
            continue;
        if (seen == index)
            return &devices[i];
        seen++;
    }
    return NULL;
}

const char *usb_speed_name(uint8_t speed)
{
    switch (speed) {
    case 1: return "Full Speed";
    case 2: return "Low Speed";
    case 3: return "High Speed";
    case 4: return "SuperSpeed";
    case 5: return "SuperSpeed+";
    default: return "unbekannt";
    }
}

/* ------------------------------------------------------------------ */
/* Geraete aufzaehlen                                                  */
/* ------------------------------------------------------------------ */

/* Die Grundgroesse des Steuerpakets haengt von der Geschwindigkeit ab. */
static uint16_t default_packet_size(uint8_t speed)
{
    switch (speed) {
    case 2:  return 8;      /* Low Speed  */
    case 3:  return 64;     /* High Speed */
    case 4:
    case 5:  return 512;    /* SuperSpeed */
    default: return 64;     /* Full Speed - meist 64, sonst 8 */
    }
}

static struct usb_device *alloc_device(void)
{
    for (size_t i = 0; i < MAX_DEVICES; i++)
        if (!devices[i].used)
            return &devices[i];
    return NULL;
}

static void free_device(struct usb_device *dev)
{
    if (dev->control_ring.phys)
        pmm_free_page(dev->control_ring.phys);
    if (dev->interrupt_ring.phys)
        pmm_free_page(dev->interrupt_ring.phys);
    if (dev->input_phys)
        pmm_free_page(dev->input_phys);
    if (dev->device_phys)
        pmm_free_page(dev->device_phys);
    if (dev->report_phys)
        pmm_free_page(dev->report_phys);
    memset(dev, 0, sizeof(*dev));
}

/* Legt Kontext und Steuerring an und meldet das Geraet beim Controller. */
static bool address_device(struct usb_device *dev)
{
    if (!ring_create(&dev->control_ring, true))
        return false;

    dev->input_phys = pmm_alloc_page();
    dev->device_phys = pmm_alloc_page();
    dev->report_phys = pmm_alloc_page();
    if (!dev->input_phys || !dev->device_phys || !dev->report_phys)
        return false;

    dev->input_context = phys_to_virt(dev->input_phys);
    dev->device_context = phys_to_virt(dev->device_phys);
    dev->report = phys_to_virt(dev->report_phys);
    memset(dev->input_context, 0, PAGE_SIZE);
    memset(dev->device_context, 0, PAGE_SIZE);
    memset(dev->report, 0, PAGE_SIZE);

    /* Der Eingabekontext sagt, welche Teile gueltig sind: Steckplatz
     * und Endpunkt null. */
    uint32_t *control = (uint32_t *)dev->input_context;

    control[1] = 0x3;

    uint32_t *slot = slot_context(dev->input_context, true);

    /* Ein Kontexteintrag, die Wurzel-Anschlussnummer, die Geschwindigkeit. */
    slot[0] = (1u << 27) | ((uint32_t)dev->speed << 20);
    slot[1] = (uint32_t)dev->port << 16;

    uint32_t *endpoint = endpoint_context(dev->input_context, 0, true);

    /* Steuerendpunkt, drei Versuche, Paketgroesse, Ringadresse. */
    endpoint[1] = (4u << 3) | (3u << 1) |
                  ((uint32_t)default_packet_size(dev->speed) << 16);
    endpoint[2] = (uint32_t)(dev->control_ring.phys | 1);
    endpoint[3] = (uint32_t)(dev->control_ring.phys >> 32);
    endpoint[4] = 8;                    /* mittlere Last */

    dcbaa[dev->slot] = dev->device_phys;

    return command_run(dev->input_phys,
                       (TRB_ADDRESS_DEVICE << 10) |
                       ((uint32_t)dev->slot << 24), NULL);
}

/* Richtet den Endpunkt ein, ueber den das Geraet von sich aus meldet. */
static bool configure_interrupt_endpoint(struct usb_device *dev,
                                         uint8_t address, uint16_t packet,
                                         uint8_t interval)
{
    uint8_t number = address & 0x0F;
    bool input = (address & 0x80) != 0;

    if (!input || number == 0 || number > 15)
        return false;
    if (!ring_create(&dev->interrupt_ring, true))
        return false;

    uint32_t index = (uint32_t)number * 2 + 1;   /* Eingang */
    uint32_t *control = (uint32_t *)dev->input_context;

    memset(dev->input_context, 0, PAGE_SIZE);
    control[1] = 1u | (1u << index);             /* Steckplatz + Endpunkt */

    uint32_t *slot = slot_context(dev->input_context, true);

    slot[0] = (index << 27) | ((uint32_t)dev->speed << 20);
    slot[1] = (uint32_t)dev->port << 16;

    uint32_t *endpoint = endpoint_context(dev->input_context, index - 1, true);

    /* Typ 7 ist ein Eingang mit Unterbrechung. */
    endpoint[0] = (uint32_t)interval << 16;
    endpoint[1] = (7u << 3) | (3u << 1) | ((uint32_t)packet << 16);
    endpoint[2] = (uint32_t)(dev->interrupt_ring.phys | 1);
    endpoint[3] = (uint32_t)(dev->interrupt_ring.phys >> 32);
    endpoint[4] = packet;

    if (!command_run(dev->input_phys,
                     (TRB_CONFIG_ENDPOINT << 10) |
                     ((uint32_t)dev->slot << 24), NULL))
        return false;

    dev->interrupt_endpoint = number;
    dev->interrupt_size = packet;

    /* Den ersten Auftrag einstellen. */
    ring_push(&dev->interrupt_ring, dev->report_phys, packet,
              (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
    doorbell_ring(dev->slot, index);
    return true;
}

/* Liest einen Deskriptor ueber den Steuerkanal. */
static bool get_descriptor(struct usb_device *dev, uint8_t type, uint8_t index,
                           void *buffer, uint16_t length)
{
    struct usb_setup setup = {
        .request_type = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (uint16_t)((type << 8) | index),
        .index = 0,
        .length = length,
    };

    return usb_control(dev, &setup, buffer);
}

/* Sucht in der Konfiguration die erste Schnittstelle mit
 * Unterbrechungseingang - bei Tastatur und Maus ist das die einzige. */
static bool read_configuration(struct usb_device *dev)
{
    uint8_t header[9];

    if (!get_descriptor(dev, USB_DESC_CONFIG, 0, header, sizeof(header)))
        return false;

    uint16_t total = (uint16_t)(header[2] | (header[3] << 8));

    if (total < 9 || total > 512)
        return false;

    uint8_t full[512];

    if (!get_descriptor(dev, USB_DESC_CONFIG, 0, full, total))
        return false;

    uint8_t configuration = full[5];
    size_t at = 0;
    bool have_interface = false;
    uint8_t endpoint_address = 0;
    uint16_t endpoint_packet = 8;
    uint8_t endpoint_interval = 8;

    while (at + 2 <= total) {
        uint8_t length = full[at];
        uint8_t type = full[at + 1];

        if (length < 2 || at + length > total)
            break;

        if (type == USB_DESC_INTERFACE && length >= 9) {
            if (have_interface && endpoint_address)
                break;                   /* die erste taugliche genuegt */

            dev->info.interface_number = full[at + 2];
            dev->info.interface_class = full[at + 5];
            dev->info.interface_subclass = full[at + 6];
            dev->info.interface_protocol = full[at + 7];
            have_interface = true;
            endpoint_address = 0;
        } else if (type == USB_DESC_ENDPOINT && length >= 7 && have_interface) {
            uint8_t address = full[at + 2];
            uint8_t attributes = full[at + 3];

            if ((attributes & 3) == 3 && (address & 0x80)) {
                endpoint_address = address;
                endpoint_packet = (uint16_t)((full[at + 4] |
                                              (full[at + 5] << 8)) & 0x7FF);
                endpoint_interval = full[at + 6];
            }
        }
        at += length;
    }

    if (!have_interface)
        return false;

    /* Die Konfiguration auswaehlen. */
    struct usb_setup setup = {
        .request_type = USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = USB_REQ_SET_CONFIG,
        .value = configuration,
        .index = 0,
        .length = 0,
    };

    if (!usb_control(dev, &setup, NULL))
        return false;

    if (endpoint_address) {
        /* Der Abstand steht bei langsamen Geraeten in Millisekunden, bei
         * schnellen als Zweierpotenz von 125 Mikrosekunden. */
        uint8_t interval = endpoint_interval;

        if (dev->speed == 1 || dev->speed == 2) {
            interval = 3;
            while ((1u << interval) < (uint32_t)endpoint_interval * 8 &&
                   interval < 10)
                interval++;
        } else if (interval > 0) {
            interval--;
        }
        configure_interrupt_endpoint(dev, endpoint_address, endpoint_packet,
                                     interval);
    }
    return true;
}

static void enumerate_port(uint32_t port)
{
    uint32_t status = port_read(port);

    if (!(status & PORT_CCS))
        return;

    /* Ein Anschluss mit USB-2-Geraet muss erst zurueckgesetzt werden;
     * SuperSpeed erledigt das der Controller selbst. */
    if (!(status & PORT_PED)) {
        port_write(port, port_keep(status) | PORT_RESET);

        uint64_t deadline = timer_ms() + 200;

        while (timer_ms() < deadline) {
            status = port_read(port);
            if (status & PORT_PRC)
                break;
            timer_sleep(2);
        }
        port_write(port, port_keep(status) | PORT_PRC | PORT_CSC);
        timer_sleep(10);
        status = port_read(port);
    }

    if (!(status & PORT_PED))
        return;

    uint8_t slot = 0;

    if (!command_run(0, TRB_ENABLE_SLOT << 10, &slot) || slot == 0)
        return;

    struct usb_device *dev = alloc_device();

    if (!dev)
        return;

    memset(dev, 0, sizeof(*dev));
    dev->used = true;
    dev->slot = slot;
    dev->port = (uint8_t)port;
    dev->speed = (uint8_t)((status >> 10) & 0xF);

    if (!address_device(dev)) {
        free_device(dev);
        return;
    }

    uint8_t descriptor[18];

    if (!get_descriptor(dev, USB_DESC_DEVICE, 0, descriptor, 8)) {
        free_device(dev);
        return;
    }

    /* Jetzt ist die wirkliche Paketgroesse bekannt. */
    uint16_t packet = descriptor[7];

    if (dev->speed == 4 || dev->speed == 5)
        packet = (uint16_t)(1u << descriptor[7]);
    dev->info.max_packet = packet;

    if (!get_descriptor(dev, USB_DESC_DEVICE, 0, descriptor,
                        sizeof(descriptor))) {
        free_device(dev);
        return;
    }

    dev->info.device_class = descriptor[4];
    dev->info.vendor_id = (uint16_t)(descriptor[8] | (descriptor[9] << 8));
    dev->info.product_id = (uint16_t)(descriptor[10] | (descriptor[11] << 8));
    dev->info.port = (uint8_t)port;
    dev->info.speed = dev->speed;

    if (!read_configuration(dev)) {
        free_device(dev);
        return;
    }

    device_count++;

    kprintf("USB         : Anschluss %u, %04x:%04x, Klasse %u.%u.%u, %s\n",
            (unsigned)port, dev->info.vendor_id, dev->info.product_id,
            dev->info.interface_class, dev->info.interface_subclass,
            dev->info.interface_protocol, usb_speed_name(dev->speed));

    usb_hid_attach(dev);
}

/* ------------------------------------------------------------------ */
/* Einrichten                                                          */
/* ------------------------------------------------------------------ */

/* Vor dem Zugriff muss der Controller dem BIOS abgenommen werden. */
static void take_ownership(uint32_t hccparams)
{
    uint32_t offset = (hccparams >> 16) & 0xFFFF;

    if (!offset)
        return;

    volatile uint32_t *cap = (volatile uint32_t *)(cap_base + offset * 4);

    for (int guard = 0; guard < 32; guard++) {
        uint32_t entry = *cap;

        if ((entry & 0xFF) == 1) {          /* USB Legacy Support */
            /* Bit 24 sagt: der Kern will ihn haben. */
            *cap = entry | (1u << 24);

            uint64_t deadline = timer_ms() + 500;

            while (timer_ms() < deadline) {
                if (!(*cap & (1u << 16)))
                    break;
                __asm__ volatile("pause");
            }
            /* Alle Altlasten des BIOS abschalten. */
            cap[1] = 0;
            return;
        }

        uint32_t next = (entry >> 8) & 0xFF;

        if (!next)
            return;
        cap += next;
    }
}

static bool reset_controller(void)
{
    /* Erst anhalten. */
    uint32_t command = op_read(OP_USBCMD);

    op_write(OP_USBCMD, command & ~CMD_RUN);

    uint64_t deadline = timer_ms() + 500;

    while (!(op_read(OP_USBSTS) & STS_HALTED)) {
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }

    op_write(OP_USBCMD, CMD_RESET);
    deadline = timer_ms() + 1000;
    while (op_read(OP_USBCMD) & CMD_RESET) {
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }

    /* Danach meldet der Controller eine Weile "noch nicht bereit". */
    deadline = timer_ms() + 1000;
    while (op_read(OP_USBSTS) & STS_CNR) {
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }
    return true;
}

/* Manche Controller wollen Kratzflaechen im Arbeitsspeicher. */
static bool setup_scratchpad(uint32_t hcsparams2)
{
    uint32_t count = ((hcsparams2 >> 27) & 0x1F) |
                     (((hcsparams2 >> 21) & 0x1F) << 5);

    if (count == 0)
        return true;
    if (count > 512)
        return false;

    uint64_t list_phys = pmm_alloc_page();

    if (!list_phys)
        return false;

    uint64_t *list = phys_to_virt(list_phys);

    memset(list, 0, PAGE_SIZE);

    for (uint32_t i = 0; i < count; i++) {
        uint64_t page = pmm_alloc_page();

        if (!page)
            return false;
        memset(phys_to_virt(page), 0, PAGE_SIZE);
        list[i] = page;
    }

    dcbaa[0] = list_phys;
    return true;
}

static bool setup_controller(const struct pci_device *pci)
{
    if (pci->bar[0] == 0 || pci->bar_is_io[0])
        return false;

    cap_base = phys_to_virt(pci->bar[0]);
    pci_enable_bus_master(pci);

    uint8_t  length = *(volatile uint8_t *)(cap_base + CAP_LENGTH);
    uint32_t hcs1 = *(volatile uint32_t *)(cap_base + CAP_HCSPARAMS1);
    uint32_t hcs2 = *(volatile uint32_t *)(cap_base + CAP_HCSPARAMS2);
    uint32_t hcc1 = *(volatile uint32_t *)(cap_base + CAP_HCCPARAMS1);
    uint32_t dboff = *(volatile uint32_t *)(cap_base + CAP_DBOFF);
    uint32_t rtsoff = *(volatile uint32_t *)(cap_base + CAP_RTSOFF);

    op_base = cap_base + length;
    doorbells = (volatile uint32_t *)(cap_base + (dboff & ~0x3u));
    runtime = cap_base + (rtsoff & ~0x1Fu);

    max_slots = hcs1 & 0xFF;
    max_ports = (hcs1 >> 24) & 0xFF;
    context_size = (hcc1 & (1u << 2)) ? 64 : 32;

    if (max_slots == 0 || max_ports == 0 || max_ports > MAX_PORTS)
        return false;
    if (max_slots > MAX_DEVICES)
        max_slots = MAX_DEVICES;

    take_ownership(hcc1);

    if (!reset_controller())
        return false;

    /* Verzeichnis der Geraetekontexte. */
    dcbaa_phys = pmm_alloc_page();
    if (!dcbaa_phys)
        return false;
    dcbaa = phys_to_virt(dcbaa_phys);
    memset(dcbaa, 0, PAGE_SIZE);

    if (!setup_scratchpad(hcs2))
        return false;

    op_write(OP_CONFIG, max_slots);
    op_write64(OP_DCBAAP, dcbaa_phys);

    if (!ring_create(&command_ring, true))
        return false;
    op_write64(OP_CRCR, command_ring.phys | 1);

    /* Der Ereignisring braucht eine Tabelle, die auf ihn zeigt. */
    if (!ring_create(&event_ring, false))
        return false;

    erst_phys = pmm_alloc_page();
    if (!erst_phys)
        return false;

    uint64_t *erst = phys_to_virt(erst_phys);

    memset(erst, 0, PAGE_SIZE);
    erst[0] = event_ring.phys;
    erst[1] = RING_SIZE;

    rt_write(RT_ERSTSZ, 1);
    rt_write64(RT_ERDP, event_ring.phys);
    rt_write64(RT_ERSTBA, erst_phys);

    /* Unterbrechung anmelden - ueber MSI, wo es geht. */
    int32_t vector = irq_alloc_vector(xhci_irq);
    bool msi = false;

    if (vector >= 0)
        msi = pci_enable_msi(pci, (uint8_t)vector);
    if (!msi) {
        if (vector >= 0)
            irq_free_vector((uint8_t)vector);
        if (pci->irq_line < 16)
            irq_install(pci->irq_line, xhci_irq);
    }

    rt_write(RT_IMOD, 0);
    rt_write(RT_IMAN, 3);               /* freigeben und quittieren */

    op_write(OP_USBSTS, op_read(OP_USBSTS));
    op_write(OP_USBCMD, CMD_RUN | CMD_INTE | CMD_HSEE);

    uint64_t deadline = timer_ms() + 500;

    while (op_read(OP_USBSTS) & STS_HALTED) {
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }

    kprintf("USB         : xHCI mit %u Anschluessen, %u Steckplaetzen%s\n",
            (unsigned)max_ports, (unsigned)max_slots,
            msi ? ", MSI" : "");

    /* Anschluesse mit Strom versorgen und dann absuchen. */
    for (uint32_t port = 1; port <= max_ports; port++) {
        uint32_t status = port_read(port);

        if (!(status & PORT_POWER))
            port_write(port, port_keep(status) | PORT_POWER);
    }
    timer_sleep(100);

    for (uint32_t port = 1; port <= max_ports; port++)
        enumerate_port(port);

    return true;
}

void xhci_init(void)
{
    memset(devices, 0, sizeof(devices));
    device_count = 0;

    for (size_t i = 0; i < pci_device_count(); i++) {
        const struct pci_device *dev = pci_device_at(i);

        /* Klasse 12, Unterklasse 3, Schnittstelle 0x30 ist xHCI. */
        if (!dev || dev->class_code != 0x0C || dev->subclass != 0x03)
            continue;
        if (dev->prog_if != 0x30)
            continue;

        if (!setup_controller(dev))
            kprintf("USB         : xHCI %02x:%02x.%u antwortet nicht\n",
                    dev->bus, dev->slot, dev->func);
        break;                       /* einer genuegt */
    }
}
