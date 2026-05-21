#include "p4display.hpp"
#include "p4settings.hpp"
#include "i18n.hpp"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "libs/tiny_ttf/lv_tiny_ttf.h"
#include "nvs.h"
#include <stdio.h>
#include <cmath>
#include <string.h>

static const char* TAG = "display";

// Embedded TTFs (declared by ESP-IDF via EMBED_FILES in main/CMakeLists.txt).
extern const uint8_t font_regular_ttf_start[] asm("_binary_font_regular_ttf_start");
extern const uint8_t font_regular_ttf_end[]   asm("_binary_font_regular_ttf_end");
extern const uint8_t font_bold_ttf_start[]    asm("_binary_font_bold_ttf_start");
extern const uint8_t font_bold_ttf_end[]      asm("_binary_font_bold_ttf_end");
extern const uint8_t font_icons_ttf_start[]   asm("_binary_font_icons_ttf_start");
extern const uint8_t font_icons_ttf_end[]     asm("_binary_font_icons_ttf_end");

// Runtime-loaded fonts (Tiny TTF supports full Unicode → Latin chars work).
static lv_font_t* s_font_18      = nullptr;
static lv_font_t* s_font_20      = nullptr;
static lv_font_t* s_font_22      = nullptr;
static lv_font_t* s_font_24      = nullptr;
static lv_font_t* s_font_28      = nullptr;
static lv_font_t* s_font_title   = nullptr;  // bold 26
static lv_font_t* s_font_logo    = nullptr;  // bold 28
static lv_font_t* s_font_splash  = nullptr;  // bold 52 — splash title only
static lv_font_t* s_font_icons22 = nullptr;  // FontAwesome 4.x @ 22
static lv_font_t* s_font_icons24 = nullptr;  // FontAwesome 4.x @ 24
static lv_font_t* s_font_icons36 = nullptr;  // FontAwesome 4.x @ 36 (settings menu)

static P4Fonts s_fonts;

const P4Fonts* p4GetFonts() { return &s_fonts; }

// FontAwesome 4 codepoints (UTF-8 encoded for inline use in label strings).
#define FA_TINT       "\xEF\x81\x83"   // U+F043
#define FA_FIRE       "\xEF\x9F\xA4"   // U+F7E4 (fire-flame-curved)
#define FA_HOUSE_CHIM "\xEE\x8E\xAF"   // U+E3AF (house-chimney)
#define FA_THERM_HALF "\xEF\x8B\x89"   // U+F2C9 (thermometer-half / temperature-half)
#define FA_CHEVRON_L  "\xEF\x81\x93"   // U+F053 (chevron-left)
#define FA_CHEVRON_R  "\xEF\x81\x94"   // U+F054
#define FA_CARET_U    "\xEF\x83\x97"   // U+F0D7 (caret-up — setpoint/fan up)
#define FA_CARET_D    "\xEF\x83\x98"   // U+F0D8 (caret-down — setpoint/fan down)
#define FA_WIFI       "\xEF\x87\xAB"   // U+F1EB
#define FA_RANDOM     "\xEF\x81\xB4"   // U+F074 (random/shuffle in FA4)
#define FA_COG        "\xEF\x80\x93"   // U+F013
#define FA_BOLT        "\xEF\x83\xA7"   // U+F0E7 (lightning, for boost)
#define FA_SIGN_IN    "\xEF\x8B\xB6"   // U+F2F6 (right-to-bracket, outdoor temp arrow)
#define FA_BLUETOOTH  "\xEF\x8A\x93"   // U+F293 (bluetooth brand icon, BLE status)
#define FA_CLOUD      "\xEF\x83\x82"   // U+F0C2 (cloud, Serveo SSH tunnel status)

// ── Layout (800×480 landscape) ────────────────────────────────────────────────
static constexpr int W         = 800;
static constexpr int H         = 480;
static constexpr int TOP_H     = 55;
static constexpr int STATUS_H  = 64;
static constexpr int CONTENT_Y = TOP_H;
static constexpr int CONTENT_H = H - TOP_H - STATUS_H;   // 361

// 3-column × 2-row content grid (columns are not aligned between rows)
static constexpr int ROW1_H  = 185;
static constexpr int ROW2_Y  = CONTENT_Y + ROW1_H;       // 240
static constexpr int ROW2_H  = CONTENT_H - ROW1_H;       // 176

// Row 1: Heating | Water | (battery stub)
static constexpr int HEAT_W  = 301;
static constexpr int WATER_X = 301;
static constexpr int WATER_W = 350;

// Row 2: Fan | Solar | (energy stub)
static constexpr int FAN_W   = 213;
static constexpr int SOLAR_X = 213;
static constexpr int SOLAR_W = 304;

// Vertical bar dimensions
static constexpr int TANK_W        = 64;
static constexpr int TANK_H_WATER  = 118;
static constexpr int TANK_H_BATT   = 101;

// ── Colour palette (matches original CYD aesthetic) ───────────────────────────
#define C_BG          lv_color_hex(0x1a1a2e)
#define C_TOPBAR      lv_color_hex(0x0f0f22)
#define C_SEP         lv_color_hex(0x444466)
#define C_LABEL       lv_color_hex(0x8888aa)
#define C_TEXT        lv_color_hex(0xffffff)
#define C_DIM         lv_color_hex(0xaaccff)
#define C_CYAN        lv_color_hex(0xaaccff)
#define C_CYAN_BR     lv_color_hex(0x88ccff)
#define C_YELLOW      lv_color_hex(0xffdd66)
#define C_AMBER       lv_color_hex(0xffcc88)
#define C_BTN         lv_color_hex(0x2a2a4a)
#define C_BTN_ACTIVE  lv_color_hex(0x3a7bd5)
#define C_HEAT_ON     lv_color_hex(0x1a8a3a)
#define C_GREEN       lv_color_hex(0x44ff44)
#define C_RED         lv_color_hex(0xff4444)
#define C_AMBER_BAR   lv_color_hex(0xffaa00)
#define C_WATER_COLD  lv_color_hex(0x4488ff)
#define C_WATER_WARM  lv_color_hex(0xffaa00)
#define C_WATER_HOT   lv_color_hex(0xff4444)
#define C_BORDER_BAT  lv_color_hex(0x888888)

// ── Local UI state ────────────────────────────────────────────────────────────
static struct {
    float roomSetpoint = 20.0f;
    bool  heatingOn    = false;
    int   fanMode      = 0;
    int   boilerMode   = 0;
    int   energyIdx    = 0;
} st;

// ── Widget handles ────────────────────────────────────────────────────────────
static struct {
    // Top bar
    lv_obj_t* lbl_room_temp;
    lv_obj_t* lbl_outdoor;
    lv_obj_t* icon_wifi;
    lv_obj_t* icon_bt;
    lv_obj_t* icon_cloud;
    lv_obj_t* icon_lin;
    lv_obj_t* icon_tint;   // water (boiler) status indicator
    lv_obj_t* icon_flame;  // heating status indicator
    lv_obj_t* btn_conf;

    // HEATING panel
    lv_obj_t* lbl_room_sp;
    lv_obj_t* btn_sp_dn;
    lv_obj_t* btn_sp_up;
    lv_obj_t* btn_heat;
    lv_obj_t* lbl_btn_heat;
    lv_obj_t* row_sp;

    // FAN panel
    lv_obj_t* btnmx_fan_heat;
    lv_obj_t* btnmx_fan_off;
    lv_obj_t* btn_fan_dn;
    lv_obj_t* btn_fan_up;
    lv_obj_t* lbl_fan_lvl;

