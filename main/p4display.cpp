#include "p4display.hpp"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char* TAG = "display";

// ── Layout (800×480 landscape) ────────────────────────────────────────────────
static constexpr int W         = 800;
static constexpr int H         = 480;
static constexpr int TOP_H     = 55;
static constexpr int STATUS_H  = 38;
static constexpr int CONTENT_Y = TOP_H;
static constexpr int CONTENT_H = H - TOP_H - STATUS_H;   // 387

// Left column: CALEFACCION + VENTILADOR
static constexpr int LEFT_W  = 370;
static constexpr int HEAT_H  = 210;
static constexpr int FAN_Y   = CONTENT_Y + HEAT_H;
static constexpr int FAN_H   = CONTENT_H - HEAT_H;       // 177

// Right column: AGUA CALIENTE + CARGA SOLAR
static constexpr int RIGHT_X  = LEFT_W + 1;
static constexpr int RIGHT_W  = W - LEFT_W - 1;          // 429
static constexpr int WATER_H  = 200;
static constexpr int SOLAR_Y  = CONTENT_Y + WATER_H;
static constexpr int SOLAR_H  = CONTENT_H - WATER_H;     // 187

// ── Colour palette (blue/navy — matches original CYD aesthetic) ───────────────
#define C_BG          lv_color_hex(0x09111e)
#define C_PANEL       lv_color_hex(0x0d1829)
#define C_BORDER      lv_color_hex(0x1a4878)
#define C_ACCENT      lv_color_hex(0x2980b9)
#define C_TEXT        lv_color_hex(0xffffff)
#define C_DIM         lv_color_hex(0x6fa8c8)
#define C_BTN         lv_color_hex(0x12233d)
#define C_BTN_ACTIVE  lv_color_hex(0x1565c0)
#define C_GREEN       lv_color_hex(0x27ae60)
#define C_RED         lv_color_hex(0xe74c3c)
#define C_AMBER       lv_color_hex(0xf39c12)
#define C_WATER_COLD  lv_color_hex(0x2e86c1)
#define C_WATER_WARM  lv_color_hex(0xf39c12)
#define C_WATER_HOT   lv_color_hex(0xe74c3c)

// ── Local UI state ────────────────────────────────────────────────────────────
static struct {
    float roomSetpoint = 20.0f;
    bool  heatingOn    = false;
    int   fanMode      = 0;   // 0=off  1=eco  2=high  3..12=level 1..10
    int   boilerMode   = 0;   // 0=off  1=40°C  2=60°C  3=boost
    int   energyIdx    = 0;
} st;

// ── Widget handles ────────────────────────────────────────────────────────────
static struct {
    // Top bar
    lv_obj_t* lbl_room_temp;
    lv_obj_t* lbl_outdoor;
    lv_obj_t* dot_wifi;
    lv_obj_t* dot_lin;
    lv_obj_t* btn_conf;

    // CALEFACCION panel
    lv_obj_t* lbl_room_sp;
    lv_obj_t* btn_sp_dn;
    lv_obj_t* btn_sp_up;
    lv_obj_t* btn_heat;
    lv_obj_t* lbl_btn_heat;

    // VENTILADOR panel
    lv_obj_t* btnmx_fan;
    lv_obj_t* slider_fan_lvl;
    lv_obj_t* lbl_fan_lvl;

    // AGUA CALIENTE panel
    lv_obj_t* lbl_water_temp;
    lv_obj_t* bar_water;
    lv_obj_t* btnmx_boiler;

