/* BLE GATT bridge between the phone app and the terminal (contract v4 in
 * components/lc_term/include/lc_term_gatt.h): app data on UP/DOWN, signalling
 * commands on COMMAND, events on EVENT, the scan list on SCAN (and COMMAND
 * 0x07). NimBLE host on its own task; DOWN and EVENT notifications are queued
 * so the link task never blocks on BLE.
 *
 * Security (spec 2026-09-27-ble-pairing-design.md §2): LE Secure Connections
 * only, passkey entry with the terminal as DisplayOnly, bonding with the keys
 * in NVS. Every characteristic needs an encrypted, authenticated link; the
 * passkey is lc_term_pair's rolling code, shown on the OLED's Pairing screen. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "lc_term_gatt.h"
#include "lc_term_pair.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "term.h"

/* firmware/sdkconfig is git-ignored. IDF re-applies sdkconfig.defaults only to
 * values it marked "# default:"; one set through menuconfig is kept, and an
 * old one silently builds a terminal without pairing, or whose bonds vanish on
 * reboot, or whose third phone's CCCD writes fail. */
#if !CONFIG_BT_NIMBLE_NVS_PERSIST || CONFIG_BT_NIMBLE_MAX_CCCDS < 12 || !CONFIG_BT_NIMBLE_SECURITY_ENABLE
#error "firmware/sdkconfig is stale: delete it so sdkconfig.defaults applies"
#endif

static const char *TAG = "lc_ble";

void ble_store_config_init(void); /* NimBLE's NVS-backed store; no public header declares it */

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
static const ble_uuid128_t k_scan = BLE_UUID128_INIT(LC_GATT_UUID_BYTES(LC_GATT_ID_SCAN));

static uint16_t s_down_handle;
static uint16_t s_status_handle;
static uint16_t s_event_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_addr_type;
static char s_name[20];
static QueueHandle_t s_down_q;
static QueueHandle_t s_event_q;
static QueueSetHandle_t s_out_set;
static volatile int s_status_dirty;

/* Pairing. s_pair is shared with the OLED task (term_ble_pair_view), so it is
 * only touched inside s_pair_mux. s_pairing and s_bonds are written by the
 * NimBLE host task only. */
static lc_term_pair_t s_pair;
static portMUX_TYPE s_pair_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_pairing;      /* a passkey was shown for the current connection */
static volatile uint8_t s_bonds;    /* bonded phones in the store */
static struct ble_npl_event s_clear_ev;
static bool s_started; /* true once s_clear_ev is safe to post to (nimble_port_init succeeded) */

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
    if (len >= 1 && cmd[0] == LC_SIG_CMD_SCAN) {
        /* the scan list: no signalling needed, so not refused without it */
        term_lock();
        int rc = lc_term_gatt_scan_command(&g_term.scan, cmd, len);
        if (g_term.scan.dirty) {
            term_scan_save(&g_term.scan);
        }
        term_unlock();
        return rc;
    }
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

/* SCAN snapshots, one per connection. With an MTU below 144 the phone reads
 * SCAN as a Read (offset 0) and then Read Blobs (offset > 0), each a separate
 * chr_access call: packing the live list for each could hand the phone the
 * head of one list and the tail of another. The value is packed once when a
 * read starts at offset 0 and blobs are served from that snapshot; the next
 * offset-0 read refreshes it. NimBLE host task only (access and GAP events),
 * so no lock. */
typedef struct {
    uint16_t conn;             /* BLE_HS_CONN_HANDLE_NONE: free */
    uint16_t len;
    uint8_t data[LC_GATT_SCAN_MAX];
} scan_snap_t;

static scan_snap_t s_scan_snap[CONFIG_BT_NIMBLE_MAX_CONNECTIONS];

static void scan_snap_init(void)
{
    for (size_t i = 0; i < sizeof(s_scan_snap) / sizeof(s_scan_snap[0]); i++) {
        s_scan_snap[i].conn = BLE_HS_CONN_HANDLE_NONE;
        s_scan_snap[i].len = 0;
    }
}

