/* lcbench — bench bring-up tool for W12 bs-radio boards (Linux).
 *
 *   lcbench status <tty>
 *   lcbench config <tty> <bs|bench> <915|2g4> <radio_index> <cell_seed_hex>
 *   lcbench flash  <tty> <app.bin>
 *   lcbench link   <tx_tty> <rx_tty> <freq_hz> <near|mid|edge> <frames> [--internal] [--offset-us N]
 *                  [--rx-window-us N] [--len N]
 *   lcbench cw     <tx_tty> <freq_hz> <near|mid|edge> <frames> [--internal] [--len N]
 *   lcbench guard  <tx_tty> <rx_tty> <freq_hz> <tier_b> <frames> --a <tier_a> <tx|rx> --gap-us N
 *                  [--freq-a HZ] [--len N]      (needs shared GPS PPS)
 *   lcbench cell   <tty_915> <near|mid|edge> <seconds> [--tty-2g4 TTY] [--dl 915|2g4] [--ul 915|2g4]
 *                  [--seed HEX] [--idle] [--page-after S] [--fallback-915] [--internal] [--one-board]
 *                  [--drop-2g4-after S]  (one-board: stop serving 2.4 GHz, to test fallback)
 *                  [--sync-ch N] [--fixed-sync]  (anchor channel 0-51, default seed % 6; FIXED:
 *                  every beacon on it, Part 97 only: `net --mode part97`)
 *                  Runs a minimal cell (lcb_cell.h) for terminal bring-up; 2.4 GHz legs need
 *                  --tty-2g4 and shared GPS PPS, or --one-board (one W12 switches bands per slot).
 *   lcbench mkqr   --number +883-1-606-555-01234 [--hss FILE] [--expires-h H] [--mode part15|part97]
 *                  Plays the web portal: issues an activation token, prints the QR text (and
 *                  the QR itself with qrencode, if installed). Refused while `lcbench net` runs
 *                  on the same HSS (it holds FILE.lock): stop net, mkqr, start net again.
 *   lcbench net    <tty_915> <near|mid|edge> <seconds> [--hss FILE] [--mode part15|part97]
 *                  [--call-in +883-1-... --after S] [--peer-hangup S]
 *                  [--chan-list MHZ[:fixed],...] [--list-ver N] [--bump-list-after S] [cell options]
 *                  `cell` plus the network stand-in (lcb_net.h): activation, registration, calls
 *                  to a simulated far end that answers after 3 s (and hangs up S s after connect
 *                  with --peer-hangup), app data echo. It pushes a channel list (CHAN_LIST) after
 *                  every registration: --chan-list (at most 12 grid channels, in order), or the
 *                  cell's own anchor; its version (--list-ver 0-255, default 1) goes in the beacon
 *                  mod 4, and --bump-list-after S (1-86400; list-ver at most 254 then) adds 1 to
 *                  it S s in (terminals then ask for the list). Bad option values exit 1 before
 *                  the HSS or a board is touched.
 *
 * The host clock must be NTP/GPS-disciplined. With GPS PPS wired to every
 * board, TIME labels are exact and frames agree with the host (default).
 * With --internal (boards configured as "bench", internal 1 Hz PPS), each
 * board's frame numbering is learned from its STATUS messages. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lc_clock.h"
#include "lc_link.h"
#include "lcb_cell.h"
#include "lcb_hss.h"
#include "lcb_merge.h"
#include "lcb_net.h"
#include "lc_exec.h" /* LC_EXEC_BAND_SWITCH_LEAD_US */
#include "lcbench_core.h"

typedef struct {
    int         fd;
    const char *name;
    lc_framer_t framer;
    uint8_t     seq;
    /* --internal: board frame = status_frame + frames elapsed since status_at */
    uint32_t    status_frame;
    uint64_t    status_at_us;
    int         have_status;
    uint32_t    acks_err;
    uint64_t    opened_us;        /* STATUS queued on the board before we opened is stale */
    uint8_t     sent_type[256];   /* message type per seq, to classify ACKs */
    uint32_t    ack_err_by[16][8]; /* [msg type][ack status] */
} board_t;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int open_board(board_t *b, const char *path)
{
    memset(b, 0, sizeof(*b));
    b->name = path;
    b->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (b->fd < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    struct termios t;
    tcgetattr(b->fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, B2000000);
    cfsetospeed(&t, B2000000);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(tcflag_t)HUPCL; /* dropping DTR/RTS on close resets an ESP32-S3 on its USB port */
    tcsetattr(b->fd, TCSANOW, &t);
    tcflush(b->fd, TCIOFLUSH);
    lc_framer_init(&b->framer);
    b->opened_us = now_us();
    return 0;
}

static int send_msg(board_t *b, lc_msg_t *m)
{
    static uint8_t wire[LC_FRAMER_RAW_CAP + 2];
    b->sent_type[b->seq] = m->type;
    m->seq = b->seq++;
    size_t n = lc_link_write_frame(m, wire, sizeof(wire));
    if (n == 0) {
        fprintf(stderr, "%s: message too large\n", b->name);
        return -1;
    }
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(b->fd, wire + off, n - off);
        if (w < 0 && errno != EAGAIN) {
            return -1;
        }
        if (w > 0) {
            off += (size_t)w;
        }
    }
    return m->seq;
}

typedef void (*on_msg_fn)(board_t *b, const lc_msg_t *m, void *ctx);

/* Read whatever is pending on the boards for up to timeout_ms. */
static void pump(board_t **boards, int n, int timeout_ms, on_msg_fn cb, void *ctx)
{
    struct pollfd pfd[2];
    for (int i = 0; i < n; i++) {
        pfd[i] = (struct pollfd){ boards[i]->fd, POLLIN, 0 };
    }
    if (poll(pfd, (nfds_t)n, timeout_ms) <= 0) {
        return;
    }
    static uint8_t buf[4096];
    static lc_msg_t m;
    for (int i = 0; i < n; i++) {
        if (!(pfd[i].revents & POLLIN)) {
            continue;
        }
        ssize_t r = read(boards[i]->fd, buf, sizeof(buf));
        for (ssize_t k = 0; k < r; k++) {
            if (lc_framer_push(&boards[i]->framer, buf[k], &m)) {
                board_t *b = boards[i];
                /* A W12 on USB queues heartbeats while nobody reads: the first
                 * ones after opening are seconds old and would seed a stale
                 * frame estimate (schedules then arrive LATE). */
                if (m.type == LC_MSG_STATUS && m.u.status.frame_number != 0 &&
                    now_us() - b->opened_us > 1500000u) {
                    b->status_frame = m.u.status.frame_number;
                    b->status_at_us = now_us();
                    b->have_status = 1;
                } else if (m.type == LC_MSG_ACK && m.u.ack.status != LC_ACK_OK) {
                    b->acks_err++;
                    b->ack_err_by[b->sent_type[m.u.ack.acked_seq] & 15][m.u.ack.status & 7]++;
                }
                if (cb) {
                    cb(b, &m, ctx);
                }
            }
        }
    }
}