    // CARGA SOLAR panel
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
static void on_fan_changed(lv_event_t* e);
static void on_fan_lvl_changed(lv_event_t* e);
static void on_boiler_changed(lv_event_t* e);
static void on_conf_clicked(lv_event_t* e);

// ── Style helpers ─────────────────────────────────────────────────────────────

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, C_PANEL, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, C_BORDER, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_radius(p, 4, 0);
    lv_obj_set_style_pad_all(p, 10, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
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

static lv_obj_t* make_section_label(lv_obj_t* parent, const char* text, int y)
{
    return make_label(parent, text, &lv_font_montserrat_16, C_DIM, 0, y);
}

static lv_obj_t* make_status_dot(lv_obj_t* parent)
{
    lv_obj_t* d = lv_obj_create(parent);
    lv_obj_set_size(d, 14, 14);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, C_RED, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    return d;
}

static void style_button(lv_obj_t* btn)
{
    lv_obj_set_style_bg_color(btn, C_BTN, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, C_ACCENT, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_text_color(btn, C_TEXT, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1e3a5f), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, C_BTN_ACTIVE, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btn, C_ACCENT, LV_STATE_CHECKED);
}

static void style_btnmatrix(lv_obj_t* bm, const lv_font_t* font)
{
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_set_style_pad_gap(bm, 5, 0);

    lv_obj_set_style_bg_color(bm, C_BTN, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(bm, C_ACCENT, LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(bm, 4, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm, font, LV_PART_ITEMS);

    lv_obj_set_style_bg_color(bm, C_BTN_ACTIVE,
        (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_set_style_border_color(bm, C_ACCENT,
        (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bm, lv_color_hex(0x1e3a5f),
        (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_PRESSED);
}

// ── Splash screen ─────────────────────────────────────────────────────────────

static void build_splash(lv_obj_t* scr)
{
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "TruMinus");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, C_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t* sub = lv_label_create(scr);
    lv_label_set_text(sub, "Iniciando...");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(sub, C_DIM, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 20);
}

// ── Main screen ───────────────────────────────────────────────────────────────

// Fan: Apag. / Eco / Alto
static const char* FAN_MAP[] = { "Apag.", "Eco", "Alto", "" };

// Boiler: 2x2 grid  Off/40°C on row 1, 60°C/Boost on row 2
static const char* BOILER_MAP[] = {
    "Apag.", "40\xc2\xb0""C",
    "\n",
    "60\xc2\xb0""C", "Boost",
    ""
};

static void build_main_screen()
{
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ───────────────────────────────────────────────────────────────
    lv_obj_t* topbar = lv_obj_create(scr);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, W, TOP_H);
    lv_obj_set_style_bg_color(topbar, C_PANEL, 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(topbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(topbar, C_BORDER, 0);
    lv_obj_set_style_border_width(topbar, 2, 0);
    lv_obj_set_style_radius(topbar, 0, 0);
    lv_obj_set_style_pad_all(topbar, 0, 0);
    lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);

    // Room temperature (left side)
    ui.lbl_room_temp = lv_label_create(topbar);
    lv_label_set_text(ui.lbl_room_temp, "Hab: --.-\xc2\xb0""C");
    lv_obj_set_style_text_font(ui.lbl_room_temp, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(ui.lbl_room_temp, C_TEXT, 0);
    lv_obj_align(ui.lbl_room_temp, LV_ALIGN_LEFT_MID, 14, 0);

    // Arrow + outdoor temp
    lv_obj_t* lbl_arrow = lv_label_create(topbar);
    lv_label_set_text(lbl_arrow, ">");
    lv_obj_set_style_text_font(lbl_arrow, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_arrow, C_DIM, 0);
    lv_obj_align(lbl_arrow, LV_ALIGN_LEFT_MID, 228, 0);

    ui.lbl_outdoor = lv_label_create(topbar);
    lv_label_set_text(ui.lbl_outdoor, "--.-\xc2\xb0""C");
    lv_obj_set_style_text_font(ui.lbl_outdoor, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(ui.lbl_outdoor, C_ACCENT, 0);
    lv_obj_align(ui.lbl_outdoor, LV_ALIGN_LEFT_MID, 248, 0);

    // Right side — chain from right to left: dot_wifi, lbl_wifi, dot_lin, lbl_lin, btn_conf
    ui.dot_wifi = make_status_dot(topbar);
    lv_obj_align(ui.dot_wifi, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_t* lbl_wifi = lv_label_create(topbar);
    lv_label_set_text(lbl_wifi, "WiFi");
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_wifi, C_DIM, 0);
    lv_obj_align_to(lbl_wifi, ui.dot_wifi, LV_ALIGN_OUT_LEFT_MID, -5, 0);

    ui.dot_lin = make_status_dot(topbar);
    lv_obj_align_to(ui.dot_lin, lbl_wifi, LV_ALIGN_OUT_LEFT_MID, -12, 0);

    lv_obj_t* lbl_lin = lv_label_create(topbar);
    lv_label_set_text(lbl_lin, "LIN");
    lv_obj_set_style_text_font(lbl_lin, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_lin, C_DIM, 0);
    lv_obj_align_to(lbl_lin, ui.dot_lin, LV_ALIGN_OUT_LEFT_MID, -5, 0);

    ui.btn_conf = lv_button_create(topbar);
    lv_obj_set_size(ui.btn_conf, 96, 38);
    lv_obj_align_to(ui.btn_conf, lbl_lin, LV_ALIGN_OUT_LEFT_MID, -14, 0);
    style_button(ui.btn_conf);
    lv_obj_t* lbl_conf = lv_label_create(ui.btn_conf);
    lv_label_set_text(lbl_conf, "Conf.");
    lv_obj_set_style_text_font(lbl_conf, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl_conf);
    lv_obj_add_event_cb(ui.btn_conf, on_conf_clicked, LV_EVENT_CLICKED, NULL);

    // Vertical separator between left and right columns
    lv_obj_t* vsep = lv_obj_create(scr);
    lv_obj_set_pos(vsep, LEFT_W, CONTENT_Y);
    lv_obj_set_size(vsep, 1, CONTENT_H);
    lv_obj_set_style_bg_color(vsep, C_BORDER, 0);
    lv_obj_set_style_bg_opa(vsep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vsep, 0, 0);

    // ── CALEFACCION panel ─────────────────────────────────────────────────────
    lv_obj_t* p_heat = make_panel(scr, 0, CONTENT_Y, LEFT_W, HEAT_H);
    const int hi = LEFT_W - 20;   // inner width (pad 10 each side)

    make_section_label(p_heat, "CALEFACCION", 0);

    // Setpoint row: [–]  20°C  [+]
    ui.btn_sp_dn = lv_button_create(p_heat);
    lv_obj_set_size(ui.btn_sp_dn, 56, 50);
    lv_obj_set_pos(ui.btn_sp_dn, 0, 28);
    style_button(ui.btn_sp_dn);
    lv_obj_t* l_dn = lv_label_create(ui.btn_sp_dn);
    lv_label_set_text(l_dn, "-");
    lv_obj_set_style_text_font(l_dn, &lv_font_montserrat_28, 0);
    lv_obj_center(l_dn);
    lv_obj_add_event_cb(ui.btn_sp_dn, on_sp_dn, LV_EVENT_CLICKED, NULL);

    ui.lbl_room_sp = lv_label_create(p_heat);
    lv_label_set_text(ui.lbl_room_sp, "20\xc2\xb0""C");
    lv_obj_set_style_text_font(ui.lbl_room_sp, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ui.lbl_room_sp, C_ACCENT, 0);
    lv_obj_align(ui.lbl_room_sp, LV_ALIGN_TOP_MID, 0, 38);

    ui.btn_sp_up = lv_button_create(p_heat);
    lv_obj_set_size(ui.btn_sp_up, 56, 50);
    lv_obj_set_pos(ui.btn_sp_up, hi - 56, 28);
    style_button(ui.btn_sp_up);
    lv_obj_t* l_up = lv_label_create(ui.btn_sp_up);
    lv_label_set_text(l_up, "+");
    lv_obj_set_style_text_font(l_up, &lv_font_montserrat_28, 0);
    lv_obj_center(l_up);
    lv_obj_add_event_cb(ui.btn_sp_up, on_sp_up, LV_EVENT_CLICKED, NULL);

    // Heating ON/OFF button — full width, prominent
    ui.btn_heat = lv_button_create(p_heat);
    lv_obj_set_size(ui.btn_heat, hi, 80);
    lv_obj_set_pos(ui.btn_heat, 0, 90);
    lv_obj_add_flag(ui.btn_heat, LV_OBJ_FLAG_CHECKABLE);
    style_button(ui.btn_heat);
    lv_obj_set_style_border_width(ui.btn_heat, 2, 0);
    ui.lbl_btn_heat = lv_label_create(ui.btn_heat);
    lv_label_set_text(ui.lbl_btn_heat, "APAGADO");
    lv_obj_set_style_text_font(ui.lbl_btn_heat, &lv_font_montserrat_24, 0);
    lv_obj_center(ui.lbl_btn_heat);
    lv_obj_add_event_cb(ui.btn_heat, on_heat_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    // ── VENTILADOR panel ──────────────────────────────────────────────────────
    lv_obj_t* p_fan = make_panel(scr, 0, FAN_Y, LEFT_W, FAN_H);
    const int fi = LEFT_W - 20;

    make_section_label(p_fan, "VENTILADOR", 0);

    ui.btnmx_fan = lv_buttonmatrix_create(p_fan);
    lv_obj_set_pos(ui.btnmx_fan, 0, 26);
    lv_obj_set_size(ui.btnmx_fan, fi, 56);
    lv_buttonmatrix_set_map(ui.btnmx_fan, FAN_MAP);
    lv_buttonmatrix_set_button_ctrl_all(ui.btnmx_fan, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(ui.btnmx_fan, true);
    lv_buttonmatrix_set_selected_button(ui.btnmx_fan, 0);
    style_btnmatrix(ui.btnmx_fan, &lv_font_montserrat_20);
    lv_obj_add_event_cb(ui.btnmx_fan, on_fan_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Fan level (visible only when fanMode >= 3)
    make_section_label(p_fan, "Nivel (1-10)", 94);
    ui.slider_fan_lvl = lv_slider_create(p_fan);
    lv_obj_set_pos(ui.slider_fan_lvl, 0, 118);
    lv_obj_set_size(ui.slider_fan_lvl, fi - 56, 22);
    lv_slider_set_range(ui.slider_fan_lvl, 1, 10);
    lv_slider_set_value(ui.slider_fan_lvl, 1, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui.slider_fan_lvl, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.slider_fan_lvl, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui.slider_fan_lvl, C_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(ui.slider_fan_lvl, on_fan_lvl_changed, LV_EVENT_VALUE_CHANGED, NULL);
    ui.lbl_fan_lvl = make_label(p_fan, "1", &lv_font_montserrat_22, C_TEXT, fi - 32, 113);

    // ── AGUA CALIENTE panel ───────────────────────────────────────────────────
    lv_obj_t* p_water = make_panel(scr, RIGHT_X, CONTENT_Y, RIGHT_W, WATER_H);
    const int wi = RIGHT_W - 20;

    // Title row: "AGUA CALIENTE" left, water temp right
    make_section_label(p_water, "AGUA CALIENTE", 2);
    ui.lbl_water_temp = lv_label_create(p_water);
    lv_label_set_text(ui.lbl_water_temp, "--\xc2\xb0""C");
    lv_obj_set_style_text_font(ui.lbl_water_temp, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(ui.lbl_water_temp, C_ACCENT, 0);
    lv_obj_align(ui.lbl_water_temp, LV_ALIGN_TOP_RIGHT, 0, 0);

    // Horizontal water temperature bar
    ui.bar_water = lv_bar_create(p_water);
    lv_obj_set_pos(ui.bar_water, 0, 30);
    lv_obj_set_size(ui.bar_water, wi, 16);
    lv_bar_set_range(ui.bar_water, 0, 70);
    lv_bar_set_value(ui.bar_water, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui.bar_water, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.bar_water, C_WATER_COLD, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.bar_water, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.bar_water, 3, LV_PART_INDICATOR);

    make_section_label(p_water, "Modo", 56);

    // Boiler mode — 2×2 button matrix: Apag./40°C on row 1, 60°C/Boost on row 2
    ui.btnmx_boiler = lv_buttonmatrix_create(p_water);
    lv_obj_set_pos(ui.btnmx_boiler, 0, 78);
    lv_obj_set_size(ui.btnmx_boiler, wi, 88);
    lv_buttonmatrix_set_map(ui.btnmx_boiler, BOILER_MAP);
    lv_buttonmatrix_set_button_ctrl_all(ui.btnmx_boiler, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(ui.btnmx_boiler, true);
    lv_buttonmatrix_set_selected_button(ui.btnmx_boiler, 0);
    style_btnmatrix(ui.btnmx_boiler, &lv_font_montserrat_20);
    lv_obj_add_event_cb(ui.btnmx_boiler, on_boiler_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // ── CARGA SOLAR panel ─────────────────────────────────────────────────────
    lv_obj_t* p_sol = make_panel(scr, RIGHT_X, SOLAR_Y, RIGHT_W, SOLAR_H);
    const int si = RIGHT_W - 20;
    const int batt_w  = 46;
    const int batt_h  = 120;
    const int batt_x  = si - batt_w;

    // Title row: "CARGA SOLAR" left, SOC % right
    make_section_label(p_sol, "CARGA SOLAR", 2);
    ui.lbl_batt_soc = lv_label_create(p_sol);
    lv_label_set_text(ui.lbl_batt_soc, "--%");
    lv_obj_set_style_text_font(ui.lbl_batt_soc, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(ui.lbl_batt_soc, C_TEXT, 0);
    lv_obj_align(ui.lbl_batt_soc, LV_ALIGN_TOP_RIGHT, 0, 0);

    // Solar data lines (left portion, leaving room for battery bar on right)
    const int data_w = si - batt_w - 14;
    ui.lbl_solar_status  = make_label(p_sol, "--",              &lv_font_montserrat_16, C_ACCENT, 0, 28);
    ui.lbl_solar_volts   = make_label(p_sol, "Volt.: --",       &lv_font_montserrat_16, C_TEXT,   0, 54);
    ui.lbl_solar_current = make_label(p_sol, "Carga: -- / --",  &lv_font_montserrat_16, C_TEXT,   0, 78);
    ui.lbl_solar_power   = make_label(p_sol, "Prod.: --",       &lv_font_montserrat_16, C_TEXT,   0, 102);
    (void)data_w;

    // Vertical battery bar (right side, fills bottom to top)
    ui.bar_batt = lv_bar_create(p_sol);
    lv_obj_set_pos(ui.bar_batt, batt_x, 28);
    lv_obj_set_size(ui.bar_batt, batt_w, batt_h);
    lv_bar_set_range(ui.bar_batt, 0, 100);
    lv_bar_set_value(ui.bar_batt, 0, LV_ANIM_OFF);
    lv_bar_set_orientation(ui.bar_batt, LV_BAR_ORIENTATION_VERTICAL);
    lv_obj_set_style_bg_color(ui.bar_batt, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.bar_batt, C_GREEN,  LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.bar_batt, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.bar_batt, 4, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(ui.bar_batt, C_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.bar_batt, 1, LV_PART_MAIN);

    // ── Status bar ────────────────────────────────────────────────────────────
    lv_obj_t* statusbar = lv_obj_create(scr);
    lv_obj_set_pos(statusbar, 0, H - STATUS_H);
    lv_obj_set_size(statusbar, W, STATUS_H);
    lv_obj_set_style_bg_color(statusbar, C_PANEL, 0);
    lv_obj_set_style_bg_opa(statusbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(statusbar, 2, 0);
    lv_obj_set_style_border_side(statusbar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(statusbar, C_BORDER, 0);
    lv_obj_set_style_radius(statusbar, 0, 0);
    lv_obj_set_style_pad_all(statusbar, 0, 0);
    lv_obj_clear_flag(statusbar, LV_OBJ_FLAG_SCROLLABLE);

    ui.lbl_conn = lv_label_create(statusbar);
    lv_label_set_text(ui.lbl_conn, "TruMinus P4");
    lv_obj_set_style_text_font(ui.lbl_conn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui.lbl_conn, C_DIM, 0);
    lv_obj_align(ui.lbl_conn, LV_ALIGN_LEFT_MID, 12, 0);

    ui.lbl_status = lv_label_create(statusbar);
    lv_label_set_text(ui.lbl_status, "Iniciando...");
    lv_obj_set_style_text_font(ui.lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui.lbl_status, C_DIM, 0);
    lv_obj_align(ui.lbl_status, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_screen_load(scr);
    refresh_controls();
}

// ── Refresh widgets from local state ──────────────────────────────────────────

static void refresh_controls()
{
    // Setpoint label
    char buf[20];
    snprintf(buf, sizeof(buf), "%.0f\xc2\xb0""C", st.roomSetpoint);
    lv_label_set_text(ui.lbl_room_sp, buf);

    // Heat toggle
    if (st.heatingOn) {
        lv_obj_add_state(ui.btn_heat, LV_STATE_CHECKED);
        lv_label_set_text(ui.lbl_btn_heat, "ENCENDIDO");
    } else {
        lv_obj_remove_state(ui.btn_heat, LV_STATE_CHECKED);
        lv_label_set_text(ui.lbl_btn_heat, "APAGADO");
    }

    // Fan matrix (modes 0-2 map to buttons 0-2; modes 3-12 = levels)
    if (st.fanMode >= 0 && st.fanMode <= 2) {
        for (int i = 0; i < 3; ++i) {
            if (i == st.fanMode)
                lv_buttonmatrix_set_button_ctrl(ui.btnmx_fan, i, LV_BUTTONMATRIX_CTRL_CHECKED);
            else
                lv_buttonmatrix_clear_button_ctrl(ui.btnmx_fan, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        }
        lv_obj_add_flag(ui.slider_fan_lvl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.lbl_fan_lvl,    LV_OBJ_FLAG_HIDDEN);
    } else {
        for (int i = 0; i < 3; ++i)
            lv_buttonmatrix_clear_button_ctrl(ui.btnmx_fan, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        int lvl = st.fanMode - 2;
        if (lvl < 1)  lvl = 1;
        if (lvl > 10) lvl = 10;
        lv_slider_set_value(ui.slider_fan_lvl, lvl, LV_ANIM_OFF);
        lv_obj_clear_flag(ui.slider_fan_lvl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui.lbl_fan_lvl,    LV_OBJ_FLAG_HIDDEN);
        char b[8];
        snprintf(b, sizeof(b), "%d", lvl);
        lv_label_set_text(ui.lbl_fan_lvl, b);
    }

    // Boiler matrix (indices 0-3 match BOILER_MAP button order)
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
    ESP_LOGI(TAG, "setpoint -> %.0f°C", st.roomSetpoint);
    refresh_controls();
}

static void on_sp_up(lv_event_t*)
{
    if (st.roomSetpoint < 30.0f) st.roomSetpoint += 1.0f;
    ESP_LOGI(TAG, "setpoint -> %.0f°C", st.roomSetpoint);
    refresh_controls();
}

static void on_heat_toggle(lv_event_t* e)
{
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    st.heatingOn = lv_obj_has_state(btn, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "heating -> %s", st.heatingOn ? "ON" : "OFF");
    refresh_controls();
}

static void on_fan_changed(lv_event_t* e)
{
    lv_obj_t* bm = (lv_obj_t*)lv_event_get_target(e);
    uint32_t idx = lv_buttonmatrix_get_selected_button(bm);
    if (idx <= 2) {
        st.fanMode = (int)idx;
        ESP_LOGI(TAG, "fan mode -> %d", st.fanMode);
        refresh_controls();
    }
}

static void on_fan_lvl_changed(lv_event_t* e)
{
    lv_obj_t* s = (lv_obj_t*)lv_event_get_target(e);
    int lvl = (int)lv_slider_get_value(s);
    st.fanMode = lvl + 2;
    char b[8];
    snprintf(b, sizeof(b), "%d", lvl);
    lv_label_set_text(ui.lbl_fan_lvl, b);
    ESP_LOGI(TAG, "fan level -> %d", lvl);
}

static void on_boiler_changed(lv_event_t* e)
{
    lv_obj_t* bm = (lv_obj_t*)lv_event_get_target(e);
    uint32_t idx = lv_buttonmatrix_get_selected_button(bm);
    if (idx <= 3) {
        st.boilerMode = (int)idx;
        ESP_LOGI(TAG, "boiler mode -> %d", st.boilerMode);
        refresh_controls();
    }
}

static void on_conf_clicked(lv_event_t*)
{
    ESP_LOGI(TAG, "settings button pressed");
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
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    bsp_display_brightness_set(100);

    if (bsp_display_lock(1000)) {
        build_splash(lv_screen_active());
        bsp_display_unlock();
    }

    ESP_LOGI(TAG, "display ready — 800x480 landscape");
}

void p4DisplayUpdate(const P4DisplayData& d)
{
    static char buf[40];

    if (!bsp_display_lock(50)) return;

    static bool s_mainBuilt = false;
    if (!s_mainBuilt) {
        build_main_screen();
        s_mainBuilt = true;
    }

    st.roomSetpoint = d.roomSetpoint;
    st.heatingOn    = d.heatingOn;
    st.fanMode      = d.fanMode;
    st.boilerMode   = d.boilerMode;
    st.energyIdx    = d.energyIdx;

    // Top bar
    if (d.roomTemp > -100.0f)
        lv_label_set_text_fmt(ui.lbl_room_temp, "Hab: %.1f\xc2\xb0""C", d.roomTemp);
    else
        lv_label_set_text(ui.lbl_room_temp, "Hab: --.-\xc2\xb0""C");

    if (d.outdoorTemp > -100.0f)
        lv_label_set_text_fmt(ui.lbl_outdoor, "%.1f\xc2\xb0""C", d.outdoorTemp);
    else
        lv_label_set_text(ui.lbl_outdoor, "--.-\xc2\xb0""C");

    lv_obj_set_style_bg_color(ui.dot_wifi, d.wifiOk ? C_GREEN : C_RED, 0);
    lv_obj_set_style_bg_color(ui.dot_lin,  d.linOk  ? C_GREEN : C_RED, 0);

    // Water temp + bar
    if (d.waterTemp > -100.0f) {
        lv_label_set_text_fmt(ui.lbl_water_temp, "%.0f\xc2\xb0""C", d.waterTemp);
        int wv = (int)d.waterTemp;
        if (wv < 0)  wv = 0;
        if (wv > 70) wv = 70;
        lv_bar_set_value(ui.bar_water, wv, LV_ANIM_ON);
        lv_color_t wc = (d.waterTemp < 30.0f) ? C_WATER_COLD
                      : (d.waterTemp < 51.0f) ? C_WATER_WARM
                                              : C_WATER_HOT;
        lv_obj_set_style_bg_color(ui.bar_water, wc, LV_PART_INDICATOR);
    } else {
        lv_label_set_text(ui.lbl_water_temp, "--\xc2\xb0""C");
        lv_bar_set_value(ui.bar_water, 0, LV_ANIM_OFF);
    }

    // Solar data
    lv_label_set_text(ui.lbl_solar_status, d.solar.status ? d.solar.status : "--");
    snprintf(buf, sizeof(buf), "Volt.: %.1f V", d.solar.voltageV);
    lv_label_set_text(ui.lbl_solar_volts, buf);
    snprintf(buf, sizeof(buf), "Carga: %.1f A / %d W", d.solar.currentA, d.solar.powerW);
    lv_label_set_text(ui.lbl_solar_current, buf);

    // Battery SOC + bar
    int soc = d.batt.soc;
    if (soc < 0)   soc = 0;
    if (soc > 100) soc = 100;
    lv_bar_set_value(ui.bar_batt, soc, LV_ANIM_ON);
    lv_color_t bc = (soc < 20) ? C_RED : (soc < 50) ? C_AMBER : C_GREEN;
    lv_obj_set_style_bg_color(ui.bar_batt, bc, LV_PART_INDICATOR);
    lv_label_set_text_fmt(ui.lbl_batt_soc, "%d%%", soc);

    // Status bar connection info
    if (d.wifiOk && d.ssid && d.ip) {
        lv_label_set_text_fmt(ui.lbl_conn, "%s  %s", d.ssid, d.ip);
        lv_obj_set_style_text_color(ui.lbl_conn, C_TEXT, 0);
    } else {
        lv_label_set_text(ui.lbl_conn, "Sin WiFi");
        lv_obj_set_style_text_color(ui.lbl_conn, C_DIM, 0);
    }

    refresh_controls();

    bsp_display_unlock();
}

void p4DisplaySetStatus(const char* msg)
{
    if (!msg) return;
    if (!bsp_display_lock(50)) return;
    if (ui.lbl_status) lv_label_set_text(ui.lbl_status, msg);
    bsp_display_unlock();
}

bool lvglLock(uint32_t timeout_ms) { return bsp_display_lock(timeout_ms); }
void lvglUnlock()                  { bsp_display_unlock(); }
