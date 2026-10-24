/*
 * ctrl_cdc.c — 控制台 CDC 通道实现。
 *
 * "boot"/"dfu" 的原理：向 RTC_CNTL_OPTION1_REG 写 FORCE_DOWNLOAD_BOOT，
 * 再 esp_restart()，ROM 即进入下载模式。DFU 另外置 USBDC_BOOT_DFU 持久化
 * 标志（参考 IDF 组件 esp_usb_cdc_rom_console/usb_console.c 的做法）。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "esp32s3/rom/usb/usb_persist.h"
#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"

#include "ctrl_cdc.h"

static const char *TAG = "ctrl_cdc";

bridge_stats_t g_bridge_stats = {0};

#define CMD_BUF_LEN 32

static char     s_line[CMD_BUF_LEN];
static uint16_t s_line_len;

static void ctrl_reply(const char *msg)
{
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_1, (const uint8_t *)msg, strlen(msg));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_1, pdMS_TO_TICKS(50));
}

/* 重启必须在独立任务里做：从 TinyUSB 回调上下文直接 esp_restart() 会死锁
 * （重启钩子等待 USB 任务收尾，而调用者正是 USB 任务本身）。 */
static void restart_task(void *arg)
{
    int mode = (int)(intptr_t)arg;   /* 0=普通重启 1=ROM 下载 2=DFU */
    vTaskDelay(pdMS_TO_TICKS(50));   /* 让回复先发到主机 */

    if (mode == 1) {
        /* 对齐官方 usb_console.c：PERSIST_ENA 让 ROM 把这次重启当作
         * "进下载模式"处理，缺了它芯片会正常启动 */
        chip_usb_set_persist_flags(USBDC_PERSIST_ENA);
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    } else if (mode == 2) {
        chip_usb_set_persist_flags(USBDC_BOOT_DFU);
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    }
    esp_restart();
}

static void queue_restart(int mode)
{
    xTaskCreate(restart_task, "ctrl_reboot", 2048, (void *)(intptr_t)mode, 5, NULL);
}

static void ctrl_exec(char *cmd)
{
    /* 去掉行尾 CR/LF */
    cmd[strcspn(cmd, "\r\n")] = '\0';

    if (strcmp(cmd, "help") == 0) {
        ctrl_reply("commands: help boot dfu restart stats\r\n");
    } else if (strcmp(cmd, "boot") == 0) {
        ctrl_reply("entering ROM download mode, re-enumerating...\r\n");
        queue_restart(1);
    } else if (strcmp(cmd, "dfu") == 0) {
        ctrl_reply("entering DFU mode, re-enumerating...\r\n");
        queue_restart(2);
    } else if (strcmp(cmd, "restart") == 0) {
        ctrl_reply("restarting...\r\n");
        queue_restart(0);
    } else if (strcmp(cmd, "stats") == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "rx=%u tx=%u rx_drop=%u tx_drop=%u\r\n",
                 (unsigned)g_bridge_stats.rx_bytes,  (unsigned)g_bridge_stats.tx_bytes,
                 (unsigned)g_bridge_stats.rx_dropped, (unsigned)g_bridge_stats.tx_dropped);
        ctrl_reply(buf);
    } else if (cmd[0] != '\0') {
        ctrl_reply("unknown command, try: help\r\n");
    }
}

static void ctrl_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    (void)event;

    uint8_t buf[64];
    size_t rx_size = 0;
    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_1, buf, sizeof(buf), &rx_size) != ESP_OK) {
        return;
    }

    for (size_t i = 0; i < rx_size; i++) {
        char c = (char)buf[i];
        if (c == '\r' || c == '\n') {
            if (s_line_len > 0) {
                s_line[s_line_len] = '\0';
                ctrl_exec(s_line);
                s_line_len = 0;
            }
        } else if (s_line_len < CMD_BUF_LEN - 1) {
            s_line[s_line_len++] = c;
        }
    }
}

esp_err_t ctrl_cdc_init(void)
{
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port                     = TINYUSB_CDC_ACM_1,
        .callback_rx                  = &ctrl_rx_callback,
        .callback_rx_wanted_char      = NULL,
        .callback_line_state_changed  = NULL,
        .callback_line_coding_changed = NULL,
    };
    esp_err_t err = tinyusb_cdcacm_init(&acm_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "control channel on CDC-ACM #1 (try: help)");
    }
    return err;
}
