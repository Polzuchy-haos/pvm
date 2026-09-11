BUILD_DIR := build

.PHONY: all build run test clean

all: build

build:
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR)

run: build
	$(BUILD_DIR)/simulator/pvmsim

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	cmake -E rm -rf $(BUILD_DIR)
