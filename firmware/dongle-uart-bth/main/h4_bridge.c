/*
 * h4_bridge.c — UART H:4 ↔ USB class 0xE0 桥（阶段 3b 最终形态核心）。
 *
 * 数据通路：
 *   经典板 UART(TX17/RX18, 921600) ─► uart_rx_task ─► H:4 解析
 *        ├─ 事件(0x04) → 事件池 → tud_bt_event_send   (EP 0x81 INT IN)
 *        └─ ACL (0x02) → ACL 池 → tud_bt_acl_data_send (EP 0x82 BULK IN)
 *   主机 EP0 命令 ─► tud_bt_hci_cmd_cb ─┐
 *   主机 BULK OUT ACL ─► tud_bt_acl_data_received_cb ─┤→ 补类型字节 → ring → uart_tx_task → UART
 *
 * H:4 解析状态机参考 slank/esp32-bt（Apache-2.0）的拼包逻辑改写为
 * 按块喂入。缓冲全部静态分配，实时路径零动态内存。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "tusb.h"

#include "h4_bridge.h"
#include "ctrl_tusb.h"

static const char *TAG = "h4_bridge";

/* UART 配置：与 radio-bridge、docs/hardware-wiring.md 严格一致 */
#define HCI_UART_NUM      UART_NUM_1
#define HCI_UART_TX_PIN   17   /* → 经典板 GPIO16 (RX2) */
#define HCI_UART_RX_PIN   18   /* ← 经典板 GPIO17 (TX2) */
#define HCI_UART_BAUDRATE 921600

/* 包尺寸上限：事件 2+255；经典蓝牙 ACL 4+1021 */
#define HCI_EVT_MAX 260
#define EVT_POOL_N  6
#define HCI_ACL_MAX 1030
#define ACL_POOL_N  10

/* ── H:4 解析（仅 uart_rx_task 单线程访问） ────────────────────────── */

typedef enum { RX_WAIT_TYPE, RX_WAIT_HDR, RX_WAIT_PAYLOAD } h4_state_t;

static h4_state_t s_state;
static uint8_t  s_pkt[HCI_ACL_MAX + 4];  /* 含类型字节，取最大 */
static uint16_t s_pos;                   /* 已写入字节数 */
static uint16_t s_hdr_need;
static uint16_t s_payload_need;

/* 上行发送池与在途管理（与 dongle-usb-bth 的 hci_tl_bth 相同的 pump 模式，
 * 但完成回调只释放槽位，无需通知控制器） */
typedef struct {
    uint8_t  data[HCI_EVT_MAX];
    uint16_t len;
    uint16_t sent;      /* 已成功发出的字节数（分片推进） */
} evt_slot_t;

typedef struct {
    uint8_t  data[HCI_ACL_MAX];
    uint16_t len;
    uint16_t sent;
} acl_slot_t;

static struct {
    evt_slot_t slot[EVT_POOL_N];
    uint8_t head, count;
    bool    in_flight;
} s_evt_q;

static struct {
    acl_slot_t slot[ACL_POOL_N];
    uint8_t head, count;
    bool    in_flight;
} s_acl_q;

static SemaphoreHandle_t s_pump_sem;
static portMUX_TYPE s_tx_lock = portMUX_INITIALIZER_UNLOCKED;

/* 下行：USB 回调（tud_task 上下文）→ ringbuffer → uart_tx_task */
static RingbufHandle_t s_uart_tx_ring;

/* ── 上行：H:4 解析与入池 ──────────────────────────────────────────── */

static uint8_t h4_hdr_len(uint8_t type)
{
    switch (type) {
    case 0x01: return 3;   /* CMD: opcode(2)+plen(1) */
    case 0x02: return 4;   /* ACL: handle(2)+dlen(2) */
    case 0x03: return 3;   /* SCO: handle(2)+dlen(1) */
    case 0x04: return 2;   /* EVT: code(1)+plen(1) */
    default:   return 0;
    }
}

static uint16_t h4_payload_len(const uint8_t *pkt)
{
    switch (pkt[0]) {
    case 0x01: return pkt[3];
    case 0x02: return (uint16_t)(pkt[3] | (pkt[4] << 8));
    case 0x03: return pkt[3];
    case 0x04: return pkt[2];
    default:   return 0;
    }
}

