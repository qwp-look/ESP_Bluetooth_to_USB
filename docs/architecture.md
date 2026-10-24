# 架构与设计

## 数据路径

电脑上的蓝牙协议栈把我们的设备当成一个普通蓝牙控制器（HCI 控制器），全部智能（配对、
Profile、GATT、A2DP 编解码管理）都在电脑侧；两块 ESP32 只做字节搬运：

```
电脑 BlueZ / Windows 蓝牙栈
   ↕ USB class 0xE0（标准蓝牙 HCI 传输，无需驱动）        ← ESP32-S3 TinyUSB bth
   ↕ UART H:4 帧（921600+ 波特率，交叉接线）
   ↕ VHCI（片内主机控制器接口）                            ← 经典 ESP32 桥固件
蓝牙无线电（BLE + 经典蓝牙）
```

关键点：**HCI H:4 帧内容不在 ESP32 上做任何解析或改写**，端到端透明。
这保证了电脑栈看到的就是一个标准控制器行为，兼容性最大化。

## 固件职责

### S3：`dongle-usb`（USB 前端）

- TinyUSB 设备栈，class 0xE0（bth）+ 阶段 1 先用 CDC-ACM 过渡
- UART 驱动收发 H:4 帧，与 USB 端点间转发
- 缓冲策略：静态环形缓冲优先，实时路径上不做动态分配

### 经典 ESP32：`radio-bridge`（蓝牙控制器端）

- 蓝牙控制器以 VHCI 模式运行（不经 Bluedroid/NimBLE 主机栈）
- 桥固件把 VHCI 的 HCI 包流转到 UART2（H:4 格式）
- 日志只输出到 UART0，严禁混入 UART2

## 阶段计划

| 阶段 | 内容 | 依赖 | 主要复用 |
|---|---|---|---|
| 1 | S3 单板 BLE：VHCI→USB CDC，Linux `btattach` 出 `hci0`，连 BLE 设备 | 仅 S3 | slank/esp32-bt |
| 2 | CDC 换 TinyUSB bth（class 0xE0），Linux 即插即用；Windows 实测 | 阶段 1 | TinyUSB bth_device |
| 3 | 经典板 VHCI↔UART 控制器固件，S3 改为从 UART 取 HCI 流 → 双模 | 两板 + 接线 | dakhnod 项目桥接逻辑 |

## PondMerge 的使用计划

PondMerge（自研 no-MMU TLSF 内存管理库，见 GitHub qwp-look/PondMerge）：

- **不用于**实时 HCI 转发路径——该路径用静态环形缓冲，硬实时路径上不做分配
- **计划用于**阶段 3 的非实时动态部分：ACL 大包重组缓冲、日志/抓包 ring、
  后续扩展功能的对象生命周期管理；其稳定句柄（`pm_ptr`）与压缩特性适合长跑固件
- 兼作 PondMerge 在真实产品场景的 dogfooding

## 风险记录

| 风险 | 等级 | 说明与对策 |
|---|---|---|
| Windows 兼容性 | 高/未知 | Windows inbox 蓝牙驱动对 class E0 设备理论上泛匹配（杂牌 CSR dongle 免驱即此原理），但微软栈对控制器特性集挑剔。按 Linux 优先、阶段 2 起实测，不做承诺 |
| 经典控制器在 BlueZ 下的特性覆盖 | 中 | dakhnod 项目已验证 BlueZ 基本可用；A2DP（走 ACL）预计可行；HFP 通话需 eSCO，ESP32 控制器支持度未知，先不承诺 |
| UART 吞吐 | 低 | A2DP SBC 约 328 kbps ≈ 41 KB/s，921600 波特率余量充足；不够再升 2M + 硬件流控（接线已预留） |
| IDF v6.0.2 兼容性 | 低 | 参考项目多基于 IDF 5.x，移植时注意 USB/VHCI API 变化 |

## 决策记录

- 2026-10-23：确定双芯片方案（S3=USB 前端，经典=无线电）；HCI 走 UART2(GPIO16/17) ↔ S3 GPIO18/17；
  阶段 1 先用 CDC+btattach 降低风险，阶段 2 再切 class 0xE0。
- 2026-10-24：**放弃 slank/esp32-bt 的旧 VHCI 桥接方式**。IDF 6.0.2 上 S3 控制器改用新的
  `esp_bt_hci_tl_t` 传输层回调接口（官方例程 `controller_hci_uart_esp32c3_and_esp32s3` 的方式），
  把传输实现从 UART 换成 USB CDC/USB bth 类。H:4 拼包由控制器 H4-TL 层自己完成，固件更薄。