/* Wait for the ACK of seq; returns its status or -1 on timeout. */
static int wait_ack(board_t *b, int seq, int timeout_ms)
{
    static lc_msg_t m;
    uint64_t end = now_us() + (uint64_t)timeout_ms * 1000u;
    static uint8_t buf[512];
    while (now_us() < end) {
        struct pollfd p = { b->fd, POLLIN, 0 };
        if (poll(&p, 1, 20) <= 0) {
            continue;
        }
        ssize_t r = read(b->fd, buf, sizeof(buf));
        for (ssize_t k = 0; k < r; k++) {
            if (lc_framer_push(&b->framer, buf[k], &m) && m.type == LC_MSG_ACK && m.u.ack.acked_seq == (uint8_t)seq) {
                return m.u.ack.status;
            }
        }
    }
    return -1;
}

static void print_status(board_t *b, const lc_msg_t *m, void *ctx)
{
    (void)ctx;
    static const char *st[] = { "UNLOCKED", "LOCKED", "HOLDOVER" };
    if (m->type == LC_MSG_STATUS) {
        const lc_status_t *s = &m->u.status;
        printf("%s: up %u ms  clock %s  frame %u  misses %u  uart_err %u  temp %d C\n", b->name,
               s->uptime_ms, s->pps_locked < 3 ? st[s->pps_locked] : "?", s->frame_number,
               s->schedule_misses, s->uart_crc_errors, s->temp_c);
    }
}

static int cmd_status(const char *tty)
{
    board_t b;
    if (open_board(&b, tty) != 0) return 1;
    board_t *bs[1] = { &b };
    uint64_t end = now_us() + 3000000u;
    while (now_us() < end) {
        pump(bs, 1, 100, print_status, NULL);
    }
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    if (argc != 7) return 2;
    board_t b;
    if (open_board(&b, argv[2]) != 0) return 1;
    lc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_MSG_CONFIG;
    m.u.config.role = strcmp(argv[3], "bench") == 0 ? LC_ROLE_BS_RADIO_BENCH : LC_ROLE_BS_RADIO;
    m.u.config.band = strcmp(argv[4], "2g4") == 0 ? LC_BAND_2G4 : LC_BAND_915;
    m.u.config.radio_index = (uint8_t)atoi(argv[5]);
    m.u.config.cell_seed = (uint32_t)strtoul(argv[6], NULL, 16);
    int st = wait_ack(&b, send_msg(&b, &m), 1000);
    printf("config: ack %d (0 = OK; the board restarts to apply a new band/role)\n", st);
    return st == 0 ? 0 : 1;
}

static int cmd_flash(const char *tty, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return 1;
    }
    board_t b;
    if (open_board(&b, tty) != 0) return 1;
    static uint8_t chunk[LC_MAX_FW_CHUNK];
    uint32_t off = 0;
    size_t n;
    lc_msg_t m;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        memset(&m, 0, sizeof(m));
        m.type = LC_MSG_FW_CHUNK;
        m.u.fw_chunk = (lc_fw_chunk_t){ off, (uint16_t)n, chunk };
        int st = -1;
        for (int tries = 0; tries < 3 && st != 0; tries++) {
            st = wait_ack(&b, send_msg(&b, &m), 2000);
        }
        if (st != 0) {
            fprintf(stderr, "chunk at %u failed: %d\n", off, st);
            fclose(f);
            return 1;
        }
        off += (uint32_t)n;
        if ((off / LC_MAX_FW_CHUNK) % 64 == 0) {
            printf("\r%u bytes", off);
            fflush(stdout);
        }
    }
    fclose(f);
    memset(&m, 0, sizeof(m));
    m.type = LC_MSG_FW_COMMIT;
    m.u.fw_commit.image_size = off;
    int st = wait_ack(&b, send_msg(&b, &m), 5000);
    printf("\ncommit %u bytes: ack %d (0 = OK, board reboots into the new image)\n", off, st);
    return st == 0 ? 0 : 1;
}

typedef struct {
    lcb_stats_t stats;
    board_t    *rx;
    board_t    *tx;
    lcb_stats_t tx_done; /* TX board's TX_DONE times from STATUS */
    lcb_stats_t tx_start; /* and preamble starts */
} link_ctx_t;

/* A STATUS's last TX done, once the board has been open long enough that it
 * can't be left over from an earlier run. */
/* LCB_DUMP=1: one line per timing sample, for distributions. */
static int dump_timing(void)
{
    static int v = -1;
    if (v < 0) v = getenv("LCB_DUMP") != NULL;
    return v;
}

static void note_tx_done(board_t *b, const lc_msg_t *m, lcb_stats_t *done, lcb_stats_t *start)
{
    if (m->type == LC_MSG_STATUS && now_us() - b->opened_us > 3000000u) {
        if (dump_timing()) {
            printf("T %s %u %d %d late %u radio_err %u last %d@%u\n", b->name, m->u.status.frame_number,
                   m->u.status.last_tx_start_us, m->u.status.last_tx_end_us, m->u.status.late_slots,
                   m->u.status.radio_errors, m->u.status.last_radio_err, m->u.status.last_radio_op);
        }
        lcb_stats_add_end(done, m->u.status.last_tx_end_us);
        lcb_stats_add_end(start, m->u.status.last_tx_start_us);
    }
}

/* Which TX frames were scheduled and which arrived, to show where losses fall. */
#define LCB_TRACK 32768
static uint32_t s_sched_frame[LCB_TRACK];
static uint32_t s_sched_n;
static uint8_t s_got[LCB_TRACK];

static void on_link_msg(board_t *b, const lc_msg_t *m, void *vctx)
{
    link_ctx_t *ctx = vctx;
    if (b == ctx->tx) {
        note_tx_done(b, m, &ctx->tx_done, &ctx->tx_start);
    }
    if (b == ctx->rx && m->type == LC_MSG_RX_REPORT) {
        if (dump_timing()) {
            printf("R %u %d\n", m->u.rx_report.frame_number, m->u.rx_report.end_us);
        }
        uint32_t before = ctx->stats.received;
        lcb_stats_add_rx(&ctx->stats, &m->u.rx_report);
        uint32_t f;
        if (ctx->stats.received != before &&
            lcb_check_payload(m->u.rx_report.payload, m->u.rx_report.payload_len, &f) == 0) {
            for (uint32_t i = 0; i < s_sched_n; i++) {
                if (s_sched_frame[i] == f) {
                    s_got[i] = 1;
                    break;
                }
            }
        }
    }
}

static void print_loss_map(void)
{
    uint32_t lost = 0, first = 0, last = 0, tenth = s_sched_n / 10;
    printf("lost at index:");
    for (uint32_t i = 0; i < s_sched_n; i++) {
        if (!s_got[i]) {
            if (lost < 40) printf(" %u", i);
            lost++;
            if (i < tenth) first++;
            if (i >= s_sched_n - tenth) last++;
        }
    }
    printf("%s\nlost %u of %u scheduled: first 10%% %u, last 10%% %u, middle %u\n", lost > 40 ? " ..." : "",
           lost, s_sched_n, first, last, lost - first - last);
}

