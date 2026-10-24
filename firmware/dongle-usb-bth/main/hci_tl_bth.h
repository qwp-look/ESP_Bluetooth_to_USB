/*
 * hci_tl_bth.h — 把 ESP32-S3 蓝牙控制器的 HCI 传输层接到
 * TinyUSB 蓝牙设备类（class 0xE0）上。
 *
 * USB 蓝牙传输不带 H:4 类型字节（命令走 EP0 控制传输、事件走中断端点、
 * ACL 走批量端点），而控制器 TL 层吃的是 H:4 字节流——本模块负责
 * 两个方向的类型字节增删。
 */

#pragma once

#include "esp_bt.h"

extern esp_bt_hci_tl_t hci_tl_bth_funcs;

/* 创建内部任务/队列。须在控制器 init 之前调用。 */
esp_err_t hci_tl_bth_init(void);
