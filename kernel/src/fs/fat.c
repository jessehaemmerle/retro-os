/* fat.c - FAT32 mit langen Dateinamen.
 *
 * Aufbau eines FAT32-Datentraegers:
 *
 *   [Bootsektor][reservierte Sektoren][FAT #1][FAT #2][  Datenbereich  ]
 *
 * Die FAT ist eine grosse Tabelle mit einem 32-Bit-Eintrag je Cluster. Der
 * Eintrag nennt den Nachfolger im selben Datenstrom; eine Datei ist also eine
 * verkettete Liste von Clustern. Verzeichnisse sind gewoehnliche Dateien mit
 * 32-Byte-Eintraegen darin.
 *
 * Lange Namen liegen in Zusatzeintraegen davor, rueckwaerts, je 13 Zeichen in
 * UTF-16. Ein Pruefsumme-Byte bindet sie an den zugehoerigen kurzen Namen.
 */

#include "fat.h"
#include "partition.h"
#include "kstring.h"
#include "mm.h"
#include "rtc.h"

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

#define FAT_EOC        0x0FFFFFF8u
#define FAT_FREE       0x00000000u
#define ENTRIES_PER_SECTOR (512 / 32)

/* Ein Sektor FAT wird zwischengespeichert - beim Verfolgen einer Kette
 * liegen aufeinanderfolgende Eintraege fast immer im selben Sektor. */
static uint8_t  fat_cache[512];
static uint64_t fat_cache_lba = (uint64_t)-1;
static struct fat_volume *fat_cache_owner;

/* ------------------------------------------------------------------ */
/* kleine Helfer                                                       */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p, int off)
{
    return (uint16_t)(p[off] | (p[off + 1] << 8));
}

