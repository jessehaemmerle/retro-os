/* vfs.h - hierarchisches Dateisystem im Arbeitsspeicher. */
#ifndef VFS_H
#define VFS_H

#include "retro.h"
#include "fat.h"

#define FS_NAME_MAX 63
#define FS_PATH_MAX 256

enum fs_type {
    FS_FILE,
    FS_DIR,
};

/* Woher ein Knoten stammt: aus dem Arbeitsspeicher oder von der Platte. */
enum fs_backend {
    FS_BACKEND_RAM,
    FS_BACKEND_FAT,
};

struct fs_node {
    char            name[FS_NAME_MAX + 1];
    uint8_t         type;
    bool            readonly;      /* Systemdateien lassen sich nicht loeschen */

    struct fs_node *parent;
    struct fs_node *first_child;
    struct fs_node *next_sibling;

    uint8_t        *data;          /* nur bei FS_FILE */
    size_t          size;
    size_t          capacity;

    uint16_t        mtime_hour, mtime_min;
    uint8_t         mtime_day, mtime_month;
    uint16_t        mtime_year;

    /* --- nur fuer Knoten auf einem Datentraeger --- */
    uint8_t         backend;
    bool            children_loaded;   /* Ordner erst bei Bedarf einlesen */
    uint32_t        fat_cluster;
    struct fat_entry_ref fat_ref;
};

void fs_init(void);
struct fs_node *fs_root(void);

/* Haengt den ersten gefundenen Datentraeger unter /Festplatte ein.
 * Liefert false, wenn keine Platte da ist oder sie kein FAT32 enthaelt. */
bool fs_mount_disk(void);
/* Loest den eingehaengten Traeger, bevor er neu beschrieben wird. */
void fs_detach_disk(void);
bool fs_disk_mounted(void);
struct fat_volume *fs_disk_volume(void);
struct fs_node    *fs_disk_root(void);
const char        *fs_disk_name(void);

/* Formatiert den ersten Datentraeger neu und haengt ihn ein. Alle Daten
 * darauf gehen dabei verloren. */
bool fs_format_disk(const char *label);

/* Sorgt dafuer, dass node->data den Dateiinhalt enthaelt. Bei Dateien im
 * Arbeitsspeicher ein Nichts-Tun, bei Dateien auf der Platte ein Lesevorgang. */
bool fs_load(struct fs_node *file);

/* Pfade werden immer absolut ("/ordner/datei") oder relativ zu base gelesen. */
struct fs_node *fs_lookup(struct fs_node *base, const char *path);
struct fs_node *fs_find_child(struct fs_node *dir, const char *name);

struct fs_node *fs_create(struct fs_node *dir, const char *name, enum fs_type type);
/* Wie fs_create, nimmt aber einen ganzen Pfad ("/Ordner/datei.txt") und
 * legt den Eintrag im passenden Ordner an. */
struct fs_node *fs_create_path(struct fs_node *base, const char *path,
                               enum fs_type type);
bool  fs_remove(struct fs_node *node);
bool  fs_rename(struct fs_node *node, const char *name);
bool  fs_move(struct fs_node *node, struct fs_node *new_parent);

bool  fs_write(struct fs_node *file, const void *data, size_t size);
bool  fs_append(struct fs_node *file, const void *data, size_t size);

size_t fs_child_count(struct fs_node *dir);
/* Kinder sortiert (Ordner zuerst, dann alphabetisch) in ein Feld schreiben. */
size_t fs_list(struct fs_node *dir, struct fs_node **out, size_t max);

void fs_path(struct fs_node *node, char *buf, size_t size);
/* Haengt der Knoten noch im Baum? Wichtig fuer Programme, die sich einen
 * Knoten gemerkt haben, waehrend ihn jemand anders geloescht hat. */
bool fs_node_alive(const struct fs_node *node);
/* Groesse eines Ordners = Summe aller enthaltenen Dateien. */
size_t fs_total_size(struct fs_node *node);
void fs_format_size(char *buf, size_t bufsize, size_t bytes);

/* Statistik fuer die Oberflaeche. */
size_t fs_node_count(void);
size_t fs_bytes_used(void);

/* Heuristik anhand der Endung - steuert Symbol und Standardprogramm. */
bool fs_is_text(const struct fs_node *node);

#endif /* VFS_H */
