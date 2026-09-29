/* oc_term_scan — the terminal's scan list (spec
 * 2026-09-27-channel-list-design.md §5): which frequency each search dwell
 * listens on, and for how long.
 *
 * The list is, in order: the last serving cell's anchor (1), the user's
 * entries (4, set over BLE), the network's (12, from CHAN_LIST), learned
 * anchors (4, earlier serving cells, most recent first) and the six default
 * anchors, ch 0-5 (the seed-derived ones, so an unconfigured network is still
 * found). An entry is active when oc_sync_anchor_ok(mode, freq, pattern)
 * allows it; inactive entries are kept (the mode can change) but never
 * scanned. The list keeps every stored entry, duplicates included (SCAN shows
 * them all). Duplicates are entries with the same frequency and FIXED flag:
 * the walk dwells there once, at the first active one; an inactive entry
 * never hides an active one.
 *
 * A round is one pass over the active entries (each duplicate once), then, once `fallback_after`
 * rounds are done (OC_SCAN_NEVER: never), up to `fallback_chunk` grid
 * channels that no active CYCLE entry covers, round-robin across rounds.
 * CYCLE entries and swept channels get OC_SCAN_DWELL_US, FIXED entries
 * OC_SCAN_FIXED_DWELL_US.
 *
 * Pure C: no radio, no NVS, no malloc. oc_term walks it; the firmware saves it
 * (term_scan.c) whenever `dirty` is set, and BLE edits it (oc_term_gatt.c). */
#ifndef OC_TERM_SCAN_H
#define OC_TERM_SCAN_H

#include <stddef.h>
#include <stdint.h>

#define OC_SCAN_SRC_NONE    0u
#define OC_SCAN_SRC_LAST    1u /* OLED letter L */
#define OC_SCAN_SRC_USER    2u /* U */
#define OC_SCAN_SRC_NET     3u /* N */
#define OC_SCAN_SRC_LEARN   4u /* K */
#define OC_SCAN_SRC_DEFAULT 5u /* D */
#define OC_SCAN_SRC_SWEEP   6u /* S: a fallback channel, not a list entry */

/* Entry flags. Stored entries use only FIXED; oc_term_scan_list adds the rest. */
#define OC_SCAN_F_FIXED      0x01u /* the cell there sends every beacon on its anchor */
#define OC_SCAN_F_SRC_SHIFT  1u
#define OC_SCAN_F_SRC_MASK   0x0Eu /* OC_SCAN_SRC_* << 1 */
#define OC_SCAN_F_ACTIVE     0x10u /* allowed in the current mode: scanned */

#define OC_SCAN_MAX_USER    4u
#define OC_SCAN_MAX_NET     12u
#define OC_SCAN_MAX_LEARN   4u
#define OC_SCAN_N_DEFAULT   6u
#define OC_SCAN_MAX         (1u + OC_SCAN_MAX_USER + OC_SCAN_MAX_NET + OC_SCAN_MAX_LEARN + OC_SCAN_N_DEFAULT)
#define OC_SCAN_NEVER       15u        /* fallback_after: never sweep */
#define OC_SCAN_DEF_AFTER   2u
#define OC_SCAN_DEF_CHUNK   13u        /* the 46 other grid channels in 4 rounds */
#define OC_SCAN_DWELL_US       1200000u /* 10 frames: one 8-frame sync cycle with margin */
#define OC_SCAN_FIXED_DWELL_US 360000u  /* 3 frames: two FIXED beacons */
#define OC_SCAN_BLOB_VER    1u
#define OC_SCAN_BLOB_MAX    160u

typedef struct {
    uint32_t freq_hz;
    uint8_t  flags;
} oc_scan_ent_t;

typedef struct {
    /* persisted (oc_term_scan_pack) */
    uint8_t       mode;           /* OC_PHY_MODE_*: the last registered mode (Part 15 until one is known) */
    uint8_t       fallback_after; /* rounds before the first sweep, 0..15; OC_SCAN_NEVER never */
    uint8_t       fallback_chunk; /* grid channels swept per round, 1..52 */
    uint8_t       net_ver;        /* version of the network entries (CHAN_LIST ver) */
    uint8_t       n_user, n_net, n_learn;
    oc_scan_ent_t last;           /* freq_hz 0: none yet */
    oc_scan_ent_t user[OC_SCAN_MAX_USER];
    oc_scan_ent_t net[OC_SCAN_MAX_NET];
    oc_scan_ent_t learn[OC_SCAN_MAX_LEARN];
    int           dirty;          /* a persisted field changed since the caller last saved */

    /* the walk (not persisted) */
    uint8_t       pos;            /* 0-based position in the round */
    uint8_t       passes;         /* rounds finished since oc_term_scan_restart (saturates) */
    uint8_t       sweep_next;     /* the sweep resumes at this grid channel */
    uint8_t       sweep_ch;       /* channel of the current dwell when it is a sweep */
    uint8_t       cur_pos;        /* the dwell oc_term_scan_next last returned: 1-based position */
    uint8_t       cur_len;        /* ...the round's length */
    uint8_t       cur_src;        /* ...its OC_SCAN_SRC_* */
    uint32_t      cur_freq;       /* ...its frequency */
} oc_term_scan_t;

