/*
 * hci_tl_usb.c — esp_bt_hci_tl_t 的 USB CDC 实现。
 *
 * 对照官方例程 examples/bluetooth/hci/controller_hci_uart_esp32c3_and_esp32s3
 * （传输层为 UART+UHCI）改写：producer/consumer 结构保留，管道换成 CDC。
 *
 * 线程模型：
 *   - CDC RX 回调（TinyUSB 任务）只往 ringbuffer 塞字节并敲信号量；
 *   - rx_task 是唯一消费者：把字节填进控制器通过 _recv() 挂起的读请求，
 *     填满后调用控制器回调并用 esp_bt_h4tl_eif_io_event_notify() 唤醒控制器任务；
 *   - _send()（控制器任务上下文）不下场写 USB——只把 {buf,len,cb,arg} 指针入队。
 *     缓冲区在回调触发前保证有效（与官方例程的 DMA 语义一致）；
 *   - tx_task 从队列取指针，阻塞式写入 CDC（write_queue + write_flush 必须在
 *     普通任务上下文里做，回调里不允许阻塞），完成后回调控制器。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "tinyusb_cdc_acm.h"

#include "hci_tl_usb.h"
#include "ctrl_cdc.h"

static const char *TAG = "hci_tl_usb";

/* host→controller 字节缓冲。深一点：CDC 突发（如全量扫描）时
 * 控制器可能来不及立刻挂起新的读请求。 */
#define TL_RX_RING_SIZE 2048

/* 写 flush 单次超时与总重试上限：主机长时间不来收（拔线、挂起）时
 * 丢弃该包并照常回调，避免控制器任务被 USB 卡死。 */
#define CDC_FLUSH_TIMEOUT_MS 100
#define CDC_FLUSH_MAX_RETRY  30

/* ── 控制器挂起的读请求（同一时刻最多一个，TL 接口的设计）────────── */

static struct {
    esp_bt_hci_tl_callback_t cb;   /* NULL = 未挂起 */
    void                     *arg;
    uint8_t                  *buf;
    uint32_t                 remaining;
} s_rx;

/* s_rx 在控制器任务（_recv）与 rx_task 之间共享，用自旋锁保护。
 * 临界区内只有指针/计数的读写，长度上限一个 HCI 包，可接受。 */
static portMUX_TYPE s_rx_lock = portMUX_INITIALIZER_UNLOCKED;

static RingbufHandle_t  s_rx_ring;   /* host→controller 字节流 */
static SemaphoreHandle_t s_rx_event; /* 有数据或有新挂起的读 */
static QueueHandle_t     s_tx_q;     /* controller→host 的发送工作项 */

typedef struct {
    uint8_t                  *buf;
    uint32_t                 len;
    esp_bt_hci_tl_callback_t cb;
    void                     *arg;
} tx_item_t;

/* ── host→controller：CDC 回调（producer） ─────────────────────────── */

void hci_tl_usb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    size_t rx_size = 0;

    esp_err_t err = tinyusb_cdcacm_read(
        itf == 0 ? TINYUSB_CDC_ACM_0 : TINYUSB_CDC_ACM_1,
        buf, sizeof(buf), &rx_size);
    if (err != ESP_OK || rx_size == 0) {
        return;
    }

    if (xRingbufferSend(s_rx_ring, buf, rx_size, 0) != pdTRUE) {
        /* ringbuffer 满意味着控制器侧严重滞后，只能丢——BlueZ
         * 的 HCI 重传/超时机制会发现链路异常。 */
        ESP_LOGW(TAG, "rx ring full, dropped %u bytes", (unsigned)rx_size);
        g_bridge_stats.rx_dropped += rx_size;
    } else {
        g_bridge_stats.rx_bytes += rx_size;
    }
    xSemaphoreGive(s_rx_event);
}

/* ── host→controller：填充挂起读请求（consumer） ──────────────────── */

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
                /* 通知控制器任务：读已完成 */
                esp_bt_h4tl_eif_io_event_notify(1);
            }
        }
    }
}

/* ── controller→host：tx_task（唯一允许阻塞写 USB 的上下文） ──────── */

static void tx_task(void *arg)
{
    (void)arg;
    tx_item_t item;
    for (;;) {
        if (xQueueReceive(s_tx_q, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint32_t sent = 0;
        int retry = 0;
        while (sent < item.len) {
            size_t w = tinyusb_cdcacm_write_queue(
                TINYUSB_CDC_ACM_0, item.buf + sent, item.len - sent);
            sent += w;

            esp_err_t err = tinyusb_cdcacm_write_flush(
                TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(CDC_FLUSH_TIMEOUT_MS));
            if (err != ESP_OK && ++retry > CDC_FLUSH_MAX_RETRY) {
                ESP_LOGE(TAG, "CDC tx stuck, dropped %u bytes (host gone?)",
                         (unsigned)(item.len - sent));
                g_bridge_stats.tx_dropped += item.len - sent;
                break;
            }
        }
        g_bridge_stats.tx_bytes += sent;

        /* 无论成败都要回调，控制器等着回收缓冲区 */
        item.cb(item.arg, ESP_BT_HCI_TL_STATUS_OK);
        esp_bt_h4tl_eif_io_event_notify(1);
    }
}

/* ── esp_bt_hci_tl_t 回调实现 ──────────────────────────────────────── */

static int tl_open(void)
{
    return 0;   /* CDC 已在 main.c 里先于控制器启动 */
}

static void tl_close(void)
{
}

static void tl_finish_transfers(void)
{
    /* 尽力而为：把 TX FIFO 刷出去 */
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0,
                               pdMS_TO_TICKS(CDC_FLUSH_TIMEOUT_MS));
}

static void tl_recv(uint8_t *buf, uint32_t len,
                    esp_bt_hci_tl_callback_t callback, void *arg)
{
    portENTER_CRITICAL(&s_rx_lock);
    s_rx.cb = callback;
    s_rx.arg = arg;
    s_rx.buf = buf;
    s_rx.remaining = len;
    portEXIT_CRITICAL(&s_rx_lock);

    /* ringbuffer 里可能已经有数据（控制器挂起前刚到达） */
    xSemaphoreGive(s_rx_event);
}

static void tl_send(uint8_t *buf, uint32_t len,
                    esp_bt_hci_tl_callback_t callback, void *arg)
{
    tx_item_t item = { .buf = buf, .len = len, .cb = callback, .arg = arg };
    if (xQueueSend(s_tx_q, &item, 0) != pdTRUE) {
        ESP_LOGE(TAG, "tx queue full, packet dropped");
        callback(arg, ESP_BT_HCI_TL_STATUS_OK);
        esp_bt_h4tl_eif_io_event_notify(1);
    }
}

static bool tl_flow_off(void)
{
    /* USB 自带流控（bulk NAK），无需额外处理 */
    return true;
}

static void tl_flow_on(void)
{
}

esp_bt_hci_tl_t hci_tl_usb_funcs = {
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

/* ── 初始化 ────────────────────────────────────────────────────────── */

esp_err_t hci_tl_usb_init(void)
{
    s_rx_ring = xRingbufferCreate(TL_RX_RING_SIZE, RINGBUF_TYPE_BYTEBUF);
    s_rx_event = xSemaphoreCreateBinary();
    s_tx_q = xQueueCreate(8, sizeof(tx_item_t));
    if (!s_rx_ring || !s_rx_event || !s_tx_q) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(rx_task, "hci_rx", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(tx_task, "hci_tx", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "HCI transport over USB CDC ready");
    return ESP_OK;
}
