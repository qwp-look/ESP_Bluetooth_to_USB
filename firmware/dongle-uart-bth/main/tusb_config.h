/*
 * tusb_config.h — TinyUSB 配置：USB 蓝牙设备类（class 0xE0）+ CDC 控制台。
 *
 * 注意 CFG_TUSB_MCU 由组件的 CMake 通过 -D 传入（OPT_MCU_ESP32S3），
 * 这里不重复定义。
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_OS                OPT_OS_FREERTOS

#define CFG_TUD_ENABLED            1

/* USB 蓝牙设备类：事件走中断端点，ACL 走批量端点 */
#define CFG_TUD_BTH                1
#define CFG_TUD_BTH_EVENT_EPSIZE   64    /* 全速中断端点上限；越大越少触发整倍数特例 */
#define CFG_TUD_BTH_DATA_EPSIZE    64    /* 全速批量端点 64 字节 */
/* ISO 交替设置数：alt0(9B) + alt1(49B)，需 ≥2 且给解析循环留余量取 3。
 * 等时端点必须非零包长（Windows 对零值拒绝启动）。 */
#define CFG_TUD_BTH_ISO_ALT_COUNT  3

/* Windows 蓝牙驱动用设备接收方 + bRequest=0xE0 发 HCI 命令（历史惯例），
 * 必须开启兼容，否则命令被 STALL、无线电初始化失败（代码10 实测踩坑） */
#define CFG_TUD_BTH_HISTORICAL_COMPATIBLE 1

/* CDC 控制台（boot/dfu/stats），非数据通道 */
#define CFG_TUD_CDC                1
#define CFG_TUD_CDC_RX_BUFSIZE     256
#define CFG_TUD_CDC_TX_BUFSIZE     256

#define CFG_TUD_ENDPOINT0_SIZE     64

#ifdef __cplusplus
}
#endif

#endif
