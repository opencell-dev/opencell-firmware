/* lc_term_scan — the terminal's scan list (spec
 * 2026-09-27-channel-list-design.md §5): which frequency each search dwell
 * listens on, and for how long.
 *
 * The list is, in order: the last serving cell's anchor (1), the user's
 * entries (4, set over BLE), the network's (12, from CHAN_LIST), learned
 * anchors (4, earlier serving cells, most recent first) and the six default
 * anchors, ch 0-5 (the seed-derived ones, so an unconfigured network is still
 * found). An entry is active when lc_sync_anchor_ok(mode, freq, pattern)
 * allows it; inactive entries are kept (the mode can change) but never
 * scanned. Duplicates are entries with the same frequency and FIXED flag:
 * the first active one wins, and an inactive entry never hides an active one.
 *
 * A round is one pass over the active entries, then, once `fallback_after`
 * rounds are done (LC_SCAN_NEVER: never), up to `fallback_chunk` grid
 * channels that no active CYCLE entry covers, round-robin across rounds.
 * CYCLE entries and swept channels get LC_SCAN_DWELL_US, FIXED entries
 * LC_SCAN_FIXED_DWELL_US.
 *
 * Pure C: no radio, no NVS, no malloc. lc_term walks it; the firmware saves it
 * (term_scan.c) whenever `dirty` is set, and BLE edits it (lc_term_gatt.c). */
#ifndef LC_TERM_SCAN_H
#define LC_TERM_SCAN_H

#include <stddef.h>
#include <stdint.h>

#define LC_SCAN_SRC_NONE    0u
#define LC_SCAN_SRC_LAST    1u /* OLED letter L */
#define LC_SCAN_SRC_USER    2u /* U */
#define LC_SCAN_SRC_NET     3u /* N */
#define LC_SCAN_SRC_LEARN   4u /* K */
#define LC_SCAN_SRC_DEFAULT 5u /* D */
#define LC_SCAN_SRC_SWEEP   6u /* S: a fallback channel, not a list entry */

/* Entry flags. Stored entries use only FIXED; lc_term_scan_list adds the rest. */
#define LC_SCAN_F_FIXED      0x01u /* the cell there sends every beacon on its anchor */
#define LC_SCAN_F_SRC_SHIFT  1u
#define LC_SCAN_F_SRC_MASK   0x0Eu /* LC_SCAN_SRC_* << 1 */
#define LC_SCAN_F_ACTIVE     0x10u /* allowed in the current mode: scanned */

#define LC_SCAN_MAX_USER    4u
#define LC_SCAN_MAX_NET     12u
#define LC_SCAN_MAX_LEARN   4u
#define LC_SCAN_N_DEFAULT   6u
#define LC_SCAN_MAX         (1u + LC_SCAN_MAX_USER + LC_SCAN_MAX_NET + LC_SCAN_MAX_LEARN + LC_SCAN_N_DEFAULT)
#define LC_SCAN_NEVER       15u        /* fallback_after: never sweep */
#define LC_SCAN_DEF_AFTER   2u
#define LC_SCAN_DEF_CHUNK   13u        /* the 46 other grid channels in 4 rounds */
#define LC_SCAN_DWELL_US       1200000u /* 10 frames: one 8-frame sync cycle with margin */
#define LC_SCAN_FIXED_DWELL_US 360000u  /* 3 frames: two FIXED beacons */
#define LC_SCAN_BLOB_VER    1u
#define LC_SCAN_BLOB_MAX    160u

typedef struct {
    uint32_t freq_hz;
    uint8_t  flags;
} lc_scan_ent_t;

typedef struct {
    /* persisted (lc_term_scan_pack) */
    uint8_t       mode;           /* LC_PHY_MODE_*: the last registered mode (Part 15 until one is known) */
    uint8_t       fallback_after; /* rounds before the first sweep, 0..15; LC_SCAN_NEVER never */
    uint8_t       fallback_chunk; /* grid channels swept per round, 1..52 */
    uint8_t       net_ver;        /* version of the network entries (CHAN_LIST ver) */
    uint8_t       n_user, n_net, n_learn;
    lc_scan_ent_t last;           /* freq_hz 0: none yet */
    lc_scan_ent_t user[LC_SCAN_MAX_USER];
    lc_scan_ent_t net[LC_SCAN_MAX_NET];
    lc_scan_ent_t learn[LC_SCAN_MAX_LEARN];
    int           dirty;          /* a persisted field changed since the caller last saved */

    /* the walk (not persisted) */
    uint8_t       pos;            /* 0-based position in the round */
    uint8_t       passes;         /* rounds finished since lc_term_scan_restart (saturates) */
    uint8_t       sweep_next;     /* the sweep resumes at this grid channel */
    uint8_t       sweep_ch;       /* channel of the current dwell when it is a sweep */
    uint8_t       cur_pos;        /* the dwell lc_term_scan_next last returned: 1-based position */
    uint8_t       cur_len;        /* ...the round's length */
    uint8_t       cur_src;        /* ...its LC_SCAN_SRC_* */
    uint32_t      cur_freq;       /* ...its frequency */
} lc_term_scan_t;

/* Empty list, Part 15, fallback 2 / 13, walk at the start. */
void    lc_term_scan_init(lc_term_scan_t *s);

/* The assembled list (priority order, flags with source and ACTIVE), into
 * out[LC_SCAN_MAX]. Returns the count. */
uint8_t lc_term_scan_list(const lc_term_scan_t *s, lc_scan_ent_t out[LC_SCAN_MAX]);

/* A search starts: first entry, no rounds done (the sweep keeps its place). */
void    lc_term_scan_restart(lc_term_scan_t *s);

/* The dwell to run now: its frequency and length. Sets cur_*. Calling it
 * again before lc_term_scan_advance returns the same dwell. */
void    lc_term_scan_next(lc_term_scan_t *s, uint32_t *freq_hz, uint32_t *dwell_us);

/* The dwell is over and found nothing: on to the next. Returns 1 if that
 * finished a round, else 0. */
int     lc_term_scan_advance(lc_term_scan_t *s);

#endif
