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
#include <unistd.h>

#include "lc_clock.h"
#include "lc_link.h"
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
    tcsetattr(b->fd, TCSANOW, &t);
    tcflush(b->fd, TCIOFLUSH);
    lc_framer_init(&b->framer);
    return 0;
}

static int send_msg(board_t *b, lc_msg_t *m)
{
    static uint8_t wire[LC_FRAMER_RAW_CAP + 2];
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
                if (m.type == LC_MSG_STATUS && m.u.status.frame_number != 0) {
                    b->status_frame = m.u.status.frame_number;
                    b->status_at_us = now_us();
                    b->have_status = 1;
                } else if (m.type == LC_MSG_ACK && m.u.ack.status != LC_ACK_OK) {
                    b->acks_err++;
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
} link_ctx_t;

static void on_link_msg(board_t *b, const lc_msg_t *m, void *vctx)
{
    link_ctx_t *ctx = vctx;
    if (b == ctx->rx && m->type == LC_MSG_RX_REPORT) {
        lcb_stats_add_rx(&ctx->stats, &m->u.rx_report);
    }
}

/* Frame the board will be in at host time t. */
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

static int run_frames(board_t *tx, board_t *rx, const lcb_link_cfg_t *cfg, uint32_t frames, int internal,
                      int cw, const guard_opts_t *g)
{
    board_t *bs[2] = { tx, rx };
    int nb = rx ? 2 : 1;
    link_ctx_t ctx = { .rx = rx };
    lcb_stats_init(&ctx.stats);
    static uint8_t payload[255];
    static lc_msg_t m;

    uint32_t last_time_s = 0;
    uint32_t last_host_frame = 0;
    uint32_t sent_frames = 0;
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
        uint32_t hf = lc_frame_from_unix_us(t);
        if (hf == last_host_frame || sent_frames >= frames) {
            if (sent_frames >= frames && end_us == 0) {
                end_us = t + 500000u; /* let the last reports arrive */
            }
            continue;
        }
        last_host_frame = hf;
        uint32_t ftx, frx = 0;
        if (board_frame(tx, internal, t, &ftx) != 0 || (rx && board_frame(rx, internal, t, &frx) != 0)) {
            continue; /* waiting for STATUS */
        }
        if (cw) {
            if (lcb_cw_schedule(cfg, ftx + 2, payload, &m) < 0) return 2;
            send_msg(tx, &m);
        } else if (g->enabled) {
            if (lcb_guard_schedule(cfg, g->freq_a_hz, g->tier_a, g->dir_a, g->gap_us, ftx + 2, 1, payload, &m) != 0) {
                return 2;
            }
            send_msg(tx, &m);
            lcb_guard_schedule(cfg, g->freq_a_hz, g->tier_a, g->dir_a, g->gap_us, frx + 2, 0, payload, &m);
            send_msg(rx, &m);
            ctx.stats.sent++;
        } else {
            if (lcb_link_schedule(cfg, ftx + 2, 1, payload, &m) != 0) return 2;
            send_msg(tx, &m);
            lcb_link_schedule(cfg, frx + 2, 0, payload, &m);
            send_msg(rx, &m);
            ctx.stats.sent++;
        }
        sent_frames++;
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
    }
    printf("ack errors: tx %u rx %u\n", tx->acks_err, rx->acks_err);
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
            "                 --gap-us N [--freq-a HZ] [--len N]\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 3) return usage();
    const char *cmd = argv[1];
    if (strcmp(cmd, "status") == 0) return cmd_status(argv[2]);
    if (strcmp(cmd, "config") == 0) return cmd_config(argc, argv) == 2 ? usage() : 0;
    if (strcmp(cmd, "flash") == 0 && argc == 4) return cmd_flash(argv[2], argv[3]);

    guard_opts_t g = { 0, LC_TIER_EDGE, LC_DIR_TX, 1000, 0 };
    g.enabled = strcmp(cmd, "guard") == 0;
    int is_link = strcmp(cmd, "link") == 0 || g.enabled;
    int is_cw = strcmp(cmd, "cw") == 0;
    int base = is_link ? 3 : 2; /* index of freq_hz */
    if ((!is_link && !is_cw) || argc < base + 3) return usage();

    lcb_link_cfg_t cfg = { (uint32_t)strtoul(argv[base], NULL, 10), LC_TIER_EDGE, 20000, 0, 28 };
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
