#!/bin/bash
#
# test_shm_queue.sh — SHM Queue 单元测试 + bus_logger 集成测试
#
# Usage: bash test_shm_queue.sh

set -eo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
BIN_DIR="./bin"
LOG_DIR="./test_logs_shm_$(date +%Y%m%d_%H%M%S)"

total_tests=0; passed_tests=0; failed_tests=0

PID_BUS=""; PID_DEV=""; PID_LOGGER=()

cleanup() {
    shm_unlink /ha_device_queue 2>/dev/null || true
    for pid in "$PID_BUS" "$PID_DEV" "${PID_LOGGER[@]}"; do
        [[ -n "$pid" && "$pid" =~ ^[0-9]+$ ]] && kill -9 "$pid" 2>/dev/null || true
    done
    rm -f /tmp/test_audit.log /tmp/test_audit2.log /tmp/test_enqueue /tmp/test_enqueue.c
}
trap cleanup EXIT

start_proc() {
    local name=$1; shift
    local lf="$LOG_DIR/${name}.log"
    echo -e "  ${YELLOW}[START]${NC} ${name}" >&2
    "$@" > "$lf" 2>&1 &
    local pid=$!
    sleep 0.8
    if ! kill -0 "$pid" 2>/dev/null; then
        echo -e "  ${RED}[FATAL] ${name} 启动失败${NC}" >&2; head -5 "$lf" >&2; exit 1
    fi
    echo "$pid"
}

kill_proc() {
    local name=$1 pid=$2
    [[ -z "$pid" || ! "$pid" =~ ^[0-9]+$ ]] && return
    echo -e "  ${YELLOW}[KILL]${NC} ${name}" >&2
    kill -9 "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
}

assert() {
    local sc=$1 desc=$2; local res=$3
    total_tests=$((total_tests+1))
    if [[ $res -eq 0 ]]; then
        echo -e "    ${GREEN}[PASS]${NC} [${sc}] ${desc}"
        passed_tests=$((passed_tests+1))
    else
        echo -e "    ${RED}[FAIL]${NC} [${sc}] ${desc}"
        failed_tests=$((failed_tests+1))
    fi
}

section() { echo ""; echo "===================================================================="; echo "  ${1}: ${2}"; echo "===================================================================="; }

mkdir -p "$LOG_DIR"
echo "===================================================================="
echo "  SHM Queue + bus_logger 测试"
echo "  日志目录: ${LOG_DIR}"
echo "===================================================================="

# ---- Build test binaries ----
echo ""
echo "Building test binaries..."
clang-7 -std=c11 -Wall -Wextra -Werror -g -O2 -Iinclude \
    test/test_shm_queue.c src/common/shm_queue.c -o bin/test_shm_queue -lrt 2>&1

clang-7 -std=c11 -Wall -Wextra -Werror -g -O2 -Iinclude \
    test/test_shm_helper.c src/common/shm_queue.c -o bin/test_shm_helper -lrt 2>&1

# =============================================
# U: SHM Queue 单元测试
# =============================================
section "U" "SHM Queue 单元测试"

U_OUTPUT=$(bin/test_shm_queue 2>&1)
echo "$U_OUTPUT"

U_PASS=$(echo "$U_OUTPUT" | grep -c '\[PASS\]') || true
U_FAIL=$(echo "$U_OUTPUT" | grep -c '\[FAIL\]') || true

assert "U" "单元测试全部通过" $( [[ $U_FAIL -eq 0 ]] && echo 0 || echo 1 )

# =============================================
# L1: bus_logger 基本启动/停止
# =============================================
section "L1" "bus_logger 基本启动/停止"

PID_BUS=$(start_proc "bus_primary" "$BIN_DIR/bus_primary" "configs/bus.conf")
assert "L1" "总线启动" $(kill -0 "$PID_BUS" 2>/dev/null && echo 0 || echo 1)

PID_LOGGER+=("$(start_proc "bus_logger" "$BIN_DIR/bus_logger" "/tmp/test_audit.log")")
assert "L1" "logger 启动" $(kill -0 "${PID_LOGGER[0]}" 2>/dev/null && echo 0 || echo 1)
sleep 1

kill_proc "bus_logger" "${PID_LOGGER[0]}"; PID_LOGGER=()
assert "L1" "日志文件已创建" $( [[ -f /tmp/test_audit.log ]] && echo 0 || echo 1 )

