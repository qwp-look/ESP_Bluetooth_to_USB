/*
 * main.c — 阶段 1 固件入口：ESP32-S3 蓝牙控制器（BLE）经 USB CDC 对外提供 HCI。
 *
 * 启动顺序有讲究：
 *   1. NVS      —— PHY 校准数据
 *   2. 传输层   —— hci_tl_usb 的 ringbuffer/任务
 *   3. TinyUSB  —— CDC-ACM 就绪并挂上 RX 回调
 *   4. BT 控制器 —— hci_tl_funcs 指向我们的 USB 传输层
 *
 * 主机侧（Linux）使用方式：
 *   sudo btattach -B /dev/ttyACM0 -P h4 -S 115200
 *   # -S 波特率对 CDC 无意义，任意值即可
 *   bluetoothctl scan on ...
 *
 * 日志在 UART0（GPIO43/44）= 开发板另一个 USB 串口口上，
 * 与 CDC 数据口（原生 USB）物理隔离，不会污染 HCI 流。
 */

#include "esp_bt.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"

#include "hci_tl_usb.h"
#include "ctrl_cdc.h"

static const char *TAG = "dongle";

void app_main(void)
{
    /* ── 1. NVS（PHY 校准数据）────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── 2. HCI 传输层内部资源 ────────────────────────────────────── */
    ESP_ERROR_CHECK(hci_tl_usb_init());

    /* ── 3. TinyUSB CDC-ACM ───────────────────────────────────────── */
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port                    = TINYUSB_CDC_ACM_0,
        .callback_rx                 = &hci_tl_usb_cdc_rx_callback,
        .callback_rx_wanted_char     = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    /* 控制台通道（CDC #1）：boot/dfu/restart/stats，免按键重烧全靠它。
     * 非核心功能——失败只告警，不拖死整个固件。 */
    esp_err_t ctrl_err = ctrl_cdc_init();
    if (ctrl_err != ESP_OK) {
        ESP_LOGW(TAG, "control channel unavailable: %s",
                 esp_err_to_name(ctrl_err));
    }
    ESP_LOGI(TAG, "USB CDC-ACM up");

    /* ── 4. 蓝牙控制器：BLE-only，HCI 传输走 USB ─────────────────── */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.hci_tl_type = 0;              /* 0 = 外部传输层（借"UART"槽位） */
    bt_cfg.hci_tl_funcs = &hci_tl_usb_funcs;

    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_LOGI(TAG, "BT controller up (BLE, HCI over USB CDC)");

    ESP_LOGI(TAG, "ready — host: sudo btattach -B /dev/ttyACM0 -P h4");
}
