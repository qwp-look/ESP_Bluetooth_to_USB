/*
 * hci_tl_bth.c — esp_bt_hci_tl_t 的 USB class 0xE0 实现（阶段 2）。
 *
 * host→controller（补 H:4 类型字节后交给控制器的 H4-TL 解析层）：
 *   tud_bt_hci_cmd_cb            (EP0)  → 0x01 + 命令 → ringbuffer
 *   tud_bt_acl_data_received_cb  (BULK OUT) → 0x02 + ACL → ringbuffer
 *   ringbuffer → rx_task → 控制器挂起的读请求（与 CDC 版相同）
 *
 * controller→host（去掉 H:4 类型字节后按包类型分流到对应端点）：
 *   TL _send(0x04 开头) → 事件池 → tud_bt_event_send (INT IN)
 *   TL _send(0x02 开头) → ACL 池  → tud_bt_acl_data_send (BULK IN)
 *   池满丢弃；发送完成在 sent_cb 里回调控制器释放缓冲。
 *   端点同一时刻只允许一个在途传输，用 pump 任务串行启动。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "tusb.h"

#include "hci_tl_bth.h"
#include "ctrl_tusb.h"

static const char *TAG = "hci_tl_bth";

#define TL_RX_RING_SIZE 2048

/* HCI 包上限：事件 2+255；ACL（BLE）4+251。留裕量。 */
#define HCI_EVT_MAX 260
#define EVT_POOL_N  6
#define HCI_ACL_MAX 512
#define ACL_POOL_N  10

/* ── host→controller：ringbuffer + 挂起读（同 CDC 版） ─────────────── */

static struct {
    esp_bt_hci_tl_callback_t cb;   /* NULL = 未挂起 */
    void                     *arg;
    uint8_t                  *buf;
    uint32_t                 remaining;
} s_rx;

static portMUX_TYPE s_rx_lock = portMUX_INITIALIZER_UNLOCKED;
static RingbufHandle_t  s_rx_ring;
static SemaphoreHandle_t s_rx_event;

/* USB 侧没有 H:4 类型字节，进 ringbuffer 前补上 */
static void rx_feed_h4(uint8_t h4_type, const void *pkt, uint32_t len)
{
    uint8_t hdr = h4_type;
    if (xRingbufferSend(s_rx_ring, &hdr, 1, 0) != pdTRUE) {
        g_bridge_stats.rx_dropped += 1 + len;
        return;
    }
    if (len && xRingbufferSend(s_rx_ring, pkt, len, 0) != pdTRUE) {
        g_bridge_stats.rx_dropped += len;
        return;
    }
    g_bridge_stats.rx_bytes += 1 + len;
    xSemaphoreGive(s_rx_event);
}

/* TinyUSB bth 类回调（tud_task 上下文） */
void tud_bt_hci_cmd_cb(void *hci_cmd, size_t cmd_len)
{
    if (cmd_len > 258) {
        g_bridge_stats.rx_dropped += cmd_len;
        return;
    }
    rx_feed_h4(0x01, hci_cmd, cmd_len);
}

/* USB bth 驱动按端点大小分片递交 ACL 包（BLE ACL 最大 255B > 64B 端点）。
 * H:4 流要求整包只有一个类型字节：包首片段补 0x02，续片裸透传。 */
static uint16_t s_acl_rx_remaining;

void tud_bt_acl_data_received_cb(void *acl_data, uint16_t data_len)
{
    uint8_t *p = (uint8_t *)acl_data;

    if (s_acl_rx_remaining == 0) {
        if (data_len < 4 || data_len > HCI_ACL_MAX) {
            g_bridge_stats.rx_dropped += data_len;
            return;
        }
        uint16_t total = 4 + (uint16_t)(p[2] | (p[3] << 8));
        if (total < data_len) {
            g_bridge_stats.rx_dropped += data_len;
            return;
        }
        s_acl_rx_remaining = total - data_len;
        rx_feed_h4(0x02, p, data_len);
    } else {
        if (xRingbufferSend(s_rx_ring, p, data_len, 0) != pdTRUE) {
            g_bridge_stats.rx_dropped += data_len;
            s_acl_rx_remaining = 0;
            return;
        }
        g_bridge_stats.rx_bytes += data_len;
        s_acl_rx_remaining = (data_len <= s_acl_rx_remaining)
                             ? (uint16_t)(s_acl_rx_remaining - data_len) : 0;
        xSemaphoreGive(s_rx_event);
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_rx_event, portMAX_DELAY);
        for (;;) {
            bool armed, complete = false;
            esp_bt_hci_tl_callback_t cb = NULL;
            void *cb_arg = NULL;

            portENTER_CRITICAL(&s_rx_lock);
            armed = s_rx.cb != NULL;
            if (!armed || s_rx.remaining == 0) {
                portEXIT_CRITICAL(&s_rx_lock);
                break;
            }
            size_t item_size = 0;
            uint8_t *data = xRingbufferReceiveUpTo(s_rx_ring, &item_size, 0,
                                                   s_rx.remaining);
            if (data == NULL || item_size == 0) {
                portEXIT_CRITICAL(&s_rx_lock);
                break;
            }
            memcpy(s_rx.buf, data, item_size);
            s_rx.buf += item_size;
            s_rx.remaining -= item_size;
            if (s_rx.remaining == 0) {
                complete = true;
                cb = s_rx.cb;
                cb_arg = s_rx.arg;
                s_rx.cb = NULL;
            }
            portEXIT_CRITICAL(&s_rx_lock);

            vRingbufferReturnItem(s_rx_ring, data);

            if (complete) {
                cb(cb_arg, ESP_BT_HCI_TL_STATUS_OK);
                esp_bt_h4tl_eif_io_event_notify(1);
            }
        }
    }
}

