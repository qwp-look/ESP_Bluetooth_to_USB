# 固件总览

五个工程，全部在 ESP-IDF v6.0.2 下编译通过（`source /home/qwp/esp/activate-idf.sh` 后在各目录 `idf.py set-target <target> && idf.py build`，首次构建需联网下载组件）。

| 工程 | 板子 | 阶段 | 干什么 | 主机侧用法 |
|---|---|---|---|---|
| `dongle-usb` | S3 | 1 | S3 自带 BLE 控制器，HCI 经 USB CDC 输出 | `sudo btattach -B /dev/ttyACM0 -P h4 -S 115200` |
| `dongle-usb-bth` | S3 | 2 | 同上，但 USB 是标准蓝牙类（0xE0） | **免配置即插即用**（btusb 自动绑定） |
| `radio-bridge` | 经典 ESP32 | 3 | 双模蓝牙控制器，HCI H:4 经 UART2 输出 | 不直连主机（配 3a/3b 用） |
| `dongle-uart-cdc` | S3 | 3a | UART↔USB CDC 透明桥（经典板的 HCI 转发到 USB） | `sudo btattach -B /dev/ttyACM0 -P h4 -S 921600` |
| `dongle-uart-bth` | S3 | **3b 最终形态** | UART H:4 解析分流 ↔ USB class 0xE0，双模即插即用 | **免配置即插即用** |

## 推荐验证顺序（回来后）

1. **阶段 1**（单板，最快见效）：烧 `dongle-usb` 到 S3 → btattach → `bluetoothctl` 扫描连 BLE 设备
2. **阶段 2**（单板即插即用）：烧 `dongle-usb-bth` → 插上就有蓝牙，测试 Windows 也在这一步
3. **阶段 3a**（两板，透明桥路线）：经典板烧 `radio-bridge` + S3 烧 `dongle-uart-cdc` → btattach → 双模
4. **阶段 3b**（两板，最终形态）：S3 换烧 `dongle-uart-bth` → 免配置双模

3a 与 3b 功能等价，3b 是产品形态；先验证 3a 是因为它结构更简单、便于定位问题。

## 烧录口速查

| 板子 | 端口 | 说明 |
|---|---|---|
| 经典 ESP32 | `/dev/ttyUSB0` | 板载串口芯片，随时可烧（`idf.py -p /dev/ttyUSB0 flash`） |
| S3 首次/恢复 | `/dev/ttyACM0`（ROM Serial/JTAG） | 重烧走 `../scripts/host/flash-s3.sh`（固件控制台发 `boot`）或 `recover-s3.sh` |

注意：S3 烧入 USB 固件后原生 USB 口会被 TinyUSB 占用，Serial/JTAG 下载口消失；
所有带控制通道的固件都支持免按键重烧（见 `components/ctrl_cdc` 或工程内 `ctrl_tusb.c`）。

## 目录结构

```
firmware/
├── components/ctrl_cdc/     # 共享组件：CDC 控制台（boot/dfu/restart/stats，esp_tinyusb 版）
├── dongle-usb/              # 阶段 1
├── dongle-usb-bth/          # 阶段 2（原生 TinyUSB + bth 类）
├── radio-bridge/            # 阶段 3（经典板）
├── dongle-uart-cdc/         # 阶段 3a
└── dongle-uart-bth/         # 阶段 3b（最终形态）
```

## 关键设计点

- **HCI 透传**：所有固件不解析、不修改主机与控制器之间的 HCI 语义（3b 只按 H:4 类型字节分流端点），配对/Profile 全在主机栈——兼容性最大化。
- **日志隔离**：所有 S3 固件日志走 UART0（GPIO43/44 = 串口桥口），与 USB 数据口物理隔离，不会污染 HCI 流。
- **实时路径零动态分配**：收发全静态缓冲/环形队列（PondMerge 计划用于后续非实时扩展，见架构文档）。
- **USB 描述符**：bth 版本是 0xE0 蓝牙主控制器接口（事件=中断 IN 16B，ACL=批量 64B×2）+ CDC 控制台的复合设备；Linux btusb 按接口类自动绑定。未配置 ISO 备用接口——HFP 语音（eSCO over USB）暂不支持，A2DP（走 ACL）不受影响。
