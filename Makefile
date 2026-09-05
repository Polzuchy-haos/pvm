BUILD_DIR := build

.PHONY: all build run clean

all: build

build:
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR)

run: build
	$(BUILD_DIR)/simulator/pvmsim

clean:
	cmake -E rm -rf $(BUILD_DIR)