# =============================================
# L2: bus_logger 将 SHM 条目写入文件
# =============================================
section "L2" "bus_logger 持续落盘验证"

PID_LOGGER+=("$(start_proc "bus_logger" "$BIN_DIR/bus_logger" "/tmp/test_audit.log")")
sleep 1

bin/test_shm_helper 50 2>&1
sleep 2

LINES=$(wc -l < /tmp/test_audit.log)
assert "L2" "日志行数 >= 50" $( [[ $LINES -ge 50 ]] && echo 0 || echo 1 )

LINE_BYTES=$(head -1 /tmp/test_audit.log | wc -c)
assert "L2" "每行 1024+1 字节" $( [[ $LINE_BYTES -eq 1025 ]] && echo 0 || echo 1 )

kill_proc "bus_logger" "${PID_LOGGER[0]}"; PID_LOGGER=()

# =============================================
# L3: 多个 logger 并行消费 (SPMC)
# =============================================
section "L3" "多个 logger 并行消费 (SPMC)"

kill_proc "bus_primary" "$PID_BUS"; PID_BUS=""
sleep 1
PID_BUS=$(start_proc "bus_primary" "$BIN_DIR/bus_primary" "configs/bus.conf")
sleep 1

PID_LOGGER+=("$(start_proc "bus_logger_1" "$BIN_DIR/bus_logger" "/tmp/test_audit.log")")
PID_LOGGER+=("$(start_proc "bus_logger_2" "$BIN_DIR/bus_logger" "/tmp/test_audit2.log")")
sleep 1

bin/test_shm_helper 30 2>&1
sleep 2

kill_proc "bus_logger_1" "${PID_LOGGER[0]}"
kill_proc "bus_logger_2" "${PID_LOGGER[1]}"
PID_LOGGER=()

L1=$(wc -l < /tmp/test_audit.log)
L2=$(wc -l < /tmp/test_audit2.log)
assert "L3" "logger1 收到条目" $( [[ $L1 -ge 30 ]] && echo 0 || echo 1 )
assert "L3" "logger2 收到条目" $( [[ $L2 -ge 30 ]] && echo 0 || echo 1 )

# =============================================
# L4: 总线重启后 SHM 清空, logger 继续追加
# =============================================
section "L4" "总线重启后 SHM 清空"

L1_BEFORE=$L1

PID_LOGGER+=("$(start_proc "bus_logger" "$BIN_DIR/bus_logger" "/tmp/test_audit.log")")
sleep 1

kill_proc "bus_primary" "$PID_BUS"; sleep 1
PID_BUS=$(start_proc "bus_primary" "$BIN_DIR/bus_primary" "configs/bus.conf")
sleep 2

bin/test_shm_helper 40 2>&1
sleep 2

kill_proc "bus_logger" "${PID_LOGGER[0]}"; PID_LOGGER=()

L1_AFTER=$(wc -l < /tmp/test_audit.log)
assert "L4" "总线重启后 logger 继续追加" $( [[ $L1_AFTER -gt $L1_BEFORE ]] && echo 0 || echo 1 )

# =============================================
# L5: 文件格式精确校验 (每行 1024B + '\n')
# =============================================
section "L5" "文件格式精确校验"

for f in /tmp/test_audit.log /tmp/test_audit2.log; do
    [[ -f "$f" ]] || continue
    tot=$(wc -c < "$f")
    cnt=$(wc -l < "$f")
    expected=$(( cnt * 1025 ))
    fname=$(basename "$f")
    assert "L5" "${fname}: ${cnt} lines × 1025 = ${expected}" \
        $( [[ $tot -eq $expected ]] && echo 0 || echo 1 )
done

# =============================================
# 结果汇总
# =============================================
echo ""
echo "===================================================================="
echo "  测试汇总"
echo "===================================================================="
echo "  总用例: ${total_tests}"
echo -e "  通过:   ${GREEN}${passed_tests}${NC}"
echo -e "  失败:   ${RED}${failed_tests}${NC}"
echo "  日志:   ${LOG_DIR}"

if [[ $failed_tests -eq 0 ]]; then
    echo -e "\n${GREEN}============================================"
    echo -e "${GREEN}  结论: 全部测试通过 ✓${NC}"
    echo -e "${GREEN}============================================${NC}"
    exit 0
else
    echo -e "\n${RED}============================================"
    echo -e "${RED}  结论: ${failed_tests}/${total_tests} 个用例失败 ✗${NC}"
    echo -e "${RED}============================================${NC}"
    exit 1
fi