static void scan_snap_drop(uint16_t conn)
{
    for (size_t i = 0; i < sizeof(s_scan_snap) / sizeof(s_scan_snap[0]); i++) {
        if (s_scan_snap[i].conn == conn) {
            s_scan_snap[i].conn = BLE_HS_CONN_HANDLE_NONE;
        }
    }
}

static const scan_snap_t *scan_snap(uint16_t conn, uint16_t offset)
{
    scan_snap_t *sn = NULL, *free_sn = NULL;
    for (size_t i = 0; i < sizeof(s_scan_snap) / sizeof(s_scan_snap[0]); i++) {
        if (s_scan_snap[i].conn == conn) {
            sn = &s_scan_snap[i];
        } else if (s_scan_snap[i].conn == BLE_HS_CONN_HANDLE_NONE && free_sn == NULL) {
            free_sn = &s_scan_snap[i];
        }
    }
    if (sn != NULL && offset > 0) {
        return sn; /* a Read Blob continuing this connection's read */
    }
    if (sn == NULL) {
        /* a new reader (a blob with no offset-0 read before it gets a fresh
         * pack too); every slot taken only if a disconnect was missed */
        sn = free_sn != NULL ? free_sn : &s_scan_snap[0];
        sn->conn = conn;
    }
    term_lock();
    sn->len = (uint16_t)lc_term_pack_scan(&g_term.scan, sn->data);
    term_unlock();
    return sn;
}

/* NimBLE refuses every access below with ATT 0x05 (insufficient
 * authentication) unless the link is encrypted with an authenticated key: the
 * _ENC/_AUTHEN flags in k_svcs, with sm_sc_only. */
static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
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
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ble_uuid_cmp(ctxt->chr->uuid, &k_scan.u) == 0) {
        /* up to 141 bytes: NimBLE serves a long read (read blob) from the whole
         * value, cutting it at ctxt->offset, so every blob comes from one
         * snapshot (see scan_snap()) */
        const scan_snap_t *sn = scan_snap(conn, ctxt->offset);
        return os_mbuf_append(ctxt->om, sn->data, sn->len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

#define F_WRITE_SEC  (BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN)
#define F_READ_SEC   (BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN)
#define F_NOTIFY_SEC (BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN) /* CCCD writes */

/* The attribute table's layout version. BUMP IT whenever k_svcs changes (a
 * characteristic added, removed, reordered, or its properties changed), and
 * whenever ble_svc_gap/ble_svc_gatt change what they register: bonded phones
 * cache the table (there is no Database Hash, CONFIG_BT_NIMBLE_GATT_CACHING is
 * off), and only a Service Changed indication makes them discover it again
 * (gatt_table_check()). 4: contract v4 (UP, DOWN, STATUS, COMMAND, EVENT,
 * SCAN). */
#define GATT_TABLE_VER 4u
#define GATT_NVS_NS    "lc_ble" /* not lc, lc_id, lc_scan or NimBLE's bond store */
#define GATT_NVS_KEY   "gatt_ver"
#define SC_NVS_PEND    "sc_pend" /* 1 while a Service Changed is still owed */
#define SC_NVS_DONE    "sc_done" /* identity addresses of the phones that confirmed it */

static const struct ble_gatt_svc_def k_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &k_up.u, .access_cb = chr_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | F_WRITE_SEC },
            { .uuid = &k_down.u, .access_cb = chr_access, .val_handle = &s_down_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY | F_NOTIFY_SEC },
            { .uuid = &k_status.u, .access_cb = chr_access, .val_handle = &s_status_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | F_READ_SEC | F_NOTIFY_SEC },
            { .uuid = &k_command.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_WRITE | F_WRITE_SEC },
            { .uuid = &k_event.u, .access_cb = chr_access, .val_handle = &s_event_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY | F_NOTIFY_SEC },
            { .uuid = &k_scan.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ | F_READ_SEC },
            { 0 },
        },
    },
    { 0 },
};

