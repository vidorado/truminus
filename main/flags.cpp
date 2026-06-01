#include "flags.h"

void flags_apply_log_levels(void) {
    // ── TruMinus subsystems ──────────────────────────────────────────────
    esp_log_level_set("main",      LOG_LEVEL_MAIN);
    esp_log_level_set("wifi",      LOG_LEVEL_WIFI);
    esp_log_level_set("truma_lin", LOG_LEVEL_TRUMA_LIN);
    esp_log_level_set("lin",       LOG_LEVEL_LIN);
    esp_log_level_set("display",   LOG_LEVEL_DISPLAY);
    esp_log_level_set("cfg",       LOG_LEVEL_CFG);
    esp_log_level_set("c6_ota",    LOG_LEVEL_C6_OTA);
    esp_log_level_set("web",       LOG_LEVEL_WEB);
    esp_log_level_set("wstunnel",  LOG_LEVEL_WSTUNNEL);

    // ── IDF infrastructure ───────────────────────────────────────────────
    esp_log_level_set("httpd",     LOG_LEVEL_HTTPD);
    esp_log_level_set("ledc",      LOG_LEVEL_LEDC);

    // ── BSP: panel, touch, LVGL, I2C bus ─────────────────────────────────
    esp_log_level_set("bsp_i2c",     LOG_LEVEL_BSP);
    esp_log_level_set("bsp_touch",   LOG_LEVEL_BSP);
    esp_log_level_set("BSP_DISPLAY", LOG_LEVEL_BSP);
    esp_log_level_set("st7701",      LOG_LEVEL_BSP);
    esp_log_level_set("st7701_mipi", LOG_LEVEL_BSP);
    esp_log_level_set("GT911",       LOG_LEVEL_BSP);
    esp_log_level_set("LVGL",        LOG_LEVEL_BSP);

    // ── ESP-Hosted (C6 coprocessor over SDIO) ────────────────────────────
    esp_log_level_set("transport",       LOG_LEVEL_HOSTED);
    esp_log_level_set("H_SDIO_DRV",      LOG_LEVEL_HOSTED);
    esp_log_level_set("H_API",           LOG_LEVEL_HOSTED);
    esp_log_level_set("sdio_wrapper",    LOG_LEVEL_HOSTED);
    esp_log_level_set("os_wrapper_esp",  LOG_LEVEL_HOSTED);
    esp_log_level_set("esp_cli",         LOG_LEVEL_HOSTED);
    esp_log_level_set("vhci_drv",        LOG_LEVEL_HOSTED);
    esp_log_level_set("RPC_WRAP",        LOG_LEVEL_HOSTED);
    esp_log_level_set("rpc_utils",       LOG_LEVEL_HOSTED);
    esp_log_level_set("esp_wifi_remote", LOG_LEVEL_HOSTED);

    // ── Network stack: netif/IP, TLS cert bundle, WS client, NimBLE ──────
    esp_log_level_set("esp_netif_handlers",  LOG_LEVEL_NET);
    esp_log_level_set("websocket_client",    LOG_LEVEL_NET);
    esp_log_level_set("esp-x509-crt-bundle", LOG_LEVEL_NET);
    esp_log_level_set("NimBLE",              LOG_LEVEL_NET);  // WARN: silence per-scan GAP chatter
}
