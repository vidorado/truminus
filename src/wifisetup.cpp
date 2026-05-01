#ifdef CYD
#include "wifisetup.hpp"
#include "victronble.hpp"
#include <Preferences.h>
#include <WiFi.h>
#include <esp32_smartdisplay.h>
#include <NimBLEDevice.h>
#include <vector>

// -----------------------------------------------------------------------
// NVS — Touch calibration
// -----------------------------------------------------------------------
static const char* NVS_CAL_NS  = "touchcal";
static const char* NVS_CAL_KEY = "data";

bool loadTouchCalibration(touch_calibration_data_t& cal) {
    Preferences prefs;
    prefs.begin(NVS_CAL_NS, true);
    if (!prefs.isKey(NVS_CAL_KEY)) { prefs.end(); return false; }
    size_t len = prefs.getBytes(NVS_CAL_KEY, &cal, sizeof(cal));
    prefs.end();
    return (len == sizeof(cal)) && cal.valid;
}

void saveTouchCalibration(const touch_calibration_data_t& cal) {
    Preferences prefs;
    prefs.begin(NVS_CAL_NS, false);
    prefs.putBytes(NVS_CAL_KEY, &cal, sizeof(cal));
    prefs.end();
}

// -----------------------------------------------------------------------
// NVS — WiFi
// -----------------------------------------------------------------------
static const char* NVS_WIFI_NS   = "wifi";
static const char* NVS_WIFI_SSID = "ssid";
static const char* NVS_WIFI_PASS = "pass";

bool loadWifiCredentials(String& ssid, String& pass) {
    Preferences prefs;
    prefs.begin(NVS_WIFI_NS, true);
    if (!prefs.isKey(NVS_WIFI_SSID)) { prefs.end(); return false; }
    ssid = prefs.getString(NVS_WIFI_SSID, "");
    pass = prefs.getString(NVS_WIFI_PASS, "");
    prefs.end();
    return ssid.length() > 0;
}

void saveWifiCredentials(const String& ssid, const String& pass) {
    Preferences prefs;
    prefs.begin(NVS_WIFI_NS, false);
    prefs.putString(NVS_WIFI_SSID, ssid);
    prefs.putString(NVS_WIFI_PASS, pass);
    prefs.end();
}

// -----------------------------------------------------------------------
// NVS — MQTT
// -----------------------------------------------------------------------
static const char* NVS_MQTT_NS   = "mqtt";
static const char* NVS_MQTT_HOST = "host";
static const char* NVS_MQTT_PORT = "port";
static const char* NVS_MQTT_USER = "user";
static const char* NVS_MQTT_PASS = "pass";

bool loadMqttConfig(String& host, String& port, String& user, String& pass) {
    Preferences prefs;
    prefs.begin(NVS_MQTT_NS, true);
    if (!prefs.isKey(NVS_MQTT_HOST)) { prefs.end(); return false; }
    host = prefs.getString(NVS_MQTT_HOST, "");
    port = prefs.getString(NVS_MQTT_PORT, "1883");
    user = prefs.getString(NVS_MQTT_USER, "");
    pass = prefs.getString(NVS_MQTT_PASS, "");
    prefs.end();
    return host.length() > 0;
}

void saveMqttConfig(const String& host, const String& port,
                    const String& user, const String& pass) {
    Preferences prefs;
    prefs.begin(NVS_MQTT_NS, false);
    prefs.putString(NVS_MQTT_HOST, host);
    prefs.putString(NVS_MQTT_PORT, port);
    prefs.putString(NVS_MQTT_USER, user);
    prefs.putString(NVS_MQTT_PASS, pass);
    prefs.end();
}

// -----------------------------------------------------------------------
// LVGL tick helper (shared by all setup screens)
// -----------------------------------------------------------------------
static uint32_t s_lvLastTick = 0;

static void lvRun(uint32_t ms = 10) {
    uint32_t end = millis() + ms;
    do {
        uint32_t now = millis();
        lv_tick_inc(now - s_lvLastTick);
        s_lvLastTick = now;
        lv_timer_handler();
        delay(2);
    } while (millis() < end);
}

// =======================================================================
// Touch calibration screen
// =======================================================================
// Screen is landscape 320×240.
// Three target points chosen to be well-spread for a stable affine solve:
//   top-left, top-right, bottom-center.
// With touch_calibration_data.valid=false, LVGL passes raw ADC-normalised
// coordinates through unchanged, so lv_indev_get_point() returns the raw
// values.  Those become our "touch_pts" for smartdisplay_compute_touch_calibration().
//
// Every step prints to Serial so you can check the raw values even if the
// computed calibration turns out to be off.
// -----------------------------------------------------------------------

static const int     CAL_N   = 3;
static const int32_t CAL_SX[CAL_N] = { 40, 280, 160 };  // screen X targets
static const int32_t CAL_SY[CAL_N] = { 40,  40, 200 };  // screen Y targets
static const char*   CAL_MSG[CAL_N] = {
    "1/3  Toca el centro de la cruz",
    "2/3  Toca el centro de la cruz",
    "3/3  Toca el centro de la cruz",
};

static lv_point_t s_calTouchPts[CAL_N];
static bool       s_calTapped;
static lv_point_t s_calLastPt;

static void calTapCb(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_get_act();
    s_calLastPt = {0, 0};
    if (indev) lv_indev_get_point(indev, &s_calLastPt);
    s_calTapped = true;
}

