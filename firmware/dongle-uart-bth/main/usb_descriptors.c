/*
 * usb_descriptors.c — USB 描述符：蓝牙 HCI（class 0xE0）+ CDC 控制台 复合设备。
 *
 * 接口布局（蓝牙部分模仿市售 CSR dongle）：
 *   Itf0  蓝牙 HCI   class 0xE0/01/01
 *         EP 0x81 INT IN  16  — HCI 事件（controller→host）
 *         EP 0x02 BULK OUT 64 — ACL 数据（host→controller）
 *         EP 0x82 BULK IN  64 — ACL 数据（controller→host）
 *         （HCI 命令不走端点：主机经 EP0 控制传输下发，bth 类驱动处理）
 *   Itf1/2 CDC-ACM 控制台（IAD 组合），EP 0x83 INT IN / 0x04 OUT / 0x84 IN
 *
 * Linux 的 btusb 按"接口 class E0/sub01/prot01"匹配并自动加载，
 * 这就是即插即用无需 btattach 的原因。
 */

#include "tusb.h"

#define USB_VID 0x303A   /* Espressif（自用实验 VID） */
#define USB_PID 0x4003   /* 双模版：区别于 BLE 单模 (0x4002) 与 CDC (0x4001) */
#define USB_BCD 0x0200

/* 蓝牙 HCI 接口描述符长度：主接口(3 EP) + isoc 接口 alt0/alt1（各 2 ISO EP） */
#define BT_ITF_DESC_LEN (9 + 7 + 7 + 7 + 9 + 7 + 7 + 9 + 7 + 7)

/* ── 设备描述符：复合设备（IAD 约定 0xEF/02/01） ───────────────────── */

static const tusb_desc_device_t const_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

/* ── 配置描述符（蓝牙接口字节内联） ────────────────────────────────── */

enum { CONFIG_TOTAL_LEN = (TUD_CONFIG_DESC_LEN + BT_ITF_DESC_LEN + TUD_CDC_DESC_LEN) };

static const uint8_t const_desc_configuration[] = {
    /* 配置：4 个接口（蓝牙主 + 蓝牙 isoc + CDC 控制 + CDC 数据） */
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* 蓝牙 HCI 接口：class 0xE0 (Wireless Controller) / sub 01 / proto 01 */
    0x09, TUSB_DESC_INTERFACE, 0x00, 0x00, 0x03,
          0xE0, 0x01, 0x01, 0x00,
    /* EP 0x81 中断 IN 16 字节，轮询间隔 1ms（HCI 事件） */
    0x07, TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_INTERRUPT,
          U16_TO_U8S_LE(CFG_TUD_BTH_EVENT_EPSIZE), 0x01,
    /* EP 0x02 批量 OUT 64 字节（host→controller ACL） */
    0x07, TUSB_DESC_ENDPOINT, 0x02, TUSB_XFER_BULK,
          U16_TO_U8S_LE(CFG_TUD_BTH_DATA_EPSIZE), 0x00,
    /* EP 0x82 批量 IN 64 字节（controller→host ACL） */
    0x07, TUSB_DESC_ENDPOINT, 0x82, TUSB_XFER_BULK,
          U16_TO_U8S_LE(CFG_TUD_BTH_DATA_EPSIZE), 0x00,

    /* 蓝牙 isoc 语音接口（独立接口号 1，两组交替设置）。
     * - 内核 btusb 无条件认领 ifnum 1 作为 isoc 接口，必须留给它；
     * - USB 规范要求等时端点 wMaxPacketSize 非零（零值会被 Windows 拒绝启动，
     *   代码 10 实测踩坑）；
     * - alt0=9B / alt1=49B 模拟真实 dongle 的带宽阶梯；我们不实际传输 eSCO
     *   语音（HFP 限制，见架构文档）。 */
    0x09, TUSB_DESC_INTERFACE, 0x01, 0x00, 0x02,
          0xE0, 0x01, 0x01, 0x00,
    0x07, TUSB_DESC_ENDPOINT, 0x85, TUSB_XFER_ISOCHRONOUS, U16_TO_U8S_LE(9), 0x01,
    0x07, TUSB_DESC_ENDPOINT, 0x05, TUSB_XFER_ISOCHRONOUS, U16_TO_U8S_LE(9), 0x01,
    0x09, TUSB_DESC_INTERFACE, 0x01, 0x01, 0x02,
          0xE0, 0x01, 0x01, 0x00,
    0x07, TUSB_DESC_ENDPOINT, 0x85, TUSB_XFER_ISOCHRONOUS, U16_TO_U8S_LE(49), 0x01,
    0x07, TUSB_DESC_ENDPOINT, 0x05, TUSB_XFER_ISOCHRONOUS, U16_TO_U8S_LE(49), 0x01,

    /* CDC 控制台：itf 2(控制) + 3(数据)，避开 btusb 专用的 itf 1 */
    TUD_CDC_DESCRIPTOR(2, 0, 0x83, 8, 0x04, 0x84, 64),
};

/* ── 字符串描述符 ──────────────────────────────────────────────────── */

static const char *const string_desc_arr[] = {
    (const char[]){0x09, 0x04},   /* 0: 英文 (USB_LANGID) */
    "qwp",                        /* 1: 厂商 */
    "ESP32 Dual-Mode BT Dongle",         /* 2: 产品 */
    "S3DUALDONGLE1",               /* 3: 序列号 */
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&const_desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return const_desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc_str[32];

    if (index == 0) {
        return (uint16_t const *)string_desc_arr[0];
    }
    if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
        return NULL;
    }

    const char *str = string_desc_arr[index];
    uint8_t len = (uint8_t)strlen(str);
    if (len > 31) {
        len = 31;
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    for (uint8_t i = 0; i < len; i++) {
        desc_str[1 + i] = str[i];
    }
    return desc_str;
}
