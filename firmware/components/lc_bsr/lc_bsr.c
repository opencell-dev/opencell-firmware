#include "lc_bsr.h"

#include <stdio.h>
#include <string.h>

static int config_valid(const lc_config_t *c)
{
    return (c->role == LC_ROLE_BS_RADIO || c->role == LC_ROLE_BS_RADIO_BENCH) &&
           c->band < LC_BAND_COUNT && c->radio_index < LC_MAX_RADIOS_PER_BAND;
}

void lc_bsr_init(lc_bsr_t *b, const lc_bsr_ops_t *ops, lc_clock_t *clock, lc_exec_t *exec,
                 lc_fwupd_t *fwupd, const lc_config_t *saved)
{
    memset(b, 0, sizeof(*b));
    b->ops = *ops;
    b->clock = clock;
    b->exec = exec;
    b->fwupd = fwupd;
    if (saved != NULL && config_valid(saved)) {
        b->config = *saved;
        b->configured = 1;
    }
    lc_exec_set_tx_enabled(exec, 0);
}

static uint8_t handle_config(lc_bsr_t *b, const lc_config_t *cfg)
{
    if (cfg->role == LC_ROLE_TERMINAL) {
        return LC_ACK_ERR_UNSUPPORTED;
    }
    if (!config_valid(cfg)) {
        return LC_ACK_ERR_MALFORMED;
    }
    if (b->ops.save_config(b->ops.ctx, cfg) != 0) {
        return LC_ACK_ERR_FLASH;
    }
    b->config = *cfg;
    b->configured = 1;
    return LC_ACK_OK;
}

int lc_bsr_handle(lc_bsr_t *b, const lc_msg_t *in, uint64_t now_us, lc_msg_t *ack)
{
    uint8_t status;
    switch (in->type) {
    case LC_MSG_CONFIG:
        status = handle_config(b, &in->u.config);
        break;
    case LC_MSG_TIME:
        status = lc_clock_on_time(b->clock, in->u.time.unix_s, now_us) == 0 ? LC_ACK_OK : LC_ACK_ERR_LATE;
        break;
    case LC_MSG_SCHEDULE:
        status = b->configured ? lc_exec_add_part(b->exec, &in->u.schedule, b->clock, now_us)
                               : LC_ACK_ERR_UNSUPPORTED;
        break;
    case LC_MSG_FW_CHUNK:
        status = lc_fwupd_chunk(b->fwupd, &in->u.fw_chunk);
        break;
    case LC_MSG_FW_COMMIT:
        status = lc_fwupd_commit(b->fwupd, &in->u.fw_commit);
        if (status == LC_ACK_OK) {
            b->reboot_pending = 1;
        }
        break;
    default:
        status = LC_ACK_ERR_UNSUPPORTED; /* device->host types, or unknown */
        break;
    }
    memset(ack, 0, sizeof(*ack));
    ack->type = LC_MSG_ACK;
    ack->seq = b->tx_seq++;
    ack->u.ack.acked_seq = in->seq;
    ack->u.ack.status = status;
    return 1;
}

void lc_bsr_tick(lc_bsr_t *b, uint64_t now_us)
{
    lc_clock_tick(b->clock, now_us);
    lc_exec_set_tx_enabled(b->exec, b->configured && b->clock->state != LC_CLOCK_UNLOCKED);
}

void lc_bsr_make_status(lc_bsr_t *b, uint64_t now_us, uint32_t uptime_ms, int8_t temp_c,
                        uint16_t uart_crc_errors, lc_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type = LC_MSG_STATUS;
    out->seq = b->tx_seq++;
    out->u.status.uptime_ms = uptime_ms;
    out->u.status.pps_locked = b->clock->state;
    out->u.status.temp_c = temp_c;
    out->u.status.schedule_misses = b->exec->schedule_misses;
    out->u.status.uart_crc_errors = uart_crc_errors;
    out->u.status.last_tx_end_us = b->exec->last_tx_end_us;
    out->u.status.last_tx_start_us = b->exec->last_tx_start_us;
    if (lc_clock_frame_at(b->clock, now_us, &out->u.status.frame_number) != 0) {
        out->u.status.frame_number = 0;
    }
}