static void evt_complete(void)
{
    uint16_t plen = s_pos - 1;   /* 去掉类型字节 */
    portENTER_CRITICAL(&s_tx_lock);
    if (s_evt_q.count < EVT_POOL_N && plen <= HCI_EVT_MAX) {
        uint8_t idx = (s_evt_q.head + s_evt_q.count) % EVT_POOL_N;
        memcpy(s_evt_q.slot[idx].data, s_pkt + 1, plen);
        s_evt_q.slot[idx].len = plen;
        s_evt_q.count++;
        portEXIT_CRITICAL(&s_tx_lock);
        g_bridge_stats.tx_bytes += s_pos;
        xSemaphoreGive(s_pump_sem);
    } else {
        portEXIT_CRITICAL(&s_tx_lock);
        g_bridge_stats.tx_dropped += s_pos;
    }
}

static void acl_complete(void)
{
    uint16_t plen = s_pos - 1;
    portENTER_CRITICAL(&s_tx_lock);
    if (s_acl_q.count < ACL_POOL_N && plen <= HCI_ACL_MAX) {
        uint8_t idx = (s_acl_q.head + s_acl_q.count) % ACL_POOL_N;
        memcpy(s_acl_q.slot[idx].data, s_pkt + 1, plen);
        s_acl_q.slot[idx].len = plen;
        s_acl_q.count++;
        portEXIT_CRITICAL(&s_tx_lock);
        g_bridge_stats.tx_bytes += s_pos;
        xSemaphoreGive(s_pump_sem);
    } else {
        portEXIT_CRITICAL(&s_tx_lock);
        g_bridge_stats.tx_dropped += s_pos;
    }
}

static void h4_rx_feed(const uint8_t *data, uint32_t len)
{
    while (len--) {
        uint8_t b = *data++;
        switch (s_state) {
        case RX_WAIT_TYPE:
            if (h4_hdr_len(b) == 0 && b != 0x03) {
                /* 未知类型：跳过（0x03 SCO 有表项，走正常解析后丢弃） */
                break;
            }
            s_pkt[0] = b;
            s_pos = 1;
            s_hdr_need = h4_hdr_len(b);
            s_state = RX_WAIT_HDR;
            break;

        case RX_WAIT_HDR:
            s_pkt[s_pos++] = b;
            if (--s_hdr_need == 0) {
                s_payload_need = h4_payload_len(s_pkt);
                if (s_payload_need == 0) {
                    /* 零载荷：整包完成 */
                    if (s_pkt[0] == 0x04) {
                        evt_complete();
                    } else if (s_pkt[0] == 0x02) {
                        acl_complete();
                    } else if (s_pkt[0] == 0x03) {
                        g_bridge_stats.tx_dropped += s_pos;  /* SCO 无处可去 */
                    }
                    s_state = RX_WAIT_TYPE;
                } else {
                    s_state = RX_WAIT_PAYLOAD;
                }
            }
            break;

        case RX_WAIT_PAYLOAD:
            s_pkt[s_pos++] = b;
            if (--s_payload_need == 0) {
                if (s_pkt[0] == 0x04) {
                    evt_complete();
                } else if (s_pkt[0] == 0x02) {
                    acl_complete();
                } else if (s_pkt[0] == 0x03) {
                    g_bridge_stats.tx_dropped += s_pos;
                }
                s_state = RX_WAIT_TYPE;
            }
            break;
        }
    }
}

static void uart_rx_task(void *arg)
{
    (void)arg;
    static uint8_t buf[512];
    for (;;) {
        int len = uart_read_bytes(HCI_UART_NUM, buf, sizeof(buf),
                                  pdMS_TO_TICKS(50));
        if (len > 0) {
            h4_rx_feed(buf, len);
        }
    }
}

/* ── 上行：USB 发送泵 ──────────────────────────────────────────────── */

