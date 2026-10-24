/*
 * main.c — 阶段 3a 固件：ESP32-S3 作为 USB 前端，把经典 ESP32
 * （radio-bridge 固件，双模蓝牙控制器）经 UART 输出的 HCI H:4 字节流
 * 透明搬运到 USB CDC。主机侧：
 *
 *   sudo btattach -B /dev/ttyACM0 -P h4 -S 921600
 *
 * CDC0 = HCI 数据口（纯字节透传，不做任何解析）
 * CDC1 = 控制台（boot/dfu/restart/stats，见 components/ctrl_cdc）
 *
 * 线程模型：
 *   host→controller：CDC RX 回调（TinyUSB 任务）→ ringbuffer → uart_tx_task → UART
 *   controller→host：uart_rx_task（阻塞读 UART）→ CDC write_queue+flush
 *
 * UART 参数必须与 radio-bridge 一致：921600 8N1，无流控
 * （流控线 GPIO4/13 未接，见 docs/hardware-wiring.md）。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include "driver/uart.h"
#include "esp_log.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"

#include "ctrl_cdc.h"

static const char *TAG = "dongle_uart";

#define HCI_UART_NUM      UART_NUM_1   /* 与 docs/hardware-wiring.md 一致 */
#define HCI_UART_TX_PIN   17           /* → 经典板 GPIO16 (RX2) */
#define HCI_UART_RX_PIN   18           /* ← 经典板 GPIO17 (TX2) */
#define HCI_UART_BAUDRATE 921600       /* 必须与 radio-bridge 一致 */

#define UART_TX_RING_SIZE 2048
#define UART_CHUNK        512

static RingbufHandle_t s_uart_tx_ring;

/* ── host→controller：CDC RX 回调只入 ringbuffer ───────────────────── */

static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    size_t rx_size = 0;

    if (tinyusb_cdcacm_read(
            itf == 0 ? TINYUSB_CDC_ACM_0 : TINYUSB_CDC_ACM_1,
            buf, sizeof(buf), &rx_size) != ESP_OK || rx_size == 0) {
        return;
    }
    if (itf != 0) {
        return; /* CDC1 由 ctrl_cdc 处理，这里不会进来 */
    }

    if (xRingbufferSend(s_uart_tx_ring, buf, rx_size, 0) != pdTRUE) {
        ESP_LOGW(TAG, "uart tx ring full, dropped %u bytes", (unsigned)rx_size);
        g_bridge_stats.rx_dropped += rx_size;
    } else {
        g_bridge_stats.rx_bytes += rx_size;
    }
}

/* ── host→controller：串口发送任务 ─────────────────────────────────── */

static void uart_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        size_t item_size = 0;
        uint8_t *data = xRingbufferReceiveUpTo(s_uart_tx_ring, &item_size,
                                               pdMS_TO_TICKS(100), UART_CHUNK);
        if (data == NULL) {
            continue;
        }
        /* uart_write_bytes 进驱动 TX 环形缓冲，缓冲满时阻塞——
         * 本任务允许阻塞，出问题（对端死机）时数据堆积在我们自己的
         * ringbuffer 里并被丢弃计数。 */
        int written = uart_write_bytes(HCI_UART_NUM, data, item_size);
        if (written < 0) {
            g_bridge_stats.tx_dropped += item_size;
        }
        vRingbufferReturnItem(s_uart_tx_ring, data);
    }
}

/* ── controller→host：UART 读取任务 ────────────────────────────────── */

static void uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[UART_CHUNK];
    for (;;) {
        int len = uart_read_bytes(HCI_UART_NUM, buf, sizeof(buf),
                                  pdMS_TO_TICKS(20));
        if (len <= 0) {
            continue;
        }
        g_bridge_stats.tx_bytes += len;

        uint32_t sent = 0;
        int retry = 0;
        while (sent < (uint32_t)len) {
            size_t w = tinyusb_cdcacm_write_queue(
                TINYUSB_CDC_ACM_0, buf + sent, len - sent);
            sent += w;
            esp_err_t err = tinyusb_cdcacm_write_flush(
                TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
            if (err != ESP_OK && ++retry > 30) {
                ESP_LOGE(TAG, "CDC tx stuck, dropped %u bytes",
                         (unsigned)(len - sent));
                g_bridge_stats.tx_dropped += len - sent;
                break;
            }
        }
    }
}

/* ── 初始化 ────────────────────────────────────────────────────────── */

void app_main(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate  = HCI_UART_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(HCI_UART_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(HCI_UART_NUM, HCI_UART_TX_PIN, HCI_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* RX 缓冲给足：HCI 突发（ACL 连续上行）不能丢 */
    ESP_ERROR_CHECK(uart_driver_install(HCI_UART_NUM, 2048, 0, 0, NULL, 0));

    s_uart_tx_ring = xRingbufferCreate(UART_TX_RING_SIZE, RINGBUF_TYPE_BYTEBUF);
    ESP_ERROR_CHECK(s_uart_tx_ring ? ESP_OK : ESP_ERR_NO_MEM);

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port                     = TINYUSB_CDC_ACM_0,
        .callback_rx                  = &cdc_rx_callback,
        .callback_rx_wanted_char      = NULL,
        .callback_line_state_changed  = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    esp_err_t ctrl_err = ctrl_cdc_init();   /* CDC1，非致命 */
    if (ctrl_err != ESP_OK) {
        ESP_LOGW(TAG, "control channel unavailable: %s",
                 esp_err_to_name(ctrl_err));
    }

    xTaskCreate(uart_tx_task, "uart_tx", 4096, NULL, 5, NULL);
    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "ready — UART1(TX=%d RX=%d)@%d <-> USB CDC0",
             HCI_UART_TX_PIN, HCI_UART_RX_PIN, HCI_UART_BAUDRATE);
    ESP_LOGI(TAG, "host: sudo btattach -B /dev/ttyACM0 -P h4 -S 921600");
}
