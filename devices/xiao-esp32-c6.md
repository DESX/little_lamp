# Seeed Studio XIAO ESP32-C6 — coordinator

Single-board hardware that hosts the entire system. Soldered LiPo backup
battery keeps the chip alive across mains outages.

## Identification

| Field | Value |
|---|---|
| Board | Seeed Studio XIAO ESP32-C6 (NOT the C3, NOT an "ESP32-U6" — that model doesn't exist) |
| MCU | ESP32-C6 (RISC-V, 160 MHz, single core) |
| Radio | 802.15.4 (Zigbee/Thread) native, plus Wi-Fi 6 / BLE 5 LE (unused here) |
| USB | Built-in USB-Serial-JTAG (CDC ACM). Appears on Linux as `/dev/ttyACM0`. `lsusb` ID `303a:1001`. |
| Antenna | PCB-trace antenna at the corner **opposite the USB-C port**. Point that corner toward the button; keep ~5 cm clear of metal/USB cable. |
| Flash | 4 MB SPI |

## Build environment

| Component | Version | Notes |
|---|---|---|
| ESP-IDF | v5.4.4 | Fetched via `graft.mk` from `../graft`, cached in `out/esp-idf`, toolchain in `out/idf-tools`. Don't use the system-wide `~/.espressif`. |
| ESP-Zigbee-SDK | `espressif/esp-zigbee-lib` `^1.6.0` (+ `esp-zboss-lib`) | Declared in `main/idf_component.yml`, pulled by IDF Component Manager into `managed_components/`. |
| Toolchain | `riscv32-esp-elf-gcc` (auto-installed by `install.sh esp32c6`) | |
| Build deps | `ninja-build`, `gperf`, `python3-venv` | Bundled into `make setup` target. |

## sdkconfig must-haves

All set in `sdkconfig.defaults`:

```
CONFIG_IDF_TARGET="esp32c6"
CONFIG_ZB_ENABLED=y
CONFIG_ZB_ZCZR=y
CONFIG_ZB_RADIO_NATIVE=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_LOG_DEFAULT_LEVEL_WARN=y     # keeps the serial console quiet by default
CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y  # quiet boot too
CONFIG_FREERTOS_USE_TICKLESS_IDLE=n # radio must stay awake on coordinator
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

`partitions.csv` adds `zb_storage` (16 K, FAT) and `zb_fct` (1 K, FAT) for
ZBOSS persistence. **Without those partitions ZBOSS won't persist network
state and the button has to rejoin on every coordinator reboot.**

## Things that bit us

### Serial port permissions

`/dev/ttyACM0` is `root:dialout 660` on Ubuntu by default. The user must be
in `dialout` AND have a fresh login session, or you have to `sudo chmod
a+rw /dev/ttyACM0` each replug. The `make setup` target adds the user to
dialout permanently, but the chmod for the current session is non-persistent
across USB replug.

### Resetting the chip programmatically

USB-Serial-JTAG resets via DTR/RTS toggle exactly like esptool does:

```
s.setDTR(False); s.setRTS(True); time.sleep(0.1)
s.setRTS(False); time.sleep(0.05)
```

The chip reboots and the next boot output streams to the same `/dev/ttyACM0`.

### USB-Serial-JTAG console quirks

The ESP-IDF console REPL doesn't support escape sequences over USB-Serial-JTAG —
expect to see `[5n` and `[6n` cursor-position-report sequences mixed into the
stream, and `lamp> ` echoed multiple times. Tools that parse the output should
strip those. `idf.py monitor` handles them; raw `cat` doesn't.

### USB-Serial-JTAG output buffering

`printf()` is line-buffered to a small TX FIFO. Use `fflush(stdout)` after
important UI messages — the `LAMP_UI()` macro does this. Otherwise you can
have multi-line UI output trickle out over hundreds of ms.

### `esp_zb_nwk_set_ed_timeout` lives in `test/` headers

The API is declared in
`managed_components/espressif__esp-zigbee-lib/include/test/esp_zigbee_test_utils.h`
not in the public `nwk/` headers. It IS production-callable; the `test/`
location is misleading. **You must call this with `ESP_ZB_ED_AGING_TIMEOUT_64MIN`
(or longer) before `esp_zb_start()` or the coordinator silently evicts every
sleepy child after 10 seconds of "no useful keepalive."** This was the single
biggest bug in the project — every "the button keeps rejoining" symptom traced
back to this default.

### `esp_zb_ota_cluster_create()` crashes the firmware

Registering the OTA Upgrade cluster as SERVER with a zeroed `esp_zb_ota_cluster_cfg_t`
causes `Zigbee stack assertion failed zcl/zcl_ota_upgrade_srv_commands.c:132`
the first time a child sends a Query Next Image Request. The SDK's OTA server
expects an actual image to be configured. Workaround: don't register the
cluster, intercept the OTA Query Next Image Request frame in
`esp_zb_aps_data_indication_handler_register` and return `true` to suppress the
default response.

### Identify cluster ticks every second once F&B target is active

When `esp_zb_bdb_finding_binding_start_target(ep, 180)` runs, the Identify
cluster's `IdentifyTime` attribute decrements once per second, triggering an
`ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID` callback for cluster `0x0003` each tick.
**These are not from the button.** Filter them out in any log analysis.

### Default child aging vs `esp_zb_zczr_cfg_t`

The `esp_zb_zczr_cfg_t` struct in `esp_zigbee_core.h` has only `max_children`.
There is no aging-timeout field there — you have to call
`esp_zb_nwk_set_ed_timeout()` separately.

### `esp_zb_factory_reset()` wipes everything

Including device-side bindings on the bulb (it forgets it was paired). Use
`commissioning_reset()` (our NVS-only wipe + `esp_zb_factory_reset()`) only
when the user explicitly asks to start over. The console `reset` command does
NOT wipe — it just restarts the chip.

### Make target name `build` collides with the directory `build/`

GNU Make complains "warning: overriding recipe for target 'build'" when graft's
`MK_DIR` creates a directory rule for `$b/`. We use `b := out` to avoid this.

### `idf.py set-target` is destructive

Calling it on every build wipes CMake cache and causes a full recompile. We
specify the target via `CONFIG_IDF_TARGET="esp32c6"` in `sdkconfig.defaults`
and never invoke `set-target` from our Makefile.

### Don't trust no-op builds with phony `build` target

`make build` invoking `idf.py build` always takes ~2.7 s of Python/cmake/ninja
startup even when nothing changed. We make `build` depend on the actual binary
(`out/firmware/lamp.bin`) which depends on `main/*.c`, `main/*.h`, CMakeLists,
sdkconfig.defaults, partitions.csv, and the Makefile. Then a no-op `make
build` is a true 5 ms no-op.

## Resetting the C6

Three levels of reset, increasing destructiveness:

| Command | What it does | Keeps |
|---|---|---|
| `reset` (console) | Reboots the chip via `esp_restart()` | Everything in NVS — pairings, verbose flag, RTC if battery held |
| `reset-commissioning` (console) | Calls `commissioning_reset()` to wipe our button/bulb NVS keys, then `esp_zb_factory_reset()` which erases `zb_storage` and self-reboots | Verbose flag, RTC if battery held |
| `make erase-nvs` | `idf.py erase-flash` — wipes ALL flash partitions | Nothing — full factory-from-bootloader state |

## RF environment notes

- The C6's PCB antenna is sensitive to nearby metal, USB cables coiled tight, and Wi-Fi routers.
- We pin Zigbee channel 25 (2475 MHz) to dodge Wi-Fi channels 1/6/11 — set in `main/main.c` as `ESP_ZB_PRIMARY_CHANNEL_MASK = (1u << 25)`.
- A Wi-Fi router within ~10 cm of the C6 measurably degrades range even on channel 25 because raw RF intensity blanks the C6's LNA.

## Sources

- ESP-Zigbee-SDK headers in `managed_components/espressif__esp-zigbee-lib/include/`
- `esp_zigbee_test_utils.h:98` — `esp_zb_nwk_set_ed_timeout` declaration
- `esp_zigbee_core.h:43-52` — `esp_zb_aging_timeout_t` enum
- ESP-Zigbee-SDK issue #21 — `esp_zb_secur_link_key_exchange_required_set` for legacy joins
- Seeed Studio XIAO ESP32-C6 wiki — pinout, antenna, USB-Serial-JTAG