static void pump_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_pump_sem, portMAX_DELAY);

        portENTER_CRITICAL(&s_tx_lock);
        bool start_evt = !s_evt_q.in_flight && s_evt_q.count > 0;
        bool start_acl = !s_acl_q.in_flight && s_acl_q.count > 0;
        evt_slot_t *e = &s_evt_q.slot[s_evt_q.head];
        acl_slot_t *a = &s_acl_q.slot[s_acl_q.head];
        portEXIT_CRITICAL(&s_tx_lock);

        /* 按端点最大包长分片发送：DWC2 驱动对中断端点的多包链式传输
         * 不可靠（第二次多包传输永远不完成），自己切片逐片确认。 */
        if (start_evt) {
            uint16_t chunk = e->len - e->sent;
            if (chunk > CFG_TUD_BTH_EVENT_EPSIZE - 1) {  /* 恒短包 */
                chunk = CFG_TUD_BTH_EVENT_EPSIZE - 1;
            }
            if (tud_bt_event_send(e->data + e->sent, chunk)) {
                portENTER_CRITICAL(&s_tx_lock);
                s_evt_q.in_flight = true;
                portEXIT_CRITICAL(&s_tx_lock);
            } else {
                xSemaphoreGive(s_pump_sem);   /* 端点忙，稍后重试 */
            }
        }
        if (start_acl) {
            uint16_t chunk = a->len - a->sent;
            if (chunk > CFG_TUD_BTH_DATA_EPSIZE - 1) {   /* 恒短包 */
                chunk = CFG_TUD_BTH_DATA_EPSIZE - 1;
            }
            if (tud_bt_acl_data_send(a->data + a->sent, chunk)) {
                portENTER_CRITICAL(&s_tx_lock);
                s_acl_q.in_flight = true;
                portEXIT_CRITICAL(&s_tx_lock);
            } else {
                xSemaphoreGive(s_pump_sem);
            }
        }
    }
}

void tud_bt_event_sent_cb(uint16_t sent_bytes)
{
    bool complete = false;
    portENTER_CRITICAL(&s_tx_lock);
    evt_slot_t *s = &s_evt_q.slot[s_evt_q.head];
    s->sent += sent_bytes;
    if (s->sent >= s->len) {
        s->sent = 0;
        s_evt_q.head = (s_evt_q.head + 1) % EVT_POOL_N;
        s_evt_q.count--;
        complete = true;
    }
    s_evt_q.in_flight = false;
    portEXIT_CRITICAL(&s_tx_lock);
    if (complete) {
        xSemaphoreGive(s_pump_sem);   /* 整包完成，发下一个 */
    } else {
        xSemaphoreGive(s_pump_sem);   /* 还有分片，继续发 */
    }
}

void tud_bt_acl_data_sent_cb(uint16_t sent_bytes)
{
    portENTER_CRITICAL(&s_tx_lock);
    acl_slot_t *s = &s_acl_q.slot[s_acl_q.head];
    s->sent += sent_bytes;
    if (s->sent >= s->len) {
        s->sent = 0;
        s_acl_q.head = (s_acl_q.head + 1) % ACL_POOL_N;
        s_acl_q.count--;
    }
    s_acl_q.in_flight = false;
    portEXIT_CRITICAL(&s_tx_lock);
    xSemaphoreGive(s_pump_sem);
}

/* ── 下行：USB 回调 → UART ─────────────────────────────────────────── */

static void feed_uart(uint8_t h4_type, const void *pkt, uint32_t len)
{
    uint8_t hdr = h4_type;
    if (xRingbufferSend(s_uart_tx_ring, &hdr, 1, 0) != pdTRUE ||
        (len && xRingbufferSend(s_uart_tx_ring, pkt, len, 0) != pdTRUE)) {
        g_bridge_stats.rx_dropped += 1 + len;
        return;
    }
    g_bridge_stats.rx_bytes += 1 + len;
}

/* 垫片：ESP32 控制器在支持命令位图里宣称支持、实际却报错的三条命令
 * （Linux 实测告警）。Linux 容忍，Windows 会因此中断扫描初始化——
 * 由 S3 本地伪造成功响应，不再转发给控制器。 */
static void inject_evt_cc(uint16_t opcode, const uint8_t *ret_params, uint8_t n)
{
    /* Command Complete 事件: [ncmd=1][opcode lo][opcode hi][status=0][ret...] */
    uint8_t buf[8];
    buf[0] = 1;
    buf[1] = (uint8_t)(opcode & 0xFF);
    buf[2] = (uint8_t)(opcode >> 8);
    buf[3] = 0x00;
    if (n) {
        memcpy(buf + 4, ret_params, n);
    }
    uint16_t plen = (uint16_t)(4 + n);       /* 事件参数长度 */
    uint16_t total = (uint16_t)(2 + plen);   /* + 事件头 */

    portENTER_CRITICAL(&s_tx_lock);
    if (s_evt_q.count < EVT_POOL_N) {
        uint8_t idx = (s_evt_q.head + s_evt_q.count) % EVT_POOL_N;
        s_evt_q.slot[idx].data[0] = 0x0E;    /* Command Complete */
        s_evt_q.slot[idx].data[1] = (uint8_t)plen;
        memcpy(&s_evt_q.slot[idx].data[2], buf, plen);
        s_evt_q.slot[idx].len = total;
        s_evt_q.count++;
        portEXIT_CRITICAL(&s_tx_lock);
        g_bridge_stats.tx_bytes += total;
        xSemaphoreGive(s_pump_sem);
        return;
    }
    portEXIT_CRITICAL(&s_tx_lock);
}

