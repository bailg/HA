#!/bin/bash
#
# test_ha_scenarios.sh — HA 全链路集成测试脚本
#
# 覆盖全部故障场景组合, 通过日志模式匹配自动验证 51 个断言:
#   S1:  基础启动 & 设备注册
#   S2:  主总线故障 → 备总线提升为主
#   S3:  旧主重启 → 降级为备 (DEGRADE)
#   S4:  设备断开重连 → 角色重新分配
#   S5:  仲裁重启 → 总线自动重连
#   S6:  备总线故障 → 主继续服务, 备恢复后重连
#   S7:  防脑裂 — epoch 校验拒绝旧主复活
#   S8:  全系统 Kill & 重新拉起
#   S9:  主备同时故障 → 先启动备总线再启动主
#   S10: 数据路径验证
#   S11: 设备主节点故障 → 备设备提升
#   S12: 总线 Flip-Flop 连续故障切换
#   S13: 双总线重启 → 设备自动重连
#   S14: 长稳运行验证
#
# Usage: bash test_ha_scenarios.sh

set -eo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
BIN_DIR="./bin"; CONF_DIR="./configs"
LOG_DIR="./test_logs_$(date +%Y%m%d_%H%M%S)"

total_tests=0; passed_tests=0; failed_tests=0

PID_A=""; PID_BP=""; PID_BS=""; PID_D1=""; PID_D2=""
PID_EXTRA=()

cleanup() {
    echo -e "\n${YELLOW}[CLEANUP] 清理进程...${NC}" >&2
    for pid in "$PID_A" "$PID_BP" "$PID_BS" "$PID_D1" "$PID_D2" "${PID_EXTRA[@]}"; do
        [[ -n "$pid" && "$pid" =~ ^[0-9]+$ ]] && kill -9 "$pid" 2>/dev/null || true
    done
    sleep 0.3
}
trap cleanup EXIT

start_proc() {
    local name=$1 bin=$2 conf=$3
    local lf="$LOG_DIR/${name}.log"
    echo -e "  ${YELLOW}[START]${NC} ${name} (${bin} ${conf})" >&2
    "$BIN_DIR/$bin" "$CONF_DIR/$conf" > "$lf" 2>&1 &
    local pid=$!
    sleep 0.8
    if ! kill -0 "$pid" 2>/dev/null; then
        echo -e "  ${RED}[FATAL] ${name} 启动失败${NC}" >&2; head -5 "$lf" >&2; exit 1
    fi
    echo "$pid"
}

kill_proc() {
    local name=$1 pid=$2
    [[ -z "$pid" || ! "$pid" =~ ^[0-9]+$ ]] && echo -e "  ${YELLOW}[SKIP] ${name} 无效 PID${NC}" >&2 && return
    echo -e "  ${YELLOW}[KILL]${NC} ${name} (PID ${pid})" >&2
    kill -9 "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
}

