#include "lc_term_screen.h"

#include <stdio.h>

static const char *state_name(uint8_t s)
{
    static const char *names[] = { "SEARCHING", "SYNCED", "ATTACHING", "IDLE", "GRANTED" };
    return s < 5 ? names[s] : "?";
}

void lc_term_status_lines(const lc_term_status_t *st, char lines[LC_TERM_SCREEN_LINES][LC_TERM_SCREEN_COLS + 1])
{
    static const char *tiers[] = { "NEAR", "MID", "EDGE" };
    const size_t n = LC_TERM_SCREEN_COLS + 1;
    snprintf(lines[0], n, "OPENCELL %s", state_name(st->state));
    if (st->state >= LC_TERM_IDLE) {
        snprintf(lines[1], n, "%s %s", st->band == LC_BAND_2G4 ? "2.4GHZ" : "915MHZ",
                 st->tier < 3 ? tiers[st->tier] : "?");
    } else {
        snprintf(lines[1], n, "NO SERVICE");
    }
    snprintf(lines[2], n, "RSSI %d SNR %d", st->rssi_dbm, st->snr_qdb / 4);
    snprintf(lines[3], n, "TMID %08X", (unsigned)st->tmid);
    snprintf(lines[4], n, "CELL %08X", (unsigned)st->cell_seed);
}