/* Frame the board will be in at host time t. */
/* Frames ahead of the (estimated) board frame to schedule. STATUS doesn't say
 * where in its frame it was taken, so the estimate can lag the real frame by
 * one: 3 ahead keeps 2-3 frames of real lead (the W12 accepts up to 3). */
#define LCB_LEAD_FRAMES 3u

static int board_frame(const board_t *b, int internal, uint64_t t, uint32_t *out)
{
    if (!internal) {
        *out = lc_frame_from_unix_us(t);
        return 0;
    }
    if (!b->have_status) {
        return -1;
    }
    *out = b->status_frame + (uint32_t)((t - b->status_at_us) / LC_FRAME_US);
    return 0;
}

static void send_time(board_t *b, uint32_t unix_s)
{
    lc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_MSG_TIME;
    m.u.time.unix_s = unix_s;
    send_msg(b, &m);
}

typedef struct {
    int       enabled;
    lc_tier_t tier_a;
    uint8_t   dir_a;
    uint32_t  gap_us;
    uint32_t  freq_a_hz;
} guard_opts_t;

/* Packet-end timing at the receiver. `ideal_us` is the TX slot start plus
 * airtime: where the packet would end with zero TX/RX latency and perfectly
 * aligned frame clocks. */
static void print_timing(const char *indent, const lcb_stats_t *st, uint32_t ideal_us)
{
    if (st->timed == 0) {
        printf("%send: not measured\n", indent);
        return;
    }
    double m = lcb_stats_end_mean(st);
    printf("%send: n %u  mean %.1f  sd %.2f  min %d  max %d us  (mean - ideal %u = %+.1f us, span %d us)\n", indent,
           st->timed, m, lcb_stats_end_sd(st), st->end_min, st->end_max, ideal_us, m - ideal_us,
           st->end_max - st->end_min);
}

static int run_frames(board_t *tx, board_t *rx, const lcb_link_cfg_t *cfg, uint32_t frames, int internal,
                      int cw, const guard_opts_t *g)
{
    board_t *bs[2] = { tx, rx };
    int nb = rx ? 2 : 1;
    link_ctx_t ctx = { .rx = rx, .tx = tx };
    lcb_stats_init(&ctx.stats);
    lcb_stats_init(&ctx.tx_done);
    lcb_stats_init(&ctx.tx_start);
    static uint8_t payload[255];
    static lc_msg_t m;

    uint32_t last_time_s = 0;
    uint32_t sent_frames = 0;
    int have_next = 0;
    uint32_t next_tx = 0, rx_minus_tx = 0;
    uint64_t end_us = 0;
    while (end_us == 0 || now_us() < end_us) {
        pump(bs, nb, 5, on_link_msg, &ctx);
        uint64_t t = now_us();
        uint32_t s = (uint32_t)(t / 1000000u);
        if (s != last_time_s && t % 1000000u > 100000u && t % 1000000u < 800000u) {
            last_time_s = s;
            for (int i = 0; i < nb; i++) {
                send_time(bs[i], s);
            }
        }
        if (sent_frames >= frames) {
            if (end_us == 0) {
                end_us = t + 500000u; /* let the last reports arrive */
            }
            continue;
        }
        uint32_t ftx, frx = 0;
        if (board_frame(tx, internal, t, &ftx) != 0 || (rx && board_frame(rx, internal, t, &frx) != 0)) {
            continue; /* waiting for STATUS */
        }
        /* Schedule by board frame number, not by host-clock ticks: the board-frame
         * estimate drifts against the laptop clock, and one schedule per host tick
         * skipped a board frame now and then (a 3-s periodic loss on the bench).
         * Every frame up to estimate+LEAD is scheduled exactly once, gaps included. */
        uint32_t target = ftx + LCB_LEAD_FRAMES;
        if (!have_next) {
            have_next = 1;
            next_tx = target;
            /* Boards on a shared timebase (GPS PPS) number frames identically; the
             * estimates can each lag by one, so a difference of -1..+1 is 0. */
            int32_t d = (int32_t)(frx - ftx);
            rx_minus_tx = (d >= -1 && d <= 1) ? 0u : (uint32_t)d;
        }
        while ((int32_t)(target - next_tx) >= 0 && sent_frames < frames) {
            uint32_t f = next_tx++;
            if (cw) {
                if (lcb_cw_schedule(cfg, f, payload, &m) < 0) return 2;
                send_msg(tx, &m);
            } else if (g->enabled) {
                if (lcb_guard_schedule(cfg, g->freq_a_hz, g->tier_a, g->dir_a, g->gap_us, f, 1, payload, &m) != 0) {
                    return 2;
                }
                send_msg(tx, &m);
                lcb_guard_schedule(cfg, g->freq_a_hz, g->tier_a, g->dir_a, g->gap_us, f + rx_minus_tx, 0, payload, &m);
                send_msg(rx, &m);
                ctx.stats.sent++;
            } else {
                if (lcb_link_schedule(cfg, f, 1, payload, &m) != 0) return 2;
                if (s_sched_n < LCB_TRACK) {
                    s_sched_frame[s_sched_n++] = f;
                }
                send_msg(tx, &m);
                lcb_link_schedule(cfg, f + rx_minus_tx, 0, payload, &m);
                send_msg(rx, &m);
                ctx.stats.sent++;
            }
            sent_frames++;
        }
    }
    if (cw) {
        printf("cw: %u frames scheduled, tx ack errors %u\n", sent_frames, tx->acks_err);
        return 0;
    }
    lcb_stats_t *st = &ctx.stats;
    printf("sent %u  received %u  crc_fail %u  bad_payload %u  lost %u  PER %.4f\n", st->sent, st->received,
           st->crc_fail, st->bad_payload, st->sent - st->received,
           st->sent ? 1.0 - (double)st->received / st->sent : 0.0);
    if (st->received) {
        printf("rssi avg %.1f min %d max %d dBm  snr avg %.2f dB\n", (double)st->rssi_sum / st->received,
               st->rssi_min, st->rssi_max, (double)st->snr_sum_qdb / st->received / 4.0);
        const lc_mode_t *mode = lc_tier_mode(lcb_band_of(cfg->freq_hz), cfg->tier);
        uint32_t ideal = cfg->offset_us + (mode ? lc_airtime_us(mode, cfg->payload_len) : 0);
        print_timing("", st, ideal);
        print_timing("tx_done ", &ctx.tx_done, ideal);
        print_timing("tx_start ", &ctx.tx_start, cfg->offset_us);
    }
    print_loss_map();
    printf("ack errors: tx %u rx %u\n", tx->acks_err, rx->acks_err);
    static const char *ackname[] = { "ok", "unsupported", "late", "flash", "malformed", "?", "?", "?" };
    for (int w = 0; w < 2; w++) {
        board_t *bb = w ? rx : tx;
        for (int t = 0; t < 16; t++) for (int k = 1; k < 8; k++) if (bb->ack_err_by[t][k])
            printf("  %s: msg type 0x%02x -> %s x%u\n", w ? "rx" : "tx", t, ackname[k], bb->ack_err_by[t][k]);
    }
    return 0;
}