// Draw a ± crosshair with a centre dot at (cx, cy) on parent.
// Bars are NOT clickable so taps pass through to the screen object.
static void calDrawCross(lv_obj_t* parent, int32_t cx, int32_t cy) {
    auto makeBar = [&](int32_t x, int32_t y, int32_t w, int32_t h) {
        lv_obj_t* bar = lv_obj_create(parent);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, w, h);
        lv_obj_set_pos(bar, x, y);
        lv_obj_set_style_bg_color(bar, lv_color_make(255, 220, 0), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
        lv_obj_clear_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_add_flag(bar, LV_OBJ_FLAG_EVENT_BUBBLE); // pass taps up to screen
    };
    makeBar(cx - 25, cy - 1, 50, 3);  // horizontal arm
    makeBar(cx -  1, cy - 25, 3, 50); // vertical arm

    // Centre dot (orange, circular)
    lv_obj_t* dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_pos(dot, cx - 5, cy - 5);
    lv_obj_set_style_bg_color(dot, lv_color_make(255, 80, 0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(dot, LV_OBJ_FLAG_EVENT_BUBBLE);
}

// Rebuild the calibration screen for a given step (clears previous content).
static void calBuildStep(lv_obj_t* scr, int step) {
    lv_obj_clean(scr);

    // Step instruction
    lv_obj_t* lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(lbl, CAL_MSG[step]);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 8);

    // Hint
    lv_obj_t* sub = lv_label_create(scr);
    lv_obj_set_style_text_color(sub, lv_color_make(160, 160, 160), LV_PART_MAIN);
    lv_label_set_text(sub, "Toca lo mas cerca del centro del simbolo +");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 26);

    calDrawCross(scr, CAL_SX[step], CAL_SY[step]);
}

void runTouchCalibration() {
    // Pass raw ADC-normalised touch coordinates through without any transform.
    touch_calibration_data.valid = false;

    s_calTapped  = false;
    s_lvLastTick = millis();

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_make(0, 0, 60), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    // Make the screen itself clickable so taps anywhere (including through
    // non-clickable crosshair children) arrive here.
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, calTapCb, LV_EVENT_CLICKED, NULL);
    lv_screen_load(scr);

    calBuildStep(scr, 0);

    Serial.println("[CAL] Iniciando calibracion de pantalla. Toca los 3 puntos.");

    for (int step = 0; step < CAL_N; ) {
        uint32_t now = millis();
        lv_tick_inc(now - s_lvLastTick);
        s_lvLastTick = now;
        lv_timer_handler();
        delay(5);

        if (s_calTapped) {
            s_calTapped = false;
            s_calTouchPts[step] = s_calLastPt;
            Serial.printf("[CAL] Punto %d: pantalla=(%d,%d)  raw_touch=(%d,%d)\n",
                step,
                (int)CAL_SX[step], (int)CAL_SY[step],
                (int)s_calLastPt.x, (int)s_calLastPt.y);
            step++;
            if (step < CAL_N) {
                calBuildStep(scr, step);
            }
        }
    }

    // Compute affine calibration from the 3-point correspondence.
    lv_point_t sp[3] = {
        {CAL_SX[0], CAL_SY[0]},
        {CAL_SX[1], CAL_SY[1]},
        {CAL_SX[2], CAL_SY[2]}
    };
    touch_calibration_data = smartdisplay_compute_touch_calibration(sp, s_calTouchPts);
    saveTouchCalibration(touch_calibration_data);

    Serial.printf("[CAL] Calibracion guardada. valid=%d\n",
        (int)touch_calibration_data.valid);
    Serial.printf("[CAL]   alphaX=%.5f  betaX=%.5f  deltaX=%d\n",
        touch_calibration_data.alphaX,
        touch_calibration_data.betaX,
        (int)touch_calibration_data.deltaX);
    Serial.printf("[CAL]   alphaY=%.5f  betaY=%.5f  deltaY=%d\n",
        touch_calibration_data.alphaY,
        touch_calibration_data.betaY,
        (int)touch_calibration_data.deltaY);

    // Brief confirmation message.
    lv_obj_clean(scr);
    lv_obj_t* done = lv_label_create(scr);
    lv_obj_set_style_text_color(done, lv_color_make(80, 255, 80), LV_PART_MAIN);
    lv_label_set_text(done, "Calibracion completada!");
    lv_obj_center(done);
    lvRun(1500);

    lv_obj_delete(scr);
}

// =======================================================================
// WiFi setup screen
// Scrollable panel so the LVGL keyboard doesn't cover the password field.
// Layout (landscape 320×240):
//   - scr  : non-scrollable root
//     - s_panel (320×240, scrollable): contains all form widgets
//         title | net label | dropdown | pass label | [passTA + eyeBtn]
//         | scanBtn | connectBtn | statusLabel | bottom spacer
//     - s_kb (320×130, fixed at bottom of scr, NOT inside panel)
// When the keyboard appears, s_panel shrinks to 110px so the user can
// scroll the TA into view above the keyboard.
// =======================================================================

static lv_obj_t* s_dropdown   = nullptr;
static lv_obj_t* s_passTA     = nullptr;
static lv_obj_t* s_passEyeLbl = nullptr;
static lv_obj_t* s_kb         = nullptr;
static lv_obj_t* s_panel      = nullptr;
static lv_obj_t* s_statusLbl  = nullptr;
static lv_obj_t* s_connectBtn     = nullptr;
static lv_obj_t* s_connectLbl     = nullptr;   // label inside connect btn
static lv_obj_t* s_connectSpinner = nullptr;   // spinner inside connect btn
static lv_obj_t* s_spinner        = nullptr;
static bool      s_done           = false;
static bool      s_cancelled      = false;
static bool      s_passVis        = false;
static int       s_netCount       = 0;

static void wifiSetStatus(const char* msg) {
    lv_label_set_text(s_statusLbl, msg);
    lvRun(20);
}

