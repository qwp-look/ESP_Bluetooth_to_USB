/*
 * ctrl_cdc.h — 控制台 CDC 通道（USB 第二个虚拟串口）。
 *
 * 用途：USB 固件跑起来以后，板子的串口下载口（USB-Serial/JTAG）就被
 * USB-OTG 外设占用了，esptool 无法再通过 DTR/RTS 让芯片复位进下载模式，
 * 重烧固件必须手按 BOOT 键。本组件在 CDC-ACM #1 上提供一个文本命令通道，
 * 从电脑上发个 "boot" 就能进下载模式，配合 scripts/host/flash-s3.sh
 * 实现免按键重烧。
 *
 * 命令一览（回车结束，大小写不敏感）：
 *   help     命令列表
 *   boot     重启进 ROM 下载模式（CDC 下载，esptool 可直接烧录）
 *   dfu      重启进 DFU 模式（esptool dfu / dfu-util 均可）
 *   restart  普通重启
 *   stats    打印桥接统计（收发字节/丢包计数）
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

/* 桥接统计，hci_tl / uart 桥代码就地累加，stats 命令打印 */
typedef struct {
    uint32_t rx_bytes;      /* host→controller 字节数 */
    uint32_t tx_bytes;      /* controller→host 字节数 */
    uint32_t rx_dropped;    /* host→controller 丢弃字节数 */
    uint32_t tx_dropped;    /* controller→host 丢弃字节数 */
} bridge_stats_t;

extern bridge_stats_t g_bridge_stats;

/* 注册 CDC-ACM #1 并挂载命令处理。
 * 须在 tinyusb_driver_install() 之后调用。 */
esp_err_t ctrl_cdc_init(void);
