/* BLE GATT bridge between the phone app and lc_term (contract in
 * components/lc_term/include/lc_term_gatt.h). NimBLE host on its own task;
 * DOWN notifications are queued so lc_term never blocks on BLE. */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
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
    uint8_t data[LC_TERM_DATA_MAX_PAYLOAD];
} down_msg_t;

static const ble_uuid128_t k_svc = BLE_UUID128_INIT(LC_GATT_UUID_BYTES(LC_GATT_ID_SERVICE));
static const ble_uuid128_t k_up = BLE_UUID128_INIT(LC_GATT_UUID_BYTES(LC_GATT_ID_UP));
static const ble_uuid128_t k_down = BLE_UUID128_INIT(LC_GATT_UUID_BYTES(LC_GATT_ID_DOWN));
static const ble_uuid128_t k_status = BLE_UUID128_INIT(LC_GATT_UUID_BYTES(LC_GATT_ID_STATUS));

static uint16_t s_down_handle;
static uint16_t s_status_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_addr_type;
static char s_name[20];
static QueueHandle_t s_down_q;
static volatile int s_status_dirty;

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(ctxt->chr->uuid, &k_up.u) == 0) {
        uint8_t buf[LC_TERM_DATA_MAX_PAYLOAD];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(buf)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        term_lock();
        int rc = lc_term_send_upper(&g_term, buf, (uint8_t)len);
        term_unlock();
        return rc == 0 ? 0 : LC_GATT_ERR_NOT_NOW;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ble_uuid_cmp(ctxt->chr->uuid, &k_status.u) == 0) {
        lc_term_status_t st;
        uint8_t out[LC_GATT_STATUS_LEN];
        term_lock();
        lc_term_status(&g_term, &st);
        term_unlock();
        lc_term_pack_status(&st, out);
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

/* Sends queued DOWN payloads and STATUS changes to the connected phone. */
static void notify_task(void *arg)
{
    (void)arg;
    down_msg_t m;
    for (;;) {
        if (xQueueReceive(s_down_q, &m, pdMS_TO_TICKS(200)) == pdTRUE && s_conn != BLE_HS_CONN_HANDLE_NONE) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(m.data, m.len);
            if (om != NULL) {
                ble_gatts_notify_custom(s_conn, s_down_handle, om);
            }
        }
        if (s_status_dirty && s_conn != BLE_HS_CONN_HANDLE_NONE) {
            s_status_dirty = 0;
            lc_term_status_t st;
            uint8_t out[LC_GATT_STATUS_LEN];
            term_lock();
            lc_term_status(&g_term, &st);
            term_unlock();
            lc_term_pack_status(&st, out);
            struct os_mbuf *om = ble_hs_mbuf_from_flat(out, sizeof(out));
            if (om != NULL) {
                ble_gatts_notify_custom(s_conn, s_status_handle, om);
            }
        }
    }
}

void term_ble_downlink(const uint8_t *data, uint8_t len)
{
    down_msg_t m;
    m.len = len > LC_TERM_DATA_MAX_PAYLOAD ? LC_TERM_DATA_MAX_PAYLOAD : len;
    memcpy(m.data, data, m.len);
    xQueueSend(s_down_q, &m, 0); /* drop if the phone isn't keeping up */
}

void term_ble_status_changed(void)
{
    s_status_dirty = 1;
}

void term_ble_start(uint32_t tmid)
{
    s_down_q = xQueueCreate(8, sizeof(down_msg_t));
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
