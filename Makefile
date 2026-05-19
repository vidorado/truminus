# TruMinus build wrapper around idf.py.
#
# Prerequisites (once per machine):
#   git clone --branch release/v6.0 https://github.com/espressif/esp-idf.git ~/esp/esp-idf
#   cd ~/esp/esp-idf && ./install.sh esp32p4
#
# Prerequisites (once per terminal session):
#   . ~/esp/esp-idf/export.sh
#
# Usage:
#   make               # build
#   make flash         # build + flash (PORT defaults to /dev/ttyACM0)
#   make monitor       # open serial monitor
#   make flash-monitor # flash then open monitor
#   make clean         # idf.py fullclean
#   PORT=/dev/ttyUSB0 make flash

PORT ?= /dev/ttyACM0

# --skip-flashed is baked into FLASH_SUB_ARGS in the top-level CMakeLists.txt
# so every esptool write-flash invocation gets it — not just `make flash` but
# also `idf.py flash` directly.

build:
	idf.py build

flash: build
	idf.py -p $(PORT) flash

monitor:
	idf.py -p $(PORT) monitor

flash-monitor: build
	idf.py -p $(PORT) flash monitor

clean:
	idf.py fullclean

# Build the ESP32-C6 co-processor (slave) firmware with BLE enabled and copy
# the resulting binary to main/slave_fw/ so it gets embedded in the next
# TruMinus build.  Run this once (or whenever ESP-Hosted is upgraded), then
# run 'make' to rebuild TruMinus with the new C6 firmware embedded.
SLAVE_DIR := managed_components/espressif__esp_hosted/slave
C6_FW_OUT := main/slave_fw/network_adapter.bin

build-c6:
	@echo "==> Building ESP-Hosted slave firmware for ESP32-C6 (WiFi + BLE)..."
	cd $(SLAVE_DIR) && idf.py set-target esp32c6 && idf.py build
	mkdir -p main/slave_fw
	cp $(SLAVE_DIR)/build/network_adapter.bin $(C6_FW_OUT)
	@echo "==> C6 firmware copied to $(C6_FW_OUT)"
	@echo "==> Now run 'make' to rebuild TruMinus with the embedded C6 firmware."

.PHONY: build flash flash-monitor monitor clean build-c6
