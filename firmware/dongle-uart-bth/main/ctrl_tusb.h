/*
 * ctrl_tusb.h — 原生 TinyUSB CDC 控制台（本工程的第二功能接口）。
 *
 * 与 components/ctrl_cdc（esp_tinyusb 版）功能一致：boot/dfu/restart/stats。
 * 本工程直接用 TinyUSB 原生栈，因此单独实现一份。
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t rx_bytes;      /* host→controller 字节数 */
    uint32_t tx_bytes;      /* controller→host 字节数 */
    uint32_t rx_dropped;    /* host→controller 丢弃字节数 */
    uint32_t tx_dropped;    /* controller→host 丢弃字节数 */
} bridge_stats_t;

extern bridge_stats_t g_bridge_stats;
