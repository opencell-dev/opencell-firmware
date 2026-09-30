#include "oc_bsr.h"

#include <stdio.h>
#include <string.h>

static int config_valid(const oc_config_t *c)
{
    return (c->role == OC_ROLE_BS_RADIO || c->role == OC_ROLE_BS_RADIO_BENCH) &&
           c->band < OC_BAND_COUNT && c->radio_index < OC_MAX_RADIOS_PER_BAND;
}

void oc_bsr_init(oc_bsr_t *b, const oc_bsr_ops_t *ops, oc_clock_t *clock, oc_exec_t *exec,
                 oc_fwupd_t *fwupd, const oc_config_t *saved)
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
    oc_exec_set_tx_enabled(exec, 0);
}

static uint8_t handle_config(oc_bsr_t *b, const oc_config_t *cfg)
{
    if (cfg->role == OC_ROLE_TERMINAL) {
        return OC_ACK_ERR_UNSUPPORTED;
    }
    if (!config_valid(cfg)) {
        return OC_ACK_ERR_MALFORMED;
    }
    if (b->ops.save_config(b->ops.ctx, cfg) != 0) {
        return OC_ACK_ERR_FLASH;
    }
    b->config = *cfg;
    b->configured = 1;
    return OC_ACK_OK;
}

int oc_bsr_handle(oc_bsr_t *b, const oc_msg_t *in, uint64_t now_us, oc_msg_t *ack)
{
    uint8_t status;
    switch (in->type) {
    case OC_MSG_CONFIG:
        status = handle_config(b, &in->u.config);
        break;
    case OC_MSG_TIME:
        status = oc_clock_on_time(b->clock, in->u.time.unix_s, now_us) == 0 ? OC_ACK_OK : OC_ACK_ERR_LATE;
        break;
    case OC_MSG_SCHEDULE:
        status = b->configured ? oc_exec_add_part(b->exec, &in->u.schedule, b->clock, now_us)
                               : OC_ACK_ERR_UNSUPPORTED;
        break;
    case OC_MSG_FW_CHUNK:
        status = oc_fwupd_chunk(b->fwupd, &in->u.fw_chunk);
        break;
    case OC_MSG_FW_COMMIT:
        status = oc_fwupd_commit(b->fwupd, &in->u.fw_commit);
        if (status == OC_ACK_OK) {
            b->reboot_pending = 1;
        }
        break;
    default:
        status = OC_ACK_ERR_UNSUPPORTED; /* device->host types, or unknown */
        break;
    }
    memset(ack, 0, sizeof(*ack));
    ack->type = OC_MSG_ACK;
    ack->seq = b->tx_seq++;
    ack->u.ack.acked_seq = in->seq;
    ack->u.ack.status = status;
    return 1;
}

void oc_bsr_tick(oc_bsr_t *b, uint64_t now_us)
{
    oc_clock_tick(b->clock, now_us);
    oc_exec_set_tx_enabled(b->exec, b->configured && b->clock->state != OC_CLOCK_UNLOCKED);
}

void oc_bsr_make_status(oc_bsr_t *b, uint64_t now_us, uint32_t uptime_ms, int8_t temp_c,
                        uint16_t uart_crc_errors, oc_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type = OC_MSG_STATUS;
    out->seq = b->tx_seq++;
    out->u.status.uptime_ms = uptime_ms;
    out->u.status.pps_locked = b->clock->state;
    out->u.status.temp_c = temp_c;
    out->u.status.schedule_misses = b->exec->schedule_misses;
    out->u.status.uart_crc_errors = uart_crc_errors;
    out->u.status.last_tx_end_us = b->exec->last_tx_end_us;
    out->u.status.last_tx_start_us = b->exec->last_tx_start_us;
    out->u.status.late_slots = (uint16_t)(b->exec->late_slots > 0xFFFFu ? 0xFFFFu : b->exec->late_slots);
    out->u.status.radio_errors = (uint16_t)(b->exec->radio_errors > 0xFFFFu ? 0xFFFFu : b->exec->radio_errors);
    out->u.status.last_radio_err = b->exec->last_radio_err;
    out->u.status.last_radio_op = b->exec->last_radio_op;
    if (oc_clock_frame_at(b->clock, now_us, &out->u.status.frame_number) != 0) {
        out->u.status.frame_number = 0;
    }
}

void oc_bsr_make_rx_report(oc_bsr_t *b, uint32_t frame_number, uint8_t slot_index,
                           const oc_radio_event_t *ev, oc_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type = OC_MSG_RX_REPORT;
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

void oc_bsr_view(const oc_bsr_t *b, uint64_t now_us, oc_bsr_view_t *v)
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
    v->have_frame = (uint8_t)(oc_clock_frame_at(b->clock, now_us, &v->frame) == 0);
    if (!v->have_frame) {
        v->frame = 0;
    }
    v->tx_on = (uint8_t)(b->exec->tx_enabled != 0);
    v->misses = b->exec->schedule_misses;
}

void oc_bsr_status_lines(const oc_bsr_view_t *v, char lines[OC_BSR_SCREEN_LINES][OC_BSR_SCREEN_COLS + 1])
{
    const size_t n = OC_BSR_SCREEN_COLS + 1;
    snprintf(lines[0], n, "OPENCELL BS-RADIO");

    if (v->configured) {
        const char *band = v->band == OC_BAND_915 ? "915 MHZ" : v->band == OC_BAND_2G4 ? "2.4 GHZ" : "?";
        snprintf(lines[1], n, "%s  RADIO %u", band, (unsigned)v->radio_index);
    } else {
        snprintf(lines[1], n, "NOT CONFIGURED");
    }

    const char *clk = v->clock_state == OC_CLOCK_UNLOCKED ? "NO PPS"
                      : v->clock_state == OC_CLOCK_LOCKED ? "LOCKED"
                      : v->clock_state == OC_CLOCK_HOLDOVER ? "HOLDOVER" : "?";
    if (v->clock_state != OC_CLOCK_UNLOCKED && v->ppm_valid && clk[0] != '?') {
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

    const char *host = v->host_ok ? "OK" : "--";
    if (v->rx_drops > 0) {
        /* RX reports dropped on the board: with the link's CRC count, in
         * full while it fits, else both clamped (C9999+ D9999+ = 21 cols) */
        char text[48], c[16], d[16];
        snprintf(text, sizeof(text), "HOST %s CRC %lu DROP %lu", host, (unsigned long)v->uart_errors,
                 (unsigned long)v->rx_drops);
        if (strlen(text) > OC_BSR_SCREEN_COLS) {
            if (v->uart_errors > 9999u) snprintf(c, sizeof(c), "9999+");
            else snprintf(c, sizeof(c), "%u", (unsigned)v->uart_errors);
            if (v->rx_drops > 9999u) snprintf(d, sizeof(d), "9999+");
            else snprintf(d, sizeof(d), "%u", (unsigned)v->rx_drops);
            snprintf(text, sizeof(text), "HOST %s C%s D%s", host, c, d);
        }
        size_t len = strlen(text);
        if (len > OC_BSR_SCREEN_COLS) len = OC_BSR_SCREEN_COLS;
        memcpy(lines[5], text, len);
        lines[5][len] = '\0';
    } else if (v->uart_errors > 99999u) {
        snprintf(lines[5], n, "HOST %s  CRC 99999+", host);
    } else {
        snprintf(lines[5], n, "HOST %s  CRC %u", host, (unsigned)(v->uart_errors % 100000u));
    }
}
