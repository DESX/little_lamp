#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_system.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "driver/usb_serial_jtag.h"
#include "esp_zigbee_core.h"

#include "commissioning.h"
#include "log.h"
#include "rtc.h"

static int cmd_set_time(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: set-time <unix-epoch-seconds>\n");
        return 1;
    }
    long long t = strtoll(argv[1], NULL, 10);
    if (t <= 0) {
        printf("invalid epoch\n");
        return 1;
    }
    rtc_set((time_t)t);
    printf("ok\n");
    return 0;
}

static int cmd_set_verbose(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: set-verbose 0|1   (currently %d)\n", log_is_verbose() ? 1 : 0);
        return 1;
    }
    int v = atoi(argv[1]);
    log_set_verbose(v != 0);
    return 0;
}

static int cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    ui_banner();
    return 0;
}

static int cmd_reset(int argc, char **argv) {
    (void)argc; (void)argv;
    LAMP_UI("Rebooting...");
    fflush(stdout);
    esp_restart();
    return 0;
}

static int cmd_reset_commissioning(int argc, char **argv) {
    (void)argc; (void)argv;
    LAMP_UI("Clearing bindings + Zigbee stack state, rebooting...");
    fflush(stdout);
    commissioning_reset();           // clears our NVS button/bulb assignments
    esp_zb_factory_reset();          // wipes zb_storage and restarts the chip
    return 0;                        // not reached
}

static int cmd_pair(int argc, char **argv) {
    (void)argc; (void)argv;
    LAMP_UI("Opening join window 180s (existing bindings preserved)");
    esp_zb_bdb_open_network(180);
    return 0;
}

void console_init(void) {
    // Use USB Serial-JTAG for both the log and the console. Keep things simple
    // — line-buffered, no fancy editing.
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "lamp> ";
    cfg.max_cmdline_length = 64;

    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &cfg, &repl));

    const esp_console_cmd_t set_time_cmd = {
        .command = "set-time",
        .help    = "Set RTC: set-time <unix-epoch-seconds>",
        .func    = &cmd_set_time,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&set_time_cmd));

    const esp_console_cmd_t set_verbose_cmd = {
        .command = "set-verbose",
        .help    = "Toggle detailed log stream: set-verbose 0|1",
        .func    = &cmd_set_verbose,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&set_verbose_cmd));

    const esp_console_cmd_t status_cmd = {
        .command = "status",
        .help    = "Print pairing + state banner",
        .func    = &cmd_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    const esp_console_cmd_t reset_cmd = {
        .command = "reset",
        .help    = "Reboot the device",
        .func    = &cmd_reset,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reset_cmd));

    const esp_console_cmd_t reset_comm_cmd = {
        .command = "reset-commissioning",
        .help    = "Clear button+bulb bindings, then reboot (re-pair fresh)",
        .func    = &cmd_reset_commissioning,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reset_comm_cmd));

    const esp_console_cmd_t pair_cmd = {
        .command = "pair",
        .help    = "Open the join window 180s without clearing existing bindings",
        .func    = &cmd_pair,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&pair_cmd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    LAMP_LOGI("console: ready (try `set-time <epoch>`)");
}
