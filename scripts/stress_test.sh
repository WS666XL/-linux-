#!/usr/bin/env bash
# DistMonitor 压测脚本：分别制造 CPU / 内存 / 磁盘 / 网络负载
#
# 用法:
#   ./scripts/stress_test.sh cpu  [--duration=30] [--workers=6]
#   ./scripts/stress_test.sh mem  [--duration=30] [--size=512M]
#   ./scripts/stress_test.sh disk [--duration=30] [--size=256M]
#   ./scripts/stress_test.sh net  [--duration=30] [--peer=<对端IP>]
#   ./scripts/stress_test.sh all  [--duration=30] [--peer=<对端IP>]
#
# 说明:
#   - 压测期间另开一个终端运行 ./build/monitor_tui，观察各指标变化
#   - net 需要另一台机器作为 iperf3 服务端（对端执行 iperf3 -s）
#   - 所有模式都有超时；Ctrl+C 会清理后台进程和临时文件

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

DURATION=30
WORKERS="$(nproc 2>/dev/null || echo 2)"
SIZE_MEM="512M"
SIZE_DISK="256M"
PEER=""
TMP_DIR="/tmp/distmonitor_stress"

usage() {
  sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

have() { command -v "$1" >/dev/null 2>&1; }

size_to_mb() {
  local v="$1"
  case "$v" in
    *G|*g) echo $(( ${v%[Gg]} * 1024 )) ;;
    *M|*m) echo "${v%[Mm]}" ;;
    *) echo "$v" ;;
  esac
}

cleanup() {
  local pids
  pids="$(jobs -p 2>/dev/null)"
  if [[ -n "$pids" ]]; then
    # shellcheck disable=SC2086
    kill $pids 2>/dev/null
    wait 2>/dev/null
  fi
  rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

MODE="${1:-}"
if [[ -z "$MODE" ]]; then
  usage
  exit 1
fi
shift

for arg in "$@"; do
  case "$arg" in
    --duration=*) DURATION="${arg#*=}" ;;
    --workers=*)  WORKERS="${arg#*=}" ;;
    --size=*)     SIZE_MEM="${arg#*=}"; SIZE_DISK="${arg#*=}" ;;
    --peer=*)     PEER="${arg#*=}" ;;
    -h|--help)    usage; exit 0 ;;
    *) echo "未知参数: $arg"; echo; usage; exit 1 ;;
  esac
done

if ! [[ "$DURATION" =~ ^[0-9]+$ ]] || [[ "$DURATION" -le 0 ]]; then
  echo "[err] --duration 必须是正整数（秒）"
  exit 1
fi
if ! [[ "$WORKERS" =~ ^[0-9]+$ ]] || [[ "$WORKERS" -le 0 ]]; then
  echo "[err] --workers 必须是正整数"
  exit 1
fi

title() {
  echo
  echo "=================================================="
  echo "  $1  (${DURATION}s)"
  echo "=================================================="
}

run_cpu() {
  title "CPU 压测：${WORKERS} 个 worker"
  if have stress; then
    stress -q --cpu "$WORKERS" --timeout "${DURATION}s"
  else
    local i
    for i in $(seq "$WORKERS"); do
      yes >/dev/null &
    done
    sleep "$DURATION"
  fi
}

run_mem() {
  local mb
  mb="$(size_to_mb "$SIZE_MEM")"
  title "内存压测：申请 ${SIZE_MEM}"
  if have stress; then
    stress -q --vm 1 --vm-bytes "$SIZE_MEM" --timeout "${DURATION}s"
  elif have python3; then
    python3 - "$mb" "$DURATION" <<'PY' &
import sys, time
mb, sec = int(sys.argv[1]), int(sys.argv[2])
chunks = []
for _ in range(mb):
    b = bytearray(1024 * 1024)
    # 逐页写入，强制分配真实物理页，而不是只保留虚拟地址
    for off in range(0, len(b), 4096):
        b[off] = 1
    chunks.append(b)
time.sleep(sec)
PY
    wait
  else
    echo "[warn] 缺少 stress / python3，跳过内存压测"
  fi
}

run_disk() {
  title "磁盘压测：读写 ${SIZE_DISK}"
  if have stress; then
    stress -q --hdd 2 --hdd-bytes "$SIZE_DISK" --timeout "${DURATION}s"
  else
    mkdir -p "$TMP_DIR"
    local mb
    mb="$(size_to_mb "$SIZE_DISK")"
    local file="$TMP_DIR/io.bin"
    local end=$((SECONDS + DURATION))
    while [[ $SECONDS -lt $end ]]; do
      dd if=/dev/zero of="$file" bs=1M count="$mb" conv=fsync status=none
      dd if="$file" of=/dev/null bs=1M status=none
    done
  fi
}

run_net() {
  if ! have iperf3; then
    echo "[err] 未安装 iperf3，Ubuntu: sudo apt install -y iperf3"
    return 1
  fi
  if [[ -z "$PEER" ]]; then
    echo "[err] 网络压测需要 --peer=<对端IP>"
    echo "      对端先执行: iperf3 -s"
    echo "      本机再执行: $0 net --peer=<对端IP>"
    echo "      注意: 本机回环地址 lo 被采集器排除，用 127.0.0.1 看不到流量变化"
    return 1
  fi
  title "网络压测：to ${PEER}"
  iperf3 -c "$PEER" -t "$DURATION" -P 4
}

case "$MODE" in
  cpu)  run_cpu ;;
  mem)  run_mem ;;
  disk) run_disk ;;
  net)  run_net ;;
  all)
    run_cpu
    run_mem
    run_disk
    if [[ -n "$PEER" ]]; then
      run_net
    else
      echo
      echo "[skip] 网络压测：未提供 --peer，跳过（需要另一台机器做 iperf3 服务端）"
    fi
    ;;
  *)
    echo "未知模式: $MODE"
    echo
    usage
    exit 1
    ;;
esac

echo
echo "[done] 压测结束。查看结果:"
echo "       ./build/monitor_tui --once --no-color"
echo "       mysql -h127.0.0.1 -umonitor -pmonitor123 monitor_db \\"
echo "         -e \"SELECT node_id,cpu_usage,mem_usage,load_avg1,disk_write_bps,net_recv_bps,ts FROM metrics ORDER BY ts DESC LIMIT 5;\""
