/*
 * main.c — 经典 ESP32 (WROOM) radio-bridge 固件入口。
 *
 * 职责：把片内双模蓝牙控制器（BLE + 经典蓝牙）以标准 HCI H:4 字节流的
 * 形式从 UART2 引出，交给 ESP32-S3 前端转发到 USB。
 * 控制器自带 UART H4 驱动（CONFIG_BTDM_CTRL_HCI_MODE_UART_H4），
 * 本文件只做初始化和引脚矩阵。
 *
 * 参考官方例程 examples/bluetooth/hci/controller_hci_uart_esp32 改写，
 * 差异：HCI UART 用 UART2（默认引脚 TX=GPIO17 / RX=GPIO16，与板间接线一致）。
 *
 * 日志走 UART0（板载 USB 串口芯片），与 HCI 流（UART2）物理隔离。
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "hal/uhci_ll.h"
#include "hal/uart_ll.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "radio_bridge";

#define UHCI_PORT_NUM (0)

/* 板间接线（docs/hardware-wiring.md）：
 *   经典板 GPIO17 (TX2) → S3 GPIO18 (RX)
 *   经典板 GPIO16 (RX2) ← S3 GPIO17 (TX)
 * RTS/CTS（GPIO4/GPIO13）暂不接，硬件流控保持关闭。 */
#define HCI_UART_TX_PIN  17
#define HCI_UART_RX_PIN  16

/* 复位 UART 与 UHCI 的总线时钟和寄存器，避免残留配置干扰控制器的
 * 直驱访问（照抄官方例程的做法）。 */
static void uart_gpio_reset(void)
{
    int __DECLARE_RCC_ATOMIC_ENV __attribute__((unused));
    uart_ll_enable_bus_clock(CONFIG_BTDM_CTRL_HCI_UART_NO, true);
    uart_ll_reset_register(CONFIG_BTDM_CTRL_HCI_UART_NO);
    uhci_ll_enable_bus_clock(UHCI_PORT_NUM, true);
    uhci_ll_reset_register(UHCI_PORT_NUM);

    ESP_ERROR_CHECK(uart_set_pin(CONFIG_BTDM_CTRL_HCI_UART_NO,
                                 HCI_UART_TX_PIN, HCI_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    int n = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        printf("[hb] alive t=%ds heap=%lu\n", ++n * 5,
               (unsigned long)esp_get_free_heap_size());
        fflush(stdout);
    }
}

void app_main(void)
{
    /* NVS：PHY 校准数据 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    uart_gpio_reset();

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    /* 双模：BLE + 经典蓝牙（A2DP / 经典 HID 的关键） */
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));

    ESP_LOGI(TAG, "dual-mode controller up, HCI H:4 on UART2 "
             "(TX=%d RX=%d) @ %d baud — waiting for S3 front-end",
             HCI_UART_TX_PIN, HCI_UART_RX_PIN,
             CONFIG_BTDM_CTRL_HCI_UART_BAUDRATE);

    /* 心跳：每 5 秒一行，不用复位就能从串口确认固件活着 */
    xTaskCreate(heartbeat_task, "hb", 2048, NULL, 2, NULL);
}