    // HOT WATER panel
    lv_obj_t* lbl_water_temp;
    lv_obj_t* bar_water;
    lv_obj_t* btnmx_boiler;

    // SOLAR panel
    lv_obj_t* lbl_solar_status;
    lv_obj_t* lbl_solar_volts;
    lv_obj_t* lbl_solar_current;
    lv_obj_t* lbl_solar_power;
    lv_obj_t* bar_batt;
    lv_obj_t* lbl_batt_soc;

    // Status bar
    lv_obj_t* lbl_conn;
    lv_obj_t* lbl_status;
} ui;

// ── Forward declarations ──────────────────────────────────────────────────────
static void refresh_controls();
static void on_sp_dn(lv_event_t* e);
static void on_sp_up(lv_event_t* e);
static void on_heat_toggle(lv_event_t* e);
static void on_fan_heat_changed(lv_event_t* e);
static void on_fan_off_changed(lv_event_t* e);
static void on_fan_dn(lv_event_t* e);
static void on_fan_up(lv_event_t* e);
static void on_boiler_changed(lv_event_t* e);
static void on_conf_clicked(lv_event_t* e);

// ── Font loader ───────────────────────────────────────────────────────────────
static void load_fonts()
{
    const size_t reg_sz  = font_regular_ttf_end - font_regular_ttf_start;
    const size_t bold_sz = font_bold_ttf_end    - font_bold_ttf_start;
    const size_t ico_sz  = font_icons_ttf_end   - font_icons_ttf_start;
    s_font_18    = lv_tiny_ttf_create_data(font_regular_ttf_start, reg_sz, 18);
    s_font_20    = lv_tiny_ttf_create_data(font_regular_ttf_start, reg_sz, 20);
    s_font_22    = lv_tiny_ttf_create_data(font_regular_ttf_start, reg_sz, 22);
    s_font_24    = lv_tiny_ttf_create_data(font_regular_ttf_start, reg_sz, 24);
    s_font_28    = lv_tiny_ttf_create_data(font_regular_ttf_start, reg_sz, 28);
    s_font_title  = lv_tiny_ttf_create_data(font_bold_ttf_start,    bold_sz, 26);
    s_font_logo   = lv_tiny_ttf_create_data(font_bold_ttf_start,    bold_sz, 28);
    s_font_splash = lv_tiny_ttf_create_data(font_bold_ttf_start,    bold_sz, 52);
    s_font_icons22 = lv_tiny_ttf_create_data(font_icons_ttf_start, ico_sz, 22);
    s_font_icons24 = lv_tiny_ttf_create_data(font_icons_ttf_start, ico_sz, 24);
    s_font_icons36 = lv_tiny_ttf_create_data(font_icons_ttf_start, ico_sz, 36);

    // Set the icon font as fallback so labels can mix Latin text + FA glyphs
    // (e.g. "FA_COG  Conf." renders correctly in a single label).
    if (s_font_18)    s_font_18->fallback    = s_font_icons22;
    if (s_font_20)    s_font_20->fallback    = s_font_icons22;
    if (s_font_22)    s_font_22->fallback    = s_font_icons22;
    if (s_font_24)    s_font_24->fallback    = s_font_icons24;
    if (s_font_28)    s_font_28->fallback    = s_font_icons24;
    if (s_font_title) s_font_title->fallback = s_font_icons24;
    if (s_font_logo)  s_font_logo->fallback  = s_font_icons24;

    s_fonts = { s_font_18, s_font_20, s_font_22, s_font_24, s_font_28,
                s_font_title, s_font_icons22, s_font_icons24, s_font_icons36 };
}

// ── Style helpers ─────────────────────────────────────────────────────────────

