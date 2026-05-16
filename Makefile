# lamp — one-button night-light controller
#
# All actions go through this Makefile. See ARCHITECTURE.md for design.

b  := out
DL := .cache

include ../graft/graft.mk

# ── Configuration ───────────────────────────────────────────────────────────
SERIAL_PORT  := /dev/ttyACM0
IDF_VERSION  := v5.4.4
IDF_TARGET   := esp32c6

# ── ESP-IDF (fetched via graft, pinned, cached) ─────────────────────────────
IDF_DIR     := $b/esp-idf
IDF_TGT     := $(IDF_DIR)/export.sh
IDF_TAR     := $(DL)/esp-idf-$(IDF_VERSION).tar.gz
IDF_TMP     := /tmp/esp-idf-$(IDF_VERSION)
IDF_COMMIT  := $(IDF_VERSION)
IDF_GIT_URL := https://github.com/espressif/esp-idf.git
$(eval $(call FETCH,IDF))

# Keep IDF's downloaded toolchains inside the project, not in ~/.espressif.
export IDF_TOOLS_PATH := $(abspath $b/idf-tools)

# Sentinel: marks that install.sh has fetched the toolchain for our target.
IDF_INSTALLED := $(IDF_DIR)/.tools-installed-$(IDF_TARGET)

$(IDF_INSTALLED): $(IDF_TGT)
	cd $(IDF_DIR) && ./install.sh $(IDF_TARGET)
	touch $@

# Run any idf.py command inside the IDF environment.
# Usage: $(IDF_RUN) <args...>
IDF_RUN = bash -c '. $(IDF_DIR)/export.sh >/dev/null && exec idf.py "$$@"' --

BUILD_DIR := $b/firmware
LAMP_BIN  := $(BUILD_DIR)/lamp.bin

# All source inputs that trigger a rebuild. If you touch any of these, the
# next `make build` runs idf.py. If you don't, Make shortcuts to a no-op
# without paying idf.py's startup tax.
LAMP_SRCS := \
	$(wildcard main/*.c) \
	$(wildcard main/*.h) \
	main/CMakeLists.txt \
	main/idf_component.yml \
	CMakeLists.txt \
	sdkconfig.defaults \
	partitions.csv \
	Makefile

# ── User-facing targets ─────────────────────────────────────────────────────
.PHONY: setup build flash monitor flash-monitor set-time erase-nvs clean distclean

## setup — one-time sudo prereqs (apt packages + serial port access)
setup:
	@echo "== Installing build prerequisites =="
	sudo apt install -y ninja-build gperf python3-venv
	@echo
	@echo "== Adding $(USER) to dialout group (for /dev/ttyACM*) =="
	sudo usermod -aG dialout $(USER)
	@echo
	@echo "== Granting immediate read/write on $(SERIAL_PORT) for this session =="
	sudo chmod a+rw $(SERIAL_PORT)
	@echo
	@echo "Setup complete."
	@echo "  - The chmod above lasts until you unplug or reboot."
	@echo "  - For permanent access, log out and back in (or run 'newgrp dialout')."

## build — compile firmware (auto-fetches ESP-IDF and toolchain on first run).
## Target is pinned via sdkconfig.defaults; no need for `idf.py set-target`
## on every invocation (it's destructive and breaks incremental builds).
##
## Real work is in the $(LAMP_BIN) rule below — `build` is a phony alias.
## Make's own mtime check makes this a true no-op when nothing has changed.
build: $(LAMP_BIN)

$(LAMP_BIN): $(LAMP_SRCS) $(IDF_INSTALLED)
	$(IDF_RUN) -B $(BUILD_DIR) build

## flash — build then flash over USB
flash: build
	$(IDF_RUN) -B $(BUILD_DIR) -p $(SERIAL_PORT) flash

## monitor — open USB serial monitor (Ctrl-] to exit)
monitor:
	$(IDF_RUN) -B $(BUILD_DIR) -p $(SERIAL_PORT) monitor

## flash-monitor — flash, then immediately open the monitor
flash-monitor: build
	$(IDF_RUN) -B $(BUILD_DIR) -p $(SERIAL_PORT) flash monitor

## set-time — write current host wall-clock time to the device RTC
set-time:
	@command -v python3 >/dev/null
	@printf 'set-time %s\r\n' "$$(date +%s)" | \
		python3 -c 'import sys,serial; s=serial.Serial("$(SERIAL_PORT)",115200,timeout=2); s.write(sys.stdin.buffer.read()); print(s.read(200).decode(errors="replace"))'

## erase-nvs — wipe device flash (clears Zigbee bindings → re-pair on next boot)
erase-nvs:
	$(IDF_RUN) -B $(BUILD_DIR) -p $(SERIAL_PORT) erase-flash

## test — run host-side unit tests (no hardware needed)
.PHONY: test
test:
	$(MAKE) -C tests/unit

## clean — remove firmware build artifacts (keeps cached ESP-IDF)
clean:
	rm -rf $(BUILD_DIR)

## distclean — remove everything (firmware, ESP-IDF, downloads)
distclean:
	rm -rf $b $(DL)

DIRS := $b $(DL) $(IDF_DIR)
$(foreach d,$(sort $(DIRS)),$(eval $(call MK_DIR,$d)))