static int parse_opts(int argc, char **argv, int first, lcb_link_cfg_t *cfg, int *internal, guard_opts_t *g)
{
    for (int i = first; i < argc; i++) {
        if (strcmp(argv[i], "--a") == 0 && i + 2 < argc) {
            if (lcb_parse_tier(argv[++i], &g->tier_a) != 0) return -1;
            g->dir_a = strcmp(argv[++i], "rx") == 0 ? LC_DIR_RX : LC_DIR_TX;
        } else if (strcmp(argv[i], "--gap-us") == 0 && i + 1 < argc) {
            g->gap_us = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--freq-a") == 0 && i + 1 < argc) {
            g->freq_a_hz = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--internal") == 0) {
            *internal = 1;
        } else if (strcmp(argv[i], "--offset-us") == 0 && i + 1 < argc) {
            cfg->offset_us = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rx-shift-us") == 0 && i + 1 < argc) {
            cfg->rx_shift_us = (int32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rx-offset-hz") == 0 && i + 1 < argc) {
            cfg->rx_offset_hz = (int32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rx-window-us") == 0 && i + 1 < argc) {
            cfg->rx_window_us = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--len") == 0 && i + 1 < argc) {
            cfg->payload_len = (uint8_t)atoi(argv[++i]);
        } else {
            return -1;
        }
    }
    return 0;
}

typedef struct {
    lcb_cell_t *cell;
    board_t    *b915;
} cell_ctx_t;

/* Terminal timing over the air: each received uplink's packet start, from
 * RX_DONE - airtime - lc_rx_done_lag_us, against its slot start (µs). */
static lcb_stats_t s_term_err[2]; /* per band the base heard it on */
static lcb_one_map_t s_one_map[LCB_ONE_MAP_FRAMES]; /* --one-board (lcb_merge.h) */
static int s_one_board;
static int s_drop_2g4; /* --drop-2g4-after: the base stops serving 2.4 GHz (fallback test) */

static void on_cell_msg(board_t *b, const lc_msg_t *m, void *vctx)
{
    cell_ctx_t *ctx = vctx;
    if (m->type == LC_MSG_RX_REPORT && s_one_board) {
        lc_rx_report_t r = m->u.rx_report;
        const lcb_one_map_t *map = &s_one_map[r.frame_number % LCB_ONE_MAP_FRAMES];
        if (map->frame != r.frame_number || r.slot_index >= map->n) {
            return;
        }
        lc_band_t band = map->band[r.slot_index] ? LC_BAND_2G4 : LC_BAND_915;
        if (r.crc_ok && r.end_us != LC_RX_END_UNKNOWN) {
            const lc_mode_t *md = &map->mode[r.slot_index];
            int32_t start = r.end_us - (int32_t)lc_airtime_us(md, r.payload_len) - (int32_t)lc_rx_done_lag_us(md);
            lcb_stats_add_end(&s_term_err[band], start - (int32_t)map->offset_us[r.slot_index]);
        }
        r.slot_index = map->idx[r.slot_index];
        lcb_cell_on_rx(ctx->cell, band, &r);
    } else if (m->type == LC_MSG_RX_REPORT) {
        lcb_cell_on_rx(ctx->cell, b == ctx->b915 ? LC_BAND_915 : LC_BAND_2G4, &m->u.rx_report);
    }
}

/* ---- network stand-in: HSS file, token issue (mkqr), lcb_net on the cell (net) ---- */

static void urandom(uint8_t *out, size_t n)
{
    while (n > 0) {
        ssize_t r = getrandom(out, n, 0);
        if (r > 0) {
            out += r;
            n -= (size_t)r;
        }
    }
}

/* ~/.config/opencell/hss.txt, creating the directories. */
static const char *hss_default_path(void)
{
    static char path[512];
    const char *home = getenv("HOME");
    snprintf(path, sizeof(path), "%s/.config", home != NULL ? home : ".");
    mkdir(path, 0700);
    strncat(path, "/opencell", sizeof(path) - strlen(path) - 1);
    mkdir(path, 0700);
    strncat(path, "/hss.txt", sizeof(path) - strlen(path) - 1);
    return path;
}

/* Load the HSS, making the network key pair on first use, and apply --mode. */
static int hss_open(lcb_hss_t *h, const char *path, const char *mode)
{
    if (lcb_hss_load(h, path) != 0) {
        if (h->err[0] != '\0') {
            fprintf(stderr, "%s\n", h->err);
        } else {
            fprintf(stderr, "%s: unreadable HSS file\n", path);
        }
        return -1;
    }
    int dirty = !h->have_network;
    if (lcb_hss_ensure_network(h, urandom) != 0) return -1;
    if (mode != NULL) {
        uint8_t m = strcmp(mode, "part97") == 0 ? LC_SIG_MODE_PART97 : LC_SIG_MODE_PART15;
        dirty |= m != h->mode;
        h->mode = m;
    }
    if (dirty && lcb_hss_save(h, path) != 0) {
        fprintf(stderr, "%s: can't write\n", path);
        return -1;
    }
    return 0;
}

static int cmd_mkqr(int argc, char **argv)
{
    const char *number = NULL, *path = NULL, *mode = NULL;
    uint32_t hours = 24;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--number") == 0 && i + 1 < argc) number = argv[++i];
        else if (strcmp(argv[i], "--hss") == 0 && i + 1 < argc) path = argv[++i];
        else if (strcmp(argv[i], "--expires-h") == 0 && i + 1 < argc) hours = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) mode = argv[++i];
        else return 2;
    }
    uint8_t bcd[LC_SIG_NUMBER_LEN];
    if (number == NULL || lc_sig_number_normalize(number, strlen(number), NULL, bcd) != 0) {
        fprintf(stderr, "--number must be a full OpenCell number, e.g. +883-1-606-555-01234\n");
        return 1;
    }
    if (lcb_hss_v1_number(bcd)) {
        fprintf(stderr, "--number %s has 13 digits (numbering v1): use the 15-digit form, e.g. +883-1-606-555-01234\n",
                number);
        return 1;
    }
    if (path == NULL) path = hss_default_path();
    /* lcbench net rewrites the whole HSS on every save: a token added under
     * it would be erased. Held until exit. */
    int lk = lcb_hss_lock(path);
    if (lk == -1) {
        fprintf(stderr, "%s: lcbench net is running on this HSS; stop it first\n", path);
        return 1;
    }
    if (lk < 0) {
        fprintf(stderr, "%s.lock: can't open the lock file\n", path);
        return 1;
    }
    static lcb_hss_t h;
    if (hss_open(&h, path, mode) != 0) return 1;
    lc_sig_sub_t *sub = lcb_hss_issue(&h, bcd, (uint32_t)time(NULL) + hours * 3600u, urandom);
    if (sub == NULL || lcb_hss_save(&h, path) != 0) {
        fprintf(stderr, "%s: HSS full or not writable\n", path);
        return 1;
    }
    lc_sig_qr_t q;
    char text[LC_SIG_QR_TEXT + 1];
    lcb_hss_qr(&h, sub, &q);
    lc_sig_qr_format(&q, text, sizeof(text));
    char num[LC_SIG_NUMBER_TEXT], show[LC_SIG_NUMBER_SHOW];
    lc_sig_number_to_text(sub->number, num);
    lc_sig_number_format(sub->number, show, sizeof(show));
    printf("%s: token for %s (%s), valid %u h, network key %u (%s)\n%s\n", path, num, show, hours, h.key_id,
           h.mode == LC_SIG_MODE_PART97 ? "part97" : "part15", text);
    fflush(stdout); /* qrencode below writes to the inherited stdout fd directly, bypassing our
                      * buffering: flush first so the text line precedes the QR art when piped. */
    FILE *qr = system("command -v qrencode >/dev/null 2>&1") == 0 ? popen("qrencode -t ANSIUTF8", "w") : NULL;
    if (qr != NULL) {
        fputs(text, qr);
        pclose(qr);
    } else {
        printf("(draw it: ~/.venvs/opencell/bin/python tools/qr/qr.py '%s')\n", text);
    }
    return 0;
}

