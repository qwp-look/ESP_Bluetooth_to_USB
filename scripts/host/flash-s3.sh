#!/usr/bin/env bash
# flash-s3.sh — 免按键重烧 S3：经 CDC 控制通道让芯片进 ROM 下载模式，再烧录。
#
# 用法: ./flash-s3.sh <固件工程目录> [控制口, 默认自动探测]
# 例:   ./flash-s3.sh ../../firmware/dongle-usb-bth
#
# 前提: S3 上跑的是本项目任一带控制通道的固件（CDC1 = 控制台）。
#       若 S3 已经变砖（重启循环），用 recover-s3.sh。

set -euo pipefail
FW_DIR="${1:?用法: flash-s3.sh <固件工程目录>}"
PORT="${2:-}"

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=/dev/null
source /home/qwp/esp/activate-idf.sh >/dev/null

find_ctrl_port() {
    # 控制台是第二个 CDC 接口（ttyACM1；若只有一个则为 ttyACM0）
    for p in /dev/ttyACM1 /dev/ttyACM0; do
        [ -e "$p" ] && { echo "$p"; return 0; }
    done
    return 1
}

if [ -z "$PORT" ]; then
    PORT="$(find_ctrl_port)" || { echo "未找到控制口（板子插好了吗？）" >&2; exit 1; }
fi
echo "[1/4] 控制口: $PORT — 发送 boot 命令"
python3 - "$PORT" <<'EOF'
import serial, sys, time
s = serial.Serial(sys.argv[1], 115200, timeout=2)
s.reset_input_buffer()
s.write(b"boot\n")
s.flush()
time.sleep(0.5)
try:
    print("  固件回复:", s.read(200).decode(errors="replace").strip())
except Exception:
    pass
s.close()
EOF

echo "[2/4] 等待下载模式枚举（原口会消失再出现）"
DL_PORT=""
for i in $(seq 1 40); do
    for p in /dev/ttyACM0 /dev/ttyACM1; do
        if [ -e "$p" ] && timeout 5 esptool --port "$p" --before no_reset chip_id >/dev/null 2>&1; then
            DL_PORT="$p"; break 2
        fi
    done
    sleep 0.5
done
[ -n "$DL_PORT" ] || { echo "未等到下载模式，尝试 recover-s3.sh" >&2; exit 1; }
echo "  下载口: $DL_PORT"

echo "[3/4] 烧录 $FW_DIR"
( cd "$FW_DIR" && idf.py -p "$DL_PORT" flash )

echo "[4/4] 完成。新固件的控制台口用 'screen <口> 115200' 进去敲 help 试试。"
