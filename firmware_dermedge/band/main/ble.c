#include "ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);  // NimBLE's NVS bond store; IDF ships no header for it

static const char *TAG = "ble";
static ble_cb_t cb;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t status_handle, data_handle;
static bool secure, data_subscribed, status_subscribed;
static uint8_t own_addr_type;
static char device_id[24] = "DE-unknown";
static char status_json[200] = "{}";

// a3c875xx-8ed3-4bdf-8a39-a01bebede295, little-endian
#define DERMEDGE_UUID(b)                                                                              \
    BLE_UUID128_INIT(0x95, 0xe2, 0xed, 0xeb, 0x1b, 0xa0, 0x39, 0x8a, 0xdf, 0x4b, 0xd3, 0x8e, b, 0x75, \
                     0xc8, 0xa3)
static const ble_uuid128_t svc_uuid = DERMEDGE_UUID(0x10);
static const ble_uuid128_t status_uuid = DERMEDGE_UUID(0x11);
static const ble_uuid128_t data_uuid = DERMEDGE_UUID(0x12);
static const ble_uuid128_t cmd_uuid = DERMEDGE_UUID(0x13);

static int chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == status_handle)
        return os_mbuf_append(ctxt->om, status_json, strlen(status_json)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[200];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        buf[len] = 0;
        cb(BLE_EV_COMMAND, 0, buf);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

#define SECURE_READ (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN)
#define SECURE_WRITE (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN)

static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &svc_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {.uuid = &status_uuid.u, .access_cb = chr_access, .flags = SECURE_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &status_handle},
             // notify-only: notifications go out only on a secure link, see notify()
             {.uuid = &data_uuid.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &data_handle},
             {.uuid = &cmd_uuid.u, .access_cb = chr_access, .flags = SECURE_WRITE},
             {0},
         }},
    {0},
};

static void advertise(void);

static int gap_event(struct ble_gap_event *ev, void *arg) {
    struct ble_gap_conn_desc desc;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status) {
            advertise();
            break;
        }
        conn = ev->connect.conn_handle;
        ESP_LOGI(TAG, "connected, asking for a secure link");
        ble_gap_security_initiate(conn);  // pair (or resume the bond) straight away
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
        bool was_secure = secure;
        conn = BLE_HS_CONN_HANDLE_NONE;
        secure = data_subscribed = status_subscribed = false;
        if (was_secure) cb(BLE_EV_LINK_DOWN, 0, NULL);
        advertise();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ev->enc_change.status == 0 && ble_gap_conn_find(ev->enc_change.conn_handle, &desc) == 0 &&
            desc.sec_state.encrypted && desc.sec_state.authenticated) {
            ESP_LOGI(TAG, "secure link up%s", desc.sec_state.bonded ? " (bonded)" : "");
            secure = true;
            cb(BLE_EV_LINK_UP, 0, NULL);
        } else {
            ESP_LOGW(TAG, "pairing failed (status %d)", ev->enc_change.status);
            ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io pk = {.action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000};
            ble_sm_inject_io(ev->passkey.conn_handle, &pk);
            ESP_LOGI(TAG, "pairing code shown on screen");
            cb(BLE_EV_PASSKEY, pk.passkey, NULL);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // the laptop forgot the band: drop the old bond and pair again
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == data_handle) data_subscribed = ev->subscribe.cur_notify;
        if (ev->subscribe.attr_handle == status_handle) status_subscribed = ev->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", ev->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void) {
    struct ble_hs_adv_fields adv = {0}, rsp = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = &svc_uuid;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    const char *name = ble_svc_gap_device_name();
    rsp.name = (const uint8_t *)name;  // the name in the scan response: the 128-bit UUID fills the advert
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (!rc) rc = ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    if (!rc) rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) ESP_LOGE(TAG, "advertising failed: %d", rc);
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    uint8_t a[6];
    ble_hs_id_copy_addr(own_addr_type, a, NULL);
    snprintf(device_id, sizeof(device_id), "DE-%02X%02X%02X%02X%02X%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    ESP_LOGI(TAG, "advertising as DermEdge, id %s", device_id);
    advertise();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_start(ble_cb_t callback) {
    cb = callback;
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;  // the band shows the code, the laptop types it
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set("DermEdge"));
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

bool ble_secure(void) { return secure; }
bool ble_data_ready(void) { return secure && data_subscribed; }
const char *ble_device_id(void) { return device_id; }

static bool notify(uint16_t handle, const char *s) {
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s, strlen(s));
    if (!om) return false;
    return ble_gatts_notify_custom(conn, handle, om) == 0;  // frees om either way
}

bool ble_notify_status(const char *json) {
    strlcpy(status_json, json, sizeof(status_json));  // what a plain read returns
    return secure && status_subscribed ? notify(status_handle, json) : false;
}

bool ble_notify_data(const char *json) {
    return ble_data_ready() && notify(data_handle, json);
}

void ble_forget_bonds(void) {
    if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    ble_store_clear();
}