void lc_bsr_make_rx_report(lc_bsr_t *b, uint32_t frame_number, uint8_t slot_index,
                           const lc_radio_event_t *ev, lc_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type = LC_MSG_RX_REPORT;
    out->seq = b->tx_seq++;
    out->u.rx_report.frame_number = frame_number;
    out->u.rx_report.slot_index = slot_index;
    out->u.rx_report.rssi_dbm = ev->rssi_dbm;
    out->u.rx_report.snr_qdb = ev->snr_qdb;
    out->u.rx_report.crc_ok = ev->crc_ok;
    out->u.rx_report.payload_len = ev->len;
    out->u.rx_report.payload = ev->data;
    out->u.rx_report.end_us = ev->frame_offset_us;
}

void lc_bsr_view(const lc_bsr_t *b, uint64_t now_us, lc_bsr_view_t *v)
{
    memset(v, 0, sizeof(*v));
    v->configured = (uint8_t)(b->configured != 0);
    v->band = b->config.band;
    v->radio_index = b->config.radio_index;
    v->clock_state = (uint8_t)b->clock->state;
    if (b->clock->period_us != 0) {
        v->ppm_valid = 1;
        v->ppm = (int32_t)b->clock->period_us - 1000000;
    }
    v->have_frame = (uint8_t)(lc_clock_frame_at(b->clock, now_us, &v->frame) == 0);
    if (!v->have_frame) {
        v->frame = 0;
    }
    v->tx_on = (uint8_t)(b->exec->tx_enabled != 0);
    v->misses = b->exec->schedule_misses;
}

void lc_bsr_status_lines(const lc_bsr_view_t *v, char lines[LC_BSR_SCREEN_LINES][LC_BSR_SCREEN_COLS + 1])
{
    const size_t n = LC_BSR_SCREEN_COLS + 1;
    snprintf(lines[0], n, "OPENCELL BS-RADIO");

    if (v->configured) {
        const char *band = v->band == LC_BAND_915 ? "915 MHZ" : v->band == LC_BAND_2G4 ? "2.4 GHZ" : "?";
        snprintf(lines[1], n, "%s  RADIO %u", band, (unsigned)v->radio_index);
    } else {
        snprintf(lines[1], n, "NOT CONFIGURED");
    }

    const char *clk = v->clock_state == LC_CLOCK_UNLOCKED ? "NO PPS"
                      : v->clock_state == LC_CLOCK_LOCKED ? "LOCKED"
                      : v->clock_state == LC_CLOCK_HOLDOVER ? "HOLDOVER" : "?";
    if (v->clock_state != LC_CLOCK_UNLOCKED && v->ppm_valid && clk[0] != '?') {
        /* Clamp so the longest line ("CLK HOLDOVER -9999PPM") still fits. */
        int32_t ppm = v->ppm > 9999 ? 9999 : v->ppm < -9999 ? -9999 : v->ppm;
        unsigned mag = (unsigned)(ppm < 0 ? -ppm : ppm) % 10000u;
        snprintf(lines[2], n, "CLK %s %c%uPPM", clk, ppm < 0 ? '-' : '+', mag);
    } else {
        snprintf(lines[2], n, "CLK %s", clk);
    }

    if (v->have_frame) {
        snprintf(lines[3], n, "FRAME %lu", (unsigned long)v->frame);
    } else {
        snprintf(lines[3], n, "FRAME --");
    }

    snprintf(lines[4], n, "TX %s  MISS %u", v->tx_on ? "ON " : "OFF", (unsigned)v->misses);

    if (v->uart_errors > 99999u) {
        snprintf(lines[5], n, "HOST %s  CRC 99999+", v->host_ok ? "OK" : "--");
    } else {
        snprintf(lines[5], n, "HOST %s  CRC %u", v->host_ok ? "OK" : "--", (unsigned)(v->uart_errors % 100000u));
    }
}
