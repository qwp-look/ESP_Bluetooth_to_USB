# esp32-usb-bt-dongle

用 **ESP32-S3 + 经典 ESP32(WROOM)** 自制 USB 蓝牙适配器：插到电脑上即被识别为标准蓝牙适配器，可在系统设置里正常发现、连接蓝牙设备。

## 当前状态（2026-10-24，全链路实测通过 ✅）

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | `dongle-usb`：S3 单板 BLE，CDC + btattach | 代码完成（实测聚焦双模，未单测） |
| 2 | `dongle-usb-bth`：S3 单板 BLE，USB class 0xE0 即插即用 | 代码完成（含全部修复） |
| 3 | `radio-bridge`：经典板双模控制器 → UART H:4 | **实测通过** |
| 3a | `dongle-uart-cdc`：S3 UART↔CDC 透明桥（双模+btattach） | **实测通过**：扫描/配对/A2DP 播放 |
| 3b | `dongle-uart-bth`：**最终形态**——UART H:4 ↔ class 0xE0，双模即插即用 | **实测通过**：btusb 自动绑定、免 btattach、HP BTS02 音箱 A2DP 播放（572KB/10s 零错误） |

实测过程中修复的关键问题（完整记录见 [架构文档](docs/architecture.md) 的决策记录）：
经典板 UART 流控默认值、btusb 的 isoc 接口布局假设、DWC2 端点整包倍数缺陷、
USB ACL 分片重组、esp_restart 死锁、ROM 下载模式进入方式。

固件明细与烧录说明见 [firmware/README.md](firmware/README.md)；
复现验证流程见 [docs/verification-checklist.md](docs/verification-checklist.md)。

## 为什么需要两块板

| 能力 | ESP32-S3 | 经典 ESP32 |
|---|---|---|
| 蓝牙 | 仅 BLE（无经典蓝牙） | BLE + 经典蓝牙（双模） |
| 原生 USB | 有（USB-OTG，可自定义设备类） | 无 |

蓝牙音频（A2DP）和老式蓝牙设备走经典蓝牙，只有经典 ESP32 的无线电支持；
而"插上就被电脑当标准适配器识别"需要原生 USB 实现蓝牙设备类（class 0xE0），只有 S3 能做。
两板组合 = 一个双模 USB 蓝牙 dongle。

## 架构

```
电脑 ══USB class 0xE0══ ESP32-S3（dongle-uart-bth）
                          │
                          ├── UART H:4 (GPIO17/18, 921600)
                          └── 5V 供电（最终形态）
                          │
                       经典 ESP32（radio-bridge，双模控制器）
```

电脑上的蓝牙协议栈（Linux BlueZ / Windows inbox 栈）跑全部配对和 Profile，
两块板只做"无线电 ↔ USB"的搬运。详见 [架构文档](docs/architecture.md)。

## 目录

```
firmware/        # 5 个固件工程 + 共享组件（见 firmware/README.md）
scripts/host/    # 烧录/恢复/挂载/测试脚本 + udev 规则 + systemd 单元
docs/            # 接线、架构、决策记录、验证清单
```

## 环境

- ESP-IDF v6.0.2（`/home/qwp/esp/`，`source activate-idf.sh` 激活），首次构建需联网拉组件
- 串口：经典板 = `/dev/ttyUSB0`；S3 原生口 = `/dev/ttyACM0`（跑 USB 固件后变成 CDC/蓝牙口）

## 参考

- [ESP-IDF 官方 HCI over UART 例程](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/hci)（esp_bt_hci_tl_t 传输层接口）
- [slank/esp32-bt](https://github.com/slank/esp32-bt) — S3 VHCI→USB CDC 先例（H:4 解析参考）
- [dakhnod/ESP32-Bluetooth-USB-dongle](https://github.com/dakhnod/ESP32-Bluetooth-USB-dongle) — 经典 ESP32 先例
- [TinyUSB bth 设备类](https://github.com/hathach/tinyusb/blob/master/src/class/bth/bth_device.h)
- [PondMerge](https://github.com/qwp-look/PondMerge) — 自研 no-MMU 内存管理库，计划用于后续非实时扩展
