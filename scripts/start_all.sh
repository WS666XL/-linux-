#!/usr/bin/env bash
# DistMonitor 一键启停脚本
#
# 用法:
#   ./scripts/start_all.sh            启动 manager + agent，然后进入终端面板
#   ./scripts/start_all.sh --no-tui   只启动后台服务，不进面板
#   ./scripts/start_all.sh status     查看运行状态与最近日志
#   ./scripts/start_all.sh stop       停止由本脚本启动的 manager / agent
#
# 说明:
#   - manager / agent 以 setsid 方式脱离终端运行，关掉当前终端也不会退出
#   - 以 root 运行本脚本时 agent 可启用 TC/eBPF；普通用户运行会回退 /proc
#   - 运行日志与 PID 文件统一放在 /tmp/distmonitor/

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

MANAGER_BIN="./build/manager"
AGENT_BIN="./build/agent"
TUI_BIN="./build/monitor_tui"
MANAGER_CONF="configs/manager.conf"
AGENT_CONF="configs/agent.conf"
GRPC_ADDR="127.0.0.1:50051"

RUN_DIR="/tmp/distmonitor"
MANAGER_PID="$RUN_DIR/manager.pid"
AGENT_PID="$RUN_DIR/agent.pid"
MANAGER_LOG="$RUN_DIR/manager.log"
AGENT_LOG="$RUN_DIR/agent.log"

mkdir -p "$RUN_DIR"

