/*
 * main.c — 阶段 3b 固件入口（最终形态）：
 * ESP32-S3 作 USB 前端（class 0xE0 即插即用），HCI 数据源是
 * 经典 ESP32（radio-bridge 固件）经 UART 输出的 H:4 流。
 *
 * 本工程不含蓝牙协议栈——只是"USB 蓝牙类设备 ↔ UART 字节流"的搬运工，
 * 配对、Profile 全部由主机（电脑）蓝牙栈处理。
 *
 * 日志走 UART0（GPIO43/44，串口桥 USB 口）。
 */

#include "esp_log.h"
#include "esp_private/usb_phy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tusb.h"

#include "h4_bridge.h"

static const char *TAG = "dongle_uart_bth";

static usb_phy_handle_t s_phy_hdl;

/* TinyUSB 事件泵任务 */
static void tud_task_loop(void *arg)
{
    (void)arg;
    while (1) {
        tud_task();
    }
}

void app_main(void)
{
    /* UART + H:4 解析 + 发送池 */
    ESP_ERROR_CHECK(h4_bridge_init());

    /* USB PHY（S3 内部全速 PHY，设备模式） */
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_FULL,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_conf, &s_phy_hdl));

    /* TinyUSB 设备栈 */
    tusb_rhport_init_t rh_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    if (!tusb_rhport_init(0, &rh_init)) {
        ESP_LOGE(TAG, "tusb init failed");
        return;
    }
    if (xTaskCreate(tud_task_loop, "usbd", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "usbd task create failed");
        return;
    }

    ESP_LOGI(TAG, "ready — dual-mode plug & play BT dongle via classic ESP32 radio");
}
