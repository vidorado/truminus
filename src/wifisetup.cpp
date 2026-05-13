#ifdef CYD
#include "wifisetup.hpp"
#include "i18n.hpp"
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include <Preferences.h>
#include <WiFi.h>
#include <esp32_smartdisplay.h>
#include <vector>
#include <esp_task_wdt.h>
#if defined(BLE)
#include <NimBLEDevice.h>
#endif

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
// Place crosses near the actual screen corners so the linear 3-point fit
// covers the full touch range. Earlier values (40,280 / 40,200) were too
// far from the edges and produced compressed X readings (taps at the right
// edge landed at LVGL x≈225 instead of 320).
static const int32_t CAL_SX[CAL_N] = { 15, 305, 160 };  // screen X targets
static const int32_t CAL_SY[CAL_N] = { 15,  15, 225 };  // screen Y targets

static lv_point_t s_calTouchPts[CAL_N];
static bool       s_calTapped;
static lv_point_t s_calLastPt;

// Un-rotate a point that has been through `lv_display_rotate_point()` so we
// recover the raw panel coordinates as seen by the touch driver BEFORE LVGL's
// indev-pointer-processor applied the display rotation.
//
// Background: in LVGL 9, `indev_pointer_proc()` rotates the touch point AFTER
// `read_cb` returns. That means `lvgl_touch_calibration_transform()` sees the
// RAW pre-rotation point at runtime, but `lv_indev_get_point()` (which we use
// to capture calibration samples from a click event) returns the POST-rotation
// point. Computing the calibration with post-rotation inputs but applying it
// to pre-rotation inputs at runtime is the root cause of the "axis inverted"
// symptom we hit on the CYD_C5.
static lv_point_t unrotateToPanel(lv_point_t pt) {
    lv_display_t* disp = lv_display_get_default();
    if (!disp) return pt;
    // disp->hor_res / ver_res are stored PRE-rotation (panel-native).
    const int32_t orig_hor = lv_display_get_original_horizontal_resolution(disp);
    const int32_t orig_ver = lv_display_get_original_vertical_resolution(disp);
    switch (lv_display_get_rotation(disp)) {
        case LV_DISPLAY_ROTATION_90:  // forward: x'=ver-y-1, y'=x
            return { pt.y, orig_ver - 1 - pt.x };
        case LV_DISPLAY_ROTATION_180: // self-inverse
            return { orig_hor - 1 - pt.x, orig_ver - 1 - pt.y };
        case LV_DISPLAY_ROTATION_270: // forward: x'=y, y'=hor-x-1
            return { orig_hor - 1 - pt.y, pt.x };
        case LV_DISPLAY_ROTATION_0:
        default:
            return pt;
    }
}

static void calTapCb(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_get_act();
    s_calLastPt = {0, 0};
    if (indev) lv_indev_get_point(indev, &s_calLastPt);
    // Reverse LVGL's post-read rotation so the captured point matches what
    // the calibration transform will be fed at runtime (pre-rotation raw).
    s_calLastPt = unrotateToPanel(s_calLastPt);
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
    lv_label_set_text_fmt(lbl, t(TK::TOUCH_CAL_STEP_FMT), step + 1);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 8);

    // Hint
    lv_obj_t* sub = lv_label_create(scr);
    lv_obj_set_style_text_color(sub, lv_color_make(160, 160, 160), LV_PART_MAIN);
    lv_label_set_text(sub, t(TK::TOUCH_CAL_INSTR));
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
    //
    // The cross at landscape (CAL_SX, CAL_SY) is actually rendered onto the
    // physical panel at the PRE-ROTATION pixel — `unrotateToPanel()` gives us
    // that portrait-orientation coordinate. We must map raw_panel touch →
    // portrait_screen, because at runtime LVGL will then rotate the cal output
    // into landscape coords (see indev_pointer_proc).
    lv_point_t sp[3] = {
        unrotateToPanel({CAL_SX[0], CAL_SY[0]}),
        unrotateToPanel({CAL_SX[1], CAL_SY[1]}),
        unrotateToPanel({CAL_SX[2], CAL_SY[2]})
    };
    Serial.printf("[CAL] Landscape targets: (%d,%d) (%d,%d) (%d,%d)\n",
        CAL_SX[0], CAL_SY[0], CAL_SX[1], CAL_SY[1], CAL_SX[2], CAL_SY[2]);
    Serial.printf("[CAL] Portrait targets:  (%d,%d) (%d,%d) (%d,%d)\n",
        sp[0].x, sp[0].y, sp[1].x, sp[1].y, sp[2].x, sp[2].y);
    Serial.printf("[CAL] Panel raw touches: (%d,%d) (%d,%d) (%d,%d)\n",
        s_calTouchPts[0].x, s_calTouchPts[0].y,
        s_calTouchPts[1].x, s_calTouchPts[1].y,
        s_calTouchPts[2].x, s_calTouchPts[2].y);
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
    lv_label_set_text(done, t(TK::TOUCH_CAL_DONE));
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

