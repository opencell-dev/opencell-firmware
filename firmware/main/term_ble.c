/* BLE GATT bridge between the phone app and the terminal (contract v2 in
 * components/lc_term/include/lc_term_gatt.h): app data on UP/DOWN, signalling
 * commands on COMMAND, events on EVENT. NimBLE host on its own task; DOWN and
 * EVENT notifications are queued so the link task never blocks on BLE. */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "lc_term_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "term.h"

static const char *TAG = "lc_ble";

typedef struct {
    uint8_t len;
    uint8_t data[LC_SIG_APP_MAX];
} down_msg_t;

typedef struct {
    uint8_t len;
    uint8_t data[LC_GATT_EVENT_MAX];
} event_msg_t;

static const ble_uuid128_t k_svc = BLE_UUID128_INIT(LC_GATT_UUID_BYTES(LC_GATT_ID_SERVICE));
static const ble_uuid128_t k_up = BLE_UUID128_INIT(LC_GATT_UUID_BYTES(LC_GATT_ID_UP));
static const ble_uuid128_t k_down = BLE_UUID128_INIT(LC_GATT_UUID_BYTES(LC_GATT_ID_DOWN));
static const ble_uuid128_t k_status = BLE_UUID128_INIT(LC_GATT_UUID_BYTES(LC_GATT_ID_STATUS));
static const ble_uuid128_t k_command = BLE_UUID128_INIT(LC_GATT_UUID_BYTES(LC_GATT_ID_COMMAND));
static const ble_uuid128_t k_event = BLE_UUID128_INIT(LC_GATT_UUID_BYTES(LC_GATT_ID_EVENT));

static uint16_t s_down_handle;
static uint16_t s_status_handle;
static uint16_t s_event_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_addr_type;
static char s_name[20];
static QueueHandle_t s_down_q;
static QueueHandle_t s_event_q;
static QueueSetHandle_t s_out_set;
static volatile int s_status_dirty;

/* STATUS with byte 3 = signalling state (the link fields come from lc_term). */
static void read_status(uint8_t out[LC_GATT_STATUS_LEN])
{
    lc_term_status_t st;
    term_lock();
    lc_term_status(&g_term, &st);
    uint8_t sig = g_sig_ok ? lc_sig_term_state(&g_sig.sig) : 0;
    term_unlock();
    lc_term_pack_status(&st, out);
    out[LC_GATT_STATUS_SIG] = sig;
}

