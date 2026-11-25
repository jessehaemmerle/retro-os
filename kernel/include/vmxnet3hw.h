/* vmxnet3hw.h - Wortformate und Speicheraufbau der VMware-Karte vmxnet3.
 *
 * Die vmxnet3 ist keine nachgebaute Hardware, sondern eine Abmachung
 * zwischen Gast und VMware (auch QEMU kann sie): Der Gast legt eine
 * Beschreibung seiner Warteschlangen in den Speicher, nennt dem Geraet
 * ihre Adresse und schickt dann nur noch Befehle und Zaehlerstaende
 * ueber zwei kleine Registerbereiche.
 *
 * Alles hier haengt an festen Versaetzen: Die Beschreibung ist ein
 * Block von 720 Byte, den das Geraet Feld fuer Feld liest, und ein
 * Deskriptor ein Bitfeld, in dem die "Generationsbit" genannte Stelle
 * entscheidet, ob ein Eintrag schon gueltig ist. Ein Feld um vier Byte
 * verrutscht gibt keine Fehlermeldung, sondern ein Geraet, das mit
 * Ringen von zufaelliger Groesse arbeitet. Deshalb steht das alles hier,
 * ohne Zugriff auf Hardware, und wird mit offsetof und sizeof auf dem
 * Entwicklungsrechner nachgemessen.
 */
#ifndef VMXNET3HW_H
#define VMXNET3HW_H

#include "retro.h"

/* --- Register --- */
/* BAR 1: Verwaltung */
#define VMX_REG_VRRS  0x000      /* Auswahl der Bauform (Revision)    */
#define VMX_REG_UVRS  0x008      /* Auswahl der UPT-Version           */
#define VMX_REG_DSAL  0x010      /* Adresse der Beschreibung, unten   */
#define VMX_REG_DSAH  0x018      /* ... oben                          */
#define VMX_REG_CMD   0x020
#define VMX_REG_MACL  0x028
#define VMX_REG_MACH  0x030
/* BAR 0: Klingeln */
#define VMX_REG_IMR   0x000      /* Unterbrechungen sperren, 8 je Nr. */
#define VMX_REG_TXPROD 0x600
#define VMX_REG_RXPROD 0x800     /* Ring 1; Ring 2 liegt bei 0xA00    */

/* --- Befehle --- */
#define VMX_CMD_ACTIVATE     0xCAFE0000u
#define VMX_CMD_QUIESCE      0xCAFE0001u
#define VMX_CMD_RESET        0xCAFE0002u
#define VMX_CMD_UPDATE_RXMODE 0xCAFE0003u
#define VMX_CMD_GET_LINK     0xF00D0002u

#define VMX_MAGIC            0xBABEFEE1u

#define VMX_RXM_UCAST        0x01u
#define VMX_RXM_MCAST        0x02u
#define VMX_RXM_BCAST        0x04u
#define VMX_RXM_ALL_MULTI    0x08u

#define VMX_IC_DISABLE_ALL   0x01u

#define VMX_MAX_INTRS        25
#define VMX_VFT_SIZE         128

/* --- Die Beschreibung, die dem Geraet gegeben wird --- */
struct vmx_driver_info {
    uint32_t version;
    uint32_t gos;              /* Bits, Art und Version des Gastsystems */
    uint32_t rev_supported;
    uint32_t upt_supported;
} __attribute__((packed));

struct vmx_misc_conf {
    struct vmx_driver_info info;
    uint64_t upt_features;
    uint64_t dd_pa;
    uint64_t queue_desc_pa;
    uint32_t dd_len;
    uint32_t queue_desc_len;
    uint32_t mtu;
    uint16_t max_rx_sg;
    uint8_t  num_tx_queues;
    uint8_t  num_rx_queues;
    uint32_t reserved[4];
} __attribute__((packed));

struct vmx_intr_conf {
    uint8_t  auto_mask;
    uint8_t  num_intrs;
    uint8_t  event_intr_idx;
    uint8_t  mod_levels[VMX_MAX_INTRS];
    uint32_t intr_ctrl;
    uint32_t reserved[2];
} __attribute__((packed));

struct vmx_rx_filter_conf {
    uint32_t rx_mode;
    uint16_t mf_table_len;
    uint16_t pad;
    uint64_t mf_table_pa;
    uint32_t vf_table[VMX_VFT_SIZE];
} __attribute__((packed));