/* Empty list, Part 15, fallback 2 / 13, walk at the start. */
void    oc_term_scan_init(oc_term_scan_t *s);

/* The assembled list (priority order, flags with source and ACTIVE), into
 * out[OC_SCAN_MAX]: every stored entry, duplicates included, so at most
 * OC_SCAN_MAX. Returns the count. The walk's positions (cur_pos, cur_len)
 * count active entries without duplicates and the sweep, not this list. */
uint8_t oc_term_scan_list(const oc_term_scan_t *s, oc_scan_ent_t out[OC_SCAN_MAX]);

/* A search starts: first entry, no rounds done (the sweep keeps its place). */
void    oc_term_scan_restart(oc_term_scan_t *s);

/* The dwell to run now: its frequency and length. Sets cur_*. Calling it
 * again before oc_term_scan_advance returns the same dwell. */
void    oc_term_scan_next(oc_term_scan_t *s, uint32_t *freq_hz, uint32_t *dwell_us);

/* The dwell is over and found nothing: on to the next. Returns 1 if that
 * finished a round, else 0. */
int     oc_term_scan_advance(oc_term_scan_t *s);

/* ---- changes; each sets `dirty` when a persisted field changes ---- */

/* The mode of the last registration (OC_PHY_MODE_PART15/97; others ignored). */
void    oc_term_scan_set_mode(oc_term_scan_t *s, uint8_t mode);
/* The terminal attached to the cell with this anchor: it becomes the last
 * serving entry and the previous one moves to the front of the learned
 * entries (at most OC_SCAN_MAX_LEARN, oldest dropped). Unchanged: no-op. */
void    oc_term_scan_serving(oc_term_scan_t *s, uint32_t anchor_hz, int fixed);
/* The network's entries (CHAN_LIST), in its order. Invalid ones are kept
 * (they show as inactive); more than OC_SCAN_MAX_NET are cut. The same
 * version with the same entries (after the cut; flags other than FIXED
 * ignored): no-op. */
void    oc_term_scan_set_net(oc_term_scan_t *s, uint8_t ver, uint8_t count, const oc_scan_ent_t *e);
/* The user's entries (BLE SCAN SET_USER). Each must be a 915 grid channel;
 * flags other than FIXED are ignored; the same frequency and FIXED flag twice
 * is refused. 0, or -1 (nothing changed). */
int     oc_term_scan_set_user(oc_term_scan_t *s, uint8_t count, const oc_scan_ent_t *e);
/* after 0..15 (OC_SCAN_NEVER never), chunk 1..52. 0, or -1 (nothing changed). */
int     oc_term_scan_set_fallback(oc_term_scan_t *s, uint8_t after, uint8_t chunk);
void    oc_term_scan_forget_learned(oc_term_scan_t *s);
/* DEACTIVATE: the network's and the learned entries go; the user's stay. */
void    oc_term_scan_deactivate(oc_term_scan_t *s);

/* ---- NVS blob (spec §5.4), little-endian:
 * 0 version 1 | 1 mode | 2 fallback_after | 3 fallback_chunk | 4 net_ver |
 * 5 n_user | 6 n_net | 7 n_learn | 8 last (freq_hz 4, flags 1) |
 * 13 entries, 5 bytes each: user, net, learned | CRC-16/CCITT-FALSE (oc_crc16)
 * over everything before it. 15 + 5 x entries bytes, at most 115. */
size_t  oc_term_scan_pack(const oc_term_scan_t *s, uint8_t out[OC_SCAN_BLOB_MAX]);
/* 0, or -1 (wrong version, length, CRC or a field out of range): then *s is
 * untouched, so a corrupt blob leaves the defaults. The walk is kept. */
int     oc_term_scan_unpack(oc_term_scan_t *s, const uint8_t *in, size_t len);

#endif