static int command(const uint8_t *cmd, uint16_t len)
{
    if (!g_sig_ok) {
        return LC_GATT_ERR_NOT_NOW;
    }
    if (len >= 1 && cmd[0] == LC_SIG_CMD_ACTIVATE) {
        /* X25519 (~150 ms) runs here, outside the lock, so the link task keeps
         * its slots. The identity's key pair never changes after boot. */
        lc_sig_act_prep_t p;
        int rc = lc_sig_term_act_prepare(g_sig.sig.id, g_sig.sig.tmid, cmd + 1, (size_t)(len - 1), &p);
        if (rc == 0) {
            term_lock();
            rc = lc_sig_term_activate(&g_sig.sig, &p, (uint64_t)esp_timer_get_time());
            term_sig_state_check();
            term_unlock();
        }
        memset(&p, 0, sizeof(p));
        return rc;
    }
    term_lock();
    int rc = lc_sig_term_command(&g_sig.sig, cmd, len, (uint64_t)esp_timer_get_time());
    term_sig_state_check();
    term_unlock();
    return rc;
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(ctxt->chr->uuid, &k_up.u) == 0) {
        uint8_t buf[LC_SIG_APP_MAX];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(buf)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (!g_sig_ok) {
            return LC_GATT_ERR_NOT_NOW;
        }
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        term_lock();
        int rc = lc_term_sig_app_up(&g_sig, buf, (uint8_t)len);
        term_unlock();
        return rc;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(ctxt->chr->uuid, &k_command.u) == 0) {
        uint8_t buf[LC_GATT_COMMAND_MAX];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > sizeof(buf)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        return command(buf, len);
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ble_uuid_cmp(ctxt->chr->uuid, &k_status.u) == 0) {
        uint8_t out[LC_GATT_STATUS_LEN];
        read_status(out);
        return os_mbuf_append(ctxt->om, out, sizeof(out)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def k_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &k_up.u, .access_cb = chr_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &k_down.u, .access_cb = chr_access, .val_handle = &s_down_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &k_status.u, .access_cb = chr_access, .val_handle = &s_status_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &k_command.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &k_event.u, .access_cb = chr_access, .val_handle = &s_event_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    { 0 },
};

static void advertise(void);

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_conn = ev->connect.status == 0 ? ev->connect.conn_handle : BLE_HS_CONN_HANDLE_NONE;
        if (ev->connect.status != 0) {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&k_svc;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    struct ble_hs_adv_fields rsp; /* the name doesn't fit next to a 128-bit UUID */
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (uint8_t *)s_name;
    rsp.name_len = (uint8_t)strlen(s_name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start: %d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_addr_type);
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void notify(uint16_t handle, const uint8_t *d, uint8_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, len);
    if (om != NULL) {
        ble_gatts_notify_custom(s_conn, handle, om);
    }
}

/* Sends queued DOWN payloads, EVENTs and STATUS changes to the connected phone. */
static void notify_task(void *arg)
{
    (void)arg;
    for (;;) {
        QueueSetMemberHandle_t q = xQueueSelectFromSet(s_out_set, pdMS_TO_TICKS(200));
        if (q == s_down_q) {
            down_msg_t m;
            if (xQueueReceive(s_down_q, &m, 0) == pdTRUE && s_conn != BLE_HS_CONN_HANDLE_NONE) {
                notify(s_down_handle, m.data, m.len);
            }
        } else if (q == s_event_q) {
            event_msg_t m;
            if (xQueueReceive(s_event_q, &m, 0) == pdTRUE && s_conn != BLE_HS_CONN_HANDLE_NONE) {
                notify(s_event_handle, m.data, m.len);
            }
        }
        if (s_status_dirty && s_conn != BLE_HS_CONN_HANDLE_NONE) {
            s_status_dirty = 0;
            uint8_t out[LC_GATT_STATUS_LEN];
            read_status(out);
            notify(s_status_handle, out, sizeof(out));
        }
    }
}

void term_ble_downlink(const uint8_t *data, uint8_t len)
{
    down_msg_t m;
    m.len = len > sizeof(m.data) ? sizeof(m.data) : len;
    memcpy(m.data, data, m.len);
    xQueueSend(s_down_q, &m, 0); /* drop if the phone isn't keeping up */
}

void term_ble_event(const uint8_t *ev, uint8_t len)
{
    event_msg_t m;
    m.len = len > sizeof(m.data) ? sizeof(m.data) : len;
    memcpy(m.data, ev, m.len);
    xQueueSend(s_event_q, &m, 0); /* no phone: dropped; the app reads STATUS on connect */
}

void term_ble_status_changed(void)
{
    s_status_dirty = 1;
}

void term_ble_start(uint32_t tmid)
{
    s_down_q = xQueueCreate(8, sizeof(down_msg_t));
    s_event_q = xQueueCreate(8, sizeof(event_msg_t));
    s_out_set = xQueueCreateSet(16);
    xQueueAddToSet(s_down_q, s_out_set);
    xQueueAddToSet(s_event_q, s_out_set);
    snprintf(s_name, sizeof(s_name), "OpenCell-%08lX", (unsigned long)tmid);
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(k_svcs);
    ble_gatts_add_svcs(k_svcs);
    ble_svc_gap_device_name_set(s_name);
    nimble_port_freertos_init(host_task);
    xTaskCreatePinnedToCore(notify_task, "lc_ble_tx", 4096, NULL, 4, NULL, 0); /* with NimBLE; core 1 is the radio's */
}
