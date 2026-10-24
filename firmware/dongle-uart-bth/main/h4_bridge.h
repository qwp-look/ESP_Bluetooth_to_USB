/*
 * h4_bridge.h — 阶段 3b 核心：UART H:4 字节流 ↔ USB class 0xE0 分流桥。
 *
 * 上行（经典板 → 主机）：UART 收 H:4 流 → 按类型字节解包 →
 *   0x04 事件 → tud_bt_event_send（中断 IN 端点，去掉类型字节）
 *   0x02 ACL  → tud_bt_acl_data_send（批量 IN 端点，去掉类型字节）
 *   0x03 SCO  → 丢弃计数（未配置 ISO 备用接口，HFP 语音暂不支持）
 * 下行（主机 → 经典板）：EP0 命令 / 批量 OUT ACL → 补 H:4 类型字节 → UART
 */

#pragma once

#include "esp_err.h"

/* 初始化 UART、解析器、发送池与任务。 */
esp_err_t h4_bridge_init(void);
