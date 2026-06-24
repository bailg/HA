CC = clang-7
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O2
LDFLAGS =
TARGETS = bin/arbiter bin/bus_primary bin/bus_secondary bin/device

INCLUDES = -Iinclude

COMMON_SRC = src/common/protocol.c src/common/config.c src/common/network.c
ARBITER_SRC = $(COMMON_SRC) src/arbiter/arbiter_main.c src/arbiter/arbiter_logic.c
BUS_SRC = $(COMMON_SRC) src/bus/bus_main.c src/bus/bus_state_machine.c
DEVICE_SRC = $(COMMON_SRC) src/device/device_main.c src/device/device_logic.c

all: $(TARGETS)

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

clean:
	@rm -rf bin

.PHONY: all clean