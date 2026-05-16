#include "console.h"

// SDK exception: esp_console keeps the REPL handle and the registered-
// command table in its own file-static storage. We can't pass a struct
// to it. We thread our application's objects through file-static
// pointers, set once at console_init, used by each command handler.
//
// The pointers are NOT the storage — main owns the storage. These are
// cached references for callbacks the SDK invokes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_system.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "driver/usb_serial_jtag.h"
#include "esp_zigbee_core.h"

#include "enrollment.h"
#include "rtc.h"

static log_t           *s_log           = NULL;
static commissioning_t *s_commissioning = NULL;
static button_t        *s_button        = NULL;

static int cmd_set_time(int argc, char **argv) {
    if (argc < 2) { printf("usage: set-time <epoch>\n"); return 1; }
    long long t = strtoll(argv[1], NULL, 10);
    if (t <= 0) { printf("invalid epoch\n"); return 1; }
    rtc_set((time_t)t);
    printf("ok\n");
    return 0;
}

static int cmd_set_verbose(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: set-verbose 0|1   (currently %d)\n", log_is_verbose(s_log) ? 1 : 0);
        return 1;
    }
    log_set_verbose(s_log, atoi(argv[1]) != 0);
    return 0;
}

// Forward declared from main.c so the status command can re-render the
// banner without console.c needing to know how it's composed.
extern void show_status_banner(void);

static int cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    show_status_banner();
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
    commissioning_reset(s_commissioning);
    esp_zb_factory_reset();
    return 0;
}

static int cmd_pair(int argc, char **argv) {
    (void)argc; (void)argv;
    LAMP_UI("Opening join window 180s (existing bindings preserved)");
    esp_zb_bdb_open_network(180);
    return 0;
}

static int cmd_discover(int argc, char **argv) {
    (void)argc; (void)argv;
    LAMP_UI("Discovering attributes on button's cluster 0xFF01...");
    discover_button_attrs(s_button);
    return 0;
}

static int cmd_button_info(int argc, char **argv) {
    (void)argc; (void)argv;
    LAMP_UI("Reading button's Basic-cluster attributes...");
    read_button_basic(s_button);
    return 0;
}

void console_init(log_t *log, commissioning_t *commissioning, button_t *button) {
    s_log           = log;
    s_commissioning = commissioning;
    s_button        = button;

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "lamp> ";
    cfg.max_cmdline_length = 64;

    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &cfg, &repl));

    const esp_console_cmd_t commands[] = {
        { .command = "set-time",            .help = "Set RTC: set-time <epoch>",                              .func = &cmd_set_time },
        { .command = "set-verbose",         .help = "Toggle detailed log stream: set-verbose 0|1",            .func = &cmd_set_verbose },
        { .command = "status",              .help = "Print pairing + state banner",                           .func = &cmd_status },
        { .command = "reset",               .help = "Reboot the device",                                      .func = &cmd_reset },
        { .command = "reset-commissioning", .help = "Clear bindings + Zigbee state, then reboot",             .func = &cmd_reset_commissioning },
        { .command = "pair",                .help = "Open the join window 180s without clearing bindings",    .func = &cmd_pair },
        { .command = "discover",            .help = "Send Discover Attributes for cluster 0xFF01",            .func = &cmd_discover },
        { .command = "button-info",         .help = "Read button identity (manuf, model, fw version)",        .func = &cmd_button_info },
    };
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&commands[i]));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    LAMP_LOGI("console: ready (try `set-time <epoch>`)");
}
