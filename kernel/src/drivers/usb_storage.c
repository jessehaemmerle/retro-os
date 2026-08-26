/* usb_storage.c - USB-Sticks und externe Platten als Datentraeger.
 *
 * Fast alle Speichergeraete am USB-Bus sprechen "Bulk-Only Transport":
 * Der Kern schickt einen Befehlsblock ueber den Massenendpunkt zum
 * Geraet, dann fliessen die Daten, und zuletzt kommt eine Quittung
 * zurueck. Im Befehlsblock steckt ein SCSI-Befehl - dieselbe Sprache,
 * die auch eine SAS-Platte versteht.
 */

#include "usb.h"
#include "block.h"
#include "arch.h"
#include "kstring.h"
#include "mm.h"

#define CBW_SIGNATURE 0x43425355u       /* "USBC" */
#define CSW_SIGNATURE 0x53425355u       /* "USBS" */

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY   0x25
#define SCSI_READ_10         0x28
#define SCSI_WRITE_10        0x2A

#define MAX_STORAGE 2

/* Der Befehlsblock, den das Geraet erwartet. */
struct cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t transfer_length;
    uint8_t  flags;                     /* Bit 7: 1 = zum Kern hin */
    uint8_t  lun;
    uint8_t  command_length;
    uint8_t  command[16];
} PACKED;

/* Die Quittung danach. */
struct csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residual;
    uint8_t  status;                    /* 0 = in Ordnung */
} PACKED;

struct storage {
    struct usb_device  *usb;
    uint32_t            tag;
    uint8_t             lun;
    uint32_t            block_size;
    uint64_t            block_count;

    struct block_device block;
    char                model[48];
};

static struct storage units[MAX_STORAGE];
static size_t         unit_count;

/* ------------------------------------------------------------------ */
/* Ein SCSI-Befehl von Anfang bis Ende                                 */
/* ------------------------------------------------------------------ */

static bool run_command(struct storage *s, const uint8_t *command,
                        uint8_t command_length, void *data, uint32_t length,
                        bool in)
{
    struct cbw cbw;
    struct csw csw;
    uint32_t moved = 0;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = ++s->tag;
    cbw.transfer_length = length;
    cbw.flags = in ? 0x80 : 0x00;
    cbw.lun = s->lun;
    cbw.command_length = command_length;
    memcpy(cbw.command, command, command_length);

    if (!usb_bulk(s->usb, 0, &cbw, sizeof(cbw), false, NULL))
        return false;

    if (length > 0) {
        if (!usb_bulk(s->usb, 0, data, length, in, &moved)) {
            /* Nach einem Fehler haelt der Endpunkt an; erst loesen,
             * dann die Quittung abholen. */
            usb_clear_halt(s->usb, in ? (uint8_t)0x80 : 0x00);
        }
    }

    memset(&csw, 0, sizeof(csw));
    if (!usb_bulk(s->usb, 0, &csw, sizeof(csw), true, NULL))
        return false;

    if (csw.signature != CSW_SIGNATURE || csw.tag != cbw.tag)
        return false;
    return csw.status == 0;
}

/* ------------------------------------------------------------------ */
/* Lesen und Schreiben                                                 */
/* ------------------------------------------------------------------ */

