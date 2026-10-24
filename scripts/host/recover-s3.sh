#!/usr/bin/env bash
# recover-s3.sh — 恢复重启循环/无法枚举的 S3。
#
# 原理：S3 每次 ROM/引导阶段 USB-Serial/JTAG 都会短暂枚举，
# esptool 连上的瞬间通过 DTR/RTS 硬件复位进下载模式，然后就稳定了。
# 若循环窗口太窄抓不住：按住板上 BOOT 键再按一下 RST，然后重跑本脚本。
#
# 用法: ./recover-s3.sh [端口, 默认 ttyACM0]

set -euo pipefail
PORT="${1:-/dev/ttyACM0}"

# shellcheck source=/dev/null
source /home/qwp/esp/activate-idf.sh >/dev/null

echo "在 $PORT 上反复尝试连接（最多 ~2 分钟）……"
ok=""
for i in $(seq 1 240); do
    if [ -e "$PORT" ] && timeout 5 esptool --port "$PORT" chip_id >/dev/null 2>&1; then
        ok=1
        echo "第 $i 次尝试成功——芯片已进入下载模式"
        break
    fi
    sleep 0.3
done

if [ -z "$ok" ]; then
    echo "自动恢复失败。请按住 S3 板上的 BOOT 键，按一下 RST，松开 BOOT，再跑一次本脚本。" >&2
    exit 1
fi

# 进下载模式后稳定在线，可选：擦除全片重来（去掉下行注释）
# esptool --port "$PORT" erase_flash

echo "芯片已稳定处于下载模式。现在可以烧录任意固件，例如："
echo "  ( cd firmware/dongle-usb && idf.py -p $PORT flash )"