/* ── controller→host：发送池 + pump 任务 ───────────────────────────── */

typedef struct {
    uint8_t                  data[HCI_EVT_MAX];
    uint16_t                 len;
    uint16_t                 sent;   /* 已发出字节数（分片推进） */
    esp_bt_hci_tl_callback_t cb;
    void                     *arg;
} tx_slot_t;

typedef struct {
    tx_slot_t slot[EVT_POOL_N];
    uint8_t  head, count;          /* FIFO：head=在途/下一个发送位 */
    bool     in_flight;
} txq_t;                            /* ACL 池用同一结构、更大的槽数 */

static txq_t s_evt_q;               /* 事件（INT IN） */
static txq_t s_acl_q;               /* ACL（BULK IN），槽改用 ACL 大小 */

/* ACL 需要更大的槽——单独的静态池，结构复用 */
typedef struct {
    uint8_t                  data[HCI_ACL_MAX];
    uint16_t                 len;
    uint16_t                 sent;
    esp_bt_hci_tl_callback_t cb;
    void                     *arg;
} acl_slot_t;
static struct {
    acl_slot_t slot[ACL_POOL_N];
    uint8_t   head, count;
    bool      in_flight;
} s_aclq;

static SemaphoreHandle_t s_pump_sem;   /* 有新待发 / 有完成 */
static portMUX_TYPE s_tx_lock = portMUX_INITIALIZER_UNLOCKED;

static void pump_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_pump_sem, portMAX_DELAY);

        /* 事件端点空闲且有排队 → 启动传输 */
        portENTER_CRITICAL(&s_tx_lock);
        bool start_evt = !s_evt_q.in_flight && s_evt_q.count > 0;
        bool start_acl = !s_aclq.in_flight && s_aclq.count > 0;
        tx_slot_t *e = &s_evt_q.slot[s_evt_q.head];
        acl_slot_t *a = &s_aclq.slot[s_aclq.head];
        portEXIT_CRITICAL(&s_tx_lock);

        /* 按端点最大包长分片：DWC2 对中断/批量端点的多包链式传输
         * 不可靠，逐片确认推进（与 h4_bridge.c 同款修复）。 */
        if (start_evt) {
            uint16_t chunk = e->len - e->sent;
            if (chunk > CFG_TUD_BTH_EVENT_EPSIZE - 1) chunk = CFG_TUD_BTH_EVENT_EPSIZE - 1;  /* 恒短包 */
            if (tud_bt_event_send(e->data + e->sent, chunk)) {
                portENTER_CRITICAL(&s_tx_lock);
                s_evt_q.in_flight = true;
                portEXIT_CRITICAL(&s_tx_lock);
            } else {
                xSemaphoreGive(s_pump_sem);
            }
        }

        if (start_acl) {
            uint16_t chunk = a->len - a->sent;
            if (chunk > CFG_TUD_BTH_DATA_EPSIZE - 1) chunk = CFG_TUD_BTH_DATA_EPSIZE - 1;   /* 恒短包 */
            if (tud_bt_acl_data_send(a->data + a->sent, chunk)) {
                portENTER_CRITICAL(&s_tx_lock);
                s_aclq.in_flight = true;
                portEXIT_CRITICAL(&s_tx_lock);
            } else {
                xSemaphoreGive(s_pump_sem);
            }
        }
    }
}

/* TinyUSB 发送完成回调（tud_task 上下文） */
void tud_bt_event_sent_cb(uint16_t sent_bytes)
{
    esp_bt_hci_tl_callback_t cb = NULL;
    void *arg = NULL;

    portENTER_CRITICAL(&s_tx_lock);
    tx_slot_t *s = &s_evt_q.slot[s_evt_q.head];
    s->sent += sent_bytes;
    if (s->sent >= s->len) {
        cb = s->cb;
        arg = s->arg;
        s->sent = 0;
        s_evt_q.head = (s_evt_q.head + 1) % EVT_POOL_N;
        s_evt_q.count--;
    }
    s_evt_q.in_flight = false;
    portEXIT_CRITICAL(&s_tx_lock);

    if (cb) {
        cb(arg, ESP_BT_HCI_TL_STATUS_OK);
        esp_bt_h4tl_eif_io_event_notify(1);
    }
    xSemaphoreGive(s_pump_sem);
}