static uint32_t rand32(void *ctx)
{
    (void)ctx;
    return esp_random();
}

/* Bench builds print the code on the USB console so laptop tests can pair
 * unattended (tools/ble/oc_ble.py --passkey-from-console). Production builds
 * never log it: it is shown only on the OLED. */
static void log_code(const char *why)
{
#ifdef LC_BENCH_LOW_POWER
    taskENTER_CRITICAL(&s_pair_mux);
    uint32_t code = lc_term_pair_code(&s_pair);
    taskEXIT_CRITICAL(&s_pair_mux);
    ESP_LOGW(TAG, "%s; pair code %06lu", why, (unsigned long)code);
#else
    ESP_LOGI(TAG, "%s", why);
#endif
}

/* A Service Changed still owed to bonded phones (gatt_table_check()).
 * NimBLE persists each bonded peer's "indicate on reconnect" mark in the bond
 * store, but keeps the handle range the indication carries only in RAM: after
 * a reboot the mark alone sends nothing. So "pending" lives in NVS as well,
 * with the phones that have confirmed the indication, and every boot calls
 * ble_svc_gatt_changed() again until each bonded phone subscribed to Service
 * Changed has confirmed it. Host task only. */
static bool s_sc_pend;
static bool s_sc_logged; /* the boot's "still pending" line was printed */
static ble_addr_t s_sc_done[CONFIG_BT_NIMBLE_MAX_BONDS];
static uint8_t s_sc_done_n;
static uint16_t s_sc_handle; /* Service Changed value handle, set in on_sync */

static bool sc_done_has(const ble_addr_t *a)
{
    for (uint8_t i = 0; i < s_sc_done_n; i++) {
        if (ble_addr_cmp(&s_sc_done[i], a) == 0) {
            return true;
        }
    }
    return false;
}

/* Only a phone that wrote the Service Changed CCCD is ever indicated (NimBLE
 * persists that CCCD per bond): one that never subscribed can't confirm. */
static bool sc_subscribed(const ble_addr_t *peer)
{
    struct ble_store_key_cccd k = { .peer_addr = *peer, .chr_val_handle = s_sc_handle, .idx = 0 };
    struct ble_store_value_cccd v;
    return ble_store_read_cccd(&k, &v) == 0 && v.flags != 0;
}

static void sc_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(GATT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Service Changed state: nvs_open %s", esp_err_to_name(err));
        return;
    }
    if (s_sc_pend) {
        err = nvs_set_u8(h, SC_NVS_PEND, 1);
        if (err == ESP_OK && s_sc_done_n > 0) {
            err = nvs_set_blob(h, SC_NVS_DONE, s_sc_done, s_sc_done_n * sizeof(s_sc_done[0]));
        } else if (err == ESP_OK) {
            err = nvs_erase_key(h, SC_NVS_DONE);
        }
    } else {
        err = nvs_erase_key(h, SC_NVS_PEND);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            err = nvs_erase_key(h, SC_NVS_DONE);
        }
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Service Changed state not saved: %s", esp_err_to_name(err));
    }
}

/* Checks a pending Service Changed against the current bond list, so a bond
 * added, deleted or cleared meanwhile counts right: confirmations from phones
 * no longer bonded are dropped, and with none left to tell it is done. dirty:
 * s_sc_done changed, save it; log: print what is still owed. */
