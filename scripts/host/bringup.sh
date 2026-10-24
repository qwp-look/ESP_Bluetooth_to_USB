#!/usr/bin/env bash
# bringup.sh — 阶段 3a 一次性拉起：bluetoothd + btattach + hci0 up + 双模信息 + 扫描
# 用法: sudo ./bringup.sh [hci 数据口, 默认 /dev/ttyACM0] [波特率, 默认 921600]
set -u
PORT="${1:-/dev/ttyACM0}"
RATE="${2:-921600}"

echo "[1/5] 启动 bluetoothd"
systemctl start bluetooth

echo "[2/5] 结束旧的 btattach（若有）"
pkill -f "btattach.*-B $PORT" 2>/dev/null && sleep 1

echo "[3/5] 后台启动 btattach ($PORT @ $RATE)"
nohup btattach -B "$PORT" -P h4 -S "$RATE" -N > /tmp/btattach.log 2>&1 &
sleep 3

echo "[4/5] 控制器状态"
hciconfig hci0 up 2>/dev/null
hciconfig
echo "--- btmgmt info（双模则同时有 BR/EDR 和 LE 地址）---"
btmgmt info 2>/dev/null | head -24
echo "--- btattach 日志 ---"
tail -6 /tmp/btattach.log

echo "[5/5] 经典蓝牙 inquiry 扫描 8 秒（找 HP BTS02）"
timeout 12 hcitool -i hci0 scan --length=8 2>/dev/null || echo "(扫描超时或无结果)"