usage() {
  sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

pid_alive() {
  local f=$1 pid
  [[ -f "$f" ]] || return 1
  pid="$(cat "$f" 2>/dev/null)"
  [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null
}

port_in_use() {
  ss -lnt 2>/dev/null | grep -q ":$1 "
}

check_mysql() {
  if systemctl is-active --quiet mysql 2>/dev/null; then
    echo "[ok]   mysql 服务运行中"
    return 0
  fi
  if port_in_use 3306; then
    echo "[ok]   检测到 3306 端口监听，mysql 可用"
    return 0
  fi
  echo "[warn] 未检测到 mysql（3306 未监听），manager 将无法落库，继续启动..."
}

start_manager() {
  if pid_alive "$MANAGER_PID"; then
    echo "[skip] manager 已在运行 (pid $(cat "$MANAGER_PID"))"
    return 0
  fi
  if port_in_use 50051; then
    echo "[warn] 50051 已被占用（可能是手动启动的 manager），跳过启动"
    return 0
  fi
  if [[ ! -x "$MANAGER_BIN" ]]; then
    echo "[err]  缺少 $MANAGER_BIN，请先执行: cmake --build build -j\$(nproc)"
    return 1
  fi

  setsid nohup env -u LD_LIBRARY_PATH "$MANAGER_BIN" -c "$MANAGER_CONF" \
    >"$MANAGER_LOG" 2>&1 </dev/null &
  echo $! >"$MANAGER_PID"

  local i
  for i in {1..25}; do
    port_in_use 50051 && break
    sleep 0.2
  done

  if ! pid_alive "$MANAGER_PID"; then
    echo "[err]  manager 启动失败，日志如下:"
    tail -n 20 "$MANAGER_LOG"
    return 1
  fi
  echo "[ok]   manager 已启动 (pid $(cat "$MANAGER_PID"))，监听 $GRPC_ADDR"
}

start_agent() {
  if pid_alive "$AGENT_PID"; then
    echo "[skip] agent 已在运行 (pid $(cat "$AGENT_PID"))"
    return 0
  fi
  if pgrep -f "build/agent -c" >/dev/null 2>&1; then
    echo "[warn] 已存在其他 agent 进程，跳过启动:"
    pgrep -af "build/agent -c" | sed 's/^/       /'
    return 0
  fi
  if [[ ! -x "$AGENT_BIN" ]]; then
    echo "[err]  缺少 $AGENT_BIN，请先执行: cmake --build build -j\$(nproc)"
    return 1
  fi

  setsid nohup env -u LD_LIBRARY_PATH "$AGENT_BIN" -c "$AGENT_CONF" \
    >"$AGENT_LOG" 2>&1 </dev/null &
  echo $! >"$AGENT_PID"
  sleep 1

  if ! pid_alive "$AGENT_PID"; then
    echo "[err]  agent 启动失败，日志如下:"
    tail -n 20 "$AGENT_LOG"
    return 1
  fi
  echo "[ok]   agent 已启动 (pid $(cat "$AGENT_PID"))"

  if [[ $EUID -eq 0 ]]; then
    echo "       root 模式：TC/eBPF 与内核模块可用（以启动日志为准）"
  else
    echo "[info] 普通用户模式：eBPF/TC 自动回退 /proc，softirq_rate 为 0"
    echo "       需要完整采集能力请用 sudo 运行本脚本"
  fi
}

start_tui() {
  if [[ ! -x "$TUI_BIN" ]]; then
    echo "[err]  缺少 $TUI_BIN，请先执行: cmake --build build -j\$(nproc)"
    return 1
  fi
  if [[ -t 1 ]]; then
    echo
    echo "[info] 进入终端面板，Ctrl+C 退出（退出后服务仍在后台运行）"
    "$TUI_BIN" "$GRPC_ADDR"
  else
    # 非 TTY（重定向/管道）只输出一帧，避免 ANSI 控制符污染日志
    "$TUI_BIN" "$GRPC_ADDR" --once --no-color
  fi
}

stop_one() {
  local name=$1 pidf=$2 pid
  if ! pid_alive "$pidf"; then
    echo "[skip] $name 未运行"
    rm -f "$pidf"
    return 0
  fi
  pid="$(cat "$pidf")"
  kill "$pid" 2>/dev/null
  local i
  for i in {1..15}; do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.2
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null
  fi
  rm -f "$pidf"
  echo "[ok]   $name 已停止 (pid $pid)"
}

show_status() {
  local pid
  if pid_alive "$MANAGER_PID"; then
    pid="$(cat "$MANAGER_PID")"
    echo "[运行] manager pid=$pid"
  elif port_in_use 50051; then
    echo "[运行] manager（非本脚本启动，pid 未知），$GRPC_ADDR 监听中"
  else
    echo "[停止] manager"
  fi

  if pid_alive "$AGENT_PID"; then
    echo "[运行] agent   pid=$(cat "$AGENT_PID")"
  elif pgrep -f "build/agent -c" >/dev/null 2>&1; then
    echo "[运行] agent（非本脚本启动）"
  else
    echo "[停止] agent"
  fi

  echo
  echo "--- manager 日志（末 3 行）---"
  tail -n 3 "$MANAGER_LOG" 2>/dev/null || echo "(暂无)"
  echo "--- agent 日志（末 3 行）---"
  tail -n 3 "$AGENT_LOG" 2>/dev/null || echo "(暂无)"
}

ACTION="start"
NO_TUI=0
for arg in "$@"; do
  case "$arg" in
    start|stop|status) ACTION="$arg" ;;
    --no-tui) NO_TUI=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数: $arg"; echo; usage; exit 1 ;;
  esac
done

case "$ACTION" in
  start)
    check_mysql
    start_manager || exit 1
    start_agent || exit 1
    echo
    echo "[ok]   服务已就绪"
    echo "       manager 日志: $MANAGER_LOG"
    echo "       agent   日志: $AGENT_LOG"
    echo "       停止服务:     $0 stop"
    if [[ $NO_TUI -eq 0 ]]; then
      sleep 2  # 等首个采样点上报，避免 --once 打出空面板
      start_tui
    fi
    ;;
  stop)
    stop_one manager "$MANAGER_PID"
    stop_one agent "$AGENT_PID"
    ;;
  status)
    show_status
    ;;
esac
