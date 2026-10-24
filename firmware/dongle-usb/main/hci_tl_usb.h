/*
 * hci_tl_usb.h — 把 ESP32-S3 蓝牙控制器的 HCI 传输层（esp_bt_hci_tl_t）
 * 接到 TinyUSB CDC-ACM 上。
 *
 * 数据路径（阶段 1，S3 单板）：
 *
 *   Linux BlueZ ──USB CDC──► CDC RX 回调 ──ringbuffer──► rx_task
 *        ▲                                              │填满控制器挂起的读请求
 *        │                                              ▼
 *   CDC TX◄── tx_task ◄── _send() ◄────────────── BT 控制器（内部解析 H:4）
 *
 * H:4 帧的拼包由控制器自己的 H4-TL 层完成，本模块只搬运字节流，
 * 与官方例程 controller_hci_uart_esp32c3_and_esp32s3 的职责划分一致，
 * 只是把"UART+UHCI"换成了"USB CDC"。
 */

#pragma once

#include "esp_bt.h"

/* 挂到 esp_bt_controller_config_t.hci_tl_funcs 上的传输层回调表 */
extern esp_bt_hci_tl_t hci_tl_usb_funcs;

/*
 * 初始化传输层内部资源（ringbuffer / 任务 / 队列）。
 * 必须在 esp_bt_controller_init() 之前、TinyUSB CDC 就绪之后调用。
 */
esp_err_t hci_tl_usb_init(void);

/*
 * 注册给 TinyUSB CDC 的 RX 回调（main.c 里接线用）。
 * 运行在 TinyUSB 任务上下文。
 */
void hci_tl_usb_cdc_rx_callback(int itf, cdcacm_event_t *event);
