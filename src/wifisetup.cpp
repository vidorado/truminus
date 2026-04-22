#ifdef CYD
#include "wifisetup.hpp"
#include <Preferences.h>
#include <WiFi.h>
#include <esp32_smartdisplay.h>

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
static bool      s_passVis        = false;
static bool      s_scanning       = false;
static int       s_netCount       = 0;

static void wifiSetStatus(const char* msg) {
    lv_label_set_text(s_statusLbl, msg);
    lvRun(20);
}

static void startAsyncScan() {
    s_scanning = true;
    s_netCount = 0;
    lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
    wifiSetStatus("Buscando redes WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.scanNetworks(/*async=*/true);
}

static void checkScanResult() {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return; // still going

    s_scanning = false;
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);

    s_netCount = (n < 0) ? 0 : (int)n;
    if (s_netCount <= 0) {
        lv_dropdown_set_options(s_dropdown, "---");
        wifiSetStatus("No se encontraron redes.");
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
}

static void connectCb(lv_event_t*) {
    if (s_scanning) {
        wifiSetStatus("Espera, buscando redes...");
        return;
    }
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

void runWifiSetup(String& ssid, String& pass) {
    s_done     = false;
    s_netCount = 0;
    s_passVis  = false;
    s_lvLastTick = millis();

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
    lv_obj_set_size(s_spinner, 30, 30);
    lv_obj_set_pos(s_spinner, 0, 38);

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

    // Connect button (ancho completo)
    s_connectBtn = lv_btn_create(s_panel);
    lv_obj_set_size(s_connectBtn, W, 36);
    lv_obj_set_pos(s_connectBtn, 0, 144);
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

    // Start network scan automatically
    startAsyncScan();

    // Event loop
    while (!s_done) {
        uint32_t now = millis();
        lv_tick_inc(now - s_lvLastTick);
        s_lvLastTick = now;
        lv_timer_handler();
        if (s_scanning) checkScanResult();
        delay(5);
    }

    uint16_t idx = lv_dropdown_get_selected(s_dropdown);
    ssid = WiFi.SSID(idx);
    pass = String(lv_textarea_get_text(s_passTA));

    lv_obj_delete(scr);
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
static bool      sm_skipped   = false;

static void mqttSetStatus(const char* msg) {
    lv_label_set_text(sm_statusLbl, msg);
    lvRun(20);
}

static void mqttSkipCb(lv_event_t*) {
    sm_skipped = true;
    sm_done    = true;
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

void runMqttSetup(String& uri, String& user, String& pass) {
    sm_done      = false;
    sm_skipped   = false;
    s_lvLastTick = millis();

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

    // "Omitir" (izquierda, gris oscuro)
    lv_obj_t* skipBtn = lv_btn_create(scr);
    lv_obj_set_size(skipBtn, 148, 34);
    lv_obj_align(skipBtn, LV_ALIGN_TOP_LEFT, 0, 172);
    lv_obj_set_style_bg_color(skipBtn, lv_color_make(60, 60, 60), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(skipBtn, lv_color_make(80, 80, 80), LV_STATE_PRESSED);
    lv_obj_add_event_cb(skipBtn, mqttSkipCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* skipLbl = lv_label_create(skipBtn);
    lv_label_set_text(skipLbl, "Omitir");
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

    if (sm_skipped) {
        // Fuera del callback LVGL ya es seguro escribir NVS.
        // Guardamos host vacío para que loadMqttConfig() devuelva true
        // en el siguiente arranque y no vuelva a mostrar esta pantalla.
        saveMqttConfig("_skip_", "1883", "", ""); // sentinel: MQTT omitido
        uri  = "";
        user = "";
        pass = "";
    } else {
        String host = String(lv_textarea_get_text(sm_hostTA));
        String port = String(lv_textarea_get_text(sm_portTA));
        user = String(lv_textarea_get_text(sm_userTA));
        pass = String(lv_textarea_get_text(sm_passTA));
        uri  = "mqtt://" + host + ":" + port;
    }

    lv_obj_delete(scr);
}

#endif // CYD
