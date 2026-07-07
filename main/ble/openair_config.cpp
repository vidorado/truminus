#include "openair_config.hpp"
#include "nvs_flash.h"
#include <cmath>

static bool s_openairConfigured = false;

void openairCfgReload() {
    nvs_handle_t h;
    if (nvs_open("openair", NVS_READONLY, &h) != ESP_OK) { s_openairConfigured = false; return; }
    char buf[24] = {};
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, "addr", buf, &len);
    nvs_close(h);
    s_openairConfigured = (err == ESP_OK && buf[0] != '\0');
}

bool openairCfgIsActive() { return s_openairConfigured; }

OpenAirCmd buildOpenAirCmd(const P4ControlState& cs) {
    OpenAirCmd cmd = {};
    cmd.ledBright     = 0;   // keep the console LED off by default
    cmd.ledColor      = 0;
    cmd.scheduledTime = 0;
    cmd.flaps1        = 0;
    cmd.flaps2        = 0;

    int tempTenths = (int)roundf(cs.roomSetpoint * 10.0f);
    if (tempTenths < 160) tempTenths = 160;
    if (tempTenths > 320) tempTenths = 320;
    cmd.tempTenths = tempTenths;

    if (cs.acMode == 0) {
        cmd.powerState  = 0;
        cmd.mode        = 0;
        cmd.blowerSpeed = 1;
    } else if (cs.acMode == 1) {
        cmd.powerState  = 1;
        cmd.mode        = cs.acFanAuto ? 0 : 2;
        int spd = cs.acFanSpeed;
        cmd.blowerSpeed = (spd >= 1 && spd <= 6) ? spd : 1;
    } else {
        // acMode == 2 (eco)
        cmd.powerState  = 1;
        cmd.mode        = 1;   // ECO — TO VERIFY against real unit
        cmd.blowerSpeed = 1;
    }
    return cmd;
}