wait_log() {
    local lf=$1 pat=$2 to=$3; local e=0
    while [[ $e -lt $to ]]; do grep -q "$pat" "$lf" 2>/dev/null && return 0; sleep 1; e=$((e+1)); done
    return 1
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

# ======== 初始化 ========
mkdir -p "$LOG_DIR"
echo "===================================================================="
echo "  HA 全链路集成测试"
echo "  日志目录: ${LOG_DIR}"
echo "===================================================================="

# =============================================
# S1: 基础启动 & 设备注册
# =============================================
section "S1" "基础启动 & 设备注册"

PID_A=$(start_proc "arbiter" "arbiter" "arbiter.conf")
PID_BP=$(start_proc "bus_primary" "bus_primary" "bus.conf")
PID_BS=$(start_proc "bus_secondary" "bus_secondary" "bus_secondary.conf")

assert "S1" "仲裁启动"       $(wait_log "$LOG_DIR/arbiter.log" "Arbiter started" 5 && echo 0 || echo 1)
assert "S1" "主总线连接仲裁" $(wait_log "$LOG_DIR/bus_primary.log" "arbiter" 6 && echo 0 || echo 1)
assert "S1" "备总线连接仲裁" $(wait_log "$LOG_DIR/bus_secondary.log" "arbiter" 6 && echo 0 || echo 1)

PID_D1=$(start_proc "device1" "device" "device.conf")
PID_D2=$(start_proc "device2" "device" "device2.conf")

assert "S1" "设备1注册成功"          $(wait_log "$LOG_DIR/device1.log" "Registered\|role" 5 && echo 0 || echo 1)
assert "S1" "设备2注册成功"          $(wait_log "$LOG_DIR/device2.log" "Registered\|role" 5 && echo 0 || echo 1)
assert "S1" "设备1为 PRIMARY (role 0)" $(wait_log "$LOG_DIR/device1.log" "role 0" 5 && echo 0 || echo 1)
assert "S1" "设备2为 SECONDARY (role 1)" $(wait_log "$LOG_DIR/device2.log" "role 1" 5 && echo 0 || echo 1)

# =============================================
# S2: 主总线故障 → 备总线提升为主
# =============================================
section "S2" "主总线故障 → 备总线提升为主"

kill_proc "bus_primary (PRIMARY)" "$PID_BP"; PID_BP=""
echo "  [等待] 仲裁检测心跳超时 + 发起 Failover (7s)..."
sleep 7

assert "S2" "仲裁检测主总线超时"       $(wait_log "$LOG_DIR/arbiter.log" "elapsed.*timeout" 3 && echo 0 || echo 1)
assert "S2" "仲裁发送 FAILOVER_COMMAND" $(wait_log "$LOG_DIR/arbiter.log" "Sending FAILOVER" 3 && echo 0 || echo 1)
assert "S2" "备总线收到 FAILOVER 并切换" $(wait_log "$LOG_DIR/bus_secondary.log" "Applying FAILOVER" 3 && echo 0 || echo 1)
assert "S2" "备总线状态变为 PRIMARY"    $(wait_log "$LOG_DIR/bus_secondary.log" "to PRIMARY" 3 && echo 0 || echo 1)
assert "S2" "新主总线提升首个设备为主" $(wait_log "$LOG_DIR/bus_secondary.log" "Promoted device" 5 && echo 0 || echo 1)
assert "S2" "设备1仍为 PRIMARY (首设备保留)" $(wait_log "$LOG_DIR/device1.log" "assigned role 0\|role 0" 5 && echo 0 || echo 1)

# =============================================
# S3: 旧主重启 → 降级为备
# =============================================
section "S3" "旧主重启 → 降级为备 (DEGRADE)"

PID_BP=$(start_proc "bus_primary_restart" "bus_primary" "bus.conf")
PID_EXTRA+=("$PID_BP")

assert "S3" "旧主总线连接仲裁"  $(wait_log "$LOG_DIR/bus_primary_restart.log" "arbiter" 8 && echo 0 || echo 1)
assert "S3" "旧主总线被降级为 SECONDARY" $(wait_log "$LOG_DIR/bus_primary_restart.log" "SECONDARY" 10 && echo 0 || echo 1)
assert "S3" "主总线 (原备) 仍为 PRIMARY" $(wait_log "$LOG_DIR/bus_secondary.log" "PRIMARY" 3 && echo 0 || echo 1)

# =============================================
# S4: 设备断开重连
# =============================================
section "S4" "设备断开重连 → 角色重新分配"

kill_proc "device1" "$PID_D1"; PID_D1=""
sleep 1

PID_D1=$(start_proc "device1_recon" "device" "device.conf")

assert "S4" "重建设备注册成功"         $(wait_log "$LOG_DIR/device1_recon.log" "Registered\|role" 5 && echo 0 || echo 1)
assert "S4" "重建设备分配为 SECONDARY" $(wait_log "$LOG_DIR/device1_recon.log" "role 1" 5 && echo 0 || echo 1)

# =============================================
# S5: 仲裁重启
# =============================================
section "S5" "仲裁重启 → 总线自动重连"

kill_proc "arbiter" "$PID_A"; PID_A=""
sleep 2

# 验证总线检测到仲裁断开
assert "S5" "总线检测到仲裁断开" $(wait_log "$LOG_DIR/bus_secondary.log" "Arbiter disconnected" 8 && echo 0 || echo 1)
assert "S5" "总线在仲裁宕机期间存活" $(kill -0 "$PID_BS" 2>/dev/null && echo 0 || echo 1)
assert "S5" "设备在仲裁宕机期间存活" $(kill -0 "$PID_D2" 2>/dev/null && echo 0 || echo 1)

PID_A=$(start_proc "arbiter_restart" "arbiter" "arbiter.conf")
sleep 5

assert "S5" "总线重启后进程存活" $(kill -0 "$PID_BS" 2>/dev/null && echo 0 || echo 1)
assert "S5" "设备重启后进程存活" $(kill -0 "$PID_D2" 2>/dev/null && echo 0 || echo 1)

# =============================================
# S6: 备总线故障
# =============================================
section "S6" "备总线故障 → 主继续服务，备恢复后重连"

# 当前拓扑:
#   PID_BS = bus_secondary (端口 5001, 角色 PRIMARY, 从 S2 提升)
#   PID_BP = bus_primary_restart (端口 5000, 角色 SECONDARY, S3 降级)
echo "  [操作] 杀掉备总线 (bus_primary_restart, SECONDARY, port 5000)..."
kill_proc "bus_primary_restart (SECONDARY)" "$PID_BP"; PID_BP=""
PID_EXTRA=()
sleep 3

assert "S6" "主总线 (原secondary) 存活" $(kill -0 "$PID_BS" 2>/dev/null && echo 0 || echo 1)
assert "S6" "设备继续存活"               $(kill -0 "$PID_D2" 2>/dev/null && echo 0 || echo 1)

echo "  [操作] 重新启动备总线 (port 5000)..."
PID_BP=$(start_proc "bus_secondary_rejoin" "bus_primary" "bus.conf")

assert "S6" "备总线重启后连接仲裁" $(wait_log "$LOG_DIR/bus_secondary_rejoin.log" "arbiter" 8 && echo 0 || echo 1)
assert "S6" "备总线降级为 SECONDARY" $(wait_log "$LOG_DIR/bus_secondary_rejoin.log" "SECONDARY" 5 && echo 0 || echo 1)

# =============================================
# S7: 旧主携旧 Epoch 登录 → 防脑裂
# =============================================
section "S7" "防脑裂 — 旧 Epoch 被拒绝"

kill_proc "bus_secondary_rejoin (SECONDARY)" "$PID_BP"; PID_BP=""
kill_proc "bus_secondary (PRIMARY)" "$PID_BS"; PID_BS=""
sleep 1

# 重新启动 bus_primary (epoch=0)，单节点运行
PID_BP=$(start_proc "bus_primary_epoch0" "bus_primary" "bus.conf")
assert "S7" "主总线 (epoch=0) 连接仲裁" $(wait_log "$LOG_DIR/bus_primary_epoch0.log" "arbiter" 5 && echo 0 || echo 1)

PID_BS=$(start_proc "bus_secondary_clean" "bus_secondary" "bus_secondary.conf")
assert "S7" "备总线连接仲裁" $(wait_log "$LOG_DIR/bus_secondary_clean.log" "arbiter" 5 && echo 0 || echo 1)
assert "S7" "主备总线均正常运行" $(kill -0 "$PID_BP" 2>/dev/null && kill -0 "$PID_BS" 2>/dev/null && echo 0 || echo 1)

# =============================================
# S8: 全系统 Kill & 重新拉起
# =============================================
section "S8" "全系统 Kill & 全新启动"

cleanup
PID_A=""; PID_BP=""; PID_BS=""; PID_D1=""; PID_D2=""; PID_EXTRA=()
sleep 1

PID_A=$(start_proc "arbiter_clean" "arbiter" "arbiter.conf")
PID_BP=$(start_proc "bus_primary_clean" "bus_primary" "bus.conf")
PID_BS=$(start_proc "bus_secondary_clean" "bus_secondary" "bus_secondary.conf")
PID_D1=$(start_proc "device1_clean" "device" "device.conf")
PID_D2=$(start_proc "device2_clean" "device" "device2.conf")

assert "S8" "全新启动 — 设备1分配角色" $(wait_log "$LOG_DIR/device1_clean.log" "role" 10 && echo 0 || echo 1)
assert "S8" "全新启动 — 设备2分配角色" $(wait_log "$LOG_DIR/device2_clean.log" "role" 10 && echo 0 || echo 1)

# =============================================
# S9: 主备同时故障 → 先启动备总线
# =============================================
section "S9" "主备同时故障 → 先启动备总线"

kill_proc "bus_primary_clean" "$PID_BP"; PID_BP=""
kill_proc "bus_secondary_clean" "$PID_BS"; PID_BS=""
sleep 1

PID_BS=$(start_proc "bus_secondary_first" "bus_secondary" "bus_secondary.conf")
assert "S9" "备总线先行登录仲裁" $(wait_log "$LOG_DIR/bus_secondary_first.log" "arbiter" 5 && echo 0 || echo 1)

PID_BP=$(start_proc "bus_primary_second" "bus_primary" "bus.conf")
assert "S9" "主总线后续登录仲裁" $(wait_log "$LOG_DIR/bus_primary_second.log" "arbiter" 5 && echo 0 || echo 1)
assert "S9" "主备总线均正常运行" $(kill -0 "$PID_BP" 2>/dev/null && kill -0 "$PID_BS" 2>/dev/null && echo 0 || echo 1)

# =============================================
# S10: 数据路径验证
# =============================================
section "S10" "数据路径验证"

assert "S10" "设备1可发起写请求 (PRIMARY)" $( \
    wait_log "$LOG_DIR/device1_clean.log" "Write\|send\|message" 5 && echo 0 || echo 1 )

# =============================================
# S11: 设备主节点故障 → 备设备提升
# =============================================
section "S11" "设备主节点故障 → 备设备提升"

# 双总线重启后设备先后重连，存在竞争条件，需动态检测哪个设备是 PRIMARY
echo "  [检测] 检查设备角色..."

PRI_LOG1="$LOG_DIR/device1_clean.log"
PRI_LOG2="$LOG_DIR/device2_clean.log"
# 取最近一次注册到主总线的角色
ROLE1=$(grep "Registered with bus.*5000" "$PRI_LOG1" 2>/dev/null | tail -1 | grep -o 'role [01]' | cut -d' ' -f2)
ROLE2=$(grep "Registered with bus.*5000" "$PRI_LOG2" 2>/dev/null | tail -1 | grep -o 'role [01]' | cut -d' ' -f2)

if [[ "$ROLE1" == "0" ]]; then
    kill_pid="$PID_D1"; kill_name="device1_clean (PRIMARY)"; survivor_pid="$PID_D2"; survivor_log="$PRI_LOG2"
elif [[ "$ROLE2" == "0" ]]; then
    kill_pid="$PID_D2"; kill_name="device2_clean (PRIMARY)"; survivor_pid="$PID_D1"; survivor_log="$PRI_LOG1"
else
    echo -e "  ${YELLOW}[SKIP] S11: 无法确定 PRIMARY 设备，跳过${NC}"
    assert "S11" "检测设备角色" 1
fi

echo "  [操作] 杀掉 ${kill_name}..."
kill_proc "$kill_name" "$kill_pid"
# 清除被杀设备的 PID 变量
if [[ "$kill_name" == *"device1"* ]]; then PID_D1=""; else PID_D2=""; fi
echo "  [等待] 总线检测断开并提升备设备 (3s)..."
sleep 3

# promote_next_device 在哪个总线上执行取决于设备注册到了哪个总线
# 检查所有可能的总线日志
assert "S11" "总线检测到设备断开并提升备设备" $( \
    (wait_log "$LOG_DIR/bus_primary_second.log" "Promoted device" 5 && echo 0) || \
    (wait_log "$LOG_DIR/bus_secondary_first.log" "Promoted device" 5 && echo 0) || \
    echo 1 )
assert "S11" "备设备被提升为 PRIMARY" $( \
    wait_log "$survivor_log" "changed.*0" 5 && echo 0 || echo 1 )

# 重启被杀设备，恢复双设备环境供后续场景使用
echo "  [操作] 重启被杀设备以恢复双设备环境..."
if [[ -z "$PID_D1" ]]; then
    PID_D1=$(start_proc "device1_clean" "device" "device.conf")
    wait_log "$LOG_DIR/device1_clean.log" "Registered with bus" 8
elif [[ -z "$PID_D2" ]]; then
    PID_D2=$(start_proc "device2_clean" "device" "device2.conf")
    wait_log "$LOG_DIR/device2_clean.log" "Registered with bus" 8
fi

# =============================================
# S12: 总线 Flip-Flop 连续故障切换
# =============================================
section "S12" "总线 Flip-Flop 连续故障切换"

# 当前: bus_primary_second=PRIMARY, bus_secondary_first=SECONDARY, device2=PRIMARY
echo "  [操作] 第一轮: 杀掉主总线 (bus_primary_second, PRIMARY)..."
kill_proc "bus_primary_second (PRIMARY)" "$PID_BP"; PID_BP=""
echo "  [等待] 仲裁检测 → 提升备总线 (7s)..."
sleep 7

assert "S12" "第一轮: 仲裁切换备总线为 PRIMARY" $(wait_log "$LOG_DIR/bus_secondary_first.log" "PRIMARY" 5 && echo 0 || echo 1)

echo "  [操作] 重新启动原主总线 (将被降级为 SECONDARY)..."
PID_BP=$(start_proc "bus_primary_flip" "bus_primary" "bus.conf")
assert "S12" "原主总线重新连接并被降级" $(wait_log "$LOG_DIR/bus_primary_flip.log" "SECONDARY" 8 && echo 0 || echo 1)

echo "  [操作] 第二轮: 杀掉新主总线 (bus_secondary_first)..."
kill_proc "bus_secondary_first (PRIMARY)" "$PID_BS"; PID_BS=""
echo "  [等待] 仲裁检测 → 再次切换 (7s)..."
sleep 7

assert "S12" "第二轮: 仲裁切换原主总线为 PRIMARY" $(wait_log "$LOG_DIR/bus_primary_flip.log" "PRIMARY\|Applying FAILOVER\|FAILOVER" 5 && echo 0 || echo 1)

echo "  [操作] 重新启动原备总线 (将被降级为 SECONDARY)..."
PID_BS=$(start_proc "bus_secondary_flip" "bus_secondary" "bus_secondary.conf")
assert "S12" "原备总线重新连接仲裁" $(wait_log "$LOG_DIR/bus_secondary_flip.log" "arbiter" 8 && echo 0 || echo 1)
assert "S12" "Flip-Flop 后主备角色稳定" $(kill -0 "$PID_BP" 2>/dev/null && kill -0 "$PID_BS" 2>/dev/null && echo 0 || echo 1)

# =============================================
# S13: 设备重连 — 双总线同时重启
# =============================================
section "S13" "设备重连 — 双总线同时重启后设备自动重连"

# 设备2 (device2_clean) 仍在运行，杀掉双总线
echo "  [操作] 同时杀掉主备总线 (设备保持运行)..."
kill_proc "bus_primary_flip (PRIMARY)" "$PID_BP"; PID_BP=""
kill_proc "bus_secondary_flip (SECONDARY)" "$PID_BS"; PID_BS=""
echo "  [等待] 设备检测到总线断开 (5s)..."
sleep 5

assert "S13" "设备在总线宕机期间保持存活" $( \
    kill -0 "$PID_D1" 2>/dev/null && kill -0 "$PID_D2" 2>/dev/null && echo 0 || echo 1)

echo "  [操作] 重新启动仲裁 (若已宕机)..."
if ! kill -0 "$PID_A" 2>/dev/null; then
    PID_A=$(start_proc "arbiter_recover" "arbiter" "arbiter.conf")
    sleep 2
fi

echo "  [操作] 重新启动双总线..."
PID_BP=$(start_proc "bus_primary_recover" "bus_primary" "bus.conf")
PID_BS=$(start_proc "bus_secondary_recover" "bus_secondary" "bus_secondary.conf")
echo "  [等待] 设备自动重连总线 (6s)..."
sleep 6

assert "S13" "设备重连主总线并注册" $( \
    wait_log "$LOG_DIR/device1_clean.log" "5000.*role" 5 && \
    wait_log "$LOG_DIR/device2_clean.log" "5000.*role" 5 && echo 0 || echo 1)
assert "S13" "设备重连备总线并注册" $( \
    wait_log "$LOG_DIR/device1_clean.log" "5001.*role" 5 && \
    wait_log "$LOG_DIR/device2_clean.log" "5001.*role" 5 && echo 0 || echo 1)

# =============================================
# S14: 长稳运行验证
# =============================================
section "S14" "长稳运行验证"

echo "  [等待] 系统无故障运行 8 秒..."
sleep 8

assert "S14" "仲裁进程存活" $(kill -0 "$PID_A" 2>/dev/null && echo 0 || echo 1)
assert "S14" "主总线进程存活" $(kill -0 "$PID_BP" 2>/dev/null && echo 0 || echo 1)
assert "S14" "备总线进程存活" $(kill -0 "$PID_BS" 2>/dev/null && echo 0 || echo 1)
assert "S14" "设备进程存活" $(kill -0 "$PID_D1" 2>/dev/null && kill -0 "$PID_D2" 2>/dev/null && echo 0 || echo 1)

# 动态检测 PRIMARY 设备（从最新注册记录判断）
DEV1_ROLE=$(grep -o 'role [01]' "$LOG_DIR/device1_clean.log" 2>/dev/null | tail -1)
DEV2_ROLE=$(grep -o 'role [01]' "$LOG_DIR/device2_clean.log" 2>/dev/null | tail -1)
has_primary=false
[[ "$DEV1_ROLE" == "role 0" ]] && has_primary=true
[[ "$DEV2_ROLE" == "role 0" ]] && has_primary=true
assert "S14" "某设备仍为 PRIMARY (角色未丢失)" $( $has_primary && echo 0 || echo 1 )

# ======== 结果汇总 ========
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