static bool storage_transfer(struct storage *s, uint64_t lba, uint32_t count,
                             void *buffer, bool write)
{
    uint8_t command[10];
    uint32_t bytes = count * s->block_size;

    memset(command, 0, sizeof(command));
    command[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
    command[2] = (uint8_t)(lba >> 24);
    command[3] = (uint8_t)(lba >> 16);
    command[4] = (uint8_t)(lba >> 8);
    command[5] = (uint8_t)lba;
    command[7] = (uint8_t)(count >> 8);
    command[8] = (uint8_t)count;

    return run_command(s, command, sizeof(command), buffer, bytes, !write);
}

static bool storage_read(struct block_device *dev, uint64_t lba,
                         uint32_t count, void *buffer)
{
    struct storage *s = dev->driver_data;
    uint8_t *out = buffer;
    uint32_t per_call = (16 * 4096) / s->block_size;

    while (count > 0) {
        uint32_t chunk = MIN(count, per_call);

        if (!storage_transfer(s, lba, chunk, out, false))
            return false;
        out += (size_t)chunk * s->block_size;
        lba += chunk;
        count -= chunk;
    }
    return true;
}

static bool storage_write(struct block_device *dev, uint64_t lba,
                          uint32_t count, void *buffer)
{
    struct storage *s = dev->driver_data;
    uint8_t *in = buffer;
    uint32_t per_call = (16 * 4096) / s->block_size;

    while (count > 0) {
        uint32_t chunk = MIN(count, per_call);

        if (!storage_transfer(s, lba, chunk, in, true))
            return false;
        in += (size_t)chunk * s->block_size;
        lba += chunk;
        count -= chunk;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Anmelden                                                            */
/* ------------------------------------------------------------------ */

/* Wie viele Einheiten haengen an diesem Geraet? Meist genau eine. */
static uint8_t max_lun(struct usb_device *dev, uint8_t interface)
{
    uint8_t value = 0;
    struct usb_setup setup = {
        .request_type = (uint8_t)(USB_DIR_IN | USB_TYPE_CLASS |
                                  USB_RECIP_INTERFACE),
        .request = 0xFE,
        .value = 0,
        .index = interface,
        .length = 1,
    };

    if (!usb_control(dev, &setup, &value))
        return 0;
    return value;
}

void usb_storage_attach(struct usb_device *dev)
{
    const struct usb_device_info *info = usb_device_details(dev);

    if (!info || info->interface_class != USB_CLASS_STORAGE)
        return;
    if (info->interface_protocol != STORAGE_PROTOCOL_BULK)
        return;
    if (unit_count >= MAX_STORAGE)
        return;

    if (!usb_setup_bulk_from_config(dev))
        return;

    struct storage *s = &units[unit_count];

    memset(s, 0, sizeof(*s));
    s->usb = dev;
    s->lun = 0;
    s->block_size = 512;

    (void)max_lun(dev, info->interface_number);

    /* Manche Sticks melden sich erst nach ein paar Anlaeufen bereit. */
    uint8_t ready[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };

    for (int attempt = 0; attempt < 8; attempt++) {
        if (run_command(s, ready, sizeof(ready), NULL, 0, false))
            break;

        uint8_t sense[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
        uint8_t data[18];

        run_command(s, sense, sizeof(sense), data, sizeof(data), true);
        timer_sleep(100);
    }

    uint8_t inquiry[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t answer[36];

    if (run_command(s, inquiry, sizeof(inquiry), answer, sizeof(answer),
                    true)) {
        char name[29];
        size_t at = 0;

        /* Hersteller und Modell stehen ab Byte 8 bzw. 16. */
        for (size_t i = 8; i < 32 && at + 1 < sizeof(name); i++)
            name[at++] = (char)answer[i];
        name[at] = '\0';
        while (at > 0 && name[at - 1] == ' ')
            name[--at] = '\0';
        strlcpy(s->model, name[0] ? name : "USB-Speicher", sizeof(s->model));
    } else {
        strlcpy(s->model, "USB-Speicher", sizeof(s->model));
    }

    uint8_t capacity_cmd[10];
    uint8_t capacity[8];

    memset(capacity_cmd, 0, sizeof(capacity_cmd));
    capacity_cmd[0] = SCSI_READ_CAPACITY;

    if (!run_command(s, capacity_cmd, sizeof(capacity_cmd), capacity,
                     sizeof(capacity), true))
        return;

    uint32_t last = ((uint32_t)capacity[0] << 24) |
                    ((uint32_t)capacity[1] << 16) |
                    ((uint32_t)capacity[2] << 8) | capacity[3];
    uint32_t size = ((uint32_t)capacity[4] << 24) |
                    ((uint32_t)capacity[5] << 16) |
                    ((uint32_t)capacity[6] << 8) | capacity[7];

    if (size < 512 || size > 4096 || last == 0)
        return;

    s->block_size = size;
    s->block_count = (uint64_t)last + 1;

    ksnprintf(s->block.name, sizeof(s->block.name), "usb%u",
              (unsigned)unit_count);
    strlcpy(s->block.model, s->model, sizeof(s->block.model));
    s->block.sector_count = s->block_count;
    s->block.sector_size = s->block_size;
    s->block.read = storage_read;
    s->block.write = storage_write;
    s->block.driver_data = s;

    unit_count++;
    block_register(&s->block);
}