static void net_log(const char *line)
{
    printf("net: %s\n", line);
    fflush(stdout);
}

static int cmd_cell(int argc, char **argv, int net)
{
    if (argc < 5) return 2;
    static lcb_cell_t cell;
    lc_tier_t tier;
    if (lcb_parse_tier(argv[3], &tier) != 0) return 2;
    uint32_t seconds = (uint32_t)atoi(argv[4]);
    const char *tty24 = NULL;
    lc_band_t dl = LC_BAND_915, ul = LC_BAND_915;
    uint32_t seed = 0xCAFEF00Du;
    int idle = 0, internal = 0, fallback = 0;
    uint32_t page_after = 0, drop_after = 0;
    const char *hss_path = NULL, *mode = NULL, *call_in = NULL, *chan_list = NULL;
    uint32_t call_after = 10, peer_hangup = 0, bump_after = 0;
    int sync_ch = -1, fixed_sync = 0, list_ver = 1;
    long v;
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--sync-ch") == 0 && i + 1 < argc) {
            if (lcb_parse_int(argv[++i], 0, 51, &v) != 0) {
                fprintf(stderr, "--sync-ch '%s': a 915 grid channel, 0-51\n", argv[i]);
                return 1;
            }
            sync_ch = (int)v;
        } else if (strcmp(argv[i], "--fixed-sync") == 0) {
            fixed_sync = 1;
        } else if (net && strcmp(argv[i], "--chan-list") == 0 && i + 1 < argc) {
            chan_list = argv[++i];
        } else if (net && strcmp(argv[i], "--list-ver") == 0 && i + 1 < argc) {
            if (lcb_parse_int(argv[++i], 0, 255, &v) != 0) {
                fprintf(stderr, "--list-ver '%s': a version, 0-255\n", argv[i]);
                return 1;
            }
            list_ver = (int)v;
        } else if (net && strcmp(argv[i], "--bump-list-after") == 0 && i + 1 < argc) {
            if (lcb_parse_int(argv[++i], 1, 86400, &v) != 0) {
                fprintf(stderr, "--bump-list-after '%s': seconds, 1-86400\n", argv[i]);
                return 1;
            }
            bump_after = (uint32_t)v;
        } else if (net && strcmp(argv[i], "--hss") == 0 && i + 1 < argc) {
            hss_path = argv[++i];
        } else if (net && strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (net && strcmp(argv[i], "--call-in") == 0 && i + 1 < argc) {
            call_in = argv[++i];
        } else if (net && strcmp(argv[i], "--after") == 0 && i + 1 < argc) {
            call_after = (uint32_t)atoi(argv[++i]);
        } else if (net && strcmp(argv[i], "--peer-hangup") == 0 && i + 1 < argc) {
            peer_hangup = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tty-2g4") == 0 && i + 1 < argc) {
            tty24 = argv[++i];
        } else if (strcmp(argv[i], "--dl") == 0 && i + 1 < argc) {
            dl = strcmp(argv[++i], "2g4") == 0 ? LC_BAND_2G4 : LC_BAND_915;
        } else if (strcmp(argv[i], "--ul") == 0 && i + 1 < argc) {
            ul = strcmp(argv[++i], "2g4") == 0 ? LC_BAND_2G4 : LC_BAND_915;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 16);
        } else if (strcmp(argv[i], "--idle") == 0) {
            idle = 1;
        } else if (strcmp(argv[i], "--page-after") == 0 && i + 1 < argc) {
            page_after = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fallback-915") == 0) {
            fallback = 1;
        } else if (strcmp(argv[i], "--internal") == 0) {
            internal = 1;
        } else if (strcmp(argv[i], "--one-board") == 0) {
            s_one_board = 1;
        } else if (strcmp(argv[i], "--drop-2g4-after") == 0 && i + 1 < argc) {
            drop_after = (uint32_t)atoi(argv[++i]);
        } else {
            return 2;
        }
    }
    /* Two base boards need shared GPS PPS and host-clock framing; one board
     * can take its frame numbers from its own STATUS (--internal). */
    if ((dl == LC_BAND_2G4 || ul == LC_BAND_2G4) && !s_one_board && (tty24 == NULL || internal)) {
        fprintf(stderr, "2.4 GHz legs need --tty-2g4 and shared GPS PPS (no --internal)\n");
        return 1;
    }
    if (bump_after != 0 && list_ver == 255) {
        fprintf(stderr, "--bump-list-after with --list-ver 255: the bump would wrap to 0 (use 0-254)\n");
        return 1;
    }
    static lc_sig_chan_list_t list;
    if (chan_list != NULL) {
        char err[96];
        if (lcb_net_parse_chan_list(chan_list, (uint8_t)list_ver, &list, err, sizeof(err)) != 0) {
            fprintf(stderr, "--chan-list: %s\n", err);
            return 1;
        }
    }
    lcb_cell_init(&cell, seed, tier, dl, ul);
    lcb_stats_init(&s_term_err[0]);
    lcb_stats_init(&s_term_err[1]);
    cell.attach_idle = idle;
    cell.fallback_915 = fallback;
    lc_grant_leg_t l1, l2;
    if (lcb_cell_legs(&cell, 0, &l1, &l2) != 0) {
        fprintf(stderr, "tier/bands don't fit the frame\n");
        return 1;
    }

    static lcb_hss_t hss;
    static lcb_net_t lnet;
    uint8_t call_in_bcd[LC_SIG_NUMBER_LEN];
    if (net) {
        if (hss_path == NULL) hss_path = hss_default_path();
        if (call_in != NULL && lc_sig_number_normalize(call_in, strlen(call_in), NULL, call_in_bcd) != 0) {
            fprintf(stderr, "--call-in must be a full OpenCell number, e.g. +883-1-606-555-01234\n");
            return 2;
        }
        if (call_in != NULL && lcb_hss_v1_number(call_in_bcd)) {
            fprintf(stderr,
                    "--call-in %s has 13 digits (numbering v1): use the 15-digit form, e.g. +883-1-606-555-01234\n",
                    call_in);
            return 1;
        }
        /* The HSS is ours until exit (the fd stays open): mkqr refuses meanwhile. */
        int lk = lcb_hss_lock(hss_path);
        if (lk == -1) {
            fprintf(stderr, "%s: another lcbench (net or mkqr) is using this HSS\n", hss_path);
            return 1;
        }
        if (lk < 0) {
            fprintf(stderr, "%s.lock: can't open the lock file\n", hss_path);
            return 1;
        }
        if (hss_open(&hss, hss_path, mode) != 0) return 1;
        lcb_net_init(&lnet, &cell, &hss, hss_path, urandom, now_us, net_log);
        lnet.peer_hangup_us = peer_hangup * 1000000u;
        printf("net: %s, key %u, %s, %u subscribers\n", hss_path, hss.key_id,
               hss.mode == LC_SIG_MODE_PART97 ? "part97" : "part15", hss.n);
    }
    /* After lcb_net_init: it sets the cell's mode, which decides what the anchor may be. */
    if ((sync_ch >= 0 || fixed_sync) &&
        lcb_cell_set_sync(&cell, sync_ch >= 0 ? (uint8_t)sync_ch : cell.sync_ch, fixed_sync) != 0) {
        fprintf(stderr, "--sync-ch/--fixed-sync refused: %s\n",
                fixed_sync && !cell.part97 ? "FIXED sync is Part 97 only (lcbench net --mode part97)"
                                           : "not an anchor this mode allows");
        return 1;
    }
    printf("cell: seed %08x, anchor ch %u (%u.%02u MHz), %s sync\n", (unsigned)cell.cell_seed, cell.sync_ch,
           (unsigned)(lc_channel_freq_hz(LC_BAND_915, cell.sync_ch) / 1000000u),
           (unsigned)(lc_channel_freq_hz(LC_BAND_915, cell.sync_ch) % 1000000u / 10000u),
           cell.fixed_sync ? "fixed" : "cycle");
    if (net) {
        if (chan_list == NULL) {
            lcb_net_own_chan_list(&cell, (uint8_t)list_ver, &list);
        }
        for (uint8_t i = 0; i < list.count; i++) {
            if ((list.flags[i] & LC_SIG_CHAN_FIXED) && !cell.part97) {
                fprintf(stderr, "--chan-list: ':fixed' entries are Part 97 only (--mode part97)\n");
                return 1;
            }
        }
        lcb_net_set_chan_list(&lnet, &list);
    }

    board_t b915, b24;
    if (open_board(&b915, argv[2]) != 0) return 1;
    if (tty24 != NULL && open_board(&b24, tty24) != 0) return 1;
    board_t *bs[2] = { &b915, &b24 };
    int nb = tty24 != NULL ? 2 : 1;
    cell_ctx_t ctx = { &cell, &b915 };
    static lc_msg_t m;

    uint64_t start = now_us();
    uint64_t end = start + (uint64_t)seconds * 1000000u;
    uint32_t last_time_s = 0, last_host_frame = 0, last_print_s = 0;
    uint32_t next_f[2] = { 0, 0 };
    int have_nf[2] = { 0, 0 };
    int paged = 0, bumped = 0;
    if (net && call_in != NULL) {
        lcb_net_call_in(&lnet, call_in_bcd, start + (uint64_t)call_after * 1000000u);
    }
    while (now_us() < end) {
        pump(bs, nb, 5, on_cell_msg, &ctx);
        uint64_t t = now_us();
        if (net) {
            lcb_net_tick(&lnet, t);
        }
        uint32_t s = (uint32_t)(t / 1000000u);
        if (s != last_time_s && t % 1000000u > 100000u && t % 1000000u < 800000u) {
            last_time_s = s;
            for (int i = 0; i < nb; i++) {
                send_time(bs[i], s);
            }
        }
        if (net && bump_after && !bumped && t - start >= (uint64_t)bump_after * 1000000u) {
            list.ver++; /* the beacon's cfg_ver changes: registered terminals ask (SERVICE_REQ 4) */
            lcb_net_set_chan_list(&lnet, &list);
            bumped = 1;
        }
        if (page_after && !paged && t - start >= (uint64_t)page_after * 1000000u && cell.terms[0].used) {
            lcb_cell_page(&cell, cell.terms[0].tmid);
            cell.attach_idle = 0;
            paged = 1;
            printf("paging %08x\n", cell.terms[0].tmid);
        }
        if (drop_after && !s_drop_2g4 && t - start >= (uint64_t)drop_after * 1000000u) {
            s_drop_2g4 = 1;
            printf("2.4 GHz off\n");
        }
        uint32_t hf = lc_frame_from_unix_us(t);
        if (hf != last_host_frame) {
            last_host_frame = hf;
            for (int i = 0; i < nb; i++) { /* 915 first: it promotes grants */
                uint32_t f;
                if (board_frame(bs[i], internal, t, &f) != 0) {
                    continue;
                }
                /* As in `link`: every board frame up to estimate + LEAD gets one
                 * schedule, gaps included. Scheduling only estimate + LEAD on each
                 * host-frame tick skipped a board frame whenever the host clock
                 * (NTP, -62 ms here) drifted across a boundary: no slots, lost UL. */
                uint32_t target = f + LCB_LEAD_FRAMES;
                int32_t ahead = (int32_t)(target - next_f[i]);
                if (!have_nf[i] || ahead > 8 || ahead < -8) {
                    have_nf[i] = 1;
                    next_f[i] = target;
                }
                while ((int32_t)(target - next_f[i]) >= 0) {
                    uint32_t fs = next_f[i]++;
                    if ((s_one_board ? lcb_merge_bands(&cell, fs, s_drop_2g4, s_one_map, &m)
                                     : lcb_cell_schedule(&cell, i == 0 ? LC_BAND_915 : LC_BAND_2G4, fs, &m)) == 0) {
                        send_msg(bs[i], &m);
                    }
                }
            }
        }
        if (s != last_print_s) {
            last_print_s = s;
            const lcb_cell_term_t *t0 = &cell.terms[0], *t1 = &cell.terms[1];
            char t1s[48] = ""; /* the second terminal, once one has attached */
            if (t1->used) {
                snprintf(t1s, sizeof(t1s), "term %08x granted %d ul %u | ", t1->tmid, t1->have_cur, t1->ul_rx);
            }
            printf("t=%3us rach %u attach %u page_reply %u upper %u grants %u | term %08x granted %d %s/%s ul %u "
                   "loop %u | %sack_err %u\n",
                   (unsigned)(s - (uint32_t)(start / 1000000u)), cell.rach_rx, cell.attaches, cell.page_replies,
                   cell.uppers, cell.grants_sent, t0->tmid, t0->have_cur,
                   t0->cur.dl.band == LC_BAND_2G4 ? "2g4" : "915", t0->cur.ul.band == LC_BAND_2G4 ? "2g4" : "915",
                   t0->ul_rx, t0->loops, t1s,
                   b915.acks_err + (nb > 1 ? b24.acks_err : 0));
            fflush(stdout);
        }
    }
    for (int b = 0; b < 2; b++) {
        if (s_term_err[b].timed) {
            printf("terminal TX start vs slot, heard on %s: n %u mean %+.1f sd %.1f min %d max %d us\n",
                   b ? "2.4" : "915", s_term_err[b].timed, lcb_stats_end_mean(&s_term_err[b]),
                   lcb_stats_end_sd(&s_term_err[b]), s_term_err[b].end_min, s_term_err[b].end_max);
        }
    }
    static const char *ackname[] = { "ok", "unsupported", "late", "flash", "malformed", "?", "?", "?" };
    for (int w = 0; w < nb; w++) {
        for (int ty = 0; ty < 16; ty++) for (int k = 1; k < 8; k++) if (bs[w]->ack_err_by[ty][k])
            printf("  %s: msg type 0x%02x -> %s x%u\n", w ? "2g4" : "915", ty, ackname[k], bs[w]->ack_err_by[ty][k]);
    }
    return 0;
}