static void sc_settle(bool dirty, bool log)
{
    if (!s_sc_pend) {
        return;
    }
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, CONFIG_BT_NIMBLE_MAX_BONDS) != 0) {
        return;
    }
    uint8_t kept = 0;
    for (uint8_t i = 0; i < s_sc_done_n; i++) {
        for (int j = 0; j < n; j++) {
            if (ble_addr_cmp(&s_sc_done[i], &peers[j]) == 0) {
                s_sc_done[kept++] = s_sc_done[i];
                break;
            }
        }
    }
    dirty |= kept != s_sc_done_n;
    s_sc_done_n = kept;
    int left = 0;
    for (int j = 0; j < n; j++) {
        if (!sc_done_has(&peers[j]) && sc_subscribed(&peers[j])) {
            left++;
        }
    }
    if (left == 0) {
        if (s_sc_done_n > 0) {
            ESP_LOGI(TAG, "Service Changed delivered to every bonded phone");
        } else {
            ESP_LOGI(TAG, "Service Changed no longer pending: no bonded phone subscribed to it");
        }
        s_sc_pend = false;
        s_sc_done_n = 0;
        sc_save();
        return;
    }
    if (dirty) {
        sc_save();
    }
    if (log) {
        ESP_LOGI(TAG, "Service Changed still pending for %d of %d bonded phone(s)", left, n);
    }
}

/* peer has the current table: it confirmed the indication, or just paired
 * (and so discovered the services afresh). */
static void sc_confirmed(const ble_addr_t *peer)
{
    if (!s_sc_pend) {
        return;
    }
    bool dirty = false;
    if (!sc_done_has(peer) && s_sc_done_n < CONFIG_BT_NIMBLE_MAX_BONDS) {
        s_sc_done[s_sc_done_n++] = *peer;
        dirty = true;
    }
    sc_settle(dirty, dirty);
}

static void update_bonds(void)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, CONFIG_BT_NIMBLE_MAX_BONDS) == 0) {
        s_bonds = (uint8_t)n;
    }
    sc_settle(false, false);
}

static void pair_failed(int status)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    taskENTER_CRITICAL(&s_pair_mux);
    lc_term_pair_failed(&s_pair, now);
    int locked = lc_term_pair_locked(&s_pair, now);
    taskEXIT_CRITICAL(&s_pair_mux);
    ESP_LOGW(TAG, "pairing failed (status %d)%s", status, locked ? "; pairing locked for 60 s" : "");
    log_code("new code after a failed attempt");
}

/* The central asked to pair: show the passkey (inject it into the SM). While
 * locked out, drop the link instead; the OLED still jumps to the Pairing
 * screen, which then says "LOCKED nnS" (spec §2: "OLED says so"). */
static void on_passkey(uint16_t conn, const struct ble_gap_passkey_params *p)
{
    if (p->action != BLE_SM_IOACT_DISP) {
        ESP_LOGW(TAG, "passkey action %u not supported", p->action);
        return;
    }
    uint64_t now = (uint64_t)esp_timer_get_time();
    taskENTER_CRITICAL(&s_pair_mux);
    uint32_t left = lc_term_pair_lock_left_s(&s_pair, now);
    uint32_t code = lc_term_pair_code(&s_pair);
    taskEXIT_CRITICAL(&s_pair_mux);
    if (left > 0) {
        ESP_LOGW(TAG, "pairing refused: locked for %lu s", (unsigned long)left);
        term_oled_pairing_started();
        term_oled_pairing_ended(); /* back to the previous screen 10 s from now */
        ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
        return;
    }
    s_pairing = 1;
    term_oled_pairing_started();
    log_code("pairing started");
    struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = code };
    int rc = ble_sm_inject_io(conn, &io);
    if (rc != 0) {
        ESP_LOGE(TAG, "passkey inject: %d", rc);
    }
}

