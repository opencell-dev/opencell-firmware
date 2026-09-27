/* The terminal's OLED screens as text lines (spec 2026-09-27-ble-pairing-design.md §3).
 * Drawn with lc_oled_render_lines: line 0 on the yellow rows, the rest from
 * page 2 down; the Pairing screen draws line 1 (the code) in large digits.
 * Unused lines are empty strings. Portable: no ESP-IDF. */
#ifndef LC_TERM_SCREEN_H
#define LC_TERM_SCREEN_H

#include "lc_sig.h"
#include "lc_term.h"

#define LC_TERM_SCREEN_LINES 7 /* page 0, then pages 2..7 */
#define LC_TERM_SCREEN_COLS  21

typedef enum {
    LC_SCREEN_STATUS = 0, /* link: state, band/tier, RSSI/SNR, TMID, cell */
    LC_SCREEN_PAIRING,    /* pairing code, bonds, phone, lock-out */
    LC_SCREEN_SUBSCRIBER, /* number, signalling state, mode */
    LC_SCREEN_RADIO,      /* band, tier, RSSI, SNR, frame, counters */
    LC_SCREEN_COUNT
} lc_term_screen_t;

typedef struct {
    uint32_t code;      /* 0..999999 */
    uint32_t locked_s;  /* > 0: pairing locked for this many more seconds */
    uint8_t  bonds;     /* bonded phones */
    uint8_t  max_bonds;
    uint8_t  phone;     /* 1: a phone is connected */
    uint8_t  cleared;   /* 1: bonds were just cleared (confirmation) */
} lc_term_pair_view_t;

typedef struct {
    uint8_t sig_ok;     /* 0: the crypto self-test failed, signalling is off */
    uint8_t state;      /* lc_sig_state_t */
    uint8_t activated;
    uint8_t mode;       /* REG_ACK mode: LC_SIG_MODE_PART15/97, 0 none yet */
    uint8_t number[LC_SIG_NUMBER_LEN];
} lc_term_sub_view_t;

typedef struct {
    lc_term_status_t    link;
    uint32_t            beacons;     /* beacons heard since boot */
    uint32_t            sync_losses;
    lc_term_sub_view_t  sub;
    lc_term_pair_view_t pair;
} lc_term_view_t;

typedef char lc_term_lines_t[LC_TERM_SCREEN_LINES][LC_TERM_SCREEN_COLS + 1];

void lc_term_status_lines(const lc_term_status_t *st, lc_term_lines_t lines);
void lc_term_pair_lines(const lc_term_pair_view_t *v, lc_term_lines_t lines);
void lc_term_sub_lines(const lc_term_sub_view_t *v, lc_term_lines_t lines);
void lc_term_radio_lines(const lc_term_view_t *v, lc_term_lines_t lines);
/* One of the above by screen number (out of range: Status). */
void lc_term_screen_lines(uint8_t screen, const lc_term_view_t *v, lc_term_lines_t lines);

#endif