// Synchronous scan: blocks ~2-3 s but guarantees results even when queued
// WiFi events (STA_DISCONNECTED from a previous reconnect loop) would
// corrupt the async _scanStarted flag mid-flight.
static void doScan() {
    s_netCount = 0;
    lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
    wifiSetStatus("Buscando redes WiFi...");

    // Reset WiFi to a clean STA state with no auto-reconnect.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true);
    lvRun(300);          // drain pending events while LVGL stays alive
    WiFi.mode(WIFI_STA);
    lvRun(500);          // wait for esp_wifi_start()

    WiFi.scanDelete();
    // Synchronous scan: blocks until SCAN_DONE semaphore fires — immune to
    // _scanStarted being reset early by unrelated WiFi events.
    int n = WiFi.scanNetworks(/*async=*/false);
    Serial.printf("[wifisetup] scanNetworks(sync)=%d\n", n);

    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);

    s_netCount = (n < 0) ? 0 : n;
    if (s_netCount <= 0) {
        lv_dropdown_set_options(s_dropdown, "---");
        wifiSetStatus("No se encontraron redes.");
        lvRun(20);
        return;
    }

    String opts;
    for (int i = 0; i < s_netCount; i++) {
        if (i > 0) opts += '\n';
        opts += WiFi.SSID(i);
        opts += " (";
        opts += String(WiFi.RSSI(i));
        opts += " dBm)";
        if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) opts += " *";
    }
    lv_dropdown_set_options(s_dropdown, opts.c_str());
    lv_dropdown_set_selected(s_dropdown, 0);

    char msg[40];
    snprintf(msg, sizeof(msg), "%d redes encontradas", s_netCount);
    wifiSetStatus(msg);
    lvRun(20);
}

static void connectCb(lv_event_t*) {
    if (s_netCount <= 0) {
        wifiSetStatus("No hay redes disponibles");
        return;
    }

    uint16_t idx = lv_dropdown_get_selected(s_dropdown);
    String ssid  = WiFi.SSID(idx);

    char msg[64];
    snprintf(msg, sizeof(msg), "Conectando a %s...", ssid.c_str());
    wifiSetStatus(msg);
    lv_obj_add_state(s_connectBtn, LV_STATE_DISABLED);
    lv_label_set_text(s_connectLbl, "Conectando...");
    lv_obj_align(s_connectLbl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_remove_flag(s_connectSpinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_connectSpinner, LV_ALIGN_RIGHT_MID, -6, 0);
    lvRun(20);

    const char* pass = lv_textarea_get_text(s_passTA);
    WiFi.begin(ssid.c_str(), pass);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) lvRun(200);

    if (WiFi.status() == WL_CONNECTED) {
        saveWifiCredentials(ssid, String(pass));
        snprintf(msg, sizeof(msg), "Conectado: %s", WiFi.localIP().toString().c_str());
        wifiSetStatus(msg);
        lvRun(1500);
        s_done = true;
    } else {
        WiFi.disconnect();
        wifiSetStatus("Error de conexion. Revisa la contrasena.");
        lv_obj_add_flag(s_connectSpinner, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_connectLbl, "Conectar");
        lv_obj_center(s_connectLbl);
        lv_obj_remove_state(s_connectBtn, LV_STATE_DISABLED);
    }
}

static void wifiCancelCb(lv_event_t*) {
    s_cancelled = true;
    s_done      = true;
}

