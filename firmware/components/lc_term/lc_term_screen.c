#include "lc_term_screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N (LC_TERM_SCREEN_COLS + 1)

static void blank(lc_term_lines_t lines)
{
    memset(lines, 0, sizeof(lc_term_lines_t));
}

static const char *state_name(uint8_t s)
{
    static const char *names[] = { "SEARCHING", "SYNCED", "ATTACHING", "IDLE", "GRANTED" };
    return s < 5 ? names[s] : "?";
}

/* "2.4GHZ NEAR", or "NO SERVICE" before the terminal is attached. */
static void band_tier(const lc_term_status_t *st, char *out)
{
    static const char *tiers[] = { "NEAR", "MID", "EDGE" };
    if (st->state >= LC_TERM_IDLE) {
        snprintf(out, N, "%s %s", st->band == LC_BAND_2G4 ? "2.4GHZ" : "915MHZ", st->tier < 3 ? tiers[st->tier] : "?");
    } else {
        snprintf(out, N, "NO SERVICE");
    }
}

/* "NOISE -118 DBM", or "NOISE -" before a sample (or without rssi_inst). */
static void noise_line(const lc_term_status_t *st, char *out)
{
    if (st->noise_dbm != LC_TERM_NO_DBM) {
        snprintf(out, N, "NOISE %d DBM", st->noise_dbm);
    } else {
        snprintf(out, N, "NOISE -");
    }
}

void lc_term_status_lines(const lc_term_status_t *st, lc_term_lines_t lines)
{
    blank(lines);
    snprintf(lines[0], N, "OPENCELL %s", state_name(st->state));
    band_tier(st, lines[1]);
    if (st->state == LC_TERM_SEARCH) {
        /* No cell (spec §3.1): what the scan hears; no CELL line. */
        if (st->heard) {
            snprintf(lines[2], N, "SIG %d SNR %d", st->rssi_dbm, st->snr_qdb / 4);
            snprintf(lines[3], N, "HEARD %uS AGO", (unsigned)st->heard_age_s);
        } else {
            snprintf(lines[2], N, "NO SIGNAL");
            noise_line(st, lines[3]);
        }
        snprintf(lines[4], N, "TMID %08X", (unsigned)st->tmid);
        return;
    }
    snprintf(lines[2], N, "RSSI %d SNR %d", st->rssi_dbm, st->snr_qdb / 4);
    snprintf(lines[3], N, "TMID %08X", (unsigned)st->tmid);
    snprintf(lines[4], N, "CELL %08X", (unsigned)st->cell_seed);
}

void lc_term_pair_lines(const lc_term_pair_view_t *v, lc_term_lines_t lines)
{
    blank(lines);
    snprintf(lines[0], N, "PAIR CODE");
    if (v->locked_s > 0) {
        snprintf(lines[1], N, "LOCKED %uS", (unsigned)v->locked_s);
    } else {
        snprintf(lines[1], N, "%06u", (unsigned)(v->code % 1000000u));
    }
    if (v->cleared) {
        snprintf(lines[2], N, "BONDS CLEARED");
    } else {
        snprintf(lines[2], N, "BONDED %u/%u", v->bonds, v->max_bonds);
    }
    snprintf(lines[3], N, "%s", v->phone ? "PHONE CONNECTED" : "NO PHONE");
}

static const char *sig_name(uint8_t s)
{
    static const char *names[] = { "NOT ACTIVATED", "ACTIVATING", "REGISTERING", "REGISTERED", "CALLING",
                                   "RINGING OUT",   "RINGING IN", "IN CALL",     "RELEASING" };
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

void lc_term_sub_lines(const lc_term_sub_view_t *v, lc_term_lines_t lines)
{
    blank(lines);
    snprintf(lines[0], N, "SUBSCRIBER");
    if (!v->sig_ok) {
        snprintf(lines[1], N, "SIGNALLING OFF");
        return;
    }
    if (v->activated) {
        char num[LC_SIG_NUMBER_SHOW]; /* "+883-1-606-555-01234": 20 of the 21 columns */
        if (lc_sig_number_format(v->number, num, sizeof(num)) == 0) {
            lc_sig_number_to_text(v->number, num); /* not a valid number: its digits as they are */
        }
        snprintf(lines[1], N, "%s", num);
    } else {
        snprintf(lines[1], N, "NO NUMBER");
    }
    snprintf(lines[2], N, "%s", sig_name(v->state));
    snprintf(lines[3], N, "MODE %s",
             v->mode == LC_SIG_MODE_PART15 ? "PART 15" : v->mode == LC_SIG_MODE_PART97 ? "PART 97" : "-");
}

void lc_term_radio_lines(const lc_term_view_t *v, lc_term_lines_t lines)
{
    blank(lines);
    const lc_term_status_t *st = &v->link;
    int search = st->state == LC_TERM_SEARCH;
    int tenths = st->snr_qdb * 10 / 4; /* 0.25 dB steps, shown to 0.1 dB (truncated) */
    snprintf(lines[0], N, "RADIO");
    band_tier(st, lines[1]);
    if (search && !st->heard) {
        snprintf(lines[2], N, "NO SIGNAL");
        snprintf(lines[3], N, "SNR -");
    } else {
        /* SIG: the strongest packet the scan heard, from any cell (spec §3.1) */
        snprintf(lines[2], N, "%s %d DBM", search ? "SIG" : "RSSI", st->rssi_dbm);
        snprintf(lines[3], N, "SNR %s%d.%d DB", tenths < 0 ? "-" : "", abs(tenths) / 10, abs(tenths) % 10);
    }
    if (search) {
        noise_line(st, lines[4]); /* no frame timing while searching */
    } else {
        snprintf(lines[4], N, "FRAME %u", (unsigned)st->frame);
    }
    snprintf(lines[5], N, "BEACONS %u", (unsigned)v->beacons);
    snprintf(lines[6], N, "SYNC LOSS %u", (unsigned)v->sync_losses);
}

void lc_term_screen_lines(uint8_t screen, const lc_term_view_t *v, lc_term_lines_t lines)
{
    switch (screen) {
    case LC_SCREEN_PAIRING:
        lc_term_pair_lines(&v->pair, lines);
        break;
    case LC_SCREEN_SUBSCRIBER:
        lc_term_sub_lines(&v->sub, lines);
        break;
    case LC_SCREEN_RADIO:
        lc_term_radio_lines(v, lines);
        break;
    default:
        lc_term_status_lines(&v->link, lines);
        break;
    }
}
