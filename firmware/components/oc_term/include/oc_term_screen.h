/* The terminal's OLED screens as text lines (spec 2026-09-27-ble-pairing-design.md §3).
 * Drawn with oc_oled_render_lines: line 0 on the yellow rows, the rest from
 * page 2 down; the Pairing screen draws line 1 (the code) in large digits.
 * Unused lines are empty strings. Portable: no ESP-IDF. */
#ifndef OC_TERM_SCREEN_H
#define OC_TERM_SCREEN_H

#include "oc_sig.h"
#include "oc_term.h"

#define OC_TERM_SCREEN_LINES 7 /* page 0, then pages 2..7 */
#define OC_TERM_SCREEN_COLS  21

typedef enum {
    OC_SCREEN_STATUS = 0, /* link: state, band/tier, RSSI/SNR, TMID, cell */
    OC_SCREEN_PAIRING,    /* pairing code, bonds, phone, lock-out */
    OC_SCREEN_SUBSCRIBER, /* number, signalling state, mode */
    OC_SCREEN_RADIO,      /* band, tier, RSSI, SNR, frame, counters */
    OC_SCREEN_COUNT
} oc_term_screen_t;

typedef struct {
    uint32_t code;      /* 0..999999 */
    uint32_t locked_s;  /* > 0: pairing locked for this many more seconds */
    uint8_t  bonds;     /* bonded phones */
    uint8_t  max_bonds;
    uint8_t  phone;     /* 1: a phone is connected */
    uint8_t  cleared;   /* 1: bonds were just cleared (confirmation) */
} oc_term_pair_view_t;

typedef struct {
    uint8_t sig_ok;     /* 0: the crypto self-test failed, signalling is off */
    uint8_t state;      /* oc_sig_state_t */
    uint8_t activated;
    uint8_t mode;       /* REG_ACK mode: OC_SIG_MODE_PART15/97, 0 none yet */
    uint8_t number[OC_SIG_NUMBER_LEN];
} oc_term_sub_view_t;

typedef struct {
    oc_term_status_t    link;
    uint32_t            beacons;     /* beacons heard since boot */
    uint32_t            sync_losses;
    oc_term_sub_view_t  sub;
    oc_term_pair_view_t pair;
} oc_term_view_t;

typedef char oc_term_lines_t[OC_TERM_SCREEN_LINES][OC_TERM_SCREEN_COLS + 1];

void oc_term_status_lines(const oc_term_status_t *st, oc_term_lines_t lines);
void oc_term_pair_lines(const oc_term_pair_view_t *v, oc_term_lines_t lines);
void oc_term_sub_lines(const oc_term_sub_view_t *v, oc_term_lines_t lines);
void oc_term_radio_lines(const oc_term_view_t *v, oc_term_lines_t lines);
/* One of the above by screen number (out of range: Status). */
void oc_term_screen_lines(uint8_t screen, const oc_term_view_t *v, oc_term_lines_t lines);

#endif
