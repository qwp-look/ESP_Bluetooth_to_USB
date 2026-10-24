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
#define CFG_TUD_BTH_EVENT_EPSIZE   16    /* BT USB 规范：事件端点 16 字节 */
#define CFG_TUD_BTH_DATA_EPSIZE    64    /* 全速批量端点 64 字节 */
/* ISO 备用接口数（含驱动强制的 alt1 零带宽占位）。
 * 2 = alt0(主) + alt1(零带宽 ISO)；要支持 eSCO 语音再加更多并调大端点。 */
#define CFG_TUD_BTH_ISO_ALT_COUNT  2

/* CDC 控制台（boot/dfu/stats），非数据通道 */
#define CFG_TUD_CDC                1
#define CFG_TUD_CDC_RX_BUFSIZE     256
#define CFG_TUD_CDC_TX_BUFSIZE     256

#define CFG_TUD_ENDPOINT0_SIZE     64

#ifdef __cplusplus
}
#endif

#endif
