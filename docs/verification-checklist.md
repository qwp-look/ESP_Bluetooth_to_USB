# 回来后的验证清单

> **2026-10-24 更新：本清单已全部执行完毕，阶段 3a/3b 均验证通过**
> （HP BTS02 音箱 A2DP 播放成功，最终形态 572KB/10s 零错误）。
> 以下内容保留作复现流程参考；实测中修掉的问题见 architecture.md 决策记录。
> 注意：当前板上烧的已是修复后的最终固件（S3=dongle-uart-bth，经典板=radio-bridge），
> 正常使用直接插上即可，无需再走第 0 步恢复。

## 0. 恢复环境（5 分钟）

1. **重新连接板子**：这台机器是 VMware 虚拟机，两块板子需要在宿主机上重新做 USB 直通：
   `VM → Removable Devices → Silicon Labs CP210x（经典板）/ Espressif USB（S3）→ Connect`。
   预期：`ls /dev/ttyUSB* /dev/ttyACM*` 至少能看到经典板的 `/dev/ttyUSB0`。
2. **恢复 S3**：S3 因重启循环，USB 口会不断消失/出现。运行：
   ```bash
   cd ~/code_2/scripts/host && ./recover-s3.sh
   ```
   抓住 ROM 阶段的下载口后就稳定了。若 2 分钟抓不住：按住 S3 板 BOOT 键 + 点按 RST，重跑。
3. **烧修复后的阶段 1 固件**：
   ```bash
   cd ~/code_2/firmware/dongle-usb && . /home/qwp/esp/activate-idf.sh
   idf.py -p /dev/ttyACM0 flash    # 端口名以实际为准
   ```
   预期：出现**两个** `/dev/ttyACM*`（CDC0=数据、CDC1=控制台）。以后重烧用
   `scripts/host/flash-s3.sh <工程目录>` 免按键。

## 1. 阶段 1：S3 单板 BLE（btattach 路线）

```bash
sudo ./scripts/host/attach-ble.sh /dev/ttyACM0 115200   # 另开终端
./scripts/host/test-dongle.sh
```
- 预期：`hci0` 出现，`btmgmt info` 显示真实控制器信息，BLE 扫描能看到周边设备。
- `bluetoothctl` → `scan on` → 连接一个 BLE 键鼠/传感器。
- 排障：CDC1 口敲 `stats` 看收发字节；日志在串口桥口（若插了 S3 第二个 USB 口）。

## 2. 阶段 2：S3 单板 BLE（class 0xE0 即插即用）

```bash
./scripts/host/flash-s3.sh ~/code_2/firmware/dongle-usb-bth
# 拔插一次 USB，或 dmesg 看
hciconfig   # 应直接出现 hci0，无 btattach
./scripts/host/test-dongle.sh
```
- 预期：`lsusb` 显示 `303a:4002`；`btusb` 自动绑定（`dmesg | grep btusb`）。
- **Windows 实验**（可选）：拔下来插 Windows 机器，看设备管理器是否出现蓝牙适配器。
  这是全项目最大的未知数——能通就赚到，不通也不影响 Linux 路线。

## 3. 阶段 3a：两板双模（透明桥 + btattach）

```bash
# 经典板（接线那 3 根杜邦线确认还在）
cd ~/code_2/firmware/radio-bridge && idf.py -p /dev/ttyUSB0 flash
# S3
./scripts/host/flash-s3.sh ~/code_2/firmware/dongle-uart-cdc
sudo ./scripts/host/attach-ble.sh /dev/ttyACM0 921600
./scripts/host/test-dongle.sh
```
- 预期：`btmgmt info` 显示**双模**控制器（BR/EDR + LE 地址）；
  `hcitool scan`（经典 inquiry）也能扫到手机/耳机。
- 排障：经典板日志在其 USB 口（`idf.py -p /dev/ttyUSB0 monitor`），
  应看到 "dual-mode controller up, HCI H:4 on UART2"。S3 侧 `stats` 看 UART↔USB 字节。

## 4. 阶段 3b：最终形态（class 0xE0 双模即插即用）

```bash
./scripts/host/flash-s3.sh ~/code_2/firmware/dongle-uart-bth
hciconfig && ./scripts/host/test-dongle.sh
```
- 预期：`lsusb` 显示 `303a:4003`，btusb 自动绑定，`bluetoothctl` 可连接经典蓝牙设备
  和 BLE 设备；A2DP 耳机连接放音（`pipewire`/`pulseaudio` 里选择耳机输出）。
- 已知限制：HFP 通话（eSCO over USB）未实现（无 ISO 备用接口）。

## 5. 常驻化（可选）

```bash
sudo cp scripts/host/99-esp32-btdongle.rules /etc/udev/rules.d/
sudo cp scripts/host/esp32-bt-attach@.service /etc/systemd/system/
sudo udevadm control --reload && sudo systemctl daemon-reload
```
CDC 型固件从此插上自动挂载（0xE0 型本来就免配置）。

## 故障速查

| 现象 | 原因/处理 |
|---|---|
| S3 USB 口反复消失出现 | 重启循环 → `recover-s3.sh` |
| btattach 报 timeout/no response | 波特率不匹配（3a 必须 921600）；或 S3 固件与经典板不配套 |
| hci0 出现但扫描无结果 | 天线/供电；`stats` 看 tx 是否为 0（下行走不通先查接线交叉） |
| 阶段 3 扫不到经典设备 | 确认经典板日志有 "dual-mode controller up"；确认烧的是 `radio-bridge` 而不是别的 |
| VM 里看不到串口 | VMware 直通掉了，重新 Connect |