- 2026-10-24：**经典板阶段 3 改用官方例程** `controller_hci_uart_esp32`（双模、内置 UART H4 驱动），
  仅改 UART2 引脚与波特率。比预想的 dakhnod 移植路线省事得多，且官方维护。
- 2026-10-24：**S3 固件统一带"控制通道"**（CDC 控制台：boot/dfu/restart/stats）。
  背景：第一次烧录就踩坑——esp_tinyusb 默认 `CONFIG_TINYUSB_CDC_COUNT=1`，第二个 CDC 接口
  初始化失败触发 abort 重启循环，而 USB 固件一跑起来 Serial/JTAG 下载口就没了，无法重烧。
  控制通道从软件重启进 ROM 下载模式（`RTC_CNTL_OPTION1_REG |= FORCE_DOWNLOAD_BOOT`），
  彻底解决免按键重烧问题。教训：非核心组件的初始化一律非致命（只告警）。
- 2026-10-24：**阶段 2/3b 使用原生 TinyUSB**（managed 组件 espressif/tinyusb 0.21），
  因为 esp_tinyusb 封装层不暴露 bth（0xE0）设备类。要点：tusb_config.h 经
  `target_include_directories(<tinyusb组件> PRIVATE ...)` 注入（esp_tinyusb 同款做法）；
  描述符回调在 main 定义但链接顺序在 tinyusb 之前，需 `LINK_OPTIONS -u tud_descriptor_device_cb`
  强制拉入；本版本无 TUD_BT_DESCRIPTOR 模板宏，蓝牙接口描述符手写字节。
- 2026-10-24：**USB 侧不配置 ISO 备用接口**（`CFG_TUD_BTH_ISO_ALT_COUNT=0`）：
  HFP 语音（eSCO over USB）暂不支持；A2DP 走 ACL 不受影响。将来要 HFP 需扩描述符+ISO 端点缓冲。
- 2026-10-24：硬件实测在阶段 1 烧录后中断（VMware USB 直通断开）。全部 5 个固件已编译通过，
  待验证流程见 [verification-checklist.md](verification-checklist.md)。
- 2026-10-24（下午，全链路实测）：**阶段 3a 与 3b 均验证通过**，HP BTS02 音箱经 A2DP 播放成功
  （3b 路线：btusb 即插即用 → 0xE0 → S3 → UART → 经典板 → A2DP，10 秒 572KB 音频零错误）。
  实测中修掉的坑，按发现顺序：
  1. **经典板流控**：`CONFIG_BTDM_CTRL_HCI_UART_FLOW_CTRL_EN` 默认开启（bool，不是 choice），
     我们没接 RTS/CTS 必须显式 `=n`，否则控制器 UART 输出乱码。
  2. **esp_restart 死锁**：控制台命令直接在 TinyUSB 回调里 esp_restart() 会死锁
     （重启钩子等待 USB 任务，而调用者就是 USB 任务）——重启动作挪到独立任务执行。
  3. **ROM 下载模式进入方式**：仅写 FORCE_DOWNLOAD_BOOT 不够，必须先 `chip_usb_set_persist_flags
     (USBDC_PERSIST_ENA)`（对齐官方 usb_console.c）。
  4. **btusb 的接口布局假设**：内核 btusb 无条件认领 ifnum 1 作为 isoc 语音接口。
     最终布局：itf0=BT主接口(3EP) + itf1=BT isoc(零带宽) + itf2/3=CDC 控制台。
     另外 TinyUSB bth 驱动要求主接口后必须跟一个 E0 类接口（ISO 备用接口最少一个）。
  5. **DWC2 端点整包倍数缺陷**：长度恰好等于端点 maxpacket 整倍数的中断端点传输
     永远不完成。规避：发送侧分片恒为短包（事件 15B/片、ACL 63B/片）。
  6. **USB ACL 分片语义**：bth 驱动按端点大小（64B）分片递交主机 ACL 包，
     每片回调一次；必须重组——只在包首片段加 H:4 类型字节，续片裸透传。
     （内核 btusb 对事件/ACL IN 自带重组，所以入向分片是安全的。）
  7. **VMware USB 直通极不稳**：DTR/RTS 复位脉冲会随机击穿 CH340 直通（I/O error），
     烧录后 hard-reset 常被吞（需手动按 RST），设备身份变化会导致直通脱落。
     运维手段：deauthorize/authorize 强制重探测、USBDEVFS_RESET 端口复位、低波特率(115200)烧录。
- 2026-10-24：**软件重烧的边界**：纯 CDC 固件（dongle-usb / dongle-uart-cdc）的控制台 `boot`
  命令可软件进下载模式重烧；0xE0 复合固件因 ROM 下载 CDC 落在无主机驱动的接口上，
  重烧需 BOOT+RST 组合键（结构性限制，详见 firmware/README.md）。