// Asynchronous scan: starts in background and polls while keeping LVGL alive
// so the spinner continues to animate.
static void doScan() {
    s_netCount = 0;
    lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
    wifiSetStatus(t(TK::WIFI_SCANNING));

    // Reset WiFi to a clean STA state with no auto-reconnect.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true);
    lvRun(300);          // drain pending events while LVGL stays alive
    WiFi.mode(WIFI_STA);
    lvRun(500);          // wait for esp_wifi_start()

    WiFi.scanDelete();
    int n = WiFi.scanNetworks(/*async=*/true);
    Serial.printf("[wifisetup] scanNetworks(async start)=%d\n", n);

    if (n >= 0) {
        // Scan finished immediately (cached results?)
    } else {
        // Poll until scan completes, feeding LVGL so the spinner keeps spinning.
        // Wi-Fi scan can take ~5-8 s; without periodic WDT resets the 10 s
        // task watchdog fires and aborts (we're still inside setup(), so
        // Arduino's automatic reset in loopTask hasn't started yet).
        // Also honour the Cancel/Skip button if the user taps it while
        // scanning — `s_cancelled` is set by wifiCancelCb in the LVGL event.
        while ((n = WiFi.scanComplete()) == -1) {
            lvRun(100);
            esp_task_wdt_reset();
            if (s_cancelled) {
                WiFi.scanDelete();   // abort pending scan
                n = 0;
                break;
            }
        }
    }
    Serial.printf("[wifisetup] scanNetworks(async done)=%d\n", n);

    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);

    s_netCount = (n < 0) ? 0 : n;
    if (s_netCount <= 0) {
        lv_dropdown_set_options(s_dropdown, "---");
        wifiSetStatus(t(TK::WIFI_NO_NETS));
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
    snprintf(msg, sizeof(msg), t(TK::CONNECTING_TO_FMT), ssid.c_str());
    wifiSetStatus(msg);
    lv_obj_add_state(s_connectBtn, LV_STATE_DISABLED);
    lv_label_set_text(s_connectLbl, t(TK::CONNECTING));
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
        lv_label_set_text(s_connectLbl, t(TK::CONNECT));
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
    lv_label_set_text(title, t(TK::WIFI_TITLE));
    lv_obj_set_width(title, W);
    lv_obj_set_pos(title, 0, 0);

    // Network label
    lv_obj_t* netLbl = lv_label_create(s_panel);
    lv_label_set_text(netLbl, t(TK::WIFI_NETWORK));
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
    lv_label_set_text(passLbl, t(TK::PASSWORD));
    lv_obj_set_pos(passLbl, 0, 82);

    // passTA(264) + gap(4) + eyeBtn(34) = 302 px
    s_passTA = lv_textarea_create(s_panel);
    lv_obj_set_size(s_passTA, W - 38, 36);
    lv_obj_set_pos(s_passTA, 0, 98);
    lv_textarea_set_one_line(s_passTA, true);
    lv_textarea_set_password_mode(s_passTA, true);
    lv_textarea_set_placeholder_text(s_passTA, t(TK::PASSWORD_PH));

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
    lv_label_set_text(cancelLbl, hasConfig ? t(TK::CANCEL) : t(TK::SKIP));
    lv_obj_center(cancelLbl);

    s_connectBtn = lv_btn_create(s_panel);
    lv_obj_set_size(s_connectBtn, halfW, 34);
    lv_obj_set_pos(s_connectBtn, halfW + 4, 144);
    lv_obj_add_event_cb(s_connectBtn, connectCb, LV_EVENT_CLICKED, NULL);
    s_connectLbl = lv_label_create(s_connectBtn);
    lv_label_set_text(s_connectLbl, t(TK::CONNECT));
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
        mqttSetStatus(t(TK::MQTT_ENTER_BROKER));
        return;
    }
    if (strlen(port) == 0) {
        mqttSetStatus(t(TK::MQTT_ENTER_PORT));
        return;
    }

    const char* user = lv_textarea_get_text(sm_userTA);
    const char* pass = lv_textarea_get_text(sm_passTA);

    saveMqttConfig(String(host), String(port), String(user), String(pass));
    mqttSetStatus(t(TK::CFG_SAVED));
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
    lv_label_set_text(title, t(TK::MQTT_TITLE));
    lv_obj_set_width(title, 310);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* brokerLbl = lv_label_create(scr);
    lv_label_set_text(brokerLbl, t(TK::MQTT_BROKER));
    lv_obj_align(brokerLbl, LV_ALIGN_TOP_LEFT, 0, 22);

    sm_hostTA = lv_textarea_create(scr);
    lv_obj_set_size(sm_hostTA, 228, 36);
    lv_textarea_set_one_line(sm_hostTA, true);
    lv_textarea_set_placeholder_text(sm_hostTA, "192.168.1.100");
    lv_obj_align(sm_hostTA, LV_ALIGN_TOP_LEFT, 0, 38);

    lv_obj_t* portLbl = lv_label_create(scr);
    lv_label_set_text(portLbl, t(TK::MQTT_PORT));
    lv_obj_align(portLbl, LV_ALIGN_TOP_LEFT, 236, 22);

    sm_portTA = lv_textarea_create(scr);
    lv_obj_set_size(sm_portTA, 74, 36);
    lv_textarea_set_one_line(sm_portTA, true);
    lv_textarea_set_text(sm_portTA, "1883");
    lv_obj_align(sm_portTA, LV_ALIGN_TOP_LEFT, 236, 38);

    lv_obj_t* userLbl = lv_label_create(scr);
    lv_label_set_text(userLbl, t(TK::USER_OPT));
    lv_obj_align(userLbl, LV_ALIGN_TOP_LEFT, 0, 80);

    sm_userTA = lv_textarea_create(scr);
    lv_obj_set_size(sm_userTA, 310, 36);
    lv_textarea_set_one_line(sm_userTA, true);
    lv_textarea_set_placeholder_text(sm_userTA, t(TK::USER_PH));
    lv_obj_align(sm_userTA, LV_ALIGN_TOP_LEFT, 0, 96);

    sm_passTA = lv_textarea_create(scr);
    lv_obj_set_size(sm_passTA, 310, 36);
    lv_textarea_set_one_line(sm_passTA, true);
    lv_textarea_set_password_mode(sm_passTA, true);
    lv_textarea_set_placeholder_text(sm_passTA, t(TK::PASSWORD_OPT_PH));
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
    lv_label_set_text(skipLbl, hasMqttConfig ? t(TK::CANCEL) : t(TK::SKIP));
    lv_obj_set_style_text_color(skipLbl, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_center(skipLbl);

    // "Guardar" (derecha)
    lv_obj_t* saveBtn = lv_btn_create(scr);
    lv_obj_set_size(saveBtn, 148, 34);
    lv_obj_align(saveBtn, LV_ALIGN_TOP_RIGHT, 0, 172);
    lv_obj_add_event_cb(saveBtn, mqttSaveCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, t(TK::SAVE));
    lv_obj_center(saveLbl);

    sm_statusLbl = lv_label_create(scr);
    lv_obj_set_width(sm_statusLbl, 310);
    lv_label_set_long_mode(sm_statusLbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(sm_statusLbl, t(TK::MQTT_INSTR));
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
// BLE Victron + Ultimatron setup screens — only compiled when BLE enabled
// =======================================================================
#if defined(BLE)

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

    Serial.printf("[scan] heap before BLE init: free=%u largest=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    NimBLEDevice::init("");               // idempotent
    Serial.printf("[scan] heap after BLE init:  free=%u\n", ESP.getFreeHeap());
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->stop();
    scan->setAdvertisedDeviceCallbacks(new VictronSetupScanCb(), /*wantDuplicates=*/true);
    scan->setActiveScan(true);    // active scan to receive SCAN_RSP with Instant Readout data
    scan->setInterval(100);
    scan->setWindow(99);
    scan->clearResults();
    scan->start(8, false);                // 8 s non-blocking in ESP-IDF 3.x

    // ── Build scan screen ────────────────────────────────────────────────
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 4, LV_PART_MAIN);
    lv_screen_load(scr);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, t(TK::SCAN_VICTRON));
    lv_obj_set_width(title, 310);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t* statusLbl = lv_label_create(scr);
    lv_obj_set_width(statusLbl, 310);
    lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(statusLbl, t(TK::SCANNING));
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
    lv_label_set_text(cancelLbl, t(TK::CANCEL));
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
        esp_task_wdt_reset();

        uint32_t elapsed = now - scanStart;
        lv_bar_set_value(bar, (int32_t)min((uint32_t)80, elapsed / 100), LV_ANIM_OFF);

        if (!scanDone && elapsed >= 8000) {
            scan->stop();
            scanDone = true;
            // Force re-render of status label even if device count didn't change
            // (otherwise "Buscando... 1 s" stays on screen when 0 devices were found).
            lastCount = -1;
        }

        if (!scanDone) {
            char buf[28];
            uint32_t rem = (elapsed < 8000) ? (8000 - elapsed + 999) / 1000 : 0;
            snprintf(buf, sizeof(buf), t(TK::SCANNING_FMT), (unsigned)rem);
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
                            t(TK::NO_VICTRON_DEVS));
                    else {
                        char buf[48];
                        snprintf(buf, sizeof(buf),
                                 t(TK::DEVICES_FOUND_FMT), cnt);
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

#endif // BLE

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

// Forward declarations (defined after runSolarSetup)
bool loadBattConfig(String& addr);
void saveBattConfig(const String& addr);

static lv_obj_t* ss_addrTA            = nullptr;
static lv_obj_t* ss_keyTA             = nullptr;
static lv_obj_t* ss_battAddrTA        = nullptr;
static lv_obj_t* ss_contentPanel      = nullptr;
static lv_obj_t* ss_kb                = nullptr;
static lv_obj_t* ss_statusLbl         = nullptr;
static bool      ss_done              = false;
static bool      ss_cancelled         = false;
static bool      ss_scanRequested     = false;
static bool      ss_battScanRequested = false;

static void solarSetStatus(const char* msg) {
    lv_label_set_text(ss_statusLbl, msg);
    lvRun(20);
}

static void solarCancelCb(lv_event_t*)      { ss_cancelled = true; ss_done = true; }
static void solarScanCb(lv_event_t*)        { ss_scanRequested = true; }
static void solarBattScanCb(lv_event_t*)    { ss_battScanRequested = true; }

static void solarSaveCb(lv_event_t*) {
    const char* addr     = lv_textarea_get_text(ss_addrTA);
    const char* key      = lv_textarea_get_text(ss_keyTA);
    const char* battAddr = lv_textarea_get_text(ss_battAddrTA);
    if (strlen(addr) != 12) {
        solarSetStatus("MAC Victron (12 hex sin \":\"):");
        return;
    }
    if (strlen(key) != 32) {
        solarSetStatus(t(TK::VICTRON_KEY_PROMPT));
        return;
    }
    size_t battLen = strlen(battAddr);
    if (battLen != 0 && battLen != 12) {
        solarSetStatus("MAC Bat. Ultimatron (12 hex sin \":\":");
        return;
    }
    String addrStr(addr);     addrStr.toUpperCase();
    String keyStr(key);       keyStr.toUpperCase();
    saveSolarConfig(addrStr, keyStr);
    if (battLen == 12) {
        String battStr(battAddr); battStr.toUpperCase();
        saveBattConfig(battStr);
    }
    solarSetStatus("Guardado. Reinicia para aplicar.");
    lvRun(1200);
    ss_done = true;
}

static void solarFocusCb(lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(ss_kb, ta);
    lv_keyboard_set_mode(ss_kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_remove_flag(ss_kb, LV_OBJ_FLAG_HIDDEN);
    // Shrink panel so keyboard doesn't cover it; scroll focused TA into view
    lv_obj_set_height(ss_contentPanel, 90);
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

static void solarHideKbCb(lv_event_t*) {
    lv_obj_add_flag(ss_kb, LV_OBJ_FLAG_HIDDEN);
    if (ss_contentPanel) lv_obj_set_height(ss_contentPanel, 220);
}

bool runSolarSetup(String& addr, String& key) {
    ss_done              = false;
    ss_cancelled         = false;
    ss_scanRequested     = false;
    ss_battScanRequested = false;
    s_lvLastTick         = millis();

    String existAddr, existKey;
    bool hasConfig = loadSolarConfig(existAddr, existKey);

    String existBattAddr;
    bool hasBattConfig = loadBattConfig(existBattAddr);

    // Screen: no padding, non-scrollable
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scr, (lv_obj_flag_t)LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(scr);

    // Fixed title at top
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, t(TK::BLE_TITLE));
    lv_obj_set_width(title, 320);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 3);

    // Scrollable content panel (shrinks to 90 when keyboard opens)
    ss_contentPanel = lv_obj_create(scr);
    lv_obj_set_pos(ss_contentPanel, 0, 20);
    lv_obj_set_size(ss_contentPanel, 320, 220);
    lv_obj_set_style_pad_left (ss_contentPanel, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_right(ss_contentPanel, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_top  (ss_contentPanel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(ss_contentPanel, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(ss_contentPanel, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ss_contentPanel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(ss_contentPanel, (lv_obj_flag_t)LV_OBJ_FLAG_SCROLLABLE);

    // ── Victron Solar ─────────────────────────────────────────────
    // Layout y-positions (font heights: montserrat_20≈22px, montserrat_14≈18px)
    //   0  : "Victron Solar" title (22px)
    //  26  : addr label (18px)
    //  46  : addr TA / scan btn (36px)
    //  86  : key label (18px)
    // 106  : key TA (36px)
    // 148  : "Bateria Ultimatron" title (22px)
    // 174  : batt addr label (18px)
    // 194  : batt addr TA / batt scan btn (36px)
    // 236  : Cancel / Save buttons (34px) — scroll panel to see
    // 274  : status label
    lv_obj_t* solarSecLbl = lv_label_create(ss_contentPanel);
    lv_obj_set_style_text_font(solarSecLbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(solarSecLbl, "Victron Solar");
    lv_obj_set_pos(solarSecLbl, 0, 0);

    lv_obj_t* addrLbl = lv_label_create(ss_contentPanel);
    lv_label_set_text(addrLbl, t(TK::MAC_BLE_LABEL));
    lv_obj_set_width(addrLbl, 310);
    lv_obj_set_pos(addrLbl, 0, 26);

    ss_addrTA = lv_textarea_create(ss_contentPanel);
    lv_obj_set_size(ss_addrTA, 198, 36);
    lv_textarea_set_one_line(ss_addrTA, true);
    lv_textarea_set_placeholder_text(ss_addrTA, "D8AC8D2C49FA");
    lv_textarea_set_max_length(ss_addrTA, 12);
    lv_obj_set_pos(ss_addrTA, 0, 46);
    if (hasConfig) lv_textarea_set_text(ss_addrTA, existAddr.c_str());

#if defined(BLE)
    lv_obj_t* scanBtn = lv_btn_create(ss_contentPanel);
    lv_obj_set_size(scanBtn, 107, 36);
    lv_obj_set_pos(scanBtn, 203, 46);
    lv_obj_set_style_bg_color(scanBtn, lv_color_make(0, 80, 140), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(scanBtn, lv_color_make(0, 110, 190), LV_STATE_PRESSED);
    lv_obj_add_event_cb(scanBtn, solarScanCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* scanLbl = lv_label_create(scanBtn);
    lv_label_set_text_fmt(scanLbl, LV_SYMBOL_REFRESH " %s", t(TK::SEARCH));
    lv_obj_center(scanLbl);
#endif

    lv_obj_t* keyLbl = lv_label_create(ss_contentPanel);
    lv_label_set_text(keyLbl, t(TK::ENC_KEY_LABEL));
    lv_obj_set_width(keyLbl, 310);
    lv_obj_set_pos(keyLbl, 0, 86);

    ss_keyTA = lv_textarea_create(ss_contentPanel);
    lv_obj_set_size(ss_keyTA, 310, 36);
    lv_textarea_set_one_line(ss_keyTA, true);
    lv_textarea_set_placeholder_text(ss_keyTA, "0123456789ABCDEF...");
    lv_textarea_set_max_length(ss_keyTA, 32);
    lv_obj_set_pos(ss_keyTA, 0, 106);
    if (hasConfig) lv_textarea_set_text(ss_keyTA, existKey.c_str());

    // ── Bateria Ultimatron ───────────────────────────────────
    lv_obj_t* battSecLbl = lv_label_create(ss_contentPanel);
    lv_obj_set_style_text_font(battSecLbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(battSecLbl, t(TK::BATT_SECTION));
    lv_obj_set_pos(battSecLbl, 0, 148);

    lv_obj_t* battAddrLbl = lv_label_create(ss_contentPanel);
    lv_label_set_text(battAddrLbl, t(TK::MAC_BLE_LABEL));
    lv_obj_set_width(battAddrLbl, 310);
    lv_obj_set_pos(battAddrLbl, 0, 174);

    ss_battAddrTA = lv_textarea_create(ss_contentPanel);
    lv_obj_set_size(ss_battAddrTA, 198, 36);
    lv_textarea_set_one_line(ss_battAddrTA, true);
    lv_textarea_set_placeholder_text(ss_battAddrTA, t(TK::BATT_MAC_PH));
    lv_textarea_set_max_length(ss_battAddrTA, 12);
    lv_obj_set_pos(ss_battAddrTA, 0, 194);
    if (hasBattConfig) lv_textarea_set_text(ss_battAddrTA, existBattAddr.c_str());

#if defined(BLE)
    lv_obj_t* battScanBtn = lv_btn_create(ss_contentPanel);
    lv_obj_set_size(battScanBtn, 107, 36);
    lv_obj_set_pos(battScanBtn, 203, 194);
    lv_obj_set_style_bg_color(battScanBtn, lv_color_make(0, 80, 140), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(battScanBtn, lv_color_make(0, 110, 190), LV_STATE_PRESSED);
    lv_obj_add_event_cb(battScanBtn, solarBattScanCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* battScanLbl = lv_label_create(battScanBtn);
    lv_label_set_text_fmt(battScanLbl, LV_SYMBOL_REFRESH " %s", t(TK::SEARCH));
    lv_obj_center(battScanLbl);
#endif

    // Cancel / Save
    lv_obj_t* cancelBtn = lv_btn_create(ss_contentPanel);
    lv_obj_set_size(cancelBtn, 148, 34);
    lv_obj_set_pos(cancelBtn, 0, 236);
    lv_obj_set_style_bg_color(cancelBtn, lv_color_make(60, 60, 60), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(cancelBtn, lv_color_make(80, 80, 80), LV_STATE_PRESSED);
    lv_obj_add_event_cb(cancelBtn, solarCancelCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancelLbl = lv_label_create(cancelBtn);
    lv_label_set_text(cancelLbl, hasConfig ? t(TK::CANCEL) : t(TK::SKIP));
    lv_obj_center(cancelLbl);

    lv_obj_t* saveBtn = lv_btn_create(ss_contentPanel);
    lv_obj_set_size(saveBtn, 148, 34);
    lv_obj_set_pos(saveBtn, 162, 236);
    lv_obj_add_event_cb(saveBtn, solarSaveCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, t(TK::SAVE));
    lv_obj_center(saveLbl);

    ss_statusLbl = lv_label_create(ss_contentPanel);
    lv_obj_set_width(ss_statusLbl, 310);
    lv_label_set_long_mode(ss_statusLbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ss_statusLbl, t(TK::BATT_INFO));
    lv_obj_set_pos(ss_statusLbl, 0, 274);

    // Keyboard: direct child of scr (not panel), fixed at bottom
    ss_kb = lv_keyboard_create(scr);
    lv_obj_set_size(ss_kb, 320, 130);
    lv_obj_align(ss_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(ss_kb, ss_addrTA);
    lv_keyboard_set_mode(ss_kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_add_flag(ss_kb, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(ss_addrTA,     solarFocusCb,  LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ss_keyTA,      solarFocusCb,  LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ss_battAddrTA, solarFocusCb,  LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ss_kb,         solarHideKbCb, LV_EVENT_READY,   NULL);
    lv_obj_add_event_cb(ss_kb,         solarHideKbCb, LV_EVENT_CANCEL,  NULL);

    while (!ss_done) {
        uint32_t now = millis();
        lv_tick_inc(now - s_lvLastTick);
        s_lvLastTick = now;
        lv_timer_handler();
        esp_task_wdt_reset();   // user may take >10 s to enter MAC/key

        if (ss_scanRequested || ss_battScanRequested) {
            // Free the keyboard (~16 KB) so the scan screen fits in the LVGL pool.
            bool forBatt = ss_battScanRequested;
            ss_scanRequested = ss_battScanRequested = false;
            if (ss_kb) { lv_obj_delete(ss_kb); ss_kb = nullptr; }
            if (ss_contentPanel) lv_obj_set_height(ss_contentPanel, 220);

            String picked;
#if defined(BLE)
            picked = forBatt ? runBattScan() : runSolarScan();
#endif

            // Rebuild keyboard now that scan screen is gone.
            ss_kb = lv_keyboard_create(scr);
            lv_obj_set_size(ss_kb, 320, 130);
            lv_obj_align(ss_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_keyboard_set_textarea(ss_kb, ss_addrTA);
            lv_keyboard_set_mode(ss_kb, LV_KEYBOARD_MODE_TEXT_UPPER);
            lv_obj_add_flag(ss_kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_event_cb(ss_kb, solarHideKbCb, LV_EVENT_READY,  NULL);
            lv_obj_add_event_cb(ss_kb, solarHideKbCb, LV_EVENT_CANCEL, NULL);

            lv_screen_load(scr);
            s_lvLastTick = millis();
            if (picked.length() > 0) {
                if (forBatt) lv_textarea_set_text(ss_battAddrTA, picked.c_str());
                else         lv_textarea_set_text(ss_addrTA,     picked.c_str());
            }
        }

        delay(5);
    }

    bool saved = false;
    if (!ss_cancelled) {
        addr  = String(lv_textarea_get_text(ss_addrTA));
        key   = String(lv_textarea_get_text(ss_keyTA));
        saved = true;
    }

    ss_contentPanel = nullptr;
    lv_obj_delete(scr);
    return saved;
}

#if defined(BLE)
// =======================================================================
// Battery (Ultimatron BLE) device scan
// Scans 8 s for all BLE devices so the user can find their battery by name/MAC.
// Must be called while holding the LVGL lock.
// =======================================================================

struct BattFound {
    String  mac;
    String  name;
    int16_t rssi;
};

static std::vector<BattFound> bc_devices;
static SemaphoreHandle_t      bc_mutex       = nullptr;
static String                 bc_pickedMac;
static volatile bool          bc_picked      = false;
static volatile bool          bc_scanAborted = false;

class BattSetupScanCb : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        String mac;
        for (char c : dev->getAddress().toString())
            if (c != ':') mac += (char)toupper((unsigned char)c);

        if (!bc_mutex) return;
        xSemaphoreTake(bc_mutex, portMAX_DELAY);
        bool found = false;
        for (auto& d : bc_devices)
            if (d.mac == mac) { d.rssi = (int16_t)dev->getRSSI(); found = true; break; }
        if (!found) {
            BattFound f;
            f.mac  = mac;
            f.name = dev->haveName() ? String(dev->getName().c_str()) : String("");
            f.rssi = (int16_t)dev->getRSSI();
            bc_devices.push_back(f);
        }
        xSemaphoreGive(bc_mutex);
    }
};

static void battScanItemCb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!bc_mutex) return;
    xSemaphoreTake(bc_mutex, portMAX_DELAY);
    if (idx >= 0 && idx < (int)bc_devices.size()) {
        bc_pickedMac = bc_devices[idx].mac;
        bc_picked    = true;
    }
    xSemaphoreGive(bc_mutex);
}

static void battScanAbortCb(lv_event_t*) { bc_scanAborted = true; }

static String runBattScan() {
    bc_devices.clear();
    bc_pickedMac   = "";
    bc_picked      = false;
    bc_scanAborted = false;
    if (!bc_mutex) bc_mutex = xSemaphoreCreateMutex();

    victronBleSuspend();
    ultimatronBleSuspend();

    NimBLEDevice::init("");  // idempotent
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->stop();
    scan->setAdvertisedDeviceCallbacks(new BattSetupScanCb(), /*wantDuplicates=*/false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->clearResults();
    scan->start(8, false);

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 4, LV_PART_MAIN);
    lv_screen_load(scr);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, t(TK::SCAN_BATT));
    lv_obj_set_width(title, 310);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t* statusLbl = lv_label_create(scr);
    lv_obj_set_width(statusLbl, 310);
    lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(statusLbl, t(TK::SCANNING));
    lv_obj_align(statusLbl, LV_ALIGN_TOP_LEFT, 0, 22);

    lv_obj_t* bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 310, 8);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_bar_set_range(bar, 0, 80);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    lv_obj_t* list = lv_list_create(scr);
    lv_obj_set_size(list, 312, 158);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 52);

    lv_obj_t* cancelBtn = lv_btn_create(scr);
    lv_obj_set_size(cancelBtn, 148, 28);
    lv_obj_align(cancelBtn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(cancelBtn, lv_color_make(60, 60, 60), LV_STATE_DEFAULT);
    lv_obj_add_event_cb(cancelBtn, battScanAbortCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancelLbl = lv_label_create(cancelBtn);
    lv_label_set_text(cancelLbl, t(TK::CANCEL));
    lv_obj_center(cancelLbl);

    uint32_t scanStart      = millis();
    int      lastCount      = -1;
    bool     scanDone       = false;
    uint32_t lastListUpdate = 0;
    s_lvLastTick = millis();

    while (!bc_picked && !bc_scanAborted) {
        uint32_t now = millis();
        lv_tick_inc(now - s_lvLastTick);
        s_lvLastTick = now;
        lv_timer_handler();
        esp_task_wdt_reset();

        uint32_t elapsed = now - scanStart;
        lv_bar_set_value(bar, (int32_t)min((uint32_t)80, elapsed / 100), LV_ANIM_OFF);

        if (!scanDone && elapsed >= 8000) {
            scan->stop();
            scanDone = true;
            lastCount = -1;  // force status-label re-render (see runSolarScan)
        }

        if (!scanDone) {
            char buf[28];
            uint32_t rem = (elapsed < 8000) ? (8000 - elapsed + 999) / 1000 : 0;
            snprintf(buf, sizeof(buf), t(TK::SCANNING_FMT), (unsigned)rem);
            lv_label_set_text(statusLbl, buf);
        }

        if (millis() - lastListUpdate > 600 || (scanDone && lastCount < 0)) {
            lastListUpdate = millis();
            xSemaphoreTake(bc_mutex, portMAX_DELAY);
            int cnt = (int)bc_devices.size();
            if (cnt != lastCount) {
                lastCount = cnt;
                lv_obj_clean(list);
                for (int i = 0; i < cnt; i++) {
                    const String& m = bc_devices[i].mac;
                    char tail[9];
                    snprintf(tail, sizeof(tail), "%c%c:%c%c:%c%c",
                             m[6],m[7], m[8],m[9], m[10],m[11]);
                    char buf[52];
                    snprintf(buf, sizeof(buf), "%.18s  ...%s  %d dBm",
                             bc_devices[i].name.isEmpty() ? t(TK::UNNAMED)
                                                          : bc_devices[i].name.c_str(),
                             tail, (int)bc_devices[i].rssi);
                    lv_obj_t* btn = lv_list_add_button(list, LV_SYMBOL_BLUETOOTH, buf);
                    lv_obj_add_event_cb(btn, battScanItemCb, LV_EVENT_CLICKED,
                                        (void*)(intptr_t)i);
                }
                if (scanDone) {
                    if (cnt == 0)
                        lv_label_set_text(statusLbl, t(TK::NO_BLE_DEVS));
                    else {
                        char buf[48];
                        snprintf(buf, sizeof(buf),
                                 t(TK::DEVICES_FOUND_FMT), cnt);
                        lv_label_set_text(statusLbl, buf);
                    }
                }
            }
            xSemaphoreGive(bc_mutex);
        }

        delay(10);
    }

    if (!scanDone) scan->stop();
    scan->clearResults();

    String result = bc_picked ? bc_pickedMac : String("");
    lv_obj_delete(scr);

    ultimatronBleResume();
    victronBleResume();
    return result;
}

#endif // BLE

// =======================================================================
// Battery (Ultimatron BLE) config screen
// One text area: MAC address (12 hex chars) + scan + cancel/save.
// =======================================================================

static const char* NVS_BATT_NS   = "batt";
static const char* NVS_BATT_ADDR = "addr";

bool loadBattConfig(String& addr) {
    Preferences p;
    p.begin(NVS_BATT_NS, true);
    if (!p.isKey(NVS_BATT_ADDR)) { p.end(); return false; }
    addr = p.getString(NVS_BATT_ADDR, "");
    p.end();
    return addr.length() == 12;
}

void saveBattConfig(const String& addr) {
    Preferences p;
    p.begin(NVS_BATT_NS, false);
    p.putString(NVS_BATT_ADDR, addr);
    p.end();
}

#endif // CYD
