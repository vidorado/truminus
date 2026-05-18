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

.PHONY: build flash flash-monitor monitor clean