static void on_enc_change(uint16_t conn, int status)
{
    struct ble_gap_conn_desc d;
    int found = ble_gap_conn_find(conn, &d) == 0;
    int authenticated = status == 0 && found && d.sec_state.encrypted && d.sec_state.authenticated;
    if (s_pairing) {
        s_pairing = 0;
        term_oled_pairing_ended();
        if (authenticated) {
            taskENTER_CRITICAL(&s_pair_mux);
            lc_term_pair_succeeded(&s_pair);
            taskEXIT_CRITICAL(&s_pair_mux);
            ESP_LOGI(TAG, "paired");
            sc_confirmed(&d.peer_id_addr); /* a new bond has the current table */
        } else {
            pair_failed(status);
        }
    } else if (status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING)) {
        /* The peer asked to re-encrypt with an LTK we don't have: typically a
         * phone that still holds a bond this terminal has cleared. Not a
         * passkey guess, and distinct from a refused legacy request, the
         * lock-out terminate (ENOTCONN) or an SM timeout, which land below
         * with their plain status value. */
        ESP_LOGW(TAG, "encryption failed (status %d); the phone may hold a stale bond", status);
    } else if (status != 0) {
        ESP_LOGW(TAG, "encryption failed (status %d)", status);
    } else if (authenticated) {
        ESP_LOGI(TAG, "bonded phone reconnected");
    }
    if (status == 0 && found && !d.sec_state.authenticated) {
        /* Just Works (a central without a keyboard): encrypted but not
         * authenticated, so every characteristic refuses it anyway. Don't
         * keep its bond. */
        ESP_LOGW(TAG, "unauthenticated pairing refused");
        ble_store_util_delete_peer(&d.peer_id_addr);
        ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
    }
    update_bonds();
}

/* A phone that forgot this terminal pairs again: drop its old bond and let
 * the new pairing go ahead (it still needs the code). */
static int on_repeat_pairing(const struct ble_gap_repeat_pairing *rp)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(rp->conn_handle, &d) == 0) {
        ble_store_util_delete_peer(&d.peer_id_addr);
    }
    update_bonds();
    ESP_LOGI(TAG, "repeat pairing: old bond deleted");
    return BLE_GAP_REPEAT_PAIRING_RETRY;
}

/* The store is full: the oldest bond makes room (ble_store_util_status_rr),
 * but only for a pairing that got as far as the passkey. */
static int store_status(struct ble_store_status_event *ev, void *arg)
{
    if (ev->event_code == BLE_STORE_EVENT_OVERFLOW && !s_pairing) {
        return BLE_HS_ESTORE_CAP;
    }
    return ble_store_util_status_rr(ev, arg);
}

static void advertise(void);

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_pairing = 0;
        s_conn = ev->connect.status == 0 ? ev->connect.conn_handle : BLE_HS_CONN_HANDLE_NONE;
        if (ev->connect.status != 0) {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        scan_snap_drop(ev->disconnect.conn.conn_handle);
        if (s_pairing) { /* dropped halfway through a pairing: a failed attempt */
            s_pairing = 0;
            term_oled_pairing_ended();
            pair_failed(ev->disconnect.reason);
        }
        taskENTER_CRITICAL(&s_pair_mux);
        lc_term_pair_disconnected(&s_pair);
        taskEXIT_CRITICAL(&s_pair_mux);
        log_code("phone disconnected");
        advertise();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        on_passkey(ev->passkey.conn_handle, &ev->passkey.params);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        on_enc_change(ev->enc_change.conn_handle, ev->enc_change.status);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return on_repeat_pairing(&ev->repeat_pairing);
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_NOTIFY_TX: /* EDONE: the phone confirmed the indication */
        if (ev->notify_tx.indication && ev->notify_tx.status == BLE_HS_EDONE && s_sc_handle != 0 &&
            ev->notify_tx.attr_handle == s_sc_handle) {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(ev->notify_tx.conn_handle, &d) == 0) {
                ESP_LOGI(TAG, "Service Changed confirmed by a bonded phone");
                sc_confirmed(&d.peer_id_addr);
            }
        }
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

