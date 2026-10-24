/*
 * main.c — 阶段 2 固件入口：ESP32-S3 蓝牙控制器（BLE）以 USB class 0xE0
 * 即插即用方式对外提供 HCI。主机（Linux/Windows）按标准蓝牙 dongle 识别，
 * 无需 btattach。
 *
 * 组成：
 *   - USB：原生 TinyUSB（PHY 初始化 + 自建 tud_task）
 *   - 蓝牙：控制器 TL 层接 hci_tl_bth（USB bth 类）
 *   - CDC 控制台：boot/dfu/restart/stats
 *
 * 日志走 UART0（GPIO43/44，串口桥 USB 口）。
 */

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tusb.h"

#include "hci_tl_bth.h"

static const char *TAG = "dongle_bth";

static usb_phy_handle_t s_phy_hdl;

/* TinyUSB 事件泵任务（对齐 esp_tinyusb 的实现方式） */
static void tud_task_loop(void *arg)
{
    (void)arg;
    while (1) {
        tud_task();
    }
}

void app_main(void)
{
    /* ── NVS（PHY 校准数据）────────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── HCI 传输层内部资源 ────────────────────────────────────────── */
    ESP_ERROR_CHECK(hci_tl_bth_init());

    /* ── USB PHY（S3 内部全速 PHY，设备模式）──────────────────────── */
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_FULL,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_conf, &s_phy_hdl));

    /* ── TinyUSB 设备栈 ───────────────────────────────────────────── */
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

    /* ── 蓝牙控制器：BLE-only，HCI 传输走 USB bth 类 ──────────────── */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.hci_tl_type = 0;
    bt_cfg.hci_tl_funcs = &hci_tl_bth_funcs;

    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_LOGI(TAG, "ready — plug & play BT dongle (class 0xE0, VID:PID %04x:%04x)", 0x303A, 0x4002);
}