struct vmx_var_conf {
    uint32_t version;
    uint32_t length;
    uint64_t pa;
} __attribute__((packed));

struct vmx_dev_read {
    struct vmx_misc_conf      misc;
    struct vmx_intr_conf      intr;
    struct vmx_rx_filter_conf rx_filter;
    struct vmx_var_conf       rss, pm, plugin;
} __attribute__((packed));

struct vmx_shared {
    uint32_t magic;
    uint32_t pad;
    struct vmx_dev_read dev_read;
    uint32_t ecr;
    uint32_t reserved[5];
} __attribute__((packed));

/* --- Beschreibung der Warteschlangen --- */
struct vmx_tx_queue_desc {
    /* ctrl */
    uint32_t tx_num_deferred;
    uint32_t tx_threshold;
    uint64_t ctrl_reserved;
    /* conf */
    uint64_t tx_ring_pa;
    uint64_t data_ring_pa;
    uint64_t comp_ring_pa;
    uint64_t dd_pa;
    uint64_t conf_reserved;
    uint32_t tx_ring_size;
    uint32_t data_ring_size;
    uint32_t comp_ring_size;
    uint32_t dd_len;
    uint8_t  intr_idx;
    uint8_t  pad[7];
    /* Auskunft des Geraets: Status und Zaehler */
    uint8_t  tail[8 + 80 + 88];
} __attribute__((packed));

struct vmx_rx_queue_desc {
    /* ctrl */
    uint8_t  update_rx_prod;
    uint8_t  ctrl_pad[7];
    uint64_t ctrl_reserved;
    /* conf */
    uint64_t rx_ring_pa[2];
    uint64_t comp_ring_pa;
    uint64_t dd_pa;
    uint64_t conf_reserved;
    uint32_t rx_ring_size[2];
    uint32_t comp_ring_size;
    uint32_t dd_len;
    uint8_t  intr_idx;
    uint8_t  pad[7];
    uint8_t  tail[8 + 80 + 88];
} __attribute__((packed));

/* --- Ringe und Deskriptoren --- */
#define VMX_DESC_BYTES 16

struct vmx_ring {
    uint32_t size;
    uint32_t next;             /* naechste Stelle zum Fuellen/Lesen */
    uint8_t  gen;              /* Generationsbit, beginnt bei 1      */
};

void vmx_ring_init(struct vmx_ring *ring, uint32_t size);
/* Eine Stelle weiter; am Ende geht es von vorn los, und das
 * Generationsbit kippt - so erkennt das Geraet, was neu ist. */
void vmx_ring_advance(struct vmx_ring *ring);

/* Eine Ringgroesse ist erlaubt, wenn sie ein Vielfaches von 32 ist,
 * nicht null und nicht ueber 4096. */
bool vmx_ring_size_ok(uint32_t size);

/* Sende-Deskriptor: ein Stueck, ganzer Rahmen, mit Meldung am Ende. */
void vmx_tx_desc(uint32_t d[4], uint64_t addr, uint32_t length, uint8_t gen);
/* Empfangs-Deskriptor: Kopfpuffer. */
void vmx_rx_desc(uint32_t d[4], uint64_t addr, uint32_t length, uint8_t gen);

/* Generationsbit eines Meldeeintrags (Senden wie Empfangen). */
uint8_t vmx_comp_gen(const uint32_t d[4]);

struct vmx_rx_comp {
    uint32_t index;            /* welcher Deskriptor                 */
    uint32_t length;
    uint32_t queue_id;
    bool     sop, eop, error;
};
void vmx_rx_comp_decode(const uint32_t d[4], struct vmx_rx_comp *out);

/* Welcher Sende-Deskriptor ist erledigt. */
uint32_t vmx_tx_comp_index(const uint32_t d[4]);

/* Das Wort fuer Gastsystem: 64 Bit, Art Linux, Version 1. */
uint32_t vmx_gos_word(void);

/* Aus dem Ergebnis von GET_LINK: Verbindung da? Geschwindigkeit in MBit. */
bool     vmx_link_up(uint32_t result);
uint32_t vmx_link_speed(uint32_t result);

#endif /* VMXNET3HW_H */
