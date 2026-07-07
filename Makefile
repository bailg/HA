# HA Project Makefile — builds arbiter, bus_primary, bus_secondary, device
CC = clang-7
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O2
LDFLAGS = -lrt
TARGETS = bin/arbiter bin/bus_primary bin/bus_secondary bin/device bin/bus_logger

INCLUDES = -Iinclude

# Source file groups
COMMON_SRC   = src/common/protocol.c src/common/config.c src/common/network.c src/common/memory.c src/common/shm_queue.c
ARBITER_SRC  = $(COMMON_SRC) src/arbiter/arbiter_main.c src/arbiter/arbiter_logic.c
BUS_SRC      = $(COMMON_SRC) src/bus/bus_main.c src/bus/bus_state_machine.c src/bus/bus_sync.c
DEVICE_SRC   = $(COMMON_SRC) src/device/device_main.c src/device/device_logic.c

all: $(TARGETS)

# bus_primary and bus_secondary share the same source but define different compile-time flags
bin/arbiter: $(ARBITER_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

bin/bus_primary: $(BUS_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) -DIS_BUS_PRIMARY $^ -o $@ $(LDFLAGS)

bin/bus_secondary: $(BUS_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) -DIS_BUS_SECONDARY $^ -o $@ $(LDFLAGS)

bin/device: $(DEVICE_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

LOGGER_SRC = src/common/shm_queue.c src/bus/bus_logger.c
bin/bus_logger: $(LOGGER_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

# Convenience run targets
run_arbiter: bin/arbiter
	@echo "Starting arbiter..."
	./bin/arbiter configs/arbiter.conf

run_bus_primary: bin/bus_primary
	@echo "Starting bus primary..."
	./bin/bus_primary configs/bus.conf

run_bus_secondary: bin/bus_secondary
	@echo "Starting bus secondary..."
	./bin/bus_secondary configs/bus_secondary.conf

run_device: bin/device
	@echo "Starting device..."
	./bin/device configs/device.conf

run_device2: bin/device
	@echo "Starting device 2..."
	./bin/device configs/device2.conf

test: all
	@bash test_ha_scenarios.sh

test_shm: all
	@bash test_shm_queue.sh

HA_UNIT_SRC = test/test_ha_unit.c src/bus/bus_state_machine.c src/arbiter/arbiter_logic.c \
              src/device/device_logic.c src/common/protocol.c src/common/config.c
bin/test_ha_unit: $(HA_UNIT_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

test_unit: bin/test_ha_unit
	./bin/test_ha_unit

clean:
	@rm -rf bin test_logs test_logs_shm_*

.PHONY: all clean test run_arbiter run_bus_primary run_bus_secondary run_device run_device2