/* After a firmware update that changed the attribute table, tell every
 * bonded phone its cached copy is stale: ble_svc_gatt_changed() marks the
 * Service Changed CCCD record of each bonded peer that subscribed to it
 * (persisted in the bond store) so NimBLE indicates it when that phone next
 * connects and encrypts, and the phone re-discovers the services. The new
 * version is stored at once together with sc_pend; while sc_pend is set every
 * boot calls ble_svc_gatt_changed() again (the range it sends is RAM-only),
 * until each bonded phone has confirmed (sc_settle()). Host task, from
 * on_sync. */
static void gatt_table_check(void)
{
    if (s_sc_handle == 0 &&
        ble_gatts_find_chr(BLE_UUID16_DECLARE(0x1801), BLE_UUID16_DECLARE(0x2A05), NULL, &s_sc_handle) != 0) {
        ESP_LOGE(TAG, "no Service Changed characteristic");
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(GATT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GATT table version: nvs_open %s", esp_err_to_name(err));
        return;
    }
    uint8_t stored = 0;
    err = nvs_get_u8(h, GATT_NVS_KEY, &stored);
    if (err == ESP_OK && stored == GATT_TABLE_VER) {
        uint8_t pend = 0;
        if (nvs_get_u8(h, SC_NVS_PEND, &pend) == ESP_OK && pend) {
            s_sc_pend = true;
            size_t len = sizeof(s_sc_done);
            if (nvs_get_blob(h, SC_NVS_DONE, s_sc_done, &len) == ESP_OK && len % sizeof(s_sc_done[0]) == 0) {
                s_sc_done_n = (uint8_t)(len / sizeof(s_sc_done[0]));
            } else {
                s_sc_done_n = 0;
            }
        }
        nvs_close(h);
    } else {
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "GATT table version unreadable: %s", esp_err_to_name(err));
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "GATT table v%u -> v%u: Service Changed queued for %u bonded phone(s)", stored,
                     GATT_TABLE_VER, s_bonds);
        } else {
            ESP_LOGI(TAG, "GATT table v%u (no version stored): Service Changed queued for %u bonded phone(s)",
                     GATT_TABLE_VER, s_bonds);
        }
        s_sc_pend = true;
        s_sc_done_n = 0;
        s_sc_logged = true; /* the line above says it */
        err = nvs_set_u8(h, GATT_NVS_KEY, GATT_TABLE_VER);
        if (err == ESP_OK) {
            err = nvs_set_u8(h, SC_NVS_PEND, 1);
        }
        if (err == ESP_OK) {
            err = nvs_erase_key(h, SC_NVS_DONE);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GATT table version not saved: %s", esp_err_to_name(err));
        }
    }
    if (!s_sc_pend) {
        return;
    }
    ble_svc_gatt_changed(0x0001, 0xFFFF);
    sc_settle(false, !s_sc_logged);
    s_sc_logged = true;
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_addr_type);
    update_bonds();
    ESP_LOGI(TAG, "%u bonded phone(s)", s_bonds);
    gatt_table_check();
    advertise();
}

