#!/usr/bin/env bash
# attach-ble.sh — 给 CDC 型固件（dongle-usb / dongle-uart-cdc）挂载 HCI。
# class 0xE0 固件（*-bth）不需要本脚本，插上即用。
#
# 用法: sudo ./attach-ble.sh [hci 口] [波特率]
#   sudo ./attach-ble.sh /dev/ttyACM0 921600
#
# 说明: 前台运行，Ctrl-C 断开（断开后 hci0 消失）。
#       想常驻用 systemd: 见 esp32-bt-attach@.service。

set -euo pipefail
PORT="${1:-/dev/ttyACM0}"
RATE="${2:-921600}"

exec btattach -B "$PORT" -P h4 -S "$RATE" -N