static uint32_t rd32(const uint8_t *p, int off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
           ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

static void wr16(uint8_t *p, int off, uint16_t v)
{
    p[off]     = (uint8_t)(v & 0xFF);
    p[off + 1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, int off, uint32_t v)
{
    p[off]     = (uint8_t)(v & 0xFF);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    p[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    p[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint64_t cluster_lba(struct fat_volume *vol, uint32_t cluster)
{
    return vol->data_lba + (uint64_t)(cluster - 2) * vol->sectors_per_cluster;
}

static bool cluster_valid(struct fat_volume *vol, uint32_t cluster)
{
    return cluster >= 2 && cluster < vol->cluster_count + 2;
}

static void invalidate_fat_cache(void)
{
    fat_cache_lba = (uint64_t)-1;
    fat_cache_owner = NULL;
}

/* ------------------------------------------------------------------ */
/* Zugriff auf die Zuordnungstabelle                                   */
/* ------------------------------------------------------------------ */

static uint32_t fat_get(struct fat_volume *vol, uint32_t cluster)
{
    uint64_t lba    = vol->fat_lba + (cluster * 4) / 512;
    uint32_t offset = (cluster * 4) % 512;

    if (fat_cache_owner != vol || fat_cache_lba != lba) {
        if (!block_read(vol->dev, lba, 1, fat_cache))
            return FAT_EOC;
        fat_cache_lba = lba;
        fat_cache_owner = vol;
    }
    return rd32(fat_cache, (int)offset) & 0x0FFFFFFFu;
}

static bool fat_set(struct fat_volume *vol, uint32_t cluster, uint32_t value)
{
    uint64_t lba    = vol->fat_lba + (cluster * 4) / 512;
    uint32_t offset = (cluster * 4) % 512;
    uint8_t  sector[512];

    if (!block_read(vol->dev, lba, 1, sector))
        return false;

    uint32_t old = rd32(sector, (int)offset);
    wr32(sector, (int)offset, (old & 0xF0000000u) | (value & 0x0FFFFFFFu));

    /* Alle Kopien der FAT gleich halten. */
    for (uint32_t i = 0; i < vol->fat_count; i++) {
        uint64_t copy = lba + (uint64_t)i * vol->sectors_per_fat;

        if (!block_write(vol->dev, copy, 1, sector))
            return false;
    }

    invalidate_fat_cache();
    return true;
}

static uint32_t fat_alloc_cluster(struct fat_volume *vol, uint32_t previous)
{
    for (uint32_t c = 2; c < vol->cluster_count + 2; c++) {
        if (fat_get(vol, c) != FAT_FREE)
            continue;

        if (!fat_set(vol, c, 0x0FFFFFFFu))
            return 0;
        if (previous && !fat_set(vol, previous, c))
            return 0;

        /* Neue Cluster immer leeren - sonst tauchen alte Daten wieder auf. */
        uint8_t zero[512];
        memset(zero, 0, sizeof(zero));
        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            if (!block_write(vol->dev, cluster_lba(vol, c) + s, 1, zero))
                return 0;
        }
        return c;
    }
    return 0;
}

static bool fat_free_chain(struct fat_volume *vol, uint32_t cluster)
{
    while (cluster_valid(vol, cluster)) {
        uint32_t next = fat_get(vol, cluster);

        if (!fat_set(vol, cluster, FAT_FREE))
            return false;
        cluster = next;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Namen                                                               */
/* ------------------------------------------------------------------ */

static uint8_t short_name_checksum(const uint8_t *short_name)
{
    uint8_t sum = 0;

    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i]);
    return sum;
}

static char upcase(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static bool short_name_char_ok(char c)
{
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return strchr("$%'-_@~`!(){}^#&", c) != NULL;
}

/* Baut aus einem langen Namen den zugehoerigen 8.3-Namen. */
static void make_short_name(const char *name, uint8_t out[11], uint32_t serial)
{
    memset(out, ' ', 11);

    const char *dot = strrchr(name, '.');
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    int n = 0;

    for (size_t i = 0; i < base_len && n < 8; i++) {
        char c = upcase(name[i]);

        if (c == ' ')
            continue;
        out[n++] = short_name_char_ok(c) ? (uint8_t)c : (uint8_t)'_';
    }
    if (n == 0)
        out[n++] = '_';

    if (dot) {
        int e = 0;
        for (size_t i = 1; dot[i] && e < 3; i++) {
            char c = upcase(dot[i]);
            out[8 + e++] = short_name_char_ok(c) ? (uint8_t)c : (uint8_t)'_';
        }
    }

    /* Anhaengsel "~1", "~2", ... macht den kurzen Namen eindeutig. */
    if (serial > 0) {
        char suffix[8];
        ksnprintf(suffix, sizeof(suffix), "~%u", (unsigned)serial);

        size_t slen = strlen(suffix);
        size_t pos  = MIN((size_t)n, 8 - slen);

        for (size_t i = 0; i < slen; i++)
            out[pos + i] = (uint8_t)suffix[i];
    }
}

static char lowcase(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Kurze Namen stehen in Grossbuchstaben. Zwei Bits im sonst ungenutzten
 * Byte 12 merken sich, ob Name oder Endung eigentlich klein geschrieben
 * waren - so bleibt "liesmich.txt" auch ohne langen Namen lesbar. */
static void short_name_to_text(const uint8_t *raw, char *out)
{
    bool lower_base = (raw[12] & 0x08) != 0;
    bool lower_ext  = (raw[12] & 0x10) != 0;
    int  n = 0;

    for (int i = 0; i < 8 && raw[i] != ' '; i++)
        out[n++] = lower_base ? lowcase((char)raw[i]) : (char)raw[i];

    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++)
            out[n++] = lower_ext ? lowcase((char)raw[i]) : (char)raw[i];
    }
    out[n] = '\0';
}

/* Ein LFN-Eintrag traegt 13 Zeichen an drei Stellen. */
static const int lfn_offsets[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static void lfn_read_part(const uint8_t *entry, char *out13)
{
    for (int i = 0; i < 13; i++) {
        uint16_t ch = rd16(entry, lfn_offsets[i]);

        if (ch == 0xFFFF || ch == 0x0000)
            out13[i] = '\0';
        else
            out13[i] = (ch < 256) ? (char)ch : '?';
    }
}

static void lfn_write_part(uint8_t *entry, const char *name, size_t start,
                           size_t len)
{
    for (int i = 0; i < 13; i++) {
        size_t index = start + (size_t)i;
        uint16_t ch;

        if (index < len)
            ch = (uint16_t)(uint8_t)name[index];
        else if (index == len)
            ch = 0x0000;
        else
            ch = 0xFFFF;

        wr16(entry, lfn_offsets[i], ch);
    }
}

/* ------------------------------------------------------------------ */
/* Verzeichnisse durchlaufen                                           */
/* ------------------------------------------------------------------ */

/* Liest den Sektor, in dem der Eintrag mit dieser laufenden Nummer liegt.
 * Gibt false zurueck, wenn das Verzeichnis dort endet. */
static bool dir_sector_for(struct fat_volume *vol, uint32_t dir_cluster,
                           uint32_t index, uint64_t *out_lba, uint32_t *out_slot)
{
    uint32_t per_cluster = vol->sectors_per_cluster * ENTRIES_PER_SECTOR;
    uint32_t cluster = dir_cluster;

    while (index >= per_cluster) {
        cluster = fat_get(vol, cluster);
        if (!cluster_valid(vol, cluster))
            return false;
        index -= per_cluster;
    }

    *out_lba  = cluster_lba(vol, cluster) + index / ENTRIES_PER_SECTOR;
    *out_slot = index % ENTRIES_PER_SECTOR;
    return true;
}

struct dir_walk {
    struct fat_volume *vol;
    uint32_t dir_cluster;
    uint32_t index;
    uint32_t cluster;
    uint32_t index_in_cluster;
    uint8_t  sector[512];
    bool     sector_valid;
    uint64_t sector_lba;
};

static void walk_begin(struct dir_walk *w, struct fat_volume *vol,
                       uint32_t dir_cluster)
{
    memset(w, 0, sizeof(*w));
    w->vol = vol;
    w->dir_cluster = dir_cluster;
    w->cluster = dir_cluster;
}

/* Liefert den naechsten 32-Byte-Eintrag oder NULL am Ende des Verzeichnisses. */
static uint8_t *walk_next(struct dir_walk *w)
{
    uint32_t per_cluster = w->vol->sectors_per_cluster * ENTRIES_PER_SECTOR;

    if (w->index_in_cluster >= per_cluster) {
        uint32_t next = fat_get(w->vol, w->cluster);

        if (!cluster_valid(w->vol, next))
            return NULL;
        w->cluster = next;
        w->index_in_cluster = 0;
        w->sector_valid = false;
    }

    uint64_t lba = cluster_lba(w->vol, w->cluster)
                 + w->index_in_cluster / ENTRIES_PER_SECTOR;

    if (!w->sector_valid || w->sector_lba != lba) {
        if (!block_read(w->vol->dev, lba, 1, w->sector))
            return NULL;
        w->sector_lba = lba;
        w->sector_valid = true;
    }

    uint8_t *entry = &w->sector[(w->index_in_cluster % ENTRIES_PER_SECTOR) * 32];

    w->index_in_cluster++;
    w->index++;
    return entry;
}

static void fill_time(struct fat_dirent *out, uint16_t date, uint16_t time)
{
    out->year   = (uint16_t)(1980 + ((date >> 9) & 0x7F));
    out->month  = (uint8_t)((date >> 5) & 0x0F);
    out->day    = (uint8_t)(date & 0x1F);
    out->hour   = (uint8_t)((time >> 11) & 0x1F);
    out->minute = (uint8_t)((time >> 5) & 0x3F);
}

bool fat_list_dir(struct fat_volume *vol, uint32_t dir_cluster,
                  fat_dir_cb cb, void *user)
{
    if (!vol->mounted || !cluster_valid(vol, dir_cluster))
        return false;

    struct dir_walk w;
    char   lfn[FAT_NAME_MAX + 16];
    int    lfn_len = 0;
    uint8_t lfn_checksum = 0;
    uint8_t lfn_slots = 0;

    walk_begin(&w, vol, dir_cluster);
    lfn[0] = '\0';

    for (;;) {
        uint32_t index = w.index;
        uint8_t *entry = walk_next(&w);

        if (!entry)
            break;
        if (entry[0] == 0x00)
            break;                       /* Ende des Verzeichnisses */

        if (entry[0] == 0xE5) {          /* geloescht */
            lfn_len = 0;
            lfn_slots = 0;
            continue;
        }

        uint8_t attr = entry[11];

        if ((attr & ATTR_LFN) == ATTR_LFN) {
            uint8_t ord = (uint8_t)(entry[0] & 0x3F);
            char part[14];

            lfn_read_part(entry, part);
            part[13] = '\0';

            /* Die Teile kommen rueckwaerts: Ordnungszahl 1 ist der Anfang. */
            int pos = (ord - 1) * 13;
            if (pos >= 0 && pos + 13 <= FAT_NAME_MAX + 1) {
                for (int i = 0; i < 13; i++)
                    lfn[pos + i] = part[i];
                if (entry[0] & 0x40)
                    lfn[pos + (int)strlen(part)] = '\0';
                lfn_len = MAX(lfn_len, pos + (int)strlen(part));
            }
            lfn_checksum = entry[13];
            lfn_slots++;
            continue;
        }

        if (attr & ATTR_VOLUME_ID) {
            lfn_len = 0;
            lfn_slots = 0;
            continue;
        }

        struct fat_dirent out;
        memset(&out, 0, sizeof(out));

        char shortname[16];
        short_name_to_text(entry, shortname);

        if (lfn_len > 0 && lfn_checksum == short_name_checksum(entry)) {
            lfn[MIN(lfn_len, FAT_NAME_MAX)] = '\0';
            strlcpy(out.name, lfn, sizeof(out.name));
            out.ref.lfn_slots = lfn_slots;
        } else {
            strlcpy(out.name, shortname, sizeof(out.name));
            out.ref.lfn_slots = 0;
        }

        out.is_dir        = (attr & ATTR_DIRECTORY) != 0;
        out.read_only     = (attr & ATTR_READ_ONLY) != 0;
        out.first_cluster = ((uint32_t)rd16(entry, 20) << 16) | rd16(entry, 26);
        out.size          = rd32(entry, 28);
        out.ref.dir_cluster = dir_cluster;
        out.ref.index       = index;
        fill_time(&out, rd16(entry, 24), rd16(entry, 22));

        lfn_len = 0;
        lfn_slots = 0;

        if (strcmp(out.name, ".") == 0 || strcmp(out.name, "..") == 0)
            continue;

        cb(user, &out);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Dateien lesen und schreiben                                         */
/* ------------------------------------------------------------------ */

bool fat_read_file(struct fat_volume *vol, uint32_t first_cluster,
                   uint32_t size, void *buffer)
{
    if (!vol->mounted)
        return false;
    if (size == 0)
        return true;

    uint8_t *out = buffer;
    uint32_t remaining = size;
    uint32_t cluster = first_cluster;
    uint8_t  sector[512];

    while (remaining > 0 && cluster_valid(vol, cluster)) {
        uint64_t lba = cluster_lba(vol, cluster);

        for (uint32_t s = 0; s < vol->sectors_per_cluster && remaining > 0; s++) {
            if (!block_read(vol->dev, lba + s, 1, sector))
                return false;

            uint32_t take = MIN(remaining, 512u);
            memcpy(out, sector, take);
            out += take;
            remaining -= take;
        }
        cluster = fat_get(vol, cluster);
    }
    return remaining == 0;
}

/* Traegt Groesse, Startcluster und Zeitstempel im Verzeichniseintrag nach. */
static bool update_entry(struct fat_volume *vol, const struct fat_entry_ref *ref,
                         uint32_t first_cluster, uint32_t size)
{
    uint64_t lba;
    uint32_t slot;
    uint8_t  sector[512];

    if (!dir_sector_for(vol, ref->dir_cluster, ref->index, &lba, &slot))
        return false;
    if (!block_read(vol->dev, lba, 1, sector))
        return false;

    uint8_t *entry = &sector[slot * 32];

    wr16(entry, 20, (uint16_t)(first_cluster >> 16));
    wr16(entry, 26, (uint16_t)(first_cluster & 0xFFFF));
    wr32(entry, 28, size);

    struct datetime dt;
    rtc_read(&dt);
    uint16_t date = (uint16_t)(((dt.year - 1980) << 9) | (dt.month << 5) | dt.day);
    uint16_t time = (uint16_t)((dt.hour << 11) | (dt.minute << 5) | (dt.second / 2));
    wr16(entry, 22, time);
    wr16(entry, 24, date);

    return block_write(vol->dev, lba, 1, sector);
}

bool fat_write_file(struct fat_volume *vol, const struct fat_entry_ref *ref,
                    uint32_t *first_cluster, const void *data, uint32_t size)
{
    if (!vol->mounted)
        return false;

    /* Der einfachste Weg, der immer stimmt: alte Kette freigeben und die
     * Datei frisch schreiben. Bei den Dateigroessen, um die es hier geht,
     * ist das schnell genug und kann keine Reste hinterlassen. */
    if (cluster_valid(vol, *first_cluster)) {
        if (!fat_free_chain(vol, *first_cluster))
            return false;
        *first_cluster = 0;
    }

    if (size == 0)
        return update_entry(vol, ref, 0, 0);

    const uint8_t *in = data;
    uint32_t remaining = size;
    uint32_t previous = 0;
    uint32_t head = 0;
    uint8_t  sector[512];

    while (remaining > 0) {
        uint32_t cluster = fat_alloc_cluster(vol, previous);

        if (!cluster) {
            if (head)
                fat_free_chain(vol, head);
            return false;
        }
        if (!head)
            head = cluster;
        previous = cluster;

        uint64_t lba = cluster_lba(vol, cluster);
        for (uint32_t s = 0; s < vol->sectors_per_cluster && remaining > 0; s++) {
            uint32_t take = MIN(remaining, 512u);

            memset(sector, 0, sizeof(sector));
            memcpy(sector, in, take);
            if (!block_write(vol->dev, lba + s, 1, sector))
                return false;

            in += take;
            remaining -= take;
        }
    }

    *first_cluster = head;
    return update_entry(vol, ref, head, size);
}

/* ------------------------------------------------------------------ */
/* Eintraege anlegen und loeschen                                      */
/* ------------------------------------------------------------------ */

struct name_check {
    const char *name;
    bool        found;
};

static void name_check_cb(void *user, const struct fat_dirent *entry)
{
    struct name_check *c = user;

    if (strcasecmp(entry->name, c->name) == 0)
        c->found = true;
}

/* Sucht eine Folge freier Eintraege und verlaengert notfalls das Verzeichnis. */
static bool find_free_slots(struct fat_volume *vol, uint32_t dir_cluster,
                            uint32_t needed, uint32_t *out_index)
{
    struct dir_walk w;
    uint32_t run = 0, run_start = 0;

    walk_begin(&w, vol, dir_cluster);

    for (;;) {
        uint32_t index = w.index;
        uint8_t *entry = walk_next(&w);

        if (!entry)
            break;

        if (entry[0] == 0x00 || entry[0] == 0xE5) {
            if (run == 0)
                run_start = index;
            if (++run == needed) {
                *out_index = run_start;
                return true;
            }
            /* Ab dem ersten unbenutzten Eintrag ist der Rest ebenfalls frei. */
            if (entry[0] == 0x00 && run < needed)
                continue;
        } else {
            run = 0;
        }
    }

    /* Verzeichnis um einen Cluster verlaengern. */
    uint32_t last = dir_cluster;
    while (cluster_valid(vol, fat_get(vol, last)))
        last = fat_get(vol, last);

    uint32_t added = fat_alloc_cluster(vol, last);
    if (!added)
        return false;

    uint32_t per_cluster = vol->sectors_per_cluster * ENTRIES_PER_SECTOR;
    *out_index = (run > 0) ? run_start : w.index;

    /* Nach dem Anhaengen ist genug Platz, solange der Bedarf in einen
     * Cluster passt - bei hoechstens 21 Eintraegen immer der Fall. */
    return needed <= per_cluster;
}

static bool write_entry_at(struct fat_volume *vol, uint32_t dir_cluster,
                           uint32_t index, const uint8_t entry[32])
{
    uint64_t lba;
    uint32_t slot;
    uint8_t  sector[512];

    if (!dir_sector_for(vol, dir_cluster, index, &lba, &slot))
        return false;
    if (!block_read(vol->dev, lba, 1, sector))
        return false;

    memcpy(&sector[slot * 32], entry, 32);
    return block_write(vol->dev, lba, 1, sector);
}

bool fat_create(struct fat_volume *vol, uint32_t dir_cluster, const char *name,
                bool is_dir, struct fat_dirent *out)
{
    if (!vol->mounted || !name || !name[0] || strlen(name) > FAT_NAME_MAX)
        return false;

    struct name_check check = { .name = name, .found = false };
    fat_list_dir(vol, dir_cluster, name_check_cb, &check);
    if (check.found)
        return false;

    size_t len = strlen(name);
    uint32_t lfn_count = (uint32_t)((len + 12) / 13);
    uint32_t needed = lfn_count + 1;
    uint32_t index;

    if (!find_free_slots(vol, dir_cluster, needed, &index))
        return false;

    /* Kurzen Namen finden, der noch nicht vergeben ist. */
    uint8_t short_name[11];
    for (uint32_t serial = 1; serial < 1000; serial++) {
        make_short_name(name, short_name, serial);

        char text[16];
        short_name_to_text(short_name, text);

        struct name_check c2 = { .name = text, .found = false };
        fat_list_dir(vol, dir_cluster, name_check_cb, &c2);
        if (!c2.found)
            break;
    }

    uint8_t checksum = short_name_checksum(short_name);

    /* Die Namensteile stehen rueckwaerts vor dem kurzen Eintrag. */
    for (uint32_t i = 0; i < lfn_count; i++) {
        uint8_t entry[32];
        uint32_t ord = lfn_count - i;

        memset(entry, 0, sizeof(entry));
        entry[0]  = (uint8_t)(ord | (i == 0 ? 0x40 : 0x00));
        entry[11] = ATTR_LFN;
        entry[13] = checksum;
        lfn_write_part(entry, name, (size_t)(ord - 1) * 13, len);

        if (!write_entry_at(vol, dir_cluster, index + i, entry))
            return false;
    }

    uint32_t cluster = 0;
    if (is_dir) {
        cluster = fat_alloc_cluster(vol, 0);
        if (!cluster)
            return false;
    }

    struct datetime dt;
    rtc_read(&dt);
    uint16_t date = (uint16_t)(((dt.year - 1980) << 9) | (dt.month << 5) | dt.day);
    uint16_t time = (uint16_t)((dt.hour << 11) | (dt.minute << 5) | (dt.second / 2));

    uint8_t entry[32];
    memset(entry, 0, sizeof(entry));
    memcpy(entry, short_name, 11);
    entry[11] = is_dir ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    wr16(entry, 14, time);
    wr16(entry, 16, date);
    wr16(entry, 18, date);
    wr16(entry, 20, (uint16_t)(cluster >> 16));
    wr16(entry, 22, time);
    wr16(entry, 24, date);
    wr16(entry, 26, (uint16_t)(cluster & 0xFFFF));
    wr32(entry, 28, 0);

    if (!write_entry_at(vol, dir_cluster, index + lfn_count, entry))
        return false;

    /* Ein neues Verzeichnis braucht die Eintraege "." und "..". */
    if (is_dir) {
        uint8_t dot[32], dotdot[32];

        memcpy(dot, entry, 32);
        memset(dot, ' ', 11);
        dot[0] = '.';

        memcpy(dotdot, entry, 32);
        memset(dotdot, ' ', 11);
        dotdot[0] = '.';
        dotdot[1] = '.';
        uint32_t parent = (dir_cluster == vol->root_cluster) ? 0 : dir_cluster;
        wr16(dotdot, 20, (uint16_t)(parent >> 16));
        wr16(dotdot, 26, (uint16_t)(parent & 0xFFFF));

        uint8_t sector[512];
        memset(sector, 0, sizeof(sector));
        memcpy(&sector[0], dot, 32);
        memcpy(&sector[32], dotdot, 32);
        if (!block_write(vol->dev, cluster_lba(vol, cluster), 1, sector))
            return false;
    }

    if (out) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->name, name, sizeof(out->name));
        out->is_dir        = is_dir;
        out->first_cluster = cluster;
        out->size          = 0;
        out->ref.dir_cluster = dir_cluster;
        out->ref.index       = index + lfn_count;
        out->ref.lfn_slots   = (uint8_t)lfn_count;
        fill_time(out, date, time);
    }
    return true;
}

static bool mark_deleted(struct fat_volume *vol, uint32_t dir_cluster,
                         uint32_t index)
{
    uint64_t lba;
    uint32_t slot;
    uint8_t  sector[512];

    if (!dir_sector_for(vol, dir_cluster, index, &lba, &slot))
        return false;
    if (!block_read(vol->dev, lba, 1, sector))
        return false;

    sector[slot * 32] = 0xE5;
    return block_write(vol->dev, lba, 1, sector);
}

bool fat_delete(struct fat_volume *vol, const struct fat_entry_ref *ref,
                uint32_t first_cluster, bool is_dir)
{
    if (!vol->mounted)
        return false;

    if (cluster_valid(vol, first_cluster) && !fat_free_chain(vol, first_cluster))
        return false;

    /* Kurzen Eintrag und alle Namensteile davor als geloescht markieren. */
    for (uint32_t i = 0; i <= ref->lfn_slots; i++) {
        uint32_t index = ref->index - ref->lfn_slots + i;

        if (!mark_deleted(vol, ref->dir_cluster, index))
            return false;
    }
    return true;
}

struct empty_check { bool empty; };

static void empty_cb(void *user, const struct fat_dirent *entry)
{
    UNUSED(entry);
    ((struct empty_check *)user)->empty = false;
}

bool fat_dir_is_empty(struct fat_volume *vol, uint32_t dir_cluster)
{
    struct empty_check c = { .empty = true };

    fat_list_dir(vol, dir_cluster, empty_cb, &c);
    return c.empty;
}

bool fat_rename(struct fat_volume *vol, struct fat_dirent *entry,
                const char *new_name)
{
    if (!vol->mounted || !entry)
        return false;

    struct name_check check = { .name = new_name, .found = false };
    fat_list_dir(vol, entry->ref.dir_cluster, name_check_cb, &check);
    if (check.found)
        return false;

    uint32_t cluster = entry->first_cluster;
    uint32_t size    = entry->size;
    bool     is_dir  = entry->is_dir;
    uint32_t dir     = entry->ref.dir_cluster;

    /* Alten Eintrag entfernen, ohne die Daten anzutasten. */
    for (uint32_t i = 0; i <= entry->ref.lfn_slots; i++) {
        if (!mark_deleted(vol, dir, entry->ref.index - entry->ref.lfn_slots + i))
            return false;
    }

    struct fat_dirent fresh;
    if (!fat_create(vol, dir, new_name, false, &fresh))
        return false;

    /* Der neue Eintrag zeigt auf die alten Daten. */
    uint8_t attr_dir = is_dir ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    uint64_t lba;
    uint32_t slot;
    uint8_t  sector[512];

    if (!dir_sector_for(vol, dir, fresh.ref.index, &lba, &slot))
        return false;
    if (!block_read(vol->dev, lba, 1, sector))
        return false;

    uint8_t *raw = &sector[slot * 32];
    raw[11] = attr_dir;
    wr16(raw, 20, (uint16_t)(cluster >> 16));
    wr16(raw, 26, (uint16_t)(cluster & 0xFFFF));
    wr32(raw, 28, size);

    if (!block_write(vol->dev, lba, 1, sector))
        return false;

    entry->ref = fresh.ref;
    strlcpy(entry->name, new_name, sizeof(entry->name));
    return true;
}

/* ------------------------------------------------------------------ */
/* Einhaengen                                                          */
/* ------------------------------------------------------------------ */

static bool parse_bpb(struct fat_volume *vol, const uint8_t *sector,
                      uint64_t partition_lba)
{
    uint16_t bytes_per_sector = rd16(sector, 11);
    uint8_t  sectors_per_cluster = sector[13];

    if (bytes_per_sector != 512 || sectors_per_cluster == 0)
        return false;
    if (rd16(sector, 17) != 0)          /* FAT12/16 haben feste Wurzel */
        return false;
    if (rd16(sector, 22) != 0)          /* FAT16-Groesse muss 0 sein   */
        return false;

    vol->bytes_per_sector    = bytes_per_sector;
    vol->sectors_per_cluster = sectors_per_cluster;
    vol->reserved_sectors    = rd16(sector, 14);
    vol->fat_count           = sector[16];
    vol->sectors_per_fat     = rd32(sector, 36);
    vol->root_cluster        = rd32(sector, 44);
    vol->partition_lba       = partition_lba;

    if (vol->fat_count == 0 || vol->sectors_per_fat == 0)
        return false;

    vol->fat_lba  = partition_lba + vol->reserved_sectors;
    vol->data_lba = vol->fat_lba + (uint64_t)vol->fat_count * vol->sectors_per_fat;

    uint32_t total = rd32(sector, 32);
    if (total == 0)
        total = rd16(sector, 19);

    uint32_t data_sectors = total - (uint32_t)(vol->data_lba - partition_lba);
    vol->cluster_count = data_sectors / vol->sectors_per_cluster;
    vol->cluster_bytes = (uint32_t)vol->sectors_per_cluster * 512;

    memcpy(vol->label, &sector[71], 11);
    vol->label[11] = '\0';
    for (int i = 10; i >= 0 && (vol->label[i] == ' ' || vol->label[i] == '\0'); i--)
        vol->label[i] = '\0';

    return vol->cluster_count > 0;
}

bool fat_mount_at(struct block_device *dev, uint64_t lba,
                  struct fat_volume *vol)
{
    uint8_t sector[512];

    memset(vol, 0, sizeof(*vol));
    vol->dev = dev;
    invalidate_fat_cache();

    if (!block_read(dev, lba, 1, sector))
        return false;
    if (!parse_bpb(vol, sector, lba))
        return false;

    vol->mounted = true;
    return true;
}

bool fat_mount(struct block_device *dev, struct fat_volume *vol)
{
    /* Erst ohne Tabelle versuchen - ein roher Datentraeger traegt sein
     * Dateisystem gleich im ersten Sektor. */
    if (fat_mount_at(dev, 0, vol))
        return true;

    struct partition table[PARTITION_MAX];
    enum partition_scheme scheme;
    size_t count = partition_scan(dev, table, PARTITION_MAX, &scheme);

    /* Zuerst die Abschnitte, die nach FAT aussehen, dann alle uebrigen -
     * manche Tabellen tragen die Kennung falsch ein. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            if ((pass == 0) != table[i].is_fat)
                continue;
            if (fat_mount_at(dev, table[i].start, vol)) {
                kprintf("Datentraeger: %s, Abschnitt %u ab Sektor %llu\n",
                        partition_scheme_name(scheme), (unsigned)(i + 1),
                        (unsigned long long)table[i].start);
                return true;
            }
        }
    }
    return false;
}

uint64_t fat_total_bytes(struct fat_volume *vol)
{
    return (uint64_t)vol->cluster_count * vol->cluster_bytes;
}

uint64_t fat_free_bytes(struct fat_volume *vol)
{
    uint64_t free_clusters = 0;

    for (uint32_t c = 2; c < vol->cluster_count + 2; c++) {
        if (fat_get(vol, c) == FAT_FREE)
            free_clusters++;
    }
    return free_clusters * vol->cluster_bytes;
}

/* ------------------------------------------------------------------ */
/* Formatieren                                                         */
/* ------------------------------------------------------------------ */

/* Faustregel aus der FAT-Spezifikation: je groesser der Datentraeger,
 * desto groesser die Cluster - sonst wird die Tabelle unhandlich. */
static uint8_t choose_cluster_size(uint64_t total_sectors)
{
    if (total_sectors <=    532480ull) return 1;    /* bis  260 MiB */
    if (total_sectors <=  16777216ull) return 8;    /* bis    8 GiB */
    if (total_sectors <=  33554432ull) return 16;   /* bis   16 GiB */
    if (total_sectors <=  67108864ull) return 32;   /* bis   32 GiB */
    return 64;
}

bool fat_format(struct block_device *dev, const char *label)
{
    if (!dev)
        return false;

    uint64_t total = MIN(dev->sector_count, 0xFFFFFFFFull);
    uint32_t reserved = 32;
    uint32_t fat_count = 2;
    uint8_t  spc = choose_cluster_size(total);

    /* Groesse einer FAT: jeder Cluster braucht vier Byte. */
    uint64_t tmp1 = total - reserved;
    uint64_t tmp2 = (uint64_t)256 * spc + fat_count;
    uint32_t fat_size = (uint32_t)((tmp1 + tmp2 - 1) / tmp2);

    uint64_t data_start = reserved + (uint64_t)fat_count * fat_size;
    if (data_start >= total)
        return false;

    uint32_t clusters = (uint32_t)((total - data_start) / spc);
    if (clusters < 65525) {
        kprintf("FAT         : Datentraeger zu klein fuer FAT32\n");
        return false;
    }

    uint8_t sector[512];

    /* --- Bootsektor --- */
    memset(sector, 0, sizeof(sector));
    sector[0] = 0xEB; sector[1] = 0x58; sector[2] = 0x90;      /* Sprung */
    memcpy(&sector[3], "RETROOS ", 8);
    wr16(sector, 11, 512);
    sector[13] = spc;
    wr16(sector, 14, (uint16_t)reserved);
    sector[16] = (uint8_t)fat_count;
    wr16(sector, 17, 0);
    wr16(sector, 19, 0);
    sector[21] = 0xF8;                                          /* Festplatte */
    wr16(sector, 22, 0);
    wr16(sector, 24, 63);
    wr16(sector, 26, 255);
    wr32(sector, 28, 0);
    wr32(sector, 32, (uint32_t)total);
    wr32(sector, 36, fat_size);
    wr16(sector, 40, 0);
    wr16(sector, 42, 0);
    wr32(sector, 44, 2);                                        /* Wurzel */
    wr16(sector, 48, 1);                                        /* FSInfo */
    wr16(sector, 50, 6);                                        /* Sicherung */
    sector[64] = 0x80;
    sector[66] = 0x29;                                          /* Kennung folgt */
    wr32(sector, 67, 0x52455452);
    memset(&sector[71], ' ', 11);
    for (int i = 0; i < 11 && label && label[i]; i++)
        sector[71 + i] = (uint8_t)upcase(label[i]);
    memcpy(&sector[82], "FAT32   ", 8);
    wr16(sector, 510, 0xAA55);

    if (!block_write(dev, 0, 1, sector))
        return false;
    if (!block_write(dev, 6, 1, sector))          /* Sicherungskopie */
        return false;

    /* --- FSInfo --- */
    uint8_t fsinfo[512];
    memset(fsinfo, 0, sizeof(fsinfo));
    wr32(fsinfo, 0, 0x41615252);
    wr32(fsinfo, 484, 0x61417272);
    wr32(fsinfo, 488, clusters - 1);              /* freie Cluster */
    wr32(fsinfo, 492, 3);                         /* naechster freier */
    wr16(fsinfo, 510, 0xAA55);
    if (!block_write(dev, 1, 1, fsinfo))
        return false;

    /* --- Zuordnungstabellen leeren --- */
    memset(sector, 0, sizeof(sector));
    for (uint32_t i = 0; i < fat_count; i++) {
        uint64_t base = reserved + (uint64_t)i * fat_size;

        for (uint32_t s = 0; s < fat_size; s++) {
            if (!block_write(dev, base + s, 1, sector))
                return false;
        }
    }

    /* Die ersten drei Eintraege: Medienkennung, Endmarke, Wurzelcluster. */
    wr32(sector, 0, 0x0FFFFFF8);
    wr32(sector, 4, 0x0FFFFFFF);
    wr32(sector, 8, 0x0FFFFFFF);
    for (uint32_t i = 0; i < fat_count; i++) {
        if (!block_write(dev, reserved + (uint64_t)i * fat_size, 1, sector))
            return false;
    }

    /* --- Wurzelverzeichnis leeren --- */
    memset(sector, 0, sizeof(sector));
    for (uint32_t s = 0; s < spc; s++) {
        if (!block_write(dev, data_start + s, 1, sector))
            return false;
    }

    invalidate_fat_cache();
    kprintf("FAT         : formatiert - %u Cluster zu je %u Byte\n",
            (unsigned)clusters, (unsigned)(spc * 512));
    return true;
}