void tud_bt_acl_data_sent_cb(uint16_t sent_bytes)
{
    esp_bt_hci_tl_callback_t cb = NULL;
    void *arg = NULL;

    portENTER_CRITICAL(&s_tx_lock);
    acl_slot_t *s = &s_aclq.slot[s_aclq.head];
    s->sent += sent_bytes;
    if (s->sent >= s->len) {
        cb = s->cb;
        arg = s->arg;
        s->sent = 0;
        s_aclq.head = (s_aclq.head + 1) % ACL_POOL_N;
        s_aclq.count--;
    }
    s_aclq.in_flight = false;
    portEXIT_CRITICAL(&s_tx_lock);

    if (cb) {
        cb(arg, ESP_BT_HCI_TL_STATUS_OK);
        esp_bt_h4tl_eif_io_event_notify(1);
    }
    xSemaphoreGive(s_pump_sem);
}

/* ── esp_bt_hci_tl_t 回调 ──────────────────────────────────────────── */

static int tl_open(void)  { return 0; }
static void tl_close(void) { }

static void tl_recv(uint8_t *buf, uint32_t len,
                    esp_bt_hci_tl_callback_t callback, void *arg)
{
    portENTER_CRITICAL(&s_rx_lock);
    s_rx.cb = callback;
    s_rx.arg = arg;
    s_rx.buf = buf;
    s_rx.remaining = len;
    portEXIT_CRITICAL(&s_rx_lock);
    xSemaphoreGive(s_rx_event);
}

static void tl_send(uint8_t *buf, uint32_t len,
                    esp_bt_hci_tl_callback_t callback, void *arg)
{
    /* buf[0] 是 H:4 类型字节；USB 侧不需要它 */
    uint8_t type = buf[0];
    const uint8_t *payload = buf + 1;
    uint32_t plen = len - 1;

    if (type == 0x04) {          /* 事件 */
        if (plen == 0 || plen > HCI_EVT_MAX) {
            goto drop;
        }
        portENTER_CRITICAL(&s_tx_lock);
        if (s_evt_q.count >= EVT_POOL_N) {
            portEXIT_CRITICAL(&s_tx_lock);
            goto drop;
        }
        uint8_t idx = (s_evt_q.head + s_evt_q.count) % EVT_POOL_N;
        memcpy(s_evt_q.slot[idx].data, payload, plen);
        s_evt_q.slot[idx].len = plen;
        s_evt_q.slot[idx].cb  = callback;
        s_evt_q.slot[idx].arg = arg;
        s_evt_q.count++;
        portEXIT_CRITICAL(&s_tx_lock);
        g_bridge_stats.tx_bytes += len;
        xSemaphoreGive(s_pump_sem);
        return;
    }

    if (type == 0x02) {          /* ACL */
        if (plen == 0 || plen > HCI_ACL_MAX) {
            goto drop;
        }
        portENTER_CRITICAL(&s_tx_lock);
        if (s_aclq.count >= ACL_POOL_N) {
            portEXIT_CRITICAL(&s_tx_lock);
            goto drop;
        }
        uint8_t idx = (s_aclq.head + s_aclq.count) % ACL_POOL_N;
        memcpy(s_aclq.slot[idx].data, payload, plen);
        s_aclq.slot[idx].len = plen;
        s_aclq.slot[idx].cb  = callback;
        s_aclq.slot[idx].arg = arg;
        s_aclq.count++;
        portEXIT_CRITICAL(&s_tx_lock);
        g_bridge_stats.tx_bytes += len;
        xSemaphoreGive(s_pump_sem);
        return;
    }

    /* 0x03 SCO：BLE 控制器不会发；其他类型直接丢弃 */
    ESP_LOGW(TAG, "unsupported h4 type 0x%02x dropped", type);

drop:
    g_bridge_stats.tx_dropped += len;
    callback(arg, ESP_BT_HCI_TL_STATUS_OK);   /* 必须回调，控制器才能复用缓冲 */
    esp_bt_h4tl_eif_io_event_notify(1);
}

static bool tl_flow_off(void) { return true; }   /* USB 自带流控 */
static void tl_flow_on(void)  { }
static void tl_finish_transfers(void) { }

esp_bt_hci_tl_t hci_tl_bth_funcs = {
    ._magic            = ESP_BT_HCI_TL_MAGIC_VALUE,
    ._version          = ESP_BT_HCI_TL_VERSION,
    ._reserved         = 0,
    ._open             = tl_open,
    ._close            = tl_close,
    ._finish_transfers = tl_finish_transfers,
    ._recv             = tl_recv,
    ._send             = tl_send,
    ._flow_off         = tl_flow_off,
    ._flow_on          = tl_flow_on,
};

esp_err_t hci_tl_bth_init(void)
{
    s_rx_ring = xRingbufferCreate(TL_RX_RING_SIZE, RINGBUF_TYPE_BYTEBUF);
    s_rx_event = xSemaphoreCreateBinary();
    s_pump_sem = xSemaphoreCreateBinary();
    if (!s_rx_ring || !s_rx_event || !s_pump_sem) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(rx_task, "hci_rx", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(pump_task, "hci_pump", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "HCI transport over USB bth (0xE0) ready");
    return ESP_OK;
}
