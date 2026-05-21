#include "cli.hpp"
#include "esp_console.h"
#include "esp_log.h"
#include "linenoise/linenoise.h"
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "wifi_manager.hpp"
#include "wstunnel.hpp"
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <string>

static bool normalize_hex(const char* in, size_t want, std::string& out) {
    out.clear();
    if (!in) return false;
    while (*in) {
        char c = *in++;
        if (c == ':' || c == '-' || c == ' ') continue;
        if (!isxdigit((unsigned char)c)) return false;
        out.push_back((char)toupper((unsigned char)c));
        if (out.size() > want) return false;
    }
    return out.size() == want;
}

static int cmd_victron(int argc, char** argv) {
    if (argc < 3) { printf("usage: victron <mac> <key32hex>\n"); return 1; }
    std::string mac, key;
    if (!normalize_hex(argv[1], 12, mac)) { printf("bad mac (need 12 hex)\n"); return 1; }
    if (!normalize_hex(argv[2], 32, key)) { printf("bad key (need 32 hex)\n"); return 1; }
    victronSaveConfig(mac, key);
    victronBleReloadConfig();
    printf("victron saved: %s key=%.8s...\n", mac.c_str(), key.c_str());
    return 0;
}

static int cmd_ultimatron(int argc, char** argv) {
    if (argc < 2) { printf("usage: ultimatron <mac> [pass]\n"); return 1; }
    std::string mac, pass;
    if (!normalize_hex(argv[1], 12, mac)) { printf("bad mac (need 12 hex)\n"); return 1; }
    if (argc >= 3) pass = argv[2];
    ultimatronSaveConfig(mac, pass);
    ultimatronBleReloadConfig();
    printf("ultimatron saved: %s%s%s\n", mac.c_str(),
           pass.empty() ? "" : " pass=", pass.c_str());
    return 0;
}

static int cmd_wifi(int argc, char** argv) {
    if (argc < 2) { printf("usage: wifi <ssid> [pass]\n"); return 1; }
    const char* ssid = argv[1];
    const char* pass = (argc >= 3) ? argv[2] : "";
    wifi_manager_connect(ssid, pass);
    printf("wifi: connecting to '%s'...\n", ssid);
    return 0;
}

static int cmd_tunnel(int argc, char** argv) {
    if (argc < 2) { printf("usage: tunnel <on|off>\n"); return 1; }
    TunnelConfig c{}; wstunnelLoadConfig(c);
    if (strcasecmp(argv[1], "on") == 0) c.enabled = true;
    else if (strcasecmp(argv[1], "off") == 0) c.enabled = false;
    else { printf("usage: tunnel <on|off>\n"); return 1; }
    wstunnelSaveConfig(c);
    wstunnelApply();
    printf("tunnel: %s\n", c.enabled ? "enabled" : "disabled");
    return 0;
}

static int cmd_show(int, char**) {
    TunnelConfig tc{}; wstunnelLoadConfig(tc);
    printf("tunnel:     %s server=%s\n",
           tc.enabled ? "on" : "off",
           tc.server[0] ? tc.server : "-");
    WifiStatus ws = wifi_manager_get_status();
    printf("wifi:       %s%s%s ip=%s\n",
           ws.connected ? "connected " : "disconnected ",
           ws.ssid[0] ? "ssid=" : "",
           ws.ssid[0] ? ws.ssid : "",
           ws.ip[0]   ? ws.ip   : "-");
    std::string a, k;
    if (victronLoadConfig(a, k))
        printf("victron:    addr=%s key=%.8s...\n", a.c_str(), k.c_str());
    else
        printf("victron:    <unconfigured>\n");
    std::string ua, up;
    if (ultimatronLoadConfig(ua, up))
        printf("ultimatron: addr=%s%s%s\n", ua.c_str(),
               up.empty() ? "" : " pass=", up.c_str());
    else
        printf("ultimatron: <unconfigured>\n");
    return 0;
}

void cliStart() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "truminus> ";
    cfg.max_cmdline_length = 256;
    // /dev/ttyACM0 is the P4's native USB-Serial-JTAG (no external bridge);
    // building the REPL on UART0 (default) means keystrokes from picocom go
    // to the wrong sink while the prompt prints to both.  Bind the REPL to
    // USB-Serial-JTAG so input + output share the port the user is on.
    esp_console_dev_usb_serial_jtag_config_t jtag_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&jtag_cfg, &cfg, &repl));

    // USB-Serial-JTAG doesn't answer linenoise's terminal-probe escapes in
    // time, so without dumb mode arrow keys leak through as raw "[D[C..."
    // sequences and backspace echo is unreliable.  Dumb mode trades arrow
    // editing + history for char-by-char echo and a working BACKSPACE/DEL;
    // Ctrl+U still clears the whole line.
    linenoiseSetDumbMode(1);

    esp_console_cmd_t c_wifi       = {}; c_wifi.command       = "wifi";       c_wifi.help       = "wifi <ssid> [pass]: connect + persist credentials"; c_wifi.func       = cmd_wifi;
    esp_console_cmd_t c_tunnel     = {}; c_tunnel.command     = "tunnel";     c_tunnel.help     = "tunnel <on|off>: enable/disable WSS reverse tunnel"; c_tunnel.func     = cmd_tunnel;
    esp_console_cmd_t c_victron    = {}; c_victron.command    = "victron";    c_victron.help    = "victron <mac> <key32hex>: set Victron BLE creds";    c_victron.func    = cmd_victron;
    esp_console_cmd_t c_ultimatron = {}; c_ultimatron.command = "ultimatron"; c_ultimatron.help = "ultimatron <mac> [pass]: set Ultimatron BLE creds"; c_ultimatron.func = cmd_ultimatron;
    esp_console_cmd_t c_show       = {}; c_show.command       = "show";       c_show.help       = "show: print stored BLE config";                      c_show.func       = cmd_show;
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_wifi));
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_tunnel));
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_victron));
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_ultimatron));
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_show));
    esp_console_register_help_command();

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