/* ---- duplex: DL (base A -> terminal-side T) then UL (T -> A), possibly cross-band ---- */
typedef struct {
    board_t    *a, *t;
    lcb_stats_t dl, ul;
    lcb_stats_t dl_tx, ul_tx; /* TX_DONE times from each sender's STATUS */
    lcb_stats_t dl_start, ul_start;
} duplex_ctx_t;

static void on_duplex_msg(board_t *b, const lc_msg_t *m, void *vctx)
{
    duplex_ctx_t *ctx = vctx;
    note_tx_done(b, m, b == ctx->a ? &ctx->dl_tx : &ctx->ul_tx, b == ctx->a ? &ctx->dl_start : &ctx->ul_start);
    if (m->type != LC_MSG_RX_REPORT) {
        return;
    }
    lcb_stats_add_rx(b == ctx->t ? &ctx->dl : &ctx->ul, &m->u.rx_report);
}

static void print_dir(const char *name, const lcb_stats_t *st)
{
    printf("%s: sent %u  received %u  crc_fail %u  bad_payload %u  lost %u  PER %.4f", name, st->sent, st->received,
           st->crc_fail, st->bad_payload, st->sent - st->received,
           st->sent ? 1.0 - (double)st->received / st->sent : 0.0);
    if (st->received) {
        printf("  rssi %.1f dBm  snr %.2f dB", (double)st->rssi_sum / st->received,
               (double)st->snr_sum_qdb / st->received / 4.0);
    }
    printf("\n");
}

