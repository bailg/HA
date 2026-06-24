#!/bin/bash

# HA 系统全链路集成测试脚本 (涵盖控制面与数据面)
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

LOG_DIR="./test_logs"
BIN_DIR="./bin"
CONF_DIR="./configs"

rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

PID_ARBITER=
PID_BUS_PRI=
PID_BUS_SEC=
PID_DEV1=
PID_DEV2=
PID_OLD_PRI=
PID_ARBITER_NEW=

cleanup() {
    echo -e "\n${YELLOW}[INFO] 清理测试环境与进程...${NC}" >&2
    for pid in $PID_ARBITER $PID_BUS_PRI $PID_BUS_SEC $PID_DEV1 $PID_DEV2 $PID_OLD_PRI $PID_ARBITER_NEW; do
        if [ -n "$pid" ] && [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$LOG_DIR"
}
trap cleanup EXIT

start_process() {
    local name=$1
    local bin=$2
    local conf=$3
    local log_file="$LOG_DIR/${name}.log"
    
    echo -e "${YELLOW}[启动] $name ...${NC}" >&2
    "$BIN_DIR/$bin" "$CONF_DIR/$conf" > "$log_file" 2>&1 &
    local pid=$!
    sleep 0.5
    
    if ! kill -0 "$pid" 2>/dev/null; then
        echo -e "${RED}[错误] $name 启动失败！日志如下:${NC}" >&2
        cat "$log_file" >&2
        exit 1
    fi
    echo "$pid" 
}

stop_process() {
    local name=$1
    local pid=$2
    echo -e "${YELLOW}[停止] $name (PID: $pid)${NC}" >&2
    if ! [[ "$pid" =~ ^[0-9]+$ ]]; then
        echo -e "${RED}[致命错误] PID '$pid' 不是纯数字！${NC}" >&2
        exit 1
    fi
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

wait_for_log() {
    local log_file=$1
    local pattern=$2
    local timeout=$3
    local elapsed=0
    while [ $elapsed -lt $timeout ]; do
        if grep -q "$pattern" "$log_file" 2>/dev/null; then
            return 0
        fi
        sleep 0.5
        elapsed=$((elapsed + 1))
    done
    return 1
}

assert_test() {
    local test_name=$1
    local result=$2
    if [ $result -eq 0 ]; then
        echo -e "  ${GREEN}[PASS]${NC} $test_name"
    else
        echo -e "  ${RED}[FAIL]${NC} $test_name"
        TESTS_FAILED=1
    fi
}

# ==========================================
TESTS_FAILED=0
echo "========================================"
echo "    HA 全链路集成测试 (v2.0 强一致版)"
echo "========================================"

# ------------------------------------------
# 阶段零：启动基础环境
# ------------------------------------------
echo -e "\n${YELLOW}>>> 阶段零：启动基础环境${NC}"
PID_ARBITER=$(start_process "arbiter" "arbiter" "arbiter.conf")
PID_BUS_PRI=$(start_process "bus_primary" "bus_primary" "bus.conf")
PID_BUS_SEC=$(start_process "bus_secondary" "bus_secondary" "bus_secondary.conf")

# ------------------------------------------
# 阶段一：设备注册与建立长连接监控
# ------------------------------------------
echo -e "\n${YELLOW}>>> 阶段一：设备注册与先登先主 (保持长连接等待指令)${NC}"
PID_DEV1=$(start_process "device_1" "device" "device.conf")
PID_DEV2=$(start_process "device_2" "device" "device2.conf")

assert_test "设备1分配为主节点 (role 0)" $(wait_for_log "$LOG_DIR/device_1.log" "Device assigned role 0" 5 && echo 0 || echo 1)
assert_test "设备2分配为备节点 (role 1)" $(wait_for_log "$LOG_DIR/device_2.log" "Device assigned role 1" 5 && echo 0 || echo 1)

# ------------------------------------------
# 阶段二：全链路故障转移 (方向 B + 方向 C)
# ------------------------------------------
echo -e "\n${YELLOW}>>> 阶段二：总线主节点故障与全链路指令下发测试${NC}"

stop_process "bus_primary" "$PID_BUS_PRI"
echo "  [等待] 等待 Arbiter 检测超时并下发指令 (预计5秒)..."
sleep 6

# 1. 验证 Arbiter 真正发送了报文
assert_test "[方向B] Arbiter 发送 FAILOVER_COMMAND 报文" $(wait_for_log "$LOG_DIR/arbiter.log" "Sending FAILOVER_COMMAND" 3 && echo 0 || echo 1)

# 2. 验证备总线收到指令并执行了状态机翻转
assert_test "[方向B] 备总线应用 FAILOVER_COMMAND 翻转状态" $(wait_for_log "$LOG_DIR/bus_secondary.log" "Applying FAILOVER_COMMAND" 3 && echo 0 || echo 1)

# 3. 验证新主总线向备设备下发了 ROLE_CHANGE
assert_test "[方向C] 新主总线下发 ROLE_CHANGE 指令" $(wait_for_log "$LOG_DIR/bus_secondary.log" "Promoted device device_002 to PRIMARY" 3 && echo 0 || echo 1)

# 4. 验证备设备终端真实收到了指令并改变状态
assert_test "[方向C] 备设备接收指令成功升级为主设备 (role 0)" $(wait_for_log "$LOG_DIR/device_2.log" "Device role changed to 0" 3 && echo 0 || echo 1)

# 5. 验证防脑裂机制依然生效
echo "  [注入故障] 尝试启动携带旧 Epoch 的老主节点..."
PID_OLD_PRI=$(start_process "old_primary" "bus_primary" "bus.conf")
assert_test "Arbiter 成功拒绝旧主节点复活" $(wait_for_log "$LOG_DIR/arbiter.log" "Reject login from older epoch node bus_primary" 5 && echo 0 || echo 1)
stop_process "old_primary" "$PID_OLD_PRI"

# ------------------------------------------
# 阶段三：仲裁重启与总线自愈
# ------------------------------------------
echo -e "\n${YELLOW}>>> 阶段三：Arbiter 重启与总线自动重连测试${NC}"

stop_process "arbiter" "$PID_ARBITER"
echo "  [等待] Arbiter 已关闭，总线与设备应保持运行 (等待6秒)..."
sleep 6

if kill -0 "$PID_BUS_SEC" 2>/dev/null; then
    assert_test "Arbiter 宕机后，总线节点保持存活" 0
else
    assert_test "Arbiter 宕机后，总线节点保持存活" 1
fi

if kill -0 "$PID_DEV2" 2>/dev/null; then
    assert_test "Arbiter 宕机后，设备节点保持存活" 0
else
    assert_test "Arbiter 宕机后，设备节点保持存活" 1
fi

echo "  [恢复] 重新启动 Arbiter..."
PID_ARBITER_NEW=$(start_process "arbiter_new" "arbiter" "arbiter.conf")

echo "  [等待] 等待总线自动重连 (预计3-5秒)..."
sleep 5
assert_test "总线自动重连 Arbiter 成功" $(wait_for_log "$LOG_DIR/bus_secondary.log" "Re-login to arbiter successful" 5 && echo 0 || echo 1)

# ==========================================
echo -e "\n========================================"
if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "       ${GREEN}所有全链路测试用例通过！${NC}"
    echo -e "       ${GREEN}(强一致同步、指令下发、动态选主 验证完毕)${NC}"
else
    echo -e "       ${RED}存在测试用例未通过！${NC}"
fi
echo "========================================"

exit $TESTS_FAILED