void tud_bt_hci_cmd_cb(void *hci_cmd, size_t cmd_len)
{
    /* EP0 缓冲只在回调内有效，入 ring 后由 uart_tx_task 慢慢发 */
    static uint8_t staging[259];
    if (cmd_len > 258) {
        g_bridge_stats.rx_dropped += cmd_len;
        return;
    }

    if (cmd_len >= 3) {
        uint8_t *p = (uint8_t *)hci_cmd;
        uint16_t opcode = (uint16_t)(p[0] | (p[1] << 8));
        switch (opcode) {
        case 0x0C05:   /* Set Event Filter：无返回参数 */
            inject_evt_cc(opcode, NULL, 0);
            return;
        case 0x0C12:   /* Delete Stored Link Key：返回 num_keys_deleted=0 */
        case 0x0C5A:   /* Read Default Erroneous Data Reporting：返回 enabled=0 */
            inject_evt_cc(opcode, (const uint8_t[]){0x00}, 1);
            return;
        default:
            break;
        }
    }

    staging[0] = 0x01;
    memcpy(staging + 1, hci_cmd, cmd_len);
    feed_uart(0x01, staging + 1, cmd_len);
}

/* USB bth 驱动按端点大小（64B）分片递交主机 ACL 包——每片回调一次。
 * H:4 侧要求整包只有一个类型字节：包首片段补 0x02，后续片段裸透传。 */
static uint16_t s_acl_rx_remaining;   /* 当前 ACL 包尚未收到的字节数 */

void tud_bt_acl_data_received_cb(void *acl_data, uint16_t data_len)
{
    uint8_t *p = (uint8_t *)acl_data;

    if (s_acl_rx_remaining == 0) {
        if (data_len < 4) {
            g_bridge_stats.rx_dropped += data_len;
            return;
        }
        /* 新包开头：p[0..1]=句柄+标志, p[2..3]=数据总长 */
        uint16_t total = 4 + (uint16_t)(p[2] | (p[3] << 8));
        if (total < data_len) {       /* 长度场异常，丢弃 */
            g_bridge_stats.rx_dropped += data_len;
            return;
        }
        s_acl_rx_remaining = total - data_len;
        feed_uart(0x02, p, data_len); /* 带类型字节 */
    } else {
        /* 续片：不加类型字节，裸字节续在 ringbuffer 后面 */
        if (xRingbufferSend(s_uart_tx_ring, p, data_len, 0) != pdTRUE) {
            g_bridge_stats.rx_dropped += data_len;
            s_acl_rx_remaining = 0;
            return;
        }
        g_bridge_stats.rx_bytes += data_len;
        s_acl_rx_remaining = (data_len <= s_acl_rx_remaining)
                             ? (uint16_t)(s_acl_rx_remaining - data_len) : 0;
    }
}

static void uart_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        size_t item_size = 0;
        uint8_t *data = xRingbufferReceiveUpTo(s_uart_tx_ring, &item_size,
                                               pdMS_TO_TICKS(100), 512);
        if (data == NULL) {
            continue;
        }
        uart_write_bytes(HCI_UART_NUM, data, item_size);
        vRingbufferReturnItem(s_uart_tx_ring, data);
    }
}

/* ── 初始化 ────────────────────────────────────────────────────────── */

esp_err_t h4_bridge_init(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate  = HCI_UART_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,   /* GPIO4/13 未接 */
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(HCI_UART_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(HCI_UART_NUM, HCI_UART_TX_PIN, HCI_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(HCI_UART_NUM, 4096, 0, 0, NULL, 0));

    s_uart_tx_ring = xRingbufferCreate(4096, RINGBUF_TYPE_BYTEBUF);
    s_pump_sem = xSemaphoreCreateBinary();
    if (!s_uart_tx_ring || !s_pump_sem) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(uart_tx_task, "uart_tx", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(pump_task, "usb_pump", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "H4 bridge ready — UART1(TX=%d RX=%d)@%d <-> USB bth 0xE0",
             HCI_UART_TX_PIN, HCI_UART_RX_PIN, HCI_UART_BAUDRATE);
    return ESP_OK;
}
