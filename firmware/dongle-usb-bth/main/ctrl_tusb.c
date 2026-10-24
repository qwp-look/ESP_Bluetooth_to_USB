/*
 * ctrl_tusb.c — CDC 控制台命令处理（原生 TinyUSB 版）。
 * 命令与 components/ctrl_cdc 相同：help / boot / dfu / restart / stats。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "esp32s3/rom/usb/usb_persist.h"
#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"

#include "tusb.h"

#include "ctrl_tusb.h"

static const char *TAG = "ctrl_cdc";

bridge_stats_t g_bridge_stats = {0};

#define CMD_BUF_LEN 32

static char     s_line[CMD_BUF_LEN];
static uint16_t s_line_len;

static void ctrl_reply(const char *msg)
{
    tud_cdc_write(msg, strlen(msg));
    tud_cdc_write_flush();
}


/* 重启必须在独立任务里做：从 TinyUSB 上下文直接 esp_restart() 会死锁 */
static void restart_task(void *arg)
{
    int mode = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(50));
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
    (void)TAG;
}

/* TinyUSB CDC 收到数据（tud_task 上下文） */
void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
    uint8_t buf[64];
    uint32_t rx_size = 0;

    while ((rx_size = tud_cdc_read(buf, sizeof(buf))) > 0) {
        for (uint32_t i = 0; i < rx_size; i++) {
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
}