/* Runs on the NimBLE host task (term_ble_clear_bonds posts it there). */
static void clear_bonds(struct ble_npl_event *ev)
{
    (void)ev;
    int rc = ble_store_clear();
    update_bonds();
    ESP_LOGW(TAG, "all bonds cleared (rc %d, %u left)", rc, s_bonds);
    uint16_t c = s_conn;
    if (c != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM); /* the phone must pair again */
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Notifications go only to a phone on an encrypted, authenticated link.
 * Returns the checked connection handle (BLE_HS_CONN_HANDLE_NONE if the link
 * isn't secure), so the caller notifies the connection it just verified
 * instead of re-reading s_conn, which a disconnect could change in between. */
static uint16_t link_secure(void)
{
    struct ble_gap_conn_desc d;
    uint16_t c = s_conn;
    if (c == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(c, &d) != 0 || !d.sec_state.encrypted ||
        !d.sec_state.authenticated) {
        return BLE_HS_CONN_HANDLE_NONE;
    }
    return c;
}

static void notify(uint16_t conn, uint16_t handle, const uint8_t *d, uint8_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, len);
    if (om != NULL) {
        ble_gatts_notify_custom(conn, handle, om);
    }
}

/* Sends queued DOWN payloads, EVENTs and STATUS changes to the connected phone. */
static void notify_task(void *arg)
{
    (void)arg;
    for (;;) {
        QueueSetMemberHandle_t q = xQueueSelectFromSet(s_out_set, pdMS_TO_TICKS(200));
        uint16_t c;
        if (q == s_down_q) {
            down_msg_t m;
            if (xQueueReceive(s_down_q, &m, 0) == pdTRUE && (c = link_secure()) != BLE_HS_CONN_HANDLE_NONE) {
                notify(c, s_down_handle, m.data, m.len);
            }
        } else if (q == s_event_q) {
            event_msg_t m;
            if (xQueueReceive(s_event_q, &m, 0) == pdTRUE && (c = link_secure()) != BLE_HS_CONN_HANDLE_NONE) {
                notify(c, s_event_handle, m.data, m.len);
            }
        }
        if (s_status_dirty && (c = link_secure()) != BLE_HS_CONN_HANDLE_NONE) {
            s_status_dirty = 0;
            uint8_t out[LC_GATT_STATUS_LEN];
            read_status(out);
            notify(c, s_status_handle, out, sizeof(out));
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

void term_ble_pair_view(lc_term_pair_view_t *out, uint64_t now_us)
{
    memset(out, 0, sizeof(*out));
    taskENTER_CRITICAL(&s_pair_mux);
    out->code = lc_term_pair_code(&s_pair);
    out->locked_s = lc_term_pair_lock_left_s(&s_pair, now_us);
    taskEXIT_CRITICAL(&s_pair_mux);
    out->bonds = s_bonds;
    out->max_bonds = CONFIG_BT_NIMBLE_MAX_BONDS;
    out->phone = s_conn != BLE_HS_CONN_HANDLE_NONE;
}

void term_ble_clear_bonds(void)
{
    if (!s_started) { /* nimble_port_init failed: s_clear_ev was never initialised */
        return;
    }
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_clear_ev);
}

void term_ble_start(uint32_t tmid)
{
    scan_snap_init();
    s_down_q = xQueueCreate(8, sizeof(down_msg_t));
    s_event_q = xQueueCreate(8, sizeof(event_msg_t));
    s_out_set = xQueueCreateSet(16);
    xQueueAddToSet(s_down_q, s_out_set);
    xQueueAddToSet(s_event_q, s_out_set);
    snprintf(s_name, sizeof(s_name), "OpenCell-%08lX", (unsigned long)tmid);
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed");
        /* No BT (and so no true RNG source): esp_random() is only
         * pseudo-random until the RF subsystem or the bootloader's own
         * entropy source is running (IDF "Random Number Generation"). Bracket
         * the draw with the bootloader source so the pair state still gets a
         * true-random code, even though pairing itself won't come up. */
        bootloader_random_enable();
        lc_term_pair_init(&s_pair, rand32, NULL);
        bootloader_random_disable();
        log_code("boot");
        return;
    }
    /* nimble_port_init() has brought BT up, so esp_random() is now a true
     * RNG (IDF "Random Number Generation"): draw the boot code only now. */
    lc_term_pair_init(&s_pair, rand32, NULL);
    log_code("boot");
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = store_status;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_sec_lvl = 3; /* refuse a pairing request without MITM (CONFIG_BT_NIMBLE_SM_LVL=3 too) */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID; /* phones use RPAs */
    ble_npl_event_init(&s_clear_ev, clear_bonds, NULL);
    s_started = true;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(k_svcs);
    ble_gatts_add_svcs(k_svcs);
    ble_svc_gap_device_name_set(s_name);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    xTaskCreatePinnedToCore(notify_task, "lc_ble_tx", 4096, NULL, 4, NULL, 0); /* with NimBLE; core 1 is the radio's */
}