static void eyeCb(lv_event_t*) {
    s_passVis = !s_passVis;
    lv_textarea_set_password_mode(s_passTA, !s_passVis);
    lv_label_set_text(s_passEyeLbl,
        s_passVis ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void wifiKbShow(lv_obj_t* ta) {
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    // Shrink the panel to leave room for the keyboard (240 - 130 = 110 px).
    lv_obj_set_height(s_panel, 240 - 130);
    lvRun(10); // let layout settle before scrolling
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

static void wifiKbHide(lv_event_t*) {
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_panel, 240); // restore full height
}

static void passFocusCb(lv_event_t*) { wifiKbShow(s_passTA); }

bool runWifiSetup(String& ssid, String& pass) {
    s_done      = false;
    s_cancelled = false;
    s_netCount  = 0;
    s_passVis   = false;
    s_lvLastTick = millis();

    // Determinar si ya hay credenciales guardadas (para el label del botón)
    String existSSID, existPass;
    bool hasConfig = loadWifiCredentials(existSSID, existPass);

    // --- Root screen (non-scrollable) ---
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(scr);

    // --- Scrollable content panel (full screen initially) ---
    // 5 px horizontal padding → 310 px usable width.
    s_panel = lv_obj_create(scr);
    lv_obj_set_size(s_panel, 320, 240);
    lv_obj_set_pos(s_panel, 0, 0);
    lv_obj_set_scroll_dir(s_panel, LV_DIR_VER);
    lv_obj_set_style_pad_left(s_panel, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_panel, 13, LV_PART_MAIN); // deja espacio a la scrollbar
    lv_obj_set_style_pad_top(s_panel, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_panel, 0, LV_PART_MAIN);

    // All children positioned via lv_obj_set_pos() relative to content area.
    // With 5 px left padding, set_pos(0, y) = 5 px from panel left edge.

    // Ancho útil = 320 - pad_left(5) - pad_right(13) = 302 px
    // passTA(264) + gap(4) + eyeBtn(34) = 302 px
    static constexpr int W = 302;

    // Title
    lv_obj_t* title = lv_label_create(s_panel);
    lv_label_set_text(title, "TruMinus - Configuracion WiFi");
    lv_obj_set_width(title, W);
    lv_obj_set_pos(title, 0, 0);

    // Network label
    lv_obj_t* netLbl = lv_label_create(s_panel);
    lv_label_set_text(netLbl, "Red WiFi:");
    lv_obj_set_pos(netLbl, 0, 22);

    // Spinner — visible mientras escanea, luego se oculta
    s_spinner = lv_spinner_create(s_panel);
    lv_spinner_set_anim_params(s_spinner, 1000, 60);
    lv_obj_set_size(s_spinner, 20, 20);
    lv_obj_set_style_arc_width(s_spinner, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 2, LV_PART_INDICATOR);
    lv_obj_set_pos(s_spinner, 0, 44);

    // Dropdown — oculto mientras escanea, visible al terminar
    s_dropdown = lv_dropdown_create(s_panel);
    lv_obj_set_width(s_dropdown, W);
    lv_dropdown_set_options(s_dropdown, "---");
    lv_obj_set_pos(s_dropdown, 0, 38);
    lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);

    // Password label
    lv_obj_t* passLbl = lv_label_create(s_panel);
    lv_label_set_text(passLbl, "Contrasena:");
    lv_obj_set_pos(passLbl, 0, 82);

    // passTA(264) + gap(4) + eyeBtn(34) = 302 px
    s_passTA = lv_textarea_create(s_panel);
    lv_obj_set_size(s_passTA, W - 38, 36);
    lv_obj_set_pos(s_passTA, 0, 98);
    lv_textarea_set_one_line(s_passTA, true);
    lv_textarea_set_password_mode(s_passTA, true);
    lv_textarea_set_placeholder_text(s_passTA, "contrasena...");

    lv_obj_t* eyeBtn = lv_btn_create(s_panel);
    lv_obj_set_size(eyeBtn, 34, 36);
    lv_obj_set_pos(eyeBtn, W - 34, 98);
    lv_obj_add_event_cb(eyeBtn, eyeCb, LV_EVENT_CLICKED, NULL);
    s_passEyeLbl = lv_label_create(eyeBtn);
    lv_label_set_text(s_passEyeLbl, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(s_passEyeLbl);

    // Fila de botones: [Omitir/Cancelar]  [Conectar]
    // W=302, mitad = 149, gap = 4
    const int halfW = (W - 4) / 2;   // 149 px

    lv_obj_t* cancelBtn = lv_btn_create(s_panel);
    lv_obj_set_size(cancelBtn, halfW, 34);
    lv_obj_set_pos(cancelBtn, 0, 144);
    lv_obj_set_style_bg_color(cancelBtn, lv_color_make(60, 60, 60), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(cancelBtn, lv_color_make(90, 90, 90), LV_STATE_PRESSED);
    lv_obj_add_event_cb(cancelBtn, wifiCancelCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancelLbl = lv_label_create(cancelBtn);
    lv_label_set_text(cancelLbl, hasConfig ? "Cancelar" : "Omitir");
    lv_obj_center(cancelLbl);

    s_connectBtn = lv_btn_create(s_panel);
    lv_obj_set_size(s_connectBtn, halfW, 34);
    lv_obj_set_pos(s_connectBtn, halfW + 4, 144);
    lv_obj_add_event_cb(s_connectBtn, connectCb, LV_EVENT_CLICKED, NULL);
    s_connectLbl = lv_label_create(s_connectBtn);
    lv_label_set_text(s_connectLbl, "Conectar");
    lv_obj_center(s_connectLbl);
    s_connectSpinner = lv_spinner_create(s_connectBtn);
    lv_spinner_set_anim_params(s_connectSpinner, 800, 60);
    lv_obj_set_size(s_connectSpinner, 24, 24);
    lv_obj_center(s_connectSpinner);
    lv_obj_add_flag(s_connectSpinner, LV_OBJ_FLAG_HIDDEN);

    // Status label
    s_statusLbl = lv_label_create(s_panel);
    lv_obj_set_width(s_statusLbl, W);
    lv_label_set_long_mode(s_statusLbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_statusLbl, "");
    lv_obj_set_pos(s_statusLbl, 0, 190);

    // Invisible spacer at the bottom so the panel can scroll even when
    // the keyboard is not visible, and provides extra room when it is.
    // Total content bottom edge: 190 + 20 (label) + spacer = ~355 px.
    lv_obj_t* spacer = lv_obj_create(s_panel);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 140);
    lv_obj_set_pos(spacer, 0, 215);
    lv_obj_clear_flag(spacer, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // --- Keyboard (child of scr, NOT s_panel — stays fixed at bottom) ---
    s_kb = lv_keyboard_create(scr);
    lv_obj_set_size(s_kb, 320, 130);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kb, s_passTA);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

    // Show / hide keyboard events
    lv_obj_add_event_cb(s_passTA, passFocusCb,  LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_kb,     wifiKbHide,   LV_EVENT_READY,   NULL);
    lv_obj_add_event_cb(s_kb,     wifiKbHide,   LV_EVENT_CANCEL,  NULL);

    // Synchronous scan (blocks ~2-3 s, spinner shown before the call)
    doScan();

    // Event loop
    while (!s_done) {
        uint32_t now = millis();
        lv_tick_inc(now - s_lvLastTick);
        s_lvLastTick = now;
        lv_timer_handler();
        delay(5);
    }

    bool saved = false;
    if (!s_cancelled) {
        uint16_t idx = lv_dropdown_get_selected(s_dropdown);
        ssid  = WiFi.SSID(idx);
        pass  = String(lv_textarea_get_text(s_passTA));
        saved = true;
    }

    lv_obj_delete(scr);
    return saved;
}

// =======================================================================
// MQTT setup screen
// =======================================================================

static lv_obj_t* sm_hostTA    = nullptr;
static lv_obj_t* sm_portTA    = nullptr;
static lv_obj_t* sm_userTA    = nullptr;
static lv_obj_t* sm_passTA    = nullptr;
static lv_obj_t* sm_kb        = nullptr;
static lv_obj_t* sm_statusLbl = nullptr;
static bool      sm_done      = false;
static bool      sm_skipped   = false;   // Omitir (sin config previa → guarda _skip_)
static bool      sm_cancelled = false;   // Cancelar (config previa → no hace nada)

static void mqttSetStatus(const char* msg) {
    lv_label_set_text(sm_statusLbl, msg);
    lvRun(20);
}

static void mqttSkipCb(lv_event_t*) {
    sm_skipped = true;
    sm_done    = true;
}

static void mqttCancelCb(lv_event_t*) {
    sm_cancelled = true;
    sm_done      = true;
}

static void mqttSaveCb(lv_event_t*) {
    const char* host = lv_textarea_get_text(sm_hostTA);
    const char* port = lv_textarea_get_text(sm_portTA);

    if (strlen(host) == 0) {
        mqttSetStatus("Introduce la IP o nombre del broker");
        return;
    }
    if (strlen(port) == 0) {
        mqttSetStatus("Introduce el puerto (por defecto: 1883)");
        return;
    }

    const char* user = lv_textarea_get_text(sm_userTA);
    const char* pass = lv_textarea_get_text(sm_passTA);

    saveMqttConfig(String(host), String(port), String(user), String(pass));
    mqttSetStatus("Configuracion guardada");
    lvRun(1000);
    sm_done = true;
}

static void mqttAlphaFocusCb(lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(sm_kb, ta);
    lv_keyboard_set_mode(sm_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_flag(sm_kb, LV_OBJ_FLAG_HIDDEN);
}

static void mqttNumFocusCb(lv_event_t*) {
    lv_keyboard_set_textarea(sm_kb, sm_portTA);
    lv_keyboard_set_mode(sm_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_remove_flag(sm_kb, LV_OBJ_FLAG_HIDDEN);
}

static void mqttHideKbCb(lv_event_t*) {
    lv_obj_add_flag(sm_kb, LV_OBJ_FLAG_HIDDEN);
}

bool runMqttSetup(String& uri, String& user, String& pass) {
    sm_done      = false;
    sm_skipped   = false;
    sm_cancelled = false;
    s_lvLastTick = millis();

    // Determinar si ya hay configuración guardada (para el label del botón)
    String existHost, existPort, existUser, existPass;
    bool hasMqttConfig = loadMqttConfig(existHost, existPort, existUser, existPass);

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 5, LV_PART_MAIN);
    lv_screen_load(scr);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "TruMinus - Configuracion MQTT");
    lv_obj_set_width(title, 310);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* brokerLbl = lv_label_create(scr);
    lv_label_set_text(brokerLbl, "Broker:");
    lv_obj_align(brokerLbl, LV_ALIGN_TOP_LEFT, 0, 22);

    sm_hostTA = lv_textarea_create(scr);
    lv_obj_set_size(sm_hostTA, 228, 36);
    lv_textarea_set_one_line(sm_hostTA, true);
    lv_textarea_set_placeholder_text(sm_hostTA, "192.168.1.100");
    lv_obj_align(sm_hostTA, LV_ALIGN_TOP_LEFT, 0, 38);

    lv_obj_t* portLbl = lv_label_create(scr);
    lv_label_set_text(portLbl, "Puerto:");
    lv_obj_align(portLbl, LV_ALIGN_TOP_LEFT, 236, 22);

    sm_portTA = lv_textarea_create(scr);
    lv_obj_set_size(sm_portTA, 74, 36);
    lv_textarea_set_one_line(sm_portTA, true);
    lv_textarea_set_text(sm_portTA, "1883");
    lv_obj_align(sm_portTA, LV_ALIGN_TOP_LEFT, 236, 38);

    lv_obj_t* userLbl = lv_label_create(scr);
    lv_label_set_text(userLbl, "Usuario (opcional):");
    lv_obj_align(userLbl, LV_ALIGN_TOP_LEFT, 0, 80);

    sm_userTA = lv_textarea_create(scr);
    lv_obj_set_size(sm_userTA, 310, 36);
    lv_textarea_set_one_line(sm_userTA, true);
    lv_textarea_set_placeholder_text(sm_userTA, "usuario...");
    lv_obj_align(sm_userTA, LV_ALIGN_TOP_LEFT, 0, 96);

    sm_passTA = lv_textarea_create(scr);
    lv_obj_set_size(sm_passTA, 310, 36);
    lv_textarea_set_one_line(sm_passTA, true);
    lv_textarea_set_password_mode(sm_passTA, true);
    lv_textarea_set_placeholder_text(sm_passTA, "contrasena (opcional)...");
    lv_obj_align(sm_passTA, LV_ALIGN_TOP_LEFT, 0, 134);

    // "Omitir" / "Cancelar" (izquierda, gris oscuro)
    // · Sin config previa → "Omitir"  → guarda sentinel _skip_
    // · Con config previa → "Cancelar" → cierra sin cambios
    lv_obj_t* skipBtn = lv_btn_create(scr);
    lv_obj_set_size(skipBtn, 148, 34);
    lv_obj_align(skipBtn, LV_ALIGN_TOP_LEFT, 0, 172);
    lv_obj_set_style_bg_color(skipBtn, lv_color_make(60, 60, 60), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(skipBtn, lv_color_make(80, 80, 80), LV_STATE_PRESSED);
    lv_obj_add_event_cb(skipBtn,
        hasMqttConfig ? mqttCancelCb : mqttSkipCb,
        LV_EVENT_CLICKED, NULL);
    lv_obj_t* skipLbl = lv_label_create(skipBtn);
    lv_label_set_text(skipLbl, hasMqttConfig ? "Cancelar" : "Omitir");
    lv_obj_set_style_text_color(skipLbl, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_center(skipLbl);

    // "Guardar" (derecha)
    lv_obj_t* saveBtn = lv_btn_create(scr);
    lv_obj_set_size(saveBtn, 148, 34);
    lv_obj_align(saveBtn, LV_ALIGN_TOP_RIGHT, 0, 172);
    lv_obj_add_event_cb(saveBtn, mqttSaveCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, "Guardar");
    lv_obj_center(saveLbl);

    sm_statusLbl = lv_label_create(scr);
    lv_obj_set_width(sm_statusLbl, 310);
    lv_label_set_long_mode(sm_statusLbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(sm_statusLbl, "Introduce los datos del broker MQTT");
    lv_obj_align(sm_statusLbl, LV_ALIGN_TOP_LEFT, 0, 210);

    sm_kb = lv_keyboard_create(scr);
    lv_obj_set_size(sm_kb, 320, 130);
    lv_obj_align(sm_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(sm_kb, sm_hostTA);
    lv_obj_add_flag(sm_kb, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(sm_hostTA, mqttAlphaFocusCb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(sm_portTA, mqttNumFocusCb,   LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(sm_userTA, mqttAlphaFocusCb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(sm_passTA, mqttAlphaFocusCb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(sm_kb,     mqttHideKbCb,     LV_EVENT_READY,   NULL);
    lv_obj_add_event_cb(sm_kb,     mqttHideKbCb,     LV_EVENT_CANCEL,  NULL);

    while (!sm_done) {
        uint32_t now = millis();
        lv_tick_inc(now - s_lvLastTick);
        s_lvLastTick = now;
        lv_timer_handler();
        delay(5);
    }

    bool saved = false;
    if (sm_cancelled) {
        // Config previa existe — no tocamos NVS, no reiniciamos.
        uri = user = pass = "";
    } else if (sm_skipped) {
        // Sin config previa — guardamos sentinel para no volver a preguntar.
        saveMqttConfig("_skip_", "1883", "", "");
        uri = user = pass = "";
    } else {
        String host = String(lv_textarea_get_text(sm_hostTA));
        String port = String(lv_textarea_get_text(sm_portTA));
        user  = String(lv_textarea_get_text(sm_userTA));
        pass  = String(lv_textarea_get_text(sm_passTA));
        uri   = "mqtt://" + host + ":" + port;
        saved = true;
    }

    lv_obj_delete(scr);
    return saved;
}

// =======================================================================
// Victron BLE device scan (called from the Solar config screen)
// Scans 8 s for devices with Victron company ID (0xE1 0x02).
// Shows results in a list; returns the selected normalised MAC (12 uppercase
// hex chars, no colons), or "" if cancelled / no device chosen.
// Must be called while holding the LVGL lock (same as runSolarSetup).
// =======================================================================

struct VictronFound {
    String  mac;
    String  name;
    int16_t rssi;
};

static std::vector<VictronFound> sc_devices;
static SemaphoreHandle_t         sc_mutex        = nullptr;
static String                    sc_pickedMac;
static volatile bool             sc_picked       = false;
static volatile bool             sc_scanAborted  = false;

class VictronSetupScanCb : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (!dev->haveManufacturerData()) return;
        std::string raw = dev->getManufacturerData();
        if (raw.size() < 2) return;
        if ((uint8_t)raw[0] != 0xE1 || (uint8_t)raw[1] != 0x02) return;

        String mac;
        for (char c : dev->getAddress().toString())
            if (c != ':') mac += (char)toupper((unsigned char)c);

        if (!sc_mutex) return;
        xSemaphoreTake(sc_mutex, portMAX_DELAY);
        bool found = false;
        for (auto& d : sc_devices)
            if (d.mac == mac) { d.rssi = (int16_t)dev->getRSSI(); found = true; break; }
        if (!found) {
            VictronFound f;
            f.mac  = mac;
            f.name = dev->haveName() ? String(dev->getName().c_str()) : String("");
            f.rssi = (int16_t)dev->getRSSI();
            sc_devices.push_back(f);
        }
        xSemaphoreGive(sc_mutex);
    }
};

static void scanItemCb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!sc_mutex) return;
    xSemaphoreTake(sc_mutex, portMAX_DELAY);
    if (idx >= 0 && idx < (int)sc_devices.size()) {
        sc_pickedMac = sc_devices[idx].mac;
        sc_picked    = true;
    }
    xSemaphoreGive(sc_mutex);
}

static void scanAbortCb(lv_event_t*) { sc_scanAborted = true; }

static String runSolarScan() {
    sc_devices.clear();
    sc_pickedMac    = "";
    sc_picked       = false;
    sc_scanAborted  = false;
    if (!sc_mutex) sc_mutex = xSemaphoreCreateMutex();

    victronBleSuspend();                  // stop monitoring task if running

    NimBLEDevice::init("");               // idempotent
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->stop();
    scan->setAdvertisedDeviceCallbacks(new VictronSetupScanCb(), /*wantDuplicates=*/true);
    scan->setActiveScan(false);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->clearResults();
    scan->start(8, false);                // 8 s non-blocking in ESP-IDF 3.x

    // ── Build scan screen ────────────────────────────────────────────────
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 4, LV_PART_MAIN);
    lv_screen_load(scr);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "Buscar dispositivos Victron");
    lv_obj_set_width(title, 310);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t* statusLbl = lv_label_create(scr);
    lv_obj_set_width(statusLbl, 310);
    lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(statusLbl, "Buscando... 8 s");
    lv_obj_align(statusLbl, LV_ALIGN_TOP_LEFT, 0, 22);

    lv_obj_t* bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 310, 8);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_bar_set_range(bar, 0, 80);         // 80 × 100 ms = 8 s
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    lv_obj_t* list = lv_list_create(scr);
    lv_obj_set_size(list, 312, 158);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 52);

    lv_obj_t* cancelBtn = lv_btn_create(scr);
    lv_obj_set_size(cancelBtn, 148, 28);
    lv_obj_align(cancelBtn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(cancelBtn, lv_color_make(60, 60, 60), LV_STATE_DEFAULT);
    lv_obj_add_event_cb(cancelBtn, scanAbortCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancelLbl = lv_label_create(cancelBtn);
    lv_label_set_text(cancelLbl, "Cancelar");
    lv_obj_center(cancelLbl);

    // ── Event loop ────────────────────────────────────────────────────────
    uint32_t scanStart = millis();
    int      lastCount = -1;
    bool     scanDone  = false;
    uint32_t lastListUpdate = 0;
    s_lvLastTick = millis();

    while (!sc_picked && !sc_scanAborted) {
        uint32_t now = millis();
        lv_tick_inc(now - s_lvLastTick);
        s_lvLastTick = now;
        lv_timer_handler();

        uint32_t elapsed = now - scanStart;
        lv_bar_set_value(bar, (int32_t)min((uint32_t)80, elapsed / 100), LV_ANIM_OFF);

        if (!scanDone && elapsed >= 8000) {
            scan->stop();
            scanDone = true;
        }

        if (!scanDone) {
            char buf[28];
            uint32_t rem = (elapsed < 8000) ? (8000 - elapsed + 999) / 1000 : 0;
            snprintf(buf, sizeof(buf), "Buscando... %u s", (unsigned)rem);
            lv_label_set_text(statusLbl, buf);
        }

        if (millis() - lastListUpdate > 600 || (scanDone && lastCount < 0)) {
            lastListUpdate = millis();
            xSemaphoreTake(sc_mutex, portMAX_DELAY);
            int cnt = (int)sc_devices.size();
            if (cnt != lastCount) {
                lastCount = cnt;
                lv_obj_clean(list);
                for (int i = 0; i < cnt; i++) {
                    // Last 3 MAC bytes as "XX:XX:XX"
                    const String& m = sc_devices[i].mac;
                    char tail[9];
                    snprintf(tail, sizeof(tail), "%c%c:%c%c:%c%c",
                             m[6],m[7], m[8],m[9], m[10],m[11]);
                    char buf[52];
                    snprintf(buf, sizeof(buf), "%.18s  ...%s  %d dBm",
                             sc_devices[i].name.isEmpty() ? "Victron"
                                                          : sc_devices[i].name.c_str(),
                             tail, (int)sc_devices[i].rssi);
                    lv_obj_t* btn = lv_list_add_button(list, LV_SYMBOL_BLUETOOTH, buf);
                    lv_obj_add_event_cb(btn, scanItemCb, LV_EVENT_CLICKED,
                                        (void*)(intptr_t)i);
                }
                if (scanDone) {
                    if (cnt == 0)
                        lv_label_set_text(statusLbl,
                            "No se encontraron dispositivos Victron");
                    else {
                        char buf[48];
                        snprintf(buf, sizeof(buf),
                                 "%d dispositivo(s) — toca para seleccionar", cnt);
                        lv_label_set_text(statusLbl, buf);
                    }
                }
            }
            xSemaphoreGive(sc_mutex);
        }

        delay(10);
    }

    if (!scanDone) scan->stop();
    scan->clearResults();

    String result = sc_picked ? sc_pickedMac : String("");
    lv_obj_delete(scr);
    return result;
}

// =======================================================================
// Solar (Victron BLE) config screen
// Two text areas: MAC (12 hex chars) and encryption key (32 hex chars).
// Layout (landscape 320×240):
//   title | MAC label | MAC TA | Key label | Key TA | Cancel+Save | status
//   LVGL keyboard fixed at bottom, shown on TA focus.
// =======================================================================

static const char* NVS_SOLAR_NS   = "solar";
static const char* NVS_SOLAR_ADDR = "addr";
static const char* NVS_SOLAR_KEY  = "key";

bool loadSolarConfig(String& addr, String& key) {
    Preferences p;
    p.begin(NVS_SOLAR_NS, true);
    if (!p.isKey(NVS_SOLAR_ADDR)) { p.end(); return false; }
    addr = p.getString(NVS_SOLAR_ADDR, "");
    key  = p.getString(NVS_SOLAR_KEY,  "");
    p.end();
    return addr.length() == 12 && key.length() == 32;
}

void saveSolarConfig(const String& addr, const String& key) {
    Preferences p;
    p.begin(NVS_SOLAR_NS, false);
    p.putString(NVS_SOLAR_ADDR, addr);
    p.putString(NVS_SOLAR_KEY,  key);
    p.end();
}

static lv_obj_t* ss_addrTA       = nullptr;
static lv_obj_t* ss_keyTA        = nullptr;
static lv_obj_t* ss_kb           = nullptr;
static lv_obj_t* ss_statusLbl    = nullptr;
static bool      ss_done         = false;
static bool      ss_cancelled    = false;
static bool      ss_scanRequested = false;

static void solarSetStatus(const char* msg) {
    lv_label_set_text(ss_statusLbl, msg);
    lvRun(20);
}

static void solarCancelCb(lv_event_t*) { ss_cancelled = true; ss_done = true; }
static void solarScanCb(lv_event_t*)   { ss_scanRequested = true; }

static void solarSaveCb(lv_event_t*) {
    const char* addr = lv_textarea_get_text(ss_addrTA);
    const char* key  = lv_textarea_get_text(ss_keyTA);
    if (strlen(addr) != 12) {
        solarSetStatus("Introduce exactamente 12 caracteres hex (sin \":\")");;
        return;
    }
    if (strlen(key) != 32) {
        solarSetStatus("La clave debe tener exactamente 32 caracteres hex");
        return;
    }
    String addrStr(addr); addrStr.toUpperCase();
    String keyStr(key);   keyStr.toUpperCase();
    saveSolarConfig(addrStr, keyStr);
    solarSetStatus("Guardado. Reinicia para aplicar.");
    lvRun(1200);
    ss_done = true;
}

static void solarFocusCb(lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(ss_kb, ta);
    lv_keyboard_set_mode(ss_kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_remove_flag(ss_kb, LV_OBJ_FLAG_HIDDEN);
}

static void solarHideKbCb(lv_event_t*) {
    lv_obj_add_flag(ss_kb, LV_OBJ_FLAG_HIDDEN);
}

bool runSolarSetup(String& addr, String& key) {
    ss_done         = false;
    ss_cancelled    = false;
    ss_scanRequested = false;
    s_lvLastTick    = millis();

    String existAddr, existKey;
    bool hasConfig = loadSolarConfig(existAddr, existKey);

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 5, LV_PART_MAIN);
    lv_screen_load(scr);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "TruMinus - Config. Solar Victron");
    lv_obj_set_width(title, 310);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* addrLbl = lv_label_create(scr);
    lv_label_set_text(addrLbl, "MAC BLE (12 hex, sin \":\" - ej. E4E1D0AABBCC):");
    lv_obj_set_width(addrLbl, 310);
    lv_obj_align(addrLbl, LV_ALIGN_TOP_LEFT, 0, 22);

    ss_addrTA = lv_textarea_create(scr);
    lv_obj_set_size(ss_addrTA, 198, 36);
    lv_textarea_set_one_line(ss_addrTA, true);
    lv_textarea_set_placeholder_text(ss_addrTA, "E4E1D0AABBCC");
    lv_textarea_set_max_length(ss_addrTA, 12);
    lv_obj_align(ss_addrTA, LV_ALIGN_TOP_LEFT, 0, 40);
    if (hasConfig) lv_textarea_set_text(ss_addrTA, existAddr.c_str());

    lv_obj_t* scanBtn = lv_btn_create(scr);
    lv_obj_set_size(scanBtn, 107, 36);
    lv_obj_align(scanBtn, LV_ALIGN_TOP_RIGHT, 0, 40);
    lv_obj_set_style_bg_color(scanBtn, lv_color_make(0, 80, 140), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(scanBtn, lv_color_make(0, 110, 190), LV_STATE_PRESSED);
    lv_obj_add_event_cb(scanBtn, solarScanCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* scanLbl = lv_label_create(scanBtn);
    lv_label_set_text(scanLbl, LV_SYMBOL_REFRESH " Buscar");
    lv_obj_center(scanLbl);

    lv_obj_t* keyLbl = lv_label_create(scr);
    lv_label_set_text(keyLbl, "Clave cifrado (32 hex, ver VictronConnect > info):");
    lv_obj_set_width(keyLbl, 310);
    lv_obj_align(keyLbl, LV_ALIGN_TOP_LEFT, 0, 82);

    ss_keyTA = lv_textarea_create(scr);
    lv_obj_set_size(ss_keyTA, 310, 36);
    lv_textarea_set_one_line(ss_keyTA, true);
    lv_textarea_set_placeholder_text(ss_keyTA, "0123456789ABCDEF...");
    lv_textarea_set_max_length(ss_keyTA, 32);
    lv_obj_align(ss_keyTA, LV_ALIGN_TOP_LEFT, 0, 100);
    if (hasConfig) lv_textarea_set_text(ss_keyTA, existKey.c_str());

    // Cancel / Save
    lv_obj_t* cancelBtn = lv_btn_create(scr);
    lv_obj_set_size(cancelBtn, 148, 34);
    lv_obj_align(cancelBtn, LV_ALIGN_TOP_LEFT, 0, 142);
    lv_obj_set_style_bg_color(cancelBtn, lv_color_make(60, 60, 60), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(cancelBtn, lv_color_make(80, 80, 80), LV_STATE_PRESSED);
    lv_obj_add_event_cb(cancelBtn, solarCancelCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancelLbl = lv_label_create(cancelBtn);
    lv_label_set_text(cancelLbl, hasConfig ? "Cancelar" : "Omitir");
    lv_obj_center(cancelLbl);

    lv_obj_t* saveBtn = lv_btn_create(scr);
    lv_obj_set_size(saveBtn, 148, 34);
    lv_obj_align(saveBtn, LV_ALIGN_TOP_RIGHT, 0, 142);
    lv_obj_add_event_cb(saveBtn, solarSaveCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, "Guardar");
    lv_obj_center(saveLbl);

    ss_statusLbl = lv_label_create(scr);
    lv_obj_set_width(ss_statusLbl, 310);
    lv_label_set_long_mode(ss_statusLbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(ss_statusLbl, "VictronConnect > 3 puntos > Info del producto > Clave cifrado");
    lv_obj_align(ss_statusLbl, LV_ALIGN_TOP_LEFT, 0, 182);

    ss_kb = lv_keyboard_create(scr);
    lv_obj_set_size(ss_kb, 320, 130);
    lv_obj_align(ss_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(ss_kb, ss_addrTA);
    lv_keyboard_set_mode(ss_kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_add_flag(ss_kb, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(ss_addrTA, solarFocusCb,  LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ss_keyTA,  solarFocusCb,  LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ss_kb,     solarHideKbCb, LV_EVENT_READY,   NULL);
    lv_obj_add_event_cb(ss_kb,     solarHideKbCb, LV_EVENT_CANCEL,  NULL);

    while (!ss_done) {
        uint32_t now = millis();
        lv_tick_inc(now - s_lvLastTick);
        s_lvLastTick = now;
        lv_timer_handler();

        if (ss_scanRequested) {
            ss_scanRequested = false;
            String picked = runSolarScan();
            // Restore the solar setup screen
            lv_screen_load(scr);
            s_lvLastTick = millis();
            if (picked.length() > 0)
                lv_textarea_set_text(ss_addrTA, picked.c_str());
        }

        delay(5);
    }

    bool saved = false;
    if (!ss_cancelled) {
        addr  = String(lv_textarea_get_text(ss_addrTA));
        key   = String(lv_textarea_get_text(ss_keyTA));
        saved = true;
    }

    lv_obj_delete(scr);
    return saved;
}

#endif // CYD