static int cmd_duplex(int argc, char **argv)
{
    /* lcbench duplex <a_tty> <t_tty> <dl_freq> <dl_tier> <ul_freq> <ul_tier> <frames> [--internal] [--gap-us N] [--offset-us N] [--len N] */
    if (argc < 9) return 2;
    lcb_duplex_cfg_t cfg = { (uint32_t)strtoul(argv[4], NULL, 10), LC_TIER_EDGE, (uint32_t)strtoul(argv[6], NULL, 10),
                             LC_TIER_EDGE, 20000, 1500, 28 };
    if (lcb_parse_tier(argv[5], &cfg.dl_tier) != 0 || lcb_parse_tier(argv[7], &cfg.ul_tier) != 0) return 2;
    if (lcb_band_of(cfg.dl_freq_hz) != lcb_band_of(cfg.ul_freq_hz)) {
        /* each board switches band between DL and UL: leave it the executor's
         * band-switch lead, less the guard already at the end of the DL slot */
        cfg.gap_us = LC_EXEC_BAND_SWITCH_LEAD_US - LC_GUARD_US + 200u;
    } else if (lc_tier_mode(lcb_band_of(cfg.dl_freq_hz), cfg.dl_tier) != NULL &&
               lc_tier_mode(lcb_band_of(cfg.ul_freq_hz), cfg.ul_tier) != NULL &&
               lc_tier_mode(lcb_band_of(cfg.dl_freq_hz), cfg.dl_tier)->modulation !=
                   lc_tier_mode(lcb_band_of(cfg.ul_freq_hz), cfg.ul_tier)->modulation) {
        cfg.gap_us = LC_EXEC_MOD_SWITCH_LEAD_US - LC_GUARD_US + 200u; /* LoRa <-> FLRC */
    }
    uint32_t frames = (uint32_t)atoi(argv[8]);
    int internal = 0;
    for (int i = 9; i < argc; i++) {
        if (strcmp(argv[i], "--internal") == 0) internal = 1;
        else if (strcmp(argv[i], "--gap-us") == 0 && i + 1 < argc) cfg.gap_us = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--offset-us") == 0 && i + 1 < argc) cfg.offset_us = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--len") == 0 && i + 1 < argc) cfg.payload_len = (uint8_t)atoi(argv[++i]);
        else return 2;
    }
    static uint8_t dlp[255], ulp[255];
    static lc_msg_t m;
    if (lcb_duplex_schedule(&cfg, 0, 1, dlp, ulp, &m) != 0) {
        fprintf(stderr, "duplex: tier missing on that band, or the slot pair doesn't fit the frame\n");
        return 1;
    }
    board_t a, t;
    if (open_board(&a, argv[2]) != 0 || open_board(&t, argv[3]) != 0) return 1;
    board_t *bs[2] = { &a, &t };
    duplex_ctx_t ctx = { .a = &a, .t = &t };
    lcb_stats_init(&ctx.dl);
    lcb_stats_init(&ctx.ul);
    lcb_stats_init(&ctx.dl_tx);
    lcb_stats_init(&ctx.ul_tx);
    lcb_stats_init(&ctx.dl_start);
    lcb_stats_init(&ctx.ul_start);
    uint32_t last_time_s = 0, sent = 0, next = 0, t_minus_a = 0;
    int have_next = 0;
    uint64_t end_us = 0;
    while (end_us == 0 || now_us() < end_us) {
        pump(bs, 2, 5, on_duplex_msg, &ctx);
        uint64_t now = now_us();
        uint32_t s = (uint32_t)(now / 1000000u);
        if (s != last_time_s && now % 1000000u > 100000u && now % 1000000u < 800000u) {
            last_time_s = s;
            send_time(&a, s);
            send_time(&t, s);
        }
        if (sent >= frames) {
            if (end_us == 0) end_us = now + 500000u;
            continue;
        }
        uint32_t fa, ft;
        if (board_frame(&a, internal, now, &fa) != 0 || board_frame(&t, internal, now, &ft) != 0) continue;
        uint32_t target = fa + LCB_LEAD_FRAMES;
        if (!have_next) {
            have_next = 1;
            next = target;
            int32_t d = (int32_t)(ft - fa);
            t_minus_a = (d >= -1 && d <= 1) ? 0u : (uint32_t)d;
        }
        while ((int32_t)(target - next) >= 0 && sent < frames) {
            uint32_t f = next++;
            lcb_duplex_schedule(&cfg, f, 1, dlp, ulp, &m);
            send_msg(&a, &m);
            lcb_duplex_schedule(&cfg, f + t_minus_a, 0, dlp, ulp, &m);
            send_msg(&t, &m);
            ctx.dl.sent++;
            ctx.ul.sent++;
            sent++;
        }
    }
    printf("duplex dl %u Hz %s -> ul %u Hz %s, gap %u us\n", cfg.dl_freq_hz, argv[5], cfg.ul_freq_hz, argv[7], cfg.gap_us);
    const lc_mode_t *dlm = lc_tier_mode(lcb_band_of(cfg.dl_freq_hz), cfg.dl_tier);
    const lc_mode_t *ulm = lc_tier_mode(lcb_band_of(cfg.ul_freq_hz), cfg.ul_tier);
    uint32_t dl_air = lc_airtime_us(dlm, cfg.payload_len), ul_air = lc_airtime_us(ulm, cfg.payload_len);
    print_dir("DL (A->T)", &ctx.dl);
    print_timing("  ", &ctx.dl, cfg.offset_us + dl_air);
    print_timing("  tx_done ", &ctx.dl_tx, cfg.offset_us + dl_air);
    print_timing("  tx_start ", &ctx.dl_start, cfg.offset_us);
    print_dir("UL (T->A)", &ctx.ul);
    print_timing("  ", &ctx.ul, cfg.offset_us + dl_air + LC_GUARD_US + cfg.gap_us + ul_air);
    print_timing("  tx_done ", &ctx.ul_tx, cfg.offset_us + dl_air + LC_GUARD_US + cfg.gap_us + ul_air);
    print_timing("  tx_start ", &ctx.ul_start, cfg.offset_us + dl_air + LC_GUARD_US + cfg.gap_us);
    printf("ack errors: a %u t %u\n", a.acks_err, t.acks_err);
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  lcbench status <tty>\n"
            "  lcbench config <tty> <bs|bench> <915|2g4> <radio_index> <cell_seed_hex>\n"
            "  lcbench flash  <tty> <app.bin>\n"
            "  lcbench link   <tx_tty> <rx_tty> <freq_hz> <near|mid|edge> <frames> [--internal]\n"
            "                 [--offset-us N] [--rx-window-us N] [--len N]\n"
            "  lcbench cw     <tx_tty> <freq_hz> <near|mid|edge> <frames> [--internal] [--len N]\n"
            "  lcbench guard  <tx_tty> <rx_tty> <freq_hz> <tier_b> <frames> --a <tier_a> <tx|rx>\n"
            "                 --gap-us N [--freq-a HZ] [--len N]\n"
            "  lcbench duplex <a_tty> <t_tty> <dl_freq> <dl_tier> <ul_freq> <ul_tier> <frames>\n"
            "                 [--internal] [--gap-us N] [--offset-us N] [--len N]   (A: TX DL, RX UL)\n"
            "  lcbench cell   <tty_915> <near|mid|edge> <seconds> [--tty-2g4 TTY] [--dl 915|2g4]\n"
            "                 [--ul 915|2g4] [--seed HEX] [--idle] [--page-after S] [--fallback-915]\n"
            "                 [--internal] [--one-board] [--drop-2g4-after S] [--sync-ch N] [--fixed-sync]\n"
            "  lcbench mkqr   --number +883-1-606-555-01234 [--hss FILE] [--expires-h H] [--mode part15|part97]\n"
            "                 (not while lcbench net runs on the same HSS: it holds FILE.lock)\n"
            "  lcbench net    <tty_915> <near|mid|edge> <seconds> [--hss FILE] [--mode part15|part97]\n"
            "                 [--call-in +883-1-... --after S] [--peer-hangup S]\n"
            "                 [--chan-list MHZ[:fixed],...] [--list-ver N] [--bump-list-after S] [cell options]\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 3) return usage();
    const char *cmd = argv[1];
    if (strcmp(cmd, "status") == 0) return cmd_status(argv[2]);
    if (strcmp(cmd, "config") == 0) return cmd_config(argc, argv) == 2 ? usage() : 0;
    if (strcmp(cmd, "flash") == 0 && argc == 4) return cmd_flash(argv[2], argv[3]);
    if (strcmp(cmd, "cell") == 0 || strcmp(cmd, "net") == 0) {
        int r = cmd_cell(argc, argv, strcmp(cmd, "net") == 0);
        return r == 2 ? usage() : r;
    }
    if (strcmp(cmd, "mkqr") == 0) {
        int r = cmd_mkqr(argc, argv);
        return r == 2 ? usage() : r;
    }
    if (strcmp(cmd, "duplex") == 0) {
        int r = cmd_duplex(argc, argv);
        return r == 2 ? usage() : r;
    }

    guard_opts_t g = { 0, LC_TIER_EDGE, LC_DIR_TX, 1000, 0 };
    g.enabled = strcmp(cmd, "guard") == 0;
    int is_link = strcmp(cmd, "link") == 0 || g.enabled;
    int is_cw = strcmp(cmd, "cw") == 0;
    int base = is_link ? 4 : 3; /* index of freq_hz: argv[1] cmd, argv[2] tx tty, (link/guard) argv[3] rx tty */
    if ((!is_link && !is_cw) || argc < base + 3) return usage();

    lcb_link_cfg_t cfg = { (uint32_t)strtoul(argv[base], NULL, 10), LC_TIER_EDGE, 20000, 0, 28, 0, 0 };
    int internal = 0;
    if (lcb_parse_tier(argv[base + 1], &cfg.tier) != 0) return usage();
    uint32_t frames = (uint32_t)atoi(argv[base + 2]);
    if (parse_opts(argc, argv, base + 3, &cfg, &internal, &g) != 0) return usage();
    if (cfg.rx_window_us == 0) {
        /* Shared PPS: slot plus a little margin. Internal PPS: boards are not
         * aligned to each other, so listen for most of the frame. */
        cfg.rx_window_us = internal ? 100000u : lc_slot_len_us(lc_tier_mode(lcb_band_of(cfg.freq_hz), cfg.tier), cfg.payload_len) + 2000u;
    }

    board_t tx, rx;
    if (open_board(&tx, argv[2]) != 0) return 1;
    if (is_link && open_board(&rx, argv[3]) != 0) return 1;
    return run_frames(&tx, is_link ? &rx : NULL, &cfg, frames, internal, is_cw, &g);
}