static lv_obj_t* make_section(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, C_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t* make_sep(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* s = lv_obj_create(parent);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_size(s, w, h);
    lv_obj_set_style_bg_color(s, C_SEP, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_set_style_radius(s, 0, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static lv_obj_t* make_label(lv_obj_t* parent, const char* text,
                             const lv_font_t* font, lv_color_t color,
                             int x, int y)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}


static void style_button(lv_obj_t* btn)
{
    lv_obj_set_style_bg_color(btn, C_BTN, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 5, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_text_color(btn, C_TEXT, 0);
    lv_obj_set_style_bg_color(btn, C_BTN_ACTIVE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, C_BTN_ACTIVE, LV_STATE_CHECKED);
}

static void style_btnmatrix(lv_obj_t* bm, const lv_font_t* font)
{
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_set_style_pad_gap(bm, 6, 0);

    lv_obj_set_style_bg_color(bm, C_BTN, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(bm, 5, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm, font, LV_PART_ITEMS);

    lv_obj_set_style_bg_color(bm, C_BTN_ACTIVE,
        (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bm, C_BTN_ACTIVE,
        (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(bm, 0, 0);
}

// ── Splash screen ─────────────────────────────────────────────────────────────

static void build_splash(lv_obj_t* scr)
{
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "TruMinus");
    lv_obj_set_style_text_font(title, s_font_splash ? s_font_splash : s_font_28, 0);
    lv_obj_set_style_text_color(title, C_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t* sub = lv_label_create(scr);
    lv_label_set_text(sub, "Iniciando…");
    lv_obj_set_style_text_font(sub, s_font_18, 0);
    lv_obj_set_style_text_color(sub, C_LABEL, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 20);
}

// ── Main screen ───────────────────────────────────────────────────────────────

// Main screen tracking — allows rebuild on language change.
static lv_obj_t* s_main_scr  = nullptr;
static bool      s_mainBuilt = false;

// Btnmatrix maps filled from i18n strings each time build_main_screen() runs.
// Stored as module-level arrays so LVGL's pointer reference stays valid.
static const char* s_fan_heat_map[3] = {};
static const char* s_fan_off_map[3]  = {};
static const char* s_boiler_map[6]   = {};

static void build_main_screen()
{
    // Fill btnmatrix maps with current-language strings.
    s_fan_heat_map[0] = t(TK::FAN_ECO);
    s_fan_heat_map[1] = t(TK::FAN_HIGH);
    s_fan_heat_map[2] = "";
    s_fan_off_map[0]  = t(TK::FAN_OFF);
    s_fan_off_map[1]  = t(TK::ON);
    s_fan_off_map[2]  = "";
    s_boiler_map[0]   = t(TK::FAN_OFF);
    s_boiler_map[1]   = "40\xC2\xB0" "C";
    s_boiler_map[2]   = "\n";
    s_boiler_map[3]   = "60\xC2\xB0" "C";
    s_boiler_map[4]   = "60\xC2\xB0" "C " FA_BOLT;
    s_boiler_map[5]   = "";

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ───────────────────────────────────────────────────────────────
    lv_obj_t* topbar = lv_obj_create(scr);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, W, TOP_H);
    lv_obj_set_style_bg_color(topbar, C_TOPBAR, 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_radius(topbar, 0, 0);
    lv_obj_set_style_pad_all(topbar, 0, 0);
    lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);

    ui.icon_tint = lv_label_create(topbar);
    lv_label_set_text(ui.icon_tint, FA_TINT);
    lv_obj_set_style_text_font(ui.icon_tint, s_font_icons24, 0);
    lv_obj_set_style_text_color(ui.icon_tint, C_CYAN, 0);
    lv_obj_set_style_text_opa(ui.icon_tint, LV_OPA_30, 0);
    lv_obj_align(ui.icon_tint, LV_ALIGN_LEFT_MID, 16, 0);

    ui.icon_flame = lv_label_create(topbar);
    lv_label_set_text(ui.icon_flame, FA_FIRE);
    lv_obj_set_style_text_font(ui.icon_flame, s_font_icons24, 0);
    lv_obj_set_style_text_color(ui.icon_flame, C_AMBER, 0);
    lv_obj_set_style_text_opa(ui.icon_flame, LV_OPA_30, 0);
    lv_obj_align(ui.icon_flame, LV_ALIGN_LEFT_MID, 50, 0);

    lv_obj_t* icon_home = lv_label_create(topbar);
    lv_label_set_text(icon_home, FA_HOUSE_CHIM);
    lv_obj_set_style_text_font(icon_home, s_font_icons24, 0);
    lv_obj_set_style_text_color(icon_home, C_DIM, 0);
    lv_obj_align(icon_home, LV_ALIGN_LEFT_MID, 90, 0);

    ui.lbl_room_temp = lv_label_create(topbar);
    lv_label_set_text(ui.lbl_room_temp, "--°C");
    lv_obj_set_style_text_font(ui.lbl_room_temp, s_font_22, 0);
    lv_obj_set_style_text_color(ui.lbl_room_temp, C_TEXT, 0);
    lv_obj_align(ui.lbl_room_temp, LV_ALIGN_LEFT_MID, 130, 0);

    lv_obj_t* icon_thermo = lv_label_create(topbar);
    lv_label_set_text(icon_thermo, FA_THERM_HALF);
    lv_obj_set_style_text_font(icon_thermo, s_font_icons24, 0);
    lv_obj_set_style_text_color(icon_thermo, C_AMBER, 0);
    lv_obj_align(icon_thermo, LV_ALIGN_LEFT_MID, 221, 0);

    lv_obj_t* icon_chev = lv_label_create(topbar);
    lv_label_set_text(icon_chev, FA_SIGN_IN);
    lv_obj_set_style_text_font(icon_chev, s_font_icons24, 0);
    lv_obj_set_style_text_color(icon_chev, C_AMBER, 0);
    lv_obj_align(icon_chev, LV_ALIGN_LEFT_MID, 236, 0);

    ui.lbl_outdoor = lv_label_create(topbar);
    lv_label_set_text(ui.lbl_outdoor, "--°C");
    lv_obj_set_style_text_font(ui.lbl_outdoor, s_font_22, 0);
    lv_obj_set_style_text_color(ui.lbl_outdoor, C_AMBER, 0);
    lv_obj_align(ui.lbl_outdoor, LV_ALIGN_LEFT_MID, 272, 0);

    ui.icon_lin = lv_label_create(topbar);
    lv_label_set_text(ui.icon_lin, FA_RANDOM);
    lv_obj_set_style_text_font(ui.icon_lin, s_font_icons24, 0);
    lv_obj_set_style_text_color(ui.icon_lin, C_RED, 0);
    lv_obj_align(ui.icon_lin, LV_ALIGN_RIGHT_MID, -16, 0);

    ui.icon_wifi = lv_label_create(topbar);
    lv_label_set_text(ui.icon_wifi, FA_WIFI);
    lv_obj_set_style_text_font(ui.icon_wifi, s_font_icons24, 0);
    lv_obj_set_style_text_color(ui.icon_wifi, C_RED, 0);
    lv_obj_align_to(ui.icon_wifi, ui.icon_lin, LV_ALIGN_OUT_LEFT_MID, -16, 0);

    ui.icon_bt = lv_label_create(topbar);
    lv_label_set_text(ui.icon_bt, FA_BLUETOOTH);
    lv_obj_set_style_text_font(ui.icon_bt, s_font_icons24, 0);
    lv_obj_set_style_text_color(ui.icon_bt, lv_color_hex(0x444466), 0);  // dark grey = not configured
    lv_obj_align_to(ui.icon_bt, ui.icon_wifi, LV_ALIGN_OUT_LEFT_MID, -16, 0);

    ui.icon_cloud = lv_label_create(topbar);
    lv_label_set_text(ui.icon_cloud, FA_CLOUD);
    lv_obj_set_style_text_font(ui.icon_cloud, s_font_icons24, 0);
    lv_obj_set_style_text_color(ui.icon_cloud, lv_color_hex(0x444466), 0);  // dark grey = disconnected
    lv_obj_align_to(ui.icon_cloud, ui.icon_bt, LV_ALIGN_OUT_LEFT_MID, -16, 0);

    ui.btn_conf = lv_button_create(topbar);
    lv_obj_set_size(ui.btn_conf, 158, 40);
    lv_obj_align_to(ui.btn_conf, ui.icon_cloud, LV_ALIGN_OUT_LEFT_MID, -20, 0);
    style_button(ui.btn_conf);
    lv_obj_t* lbl_conf = lv_label_create(ui.btn_conf);
    lv_label_set_text(lbl_conf, FA_COG "  Config");
    lv_obj_set_style_text_font(lbl_conf, s_font_20, 0);
    lv_obj_center(lbl_conf);
    lv_obj_add_event_cb(ui.btn_conf, on_conf_clicked, LV_EVENT_CLICKED, NULL);

    // ── HEATING panel (col1 row1) ─────────────────────────────────────────────
    lv_obj_t* p_heat = make_section(scr, 0, CONTENT_Y, HEAT_W, ROW1_H);

    make_label(p_heat, t(TK::HEATING), s_font_title, C_DIM, 12, 8);

    ui.btn_heat = lv_button_create(p_heat);
    lv_obj_set_size(ui.btn_heat, HEAT_W - 24, 56);
    lv_obj_set_pos(ui.btn_heat, 12, 52);
    lv_obj_add_flag(ui.btn_heat, LV_OBJ_FLAG_CHECKABLE);
    style_button(ui.btn_heat);
    ui.lbl_btn_heat = lv_label_create(ui.btn_heat);
    lv_label_set_text(ui.lbl_btn_heat, "APAGADO");
    lv_obj_set_style_text_font(ui.lbl_btn_heat, s_font_24, 0);
    lv_obj_center(ui.lbl_btn_heat);
    lv_obj_add_event_cb(ui.btn_heat, on_heat_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    ui.row_sp = lv_obj_create(p_heat);
    lv_obj_remove_style_all(ui.row_sp);
    lv_obj_set_pos(ui.row_sp, 12, 115);
    lv_obj_set_size(ui.row_sp, HEAT_W - 24, 60);
    lv_obj_set_style_bg_opa(ui.row_sp, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ui.row_sp, LV_OBJ_FLAG_SCROLLABLE);

    ui.btn_sp_dn = lv_button_create(ui.row_sp);
    lv_obj_set_size(ui.btn_sp_dn, 70, 60);
    lv_obj_set_pos(ui.btn_sp_dn, 0, 0);
    style_button(ui.btn_sp_dn);
    lv_obj_t* l_dn = lv_label_create(ui.btn_sp_dn);
    lv_label_set_text(l_dn, FA_CARET_D);
    lv_obj_set_style_text_font(l_dn, s_font_icons24, 0);
    lv_obj_center(l_dn);
    lv_obj_add_event_cb(ui.btn_sp_dn, on_sp_dn, LV_EVENT_CLICKED, NULL);

    ui.lbl_room_sp = lv_label_create(ui.row_sp);
    lv_label_set_text(ui.lbl_room_sp, "20°C");
    lv_obj_set_style_text_font(ui.lbl_room_sp, s_font_28, 0);
    lv_obj_set_style_text_color(ui.lbl_room_sp, C_TEXT, 0);
    lv_obj_align(ui.lbl_room_sp, LV_ALIGN_CENTER, 0, 0);

    ui.btn_sp_up = lv_button_create(ui.row_sp);
    lv_obj_set_size(ui.btn_sp_up, 70, 60);
    lv_obj_set_pos(ui.btn_sp_up, HEAT_W - 24 - 70, 0);
    style_button(ui.btn_sp_up);
    lv_obj_t* l_up = lv_label_create(ui.btn_sp_up);
    lv_label_set_text(l_up, FA_CARET_U);
    lv_obj_set_style_text_font(l_up, s_font_icons24, 0);
    lv_obj_center(l_up);
    lv_obj_add_event_cb(ui.btn_sp_up, on_sp_up, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(ui.row_sp, LV_OBJ_FLAG_HIDDEN);

    // ── FAN panel (col1 row2) ─────────────────────────────────────────────────
    lv_obj_t* p_fan = make_section(scr, 0, ROW2_Y, FAN_W, ROW2_H);

    make_label(p_fan, t(TK::FAN), s_font_title, C_DIM, 12, 12);

    ui.btnmx_fan_off = lv_buttonmatrix_create(p_fan);
    lv_obj_set_pos(ui.btnmx_fan_off, 12, 54);
    lv_obj_set_size(ui.btnmx_fan_off, FAN_W - 24, 48);
    lv_buttonmatrix_set_map(ui.btnmx_fan_off, s_fan_off_map);
    lv_buttonmatrix_set_button_ctrl_all(ui.btnmx_fan_off, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(ui.btnmx_fan_off, true);
    lv_buttonmatrix_set_selected_button(ui.btnmx_fan_off, 0);
    style_btnmatrix(ui.btnmx_fan_off, s_font_20);
    lv_obj_add_event_cb(ui.btnmx_fan_off, on_fan_off_changed, LV_EVENT_VALUE_CHANGED, NULL);

    ui.btnmx_fan_heat = lv_buttonmatrix_create(p_fan);
    lv_obj_set_pos(ui.btnmx_fan_heat, 12, 54);
    lv_obj_set_size(ui.btnmx_fan_heat, FAN_W - 24, 48);
    lv_buttonmatrix_set_map(ui.btnmx_fan_heat, s_fan_heat_map);
    lv_buttonmatrix_set_button_ctrl_all(ui.btnmx_fan_heat, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(ui.btnmx_fan_heat, true);
    lv_buttonmatrix_set_selected_button(ui.btnmx_fan_heat, 0);
    style_btnmatrix(ui.btnmx_fan_heat, s_font_20);
    lv_obj_add_event_cb(ui.btnmx_fan_heat, on_fan_heat_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(ui.btnmx_fan_heat, LV_OBJ_FLAG_HIDDEN);

    // Fan level: −/+ buttons replace old slider
    ui.btn_fan_dn = lv_button_create(p_fan);
    lv_obj_set_size(ui.btn_fan_dn, 53, 50);
    lv_obj_set_pos(ui.btn_fan_dn, 14, 109);
    style_button(ui.btn_fan_dn);
    lv_obj_t* l_fdn = lv_label_create(ui.btn_fan_dn);
    lv_label_set_text(l_fdn, FA_CARET_D);
    lv_obj_set_style_text_font(l_fdn, s_font_icons24, 0);
    lv_obj_center(l_fdn);
    lv_obj_add_event_cb(ui.btn_fan_dn, on_fan_dn, LV_EVENT_CLICKED, NULL);

    ui.lbl_fan_lvl = make_label(p_fan, "5", s_font_28, C_TEXT, 80, 117);
    lv_obj_set_width(ui.lbl_fan_lvl, 50);
    lv_obj_set_style_text_align(ui.lbl_fan_lvl, LV_TEXT_ALIGN_CENTER, 0);

    ui.btn_fan_up = lv_button_create(p_fan);
    lv_obj_set_size(ui.btn_fan_up, 53, 50);
    lv_obj_set_pos(ui.btn_fan_up, 145, 109);
    style_button(ui.btn_fan_up);
    lv_obj_t* l_fup = lv_label_create(ui.btn_fan_up);
    lv_label_set_text(l_fup, FA_CARET_U);
    lv_obj_set_style_text_font(l_fup, s_font_icons24, 0);
    lv_obj_center(l_fup);
    lv_obj_add_event_cb(ui.btn_fan_up, on_fan_up, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(ui.btn_fan_dn,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.btn_fan_up,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.lbl_fan_lvl, LV_OBJ_FLAG_HIDDEN);

    // ── HOT WATER panel (col2 row1) ───────────────────────────────────────────
    lv_obj_t* p_water = make_section(scr, WATER_X, CONTENT_Y, WATER_W, ROW1_H);

    make_label(p_water, t(TK::HOT_WATER), s_font_title, C_DIM, 12, 8);

    ui.lbl_water_temp = lv_label_create(p_water);
    lv_label_set_text(ui.lbl_water_temp, "--°C");
    lv_obj_set_style_text_font(ui.lbl_water_temp, s_font_22, 0);
    lv_obj_set_style_text_color(ui.lbl_water_temp, C_TEXT, 0);
    lv_obj_set_pos(ui.lbl_water_temp, 267, 15);
    lv_obj_set_width(ui.lbl_water_temp, TANK_W);
    lv_obj_set_style_text_align(ui.lbl_water_temp, LV_TEXT_ALIGN_CENTER, 0);

    ui.bar_water = lv_bar_create(p_water);
    lv_obj_set_pos(ui.bar_water, 267, 49);
    lv_obj_set_size(ui.bar_water, TANK_W, TANK_H_WATER);
    lv_bar_set_range(ui.bar_water, 0, 70);
    lv_bar_set_value(ui.bar_water, 0, LV_ANIM_OFF);
    lv_bar_set_orientation(ui.bar_water, LV_BAR_ORIENTATION_VERTICAL);
    lv_obj_set_style_bg_color(ui.bar_water, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.bar_water, C_WATER_COLD, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.bar_water, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.bar_water, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(ui.bar_water, C_BORDER_BAT, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.bar_water, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.bar_water, 2, 0);

    ui.btnmx_boiler = lv_buttonmatrix_create(p_water);
    lv_obj_set_pos(ui.btnmx_boiler, 12, 53);
    lv_obj_set_size(ui.btnmx_boiler, 234, 116);
    lv_buttonmatrix_set_map(ui.btnmx_boiler, s_boiler_map);
    lv_buttonmatrix_set_button_ctrl_all(ui.btnmx_boiler, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(ui.btnmx_boiler, true);
    lv_buttonmatrix_set_selected_button(ui.btnmx_boiler, 0);
    style_btnmatrix(ui.btnmx_boiler, s_font_20);
    lv_obj_add_event_cb(ui.btnmx_boiler, on_boiler_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // ── SOLAR panel (col2 row2) ───────────────────────────────────────────────
    lv_obj_t* p_sol = make_section(scr, SOLAR_X, ROW2_Y, SOLAR_W, ROW2_H);

    static constexpr int BATT_X = 215;
    static constexpr int BATT_Y = 58;
    static constexpr int NUB_W  = 12, NUB_H = 7;

    make_label(p_sol, "SOLAR", s_font_title, C_DIM, 12, 12);

    ui.lbl_batt_soc = lv_label_create(p_sol);
    lv_label_set_text(ui.lbl_batt_soc, "--%");
    lv_obj_set_style_text_font(ui.lbl_batt_soc, s_font_22, 0);
    lv_obj_set_style_text_color(ui.lbl_batt_soc, C_TEXT, 0);
    lv_obj_set_pos(ui.lbl_batt_soc, BATT_X, 20);
    lv_obj_set_width(ui.lbl_batt_soc, TANK_W);
    lv_obj_set_style_text_align(ui.lbl_batt_soc, LV_TEXT_ALIGN_CENTER, 0);

    {
        char tb[28];
        ui.lbl_solar_status = make_label(p_sol, "--", s_font_22, C_TEXT, 12, 52);
        snprintf(tb, sizeof(tb), "%s --", t(TK::SOLAR_VOLT));
        ui.lbl_solar_volts = make_label(p_sol, tb, s_font_20, C_CYAN, 12, 82);
        snprintf(tb, sizeof(tb), "%s --", t(TK::SOLAR_LOAD));
        ui.lbl_solar_current = make_label(p_sol, tb, s_font_20, C_CYAN_BR, 12, 111);
        snprintf(tb, sizeof(tb), "%s --", t(TK::SOLAR_PROD));
        ui.lbl_solar_power = make_label(p_sol, tb, s_font_20, C_YELLOW, 12, 140);
    }

    auto make_nub = [&](int x) {
        lv_obj_t* n = lv_obj_create(p_sol);
        lv_obj_set_size(n, NUB_W, NUB_H);
        lv_obj_set_pos(n, x, BATT_Y - NUB_H);
        lv_obj_set_style_bg_color(n, C_BORDER_BAT, 0);
        lv_obj_set_style_bg_opa(n, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(n, 0, 0);
        lv_obj_set_style_radius(n, 1, 0);
        lv_obj_clear_flag(n, LV_OBJ_FLAG_SCROLLABLE);
    };
    make_nub(BATT_X + 10);
    make_nub(BATT_X + TANK_W - 10 - NUB_W);

    ui.bar_batt = lv_bar_create(p_sol);
    lv_obj_set_pos(ui.bar_batt, BATT_X, BATT_Y);
    lv_obj_set_size(ui.bar_batt, TANK_W, TANK_H_BATT);
    lv_bar_set_range(ui.bar_batt, 0, 100);
    lv_bar_set_value(ui.bar_batt, 0, LV_ANIM_OFF);
    lv_bar_set_orientation(ui.bar_batt, LV_BAR_ORIENTATION_VERTICAL);
    lv_obj_set_style_bg_color(ui.bar_batt, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.bar_batt, C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.bar_batt, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.bar_batt, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(ui.bar_batt, C_BORDER_BAT, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.bar_batt, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.bar_batt, 2, 0);

    // ── Status bar ────────────────────────────────────────────────────────────
    lv_obj_t* statusbar = lv_obj_create(scr);
    lv_obj_set_pos(statusbar, 0, H - STATUS_H);
    lv_obj_set_size(statusbar, W, STATUS_H);
    lv_obj_set_style_bg_color(statusbar, C_TOPBAR, 0);
    lv_obj_set_style_bg_opa(statusbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(statusbar, 0, 0);
    lv_obj_set_style_radius(statusbar, 0, 0);
    lv_obj_set_style_pad_all(statusbar, 0, 0);
    lv_obj_clear_flag(statusbar, LV_OBJ_FLAG_SCROLLABLE);

    ui.lbl_status = lv_label_create(statusbar);
    lv_label_set_text(ui.lbl_status, t(TK::STATUS_INIT));
    lv_obj_set_style_text_font(ui.lbl_status, s_font_20, 0);
    lv_obj_set_style_text_color(ui.lbl_status, C_LABEL, 0);
    lv_label_set_long_mode(ui.lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ui.lbl_status, W - 200);
    lv_obj_set_pos(ui.lbl_status, 14, 6);

    ui.lbl_conn = lv_label_create(statusbar);
    lv_label_set_text(ui.lbl_conn, t(TK::STATUS_NO_WIFI));
    lv_obj_set_style_text_font(ui.lbl_conn, s_font_18, 0);
    lv_obj_set_style_text_color(ui.lbl_conn, C_LABEL, 0);
    lv_obj_set_pos(ui.lbl_conn, 14, 34);

    lv_obj_t* lbl_logo = lv_label_create(statusbar);
    lv_label_set_text(lbl_logo, "TruMinus");
    lv_obj_set_style_text_font(lbl_logo, s_font_logo, 0);
    lv_obj_set_style_text_color(lbl_logo, C_TEXT, 0);
    lv_obj_align(lbl_logo, LV_ALIGN_RIGHT_MID, -14, 0);

    // ── Separators ────────────────────────────────────────────────────────────
    make_sep(scr, 0,                   TOP_H - 1,    W,       1);   // under top bar
    make_sep(scr, WATER_X,             CONTENT_Y,    1,  ROW1_H);  // heat | water (left)
    make_sep(scr, WATER_X + WATER_W,   CONTENT_Y,    1,  ROW1_H);  // water right edge
    make_sep(scr, SOLAR_X,             ROW2_Y,        1,  ROW2_H);  // fan  | solar (left)
    make_sep(scr, SOLAR_X + SOLAR_W,   ROW2_Y,        1,  ROW2_H);  // solar right edge
    make_sep(scr, 0,                   ROW2_Y - 1,    HEAT_W,  1);  // heat / fan
    make_sep(scr, WATER_X,             ROW2_Y - 1,    WATER_W, 1);  // water / solar
    make_sep(scr, 0,       H - STATUS_H - 1, W,   1);   // above status bar

    lv_screen_load(scr);
    if (s_main_scr) lv_obj_delete(s_main_scr);
    s_main_scr = scr;
    refresh_controls();
}

// ── Refresh widgets from local state ──────────────────────────────────────────

static void refresh_controls()
{
    char buf[20];
    snprintf(buf, sizeof(buf), "%.0f°C", st.roomSetpoint);
    lv_label_set_text(ui.lbl_room_sp, buf);

    if (st.heatingOn) {
        lv_obj_add_state(ui.btn_heat, LV_STATE_CHECKED);
        lv_label_set_text(ui.lbl_btn_heat, t(TK::HEAT_ON));
        lv_obj_set_style_bg_color(ui.btn_heat, C_HEAT_ON, 0);
        lv_obj_remove_flag(ui.row_sp, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_state(ui.btn_heat, LV_STATE_CHECKED);
        lv_label_set_text(ui.lbl_btn_heat, t(TK::HEAT_OFF));
        lv_obj_set_style_bg_color(ui.btn_heat, C_BTN, 0);
        lv_obj_add_flag(ui.row_sp, LV_OBJ_FLAG_HIDDEN);
    }

    if (st.heatingOn) {
        lv_obj_remove_flag(ui.btnmx_fan_heat, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.btnmx_fan_off, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.btn_fan_dn,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.btn_fan_up,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.lbl_fan_lvl, LV_OBJ_FLAG_HIDDEN);

        int sel = (st.fanMode == 1) ? 0 : (st.fanMode == 2) ? 1 : 0;
        for (int i = 0; i < 2; ++i) {
            if (i == sel)
                lv_buttonmatrix_set_button_ctrl(ui.btnmx_fan_heat, i, LV_BUTTONMATRIX_CTRL_CHECKED);
            else
                lv_buttonmatrix_clear_button_ctrl(ui.btnmx_fan_heat, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        }
    } else {
        lv_obj_add_flag(ui.btnmx_fan_heat, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui.btnmx_fan_off, LV_OBJ_FLAG_HIDDEN);

        bool fanOn = (st.fanMode >= 3);
        int sel = fanOn ? 1 : 0;
        for (int i = 0; i < 2; ++i) {
            if (i == sel)
                lv_buttonmatrix_set_button_ctrl(ui.btnmx_fan_off, i, LV_BUTTONMATRIX_CTRL_CHECKED);
            else
                lv_buttonmatrix_clear_button_ctrl(ui.btnmx_fan_off, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        }

        if (fanOn) {
            int lvl = st.fanMode - 2;
            if (lvl < 1)  lvl = 1;
            if (lvl > 10) lvl = 10;
            char b[8];
            snprintf(b, sizeof(b), "%d", lvl);
            lv_label_set_text(ui.lbl_fan_lvl, b);
            lv_obj_remove_flag(ui.btn_fan_dn,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui.btn_fan_up,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui.lbl_fan_lvl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui.btn_fan_dn,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui.btn_fan_up,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui.lbl_fan_lvl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    int bi = (st.boilerMode >= 0 && st.boilerMode <= 3) ? st.boilerMode : 0;
    for (int i = 0; i < 4; ++i) {
        if (i == bi)
            lv_buttonmatrix_set_button_ctrl(ui.btnmx_boiler, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        else
            lv_buttonmatrix_clear_button_ctrl(ui.btnmx_boiler, i, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
}

// ── Event callbacks ───────────────────────────────────────────────────────────

static void on_sp_dn(lv_event_t*)
{
    if (st.roomSetpoint > 5.0f) st.roomSetpoint -= 1.0f;
    refresh_controls();
}

static void on_sp_up(lv_event_t*)
{
    if (st.roomSetpoint < 30.0f) st.roomSetpoint += 1.0f;
    refresh_controls();
}

static void on_heat_toggle(lv_event_t* e)
{
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    st.heatingOn = lv_obj_has_state(btn, LV_STATE_CHECKED);
    if (st.heatingOn && (st.fanMode < 1 || st.fanMode > 2)) st.fanMode = 1;
    if (!st.heatingOn && st.fanMode >= 1 && st.fanMode <= 2) st.fanMode = 0;
    refresh_controls();
}

static void on_fan_heat_changed(lv_event_t* e)
{
    lv_obj_t* bm = (lv_obj_t*)lv_event_get_target(e);
    uint32_t idx = lv_buttonmatrix_get_selected_button(bm);
    if (idx <= 1) {
        st.fanMode = (int)idx + 1;
        refresh_controls();
    }
}

static void on_fan_off_changed(lv_event_t* e)
{
    lv_obj_t* bm = (lv_obj_t*)lv_event_get_target(e);
    uint32_t idx = lv_buttonmatrix_get_selected_button(bm);
    if (idx == 0) {
        st.fanMode = 0;
    } else if (idx == 1) {
        if (st.fanMode < 3) st.fanMode = 7;  // default level 5
    }
    refresh_controls();
}

static void on_fan_dn(lv_event_t*)
{
    if (st.fanMode > 3) { st.fanMode--; refresh_controls(); }
}

static void on_fan_up(lv_event_t*)
{
    if (st.fanMode >= 3 && st.fanMode < 12) { st.fanMode++; refresh_controls(); }
    else if (st.fanMode < 3)               { st.fanMode = 3; refresh_controls(); }
}

static void on_boiler_changed(lv_event_t* e)
{
    lv_obj_t* bm = (lv_obj_t*)lv_event_get_target(e);
    uint32_t idx = lv_buttonmatrix_get_selected_button(bm);
    if (idx <= 3) {
        st.boilerMode = (int)idx;
        refresh_controls();
    }
}

static void on_conf_clicked(lv_event_t*)
{
    p4SettingsShow();
}

// ── Screen timeout ────────────────────────────────────────────────────────────
// Idle thresholds from NVS "display/timeout_idx" (0=30s 1=1min 2=3min 3=never).
// Single source of truth — used by both the boot-time read in p4DisplayInit()
// and the live setter p4SetScreenTimeoutIdx() invoked from the settings UI.
static const uint32_t SCREEN_TIMEOUT_TBL[] = { 30000, 60000, 180000, 0 };

// Dimming starts 7 s before full off; brightness transitions are animated.
//
// Timer runs every 50 ms.
// Dim rate:     2 %/tick  → 100→15 in ~2.1 s
// Restore rate: 10 %/tick → 0→100 in ~0.5 s

static lv_display_t* s_disp             = nullptr;
static uint32_t      s_splash_tick      = 0;    // lv_tick_get() when splash was built
static uint32_t      s_timeout_ms       = 0;    // 0 = never
static int           s_brightness_normal = 100; // normal (awake) brightness
static int           s_brightness        = 100; // current actual brightness
static int           s_target            = 100; // desired brightness
static bool          s_dimmed            = false;
static bool          s_blanked           = false;
static bool          s_waking            = false; // 500 ms lockout after wake
static uint32_t      s_wake_tick         = 0;

// Dim level = 20 % of normal brightness, floor 8.
static int dim_level() { int d = s_brightness_normal / 5; return d < 8 ? 8 : d; }

static void tunnel_icon_timer_cb(lv_timer_t*);   // defined below

static void screen_timeout_cb(lv_timer_t*) {
    if (!s_disp) return;

    // Animate brightness toward target.
    if (s_brightness != s_target) {
        const int step = (s_brightness < s_target) ? 10 : 2;
        const int diff = s_target - s_brightness;
        s_brightness  += (diff > step) ? step : (diff < -step) ? -step : diff;
        bsp_display_brightness_set(s_brightness);
    }

    if (s_timeout_ms == 0) return;

    uint32_t now  = lv_tick_get();
    uint32_t idle = lv_display_get_inactive_time(s_disp);

    // During wake lockout: ignore UI inputs so the waking touch doesn't
    // trigger a button press.  Lockout lasts 500 ms after wake.
    if (s_waking) {
        if ((now - s_wake_tick) >= 500) {
            s_waking = false;
        }
        return;
    }

    // Activity while dimmed/blanked → start wake sequence.
    if (idle < 500 && (s_dimmed || s_blanked)) {
        s_target    = s_brightness_normal;
        s_dimmed    = false;
        s_blanked   = false;
        s_waking    = true;
        s_wake_tick = now;
        return;
    }

    if (!s_blanked && idle >= s_timeout_ms) {
        s_target  = 0;
        s_blanked = true;
        s_dimmed  = false;
    } else if (!s_dimmed && !s_blanked && idle >= s_timeout_ms - 7000) {
        s_target = dim_level();
        s_dimmed = true;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void p4DisplayInit()
{
    ESP_ERROR_CHECK(bsp_i2c_init());

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = 480 * 800,
        .double_buffer = false,
        .flags = {
            .buff_dma    = 0,
            .buff_spiram = 1,
            .sw_rotate   = 1,
        },
    };
    lv_display_t* disp = bsp_display_start_with_config(&cfg);
    if (!disp) {
        ESP_LOGE(TAG, "bsp_display_start_with_config() failed");
        return;
    }
    s_disp = disp;
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    bsp_display_brightness_set(100);

    if (bsp_display_lock(1000)) {
        load_fonts();
        build_splash(lv_screen_active());
        s_splash_tick = lv_tick_get();

        // Screen timeout + brightness — read NVS.
        uint8_t idx   = 3;   // default: never
        uint8_t brite = 100; // default: full brightness
        nvs_handle_t nvs_h;
        if (nvs_open("display", NVS_READONLY, &nvs_h) == ESP_OK) {
            nvs_get_u8(nvs_h, "timeout_idx", &idx);
            nvs_get_u8(nvs_h, "brightness",  &brite);
            nvs_close(nvs_h);
        }
        if (brite < 10) brite = 10;
        s_brightness_normal = brite;
        s_brightness        = brite;
        s_target            = brite;
        bsp_display_brightness_set(brite);

        s_timeout_ms = (idx < 4) ? SCREEN_TIMEOUT_TBL[idx] : 0;
        // Timer runs always (handles brightness animation); only idle logic
        // is gated on s_timeout_ms > 0.
        lv_timer_create(screen_timeout_cb, 50, nullptr);
        // 500 ms repaint cadence for the topbar cloud icon — provides the
        // CONNECTING blink without coupling to main.cpp's loop frequency.
        lv_timer_create(tunnel_icon_timer_cb, 500, nullptr);
        if (s_timeout_ms > 0)
            ESP_LOGI(TAG, "screen timeout: %lu ms, normal brightness: %d%%",
                     (unsigned long)s_timeout_ms, (int)brite);

        bsp_display_unlock();
    }

    ESP_LOGI(TAG, "display ready — 800x480 landscape");
}

void p4SetScreenTimeoutIdx(uint8_t idx)
{
    if (idx > 3) idx = 3;
    s_timeout_ms = SCREEN_TIMEOUT_TBL[idx];

    // If we just disabled the timeout while the screen was already dimmed
    // or blanked, wake it back up — otherwise the user would have to touch
    // the screen even though they just told it never to dim.
    if (s_timeout_ms == 0 && (s_dimmed || s_blanked)) {
        s_target    = s_brightness_normal;
        s_dimmed    = false;
        s_blanked   = false;
        s_waking    = true;
        s_wake_tick = lv_tick_get();
    }

    nvs_handle_t h;
    if (nvs_open("display", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "timeout_idx", idx);
        nvs_commit(h);
        nvs_close(h);
    }
}

void p4SetNormalBrightness(int pct)
{
    if (pct < 10)  pct = 10;
    if (pct > 100) pct = 100;
    s_brightness_normal = pct;
    if (!s_dimmed && !s_blanked) {
        s_target     = pct;
        s_brightness = pct;
        bsp_display_brightness_set(pct);
    }
    nvs_handle_t h;
    if (nvs_open("display", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "brightness", (uint8_t)pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

void p4GetControlState(P4ControlState& out)
{
    out.heatingOn    = st.heatingOn;
    out.fanMode      = st.fanMode;
    out.boilerMode   = st.boilerMode;
    out.energyIdx    = st.energyIdx;
    out.roomSetpoint = st.roomSetpoint;
}

// Remote setters (called from the WS dispatcher).  Take the LVGL lock so the
// st mutation + widget refresh happen atomically with the LVGL refresh task.
// A short timeout keeps a stuck UI from blocking the WS task; on timeout we
// silently drop the write (the next remote update will retry).
void p4SetHeating(bool on)
{
    if (!bsp_display_lock(50)) return;
    st.heatingOn = on;
    // Mirror the on-screen rule: turning heat off clears active heat-fan
    // modes; turning it on defaults to eco if currently in level mode.
    if (st.heatingOn && (st.fanMode < 1 || st.fanMode > 2)) st.fanMode = 1;
    if (!st.heatingOn && st.fanMode >= 1 && st.fanMode <= 2) st.fanMode = 0;
    refresh_controls();
    bsp_display_unlock();
}

void p4SetFanMode(int mode)
{
    if (mode < 0)  mode = 0;
    if (mode > 12) mode = 12;
    if (!bsp_display_lock(50)) return;
    st.fanMode = mode;
    refresh_controls();
    bsp_display_unlock();
}

void p4SetBoilerMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    if (!bsp_display_lock(50)) return;
    st.boilerMode = mode;
    refresh_controls();
    bsp_display_unlock();
}

void p4SetEnergyIdx(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 4) idx = 4;
    if (!bsp_display_lock(50)) return;
    st.energyIdx = idx;
    refresh_controls();
    bsp_display_unlock();
}

void p4SetRoomSetpoint(float celsius)
{
    if (celsius < 5.0f)  celsius = 5.0f;
    if (celsius > 30.0f) celsius = 30.0f;
    if (!bsp_display_lock(50)) return;
    st.roomSetpoint = celsius;
    refresh_controls();
    bsp_display_unlock();
}

// Latest tunnel UI state, written by main loop, read by the LVGL blink
// timer.  uint8 reads/writes are atomic on RISC-V, no lock needed.
static volatile uint8_t s_tunnel_state = 0;

void p4SetTunnelState(uint8_t state) { s_tunnel_state = state; }

// LVGL timer (500 ms period) that repaints the cloud icon based on
// s_tunnel_state.  Decoupling from p4SetTunnelState() lets us blink at a
// fixed cadence regardless of how often main.cpp polls wstunnelUiState().
static void tunnel_icon_timer_cb(lv_timer_t*)
{
    static bool blink_phase = false;
    blink_phase = !blink_phase;
    if (!ui.icon_cloud) return;
    lv_color_t c;
    switch (s_tunnel_state) {
    case 2: // CONNECTED
        c = lv_color_make(70, 131, 210); break;
    case 1: // CONNECTING
        c = blink_phase ? lv_color_make(70, 131, 210)
                        : lv_color_hex(0x444466);
        break;
    case 3: // FAILED
        c = lv_color_hex(0xC03030); break;       // red
    case 0: // DISABLED
    default:
        c = lv_color_hex(0x444466); break;       // dark grey
    }
    lv_obj_set_style_text_color(ui.icon_cloud, c, 0);
}

void p4DisplayRebuild()
{
    s_mainBuilt = false;
}

static const char* translate_solar_status(const char* s)
{
    if (!s || s[0] == '\0') return "--";
    if (strcmp(s, "Off")        == 0) return t(TK::SOL_STATUS_OFF);
    if (strcmp(s, "Low power")  == 0) return t(TK::SOL_STATUS_LOW_POWER);
    if (strcmp(s, "Fault")      == 0) return t(TK::SOL_STATUS_FAULT);
    if (strcmp(s, "Bulk")       == 0) return t(TK::SOL_STATUS_BULK);
    if (strcmp(s, "Absorption") == 0) return t(TK::SOL_STATUS_ABSORB);
    if (strcmp(s, "Float")      == 0) return t(TK::SOL_STATUS_FLOAT);
    return s;
}

void p4DisplayUpdate(const P4DisplayData& d)
{
    static char buf[40];

    if (!bsp_display_lock(50)) return;

    if (!s_mainBuilt) {
        // Enforce minimum 1 s splash before switching to main screen.
        if (lv_tick_get() - s_splash_tick < 2000) {
            bsp_display_unlock();
            return;
        }
        build_main_screen();
        s_mainBuilt = true;
    }

    // st.* is the authoritative control state — only LVGL callbacks write it.
    // Do NOT overwrite it from d here; main.cpp reads it back via p4GetControlState.

    if (!std::isnan(d.roomTemp))
        lv_label_set_text_fmt(ui.lbl_room_temp, "%.1f°C", d.roomTemp);
    else
        lv_label_set_text(ui.lbl_room_temp, "--°C");

    if (!std::isnan(d.outdoorTemp))
        lv_label_set_text_fmt(ui.lbl_outdoor, "%.1f°C", d.outdoorTemp);
    else
        lv_label_set_text(ui.lbl_outdoor, "--°C");

    lv_obj_set_style_text_color(ui.icon_wifi, d.wifiOk ? C_GREEN : C_RED, 0);
    lv_obj_set_style_text_color(ui.icon_lin,  d.linOk  ? C_GREEN : C_RED, 0);

    // Tint (water) and flame (heat) topbar indicators: bright only when LIN
    // is up AND the function is requested.  Without LIN the values reported
    // by the rest of the UI are stale/wishful — keep the icons dim to avoid
    // implying the appliance is actually doing something.  Matches the web
    // refreshIndicators() gating.
    lv_obj_set_style_text_opa(ui.icon_tint,
        (d.linOk && d.boilerMode != 0) ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_obj_set_style_text_opa(ui.icon_flame,
        (d.linOk && d.heatingOn) ? LV_OPA_COVER : LV_OPA_30, 0);

    // BT icon: dark-grey=not configured, red=configured but no data, blue=has data
    {
        lv_color_t btc = (d.bleState >= 2) ? lv_color_hex(0x44aaff)
                       : (d.bleState == 1) ? C_RED
                                           : lv_color_hex(0x444466);
        lv_obj_set_style_text_color(ui.icon_bt, btc, 0);
    }

    // Water tank
    if (!std::isnan(d.waterTemp)) {
        lv_label_set_text_fmt(ui.lbl_water_temp, "%.0f°C", d.waterTemp);
        int wv = (int)d.waterTemp;
        if (wv < 0)  wv = 0;
        if (wv > 70) wv = 70;
        lv_bar_set_value(ui.bar_water, wv, LV_ANIM_ON);
        lv_color_t wc = (d.waterTemp < 30.0f) ? C_WATER_COLD
                      : (d.waterTemp < 51.0f) ? C_WATER_WARM
                                              : C_WATER_HOT;
        lv_obj_set_style_bg_color(ui.bar_water, wc, LV_PART_INDICATOR);
    } else {
        lv_label_set_text(ui.lbl_water_temp, "--°C");
        lv_bar_set_value(ui.bar_water, 0, LV_ANIM_OFF);
    }

    // Solar data — when invalid show "--" everywhere
    if (d.solar.valid) {
        lv_label_set_text(ui.lbl_solar_status, translate_solar_status(d.solar.status));
        snprintf(buf, sizeof(buf), "%s %.1f V", t(TK::SOLAR_VOLT), d.solar.voltageV);
        lv_label_set_text(ui.lbl_solar_volts, buf);
        snprintf(buf, sizeof(buf), "%s %.1f A / %d W", t(TK::SOLAR_LOAD), d.solar.currentA, d.solar.powerW);
        lv_label_set_text(ui.lbl_solar_current, buf);
        snprintf(buf, sizeof(buf), "%s %d W", t(TK::SOLAR_PROD), d.solar.powerW);
        lv_label_set_text(ui.lbl_solar_power, buf);
    } else {
        lv_label_set_text(ui.lbl_solar_status, "--");
        snprintf(buf, sizeof(buf), "%s --", t(TK::SOLAR_VOLT));
        lv_label_set_text(ui.lbl_solar_volts, buf);
        snprintf(buf, sizeof(buf), "%s --", t(TK::SOLAR_LOAD));
        lv_label_set_text(ui.lbl_solar_current, buf);
        snprintf(buf, sizeof(buf), "%s --", t(TK::SOLAR_PROD));
        lv_label_set_text(ui.lbl_solar_power, buf);
    }

    // Battery
    if (d.batt.valid) {
        int soc = d.batt.soc;
        if (soc < 0)   soc = 0;
        if (soc > 100) soc = 100;
        lv_bar_set_value(ui.bar_batt, soc, LV_ANIM_ON);
        lv_color_t bc = (soc < 20) ? C_RED : (soc < 50) ? C_AMBER_BAR : C_GREEN;
        lv_obj_set_style_bg_color(ui.bar_batt, bc, LV_PART_INDICATOR);
        lv_label_set_text_fmt(ui.lbl_batt_soc, "%d%%", soc);
    } else {
        lv_label_set_text(ui.lbl_batt_soc, "--%");
        lv_bar_set_value(ui.bar_batt, 0, LV_ANIM_OFF);
    }

    if (d.wifiOk && d.ssid && d.ip) {
        lv_label_set_text_fmt(ui.lbl_conn, "%s / %s", d.ssid, d.ip);
        lv_obj_set_style_text_color(ui.lbl_conn, C_TEXT, 0);
    } else {
        lv_label_set_text(ui.lbl_conn, "Sin WiFi");
        lv_obj_set_style_text_color(ui.lbl_conn, C_LABEL, 0);
    }

    refresh_controls();

    bsp_display_unlock();
}

void p4DisplaySetStatus(const char* msg, bool isError)
{
    if (!msg) return;
    if (!bsp_display_lock(50)) return;
    if (ui.lbl_status) {
        lv_label_set_text(ui.lbl_status, msg);
        lv_obj_set_style_text_color(ui.lbl_status,
            isError ? C_RED : C_LABEL, 0);
    }
    bsp_display_unlock();
}

// ── OTA progress screen ───────────────────────────────────────────────────────

static lv_obj_t* s_ota_bar     = nullptr;
static lv_obj_t* s_ota_pct_lbl = nullptr;

void p4DisplayShowOtaScreen(const char* from_ver, const char* to_ver)
{
    if (!bsp_display_lock(500)) return;

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Gear icon
    lv_obj_t* icon = lv_label_create(scr);
    lv_label_set_text(icon, FA_COG);
    lv_obj_set_style_text_font(icon, s_font_icons36 ? s_font_icons36 : s_font_28, 0);
    lv_obj_set_style_text_color(icon, C_BTN_ACTIVE, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -130);

    // Title
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "Updating co-processor firmware");
    lv_obj_set_style_text_font(title, s_font_title ? s_font_title : s_font_24, 0);
    lv_obj_set_style_text_color(title, C_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -75);

    // Version line: "v2.3.0 -> v2.12.7"
    char ver_buf[48];
    snprintf(ver_buf, sizeof(ver_buf), "v%s  ->  v%s",
             from_ver ? from_ver : "?", to_ver ? to_ver : "?");
    lv_obj_t* ver_lbl = lv_label_create(scr);
    lv_label_set_text(ver_lbl, ver_buf);
    lv_obj_set_style_text_font(ver_lbl, s_font_18, 0);
    lv_obj_set_style_text_color(ver_lbl, C_LABEL, 0);
    lv_obj_align(ver_lbl, LV_ALIGN_CENTER, 0, -28);

    // Progress bar track
    lv_obj_t* bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 600, 26);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 22);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, C_BTN, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, C_BTN_ACTIVE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    s_ota_bar = bar;

    // Percentage label
    lv_obj_t* pct_lbl = lv_label_create(scr);
    lv_label_set_text(pct_lbl, "0%");
    lv_obj_set_style_text_font(pct_lbl, s_font_20, 0);
    lv_obj_set_style_text_color(pct_lbl, C_TEXT, 0);
    lv_obj_align(pct_lbl, LV_ALIGN_CENTER, 0, 65);
    s_ota_pct_lbl = pct_lbl;

    // Warning
    lv_obj_t* warn = lv_label_create(scr);
    lv_label_set_text(warn, "Do not power off");
    lv_obj_set_style_text_font(warn, s_font_18, 0);
    lv_obj_set_style_text_color(warn, C_YELLOW, 0);
    lv_obj_align(warn, LV_ALIGN_CENTER, 0, 120);

    lv_screen_load(scr);
    bsp_display_unlock();
}

void p4DisplaySetOtaProgress(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    if (!s_ota_bar || !s_ota_pct_lbl) return;
    if (!bsp_display_lock(100)) return;
    lv_bar_set_value(s_ota_bar, percent, LV_ANIM_ON);
    lv_label_set_text_fmt(s_ota_pct_lbl, "%d%%", percent);
    bsp_display_unlock();
}

bool lvglLock(uint32_t timeout_ms) { return bsp_display_lock(timeout_ms); }
void lvglUnlock()                  { bsp_display_unlock(); }
