#!/usr/bin/env bash
# test-dongle.sh — dongle 冒烟测试：hci0 是否出现、能否 up、能否扫描。
#
# 用法: ./test-dongle.sh [hci 编号, 默认 0] [扫描秒数, 默认 8]

set -euo pipefail
HCI="${1:-hci0}"
SECS="${2:-8}"

echo "== 控制器列表 =="
if command -v hciconfig >/dev/null; then
    hciconfig -a "$HCI" | head -12 || true
else
    bluetoothctl list
fi

echo "== UP 控制器 =="
hciconfig "$HCI" up 2>/dev/null || btmgmt -i "${HCI#hci}" power on || true

echo "== 控制器信息（版本/地址/特性集——来自真实蓝牙控制器！）=="
btmgmt -i "${HCI#hci}" info | head -20 || true

echo "== BLE 扫描 ${SECS}s（应能看到周边设备）=="
timeout "$SECS" btmgmt -i "${HCI#hci}" find 2>/dev/null | head -30 || true

echo "== 经典蓝牙 inquiry ${SECS}s（仅双模固件会有结果）=="
timeout "$SECS" hcitool -i "$HCI" scan --length=8 2>/dev/null | head -15 || true

echo "完成。若上面有设备出现，说明整条链路（主机栈↔USB↔[UART↔]控制器）工作正常。"
