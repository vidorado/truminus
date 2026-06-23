#include "p4display.hpp"
#include "multiplusble.hpp"   // MULTI_POWER_NA sentinel
#include "p4settings.hpp"
#include "p4_ota.hpp"
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
static lv_font_t* s_font_14      = nullptr;
static lv_font_t* s_font_18      = nullptr;
static lv_font_t* s_font_20      = nullptr;
static lv_font_t* s_font_22      = nullptr;
static lv_font_t* s_font_24      = nullptr;
static lv_font_t* s_font_28      = nullptr;
static lv_font_t* s_font_title   = nullptr;  // bold 26
static lv_font_t* s_font_logo    = nullptr;  // bold 28
static lv_font_t* s_font_splash  = nullptr;  // bold 52 — splash title only
static lv_font_t* s_font_icons14 = nullptr;  // FontAwesome 4.x @ 14 (port headers)
static lv_font_t* s_font_icons22 = nullptr;  // FontAwesome 4.x @ 22
static lv_font_t* s_font_icons24 = nullptr;  // FontAwesome 4.x @ 24
static lv_font_t* s_font_icons28 = nullptr;  // FontAwesome 4.x @ 28 (A/C eco button)
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
#define FA_DOWNLOAD   "\xEF\x80\x99"   // U+F019 (download, firmware-update reminder)
#define FA_PLUG_BOLT  "\xEE\x95\x9F"   // U+E55F (plug-circle-bolt)
#define FA_ARROW_L    "\xEF\x85\xB7"   // U+F177 (arrow-left-long)
#define FA_ARROW_R    "\xEF\x85\xB8"   // U+F178 (arrow-right-long)
#define FA_SNOWFLAKE  "\xEF\x8B\x9C"   // U+F2DC (snowflake — A/C cool)
#define FA_SUN        "\xEF\x86\x85"   // U+F185 (sun — Truma heat in climate panel)
#define FA_SNOWLEAF   "\xEE\xA4\x80"   // U+E900 (custom snowflake+leaf — A/C eco)

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

// Row 1: CALEFACCIÓN | AGUA CALIENTE | AGUA LIMPIA.  CALEFACCIÓN was bumped
// back to 256 (giving the setpoint reading more breathing room) at the
// expense of AGUA CALIENTE (350 → 330); the latter's button matrix is
// narrowed by the same 20 px so the boiler drawing stays full-size.
static constexpr int HEAT_W  = 276;
static constexpr int WATER_X = 276;
static constexpr int WATER_W = 300;

// Row 2: VENTILADOR | SOLAR | INVERSOR
static constexpr int FAN_W   = 203;
static constexpr int SOLAR_X = 203;
static constexpr int SOLAR_W = 230;
// INVERSOR panel — right slot of ROW 2 (was empty), wider after SOLAR/FAN
// were narrowed (SOLAR lost its "Volt:/Carga:/Prod:" prefix labels, and its
// readout units were shrunk so the battery column could shift left).
// Layout copied from ui/truminus_ui.eez-project ("INVERTER" panel).
static constexpr int INV_X   = SOLAR_X + SOLAR_W;   // 433
static constexpr int INV_W   = W - INV_X;           // 367
// "AGUA LIMPIA" panel — right-most slot of ROW 1 (next to AGUA CALIENTE).
// 214 × ROW1_H, flush against the right edge.  Layout matches
// ui/truminus_ui.eez-project (EMPTY 1 / SOLAR_1 panel).
static constexpr int AGUA_X  = 576;
static constexpr int AGUA_W  = 224;

// Vertical bar dimensions
static constexpr int TANK_W        = 64;
static constexpr int TANK_H_WATER  = 84;   // shortened so its top clears the temp label
static constexpr int TANK_H_BATT   = 60;   // shortened to fit the CARGA/DESCARGA port below
// Boiler thermometer pill: a narrow capsule (radius = W/2) with the fill
// clipped to its rounded outline, plus scale ticks drawn inside the pill,
// over the fill.  The fill lives inside the 2 px border, so its drawable
// area is inset by 2 px on each side.
static constexpr int WATER_PILL_W  = 24;
static constexpr int WATER_TICK_W  = 7;
static constexpr int WATER_FILL_W  = WATER_PILL_W - 4;    // inside the border
static constexpr int WATER_FILL_H  = TANK_H_WATER - 4;    // inside the border

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
#define C_GREEN       lv_color_hex(0x44ff44)
#define C_RED         lv_color_hex(0xff4444)
#define C_AMBER_BAR   lv_color_hex(0xffaa00)
#define C_WATER_COLD  lv_color_hex(0x4488ff)
#define C_WATER_WARM  lv_color_hex(0xffaa00)
#define C_WATER_HOT   lv_color_hex(0xff4444)
#define C_BORDER_BAT  lv_color_hex(0x888888)
// INVERSOR port dynamic colours (matching web styles.css)
#define C_PORT_GREY_BODY lv_color_hex(0x6a727f)
#define C_PORT_GREY_HDR  lv_color_hex(0x969ba3)
#define C_PORT_GREEN_BODY lv_color_hex(0x2e7d32)
#define C_PORT_GREEN_HDR  lv_color_hex(0x4caf50)
#define C_PORT_RED_BODY   lv_color_hex(0x7a2a2a)
#define C_PORT_RED_HDR    lv_color_hex(0xa34545)

// ── Local UI state ────────────────────────────────────────────────────────────
static struct {
    float roomSetpoint = 20.0f;
    bool  heatingOn    = false;
    int   fanMode      = 0;
    int   boilerMode   = 0;
    int   energyIdx    = 0;
    // OpenAir PLUS A/C (cooling). Heat is still the Truma (heatingOn).
    bool  openairConfigured = false;
    int   acMode       = 0;       // 0=off, 1=cool, 2=eco
    bool  acFanAuto    = true;    // cool mode: Auto (Mode AUTO) vs Man (Mode MAN)
    int   acFanSpeed   = 3;       // cool+Man blower speed, 1..6
    int   acPower      = 0;       // max power: 0=1.2 kW, 1=2.0 kW
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
    // Diagonal "slash" bars drawn over the four status icons in the FAILED
    // state (no FontAwesome glyph exists for a crossed-out icon).
    lv_obj_t* slash_wifi;
    lv_obj_t* slash_bt;
    lv_obj_t* slash_cloud;
    lv_obj_t* slash_lin;
    lv_obj_t* icon_ota;    // firmware-update reminder (hidden unless available)
    lv_obj_t* icon_tint;   // water (boiler) status indicator
    lv_obj_t* icon_flame;  // heating status indicator
    lv_obj_t* btn_conf;

    // CALEFACCIÓN / CLIMATIZACIÓN panel
    lv_obj_t* lbl_heat_title;   // "CALEFACCIÓN" or "CLIMATIZACIÓN"
    lv_obj_t* lbl_room_sp;
    lv_obj_t* btn_sp_dn;
    lv_obj_t* btn_sp_up;
    lv_obj_t* btnmx_heat;       // Off | On (Truma-only mode)
    lv_obj_t* row_ac;           // A/C mode row container (flex)
    lv_obj_t* btn_ac[4];        // [cool][eco][heat][off] individual buttons
    lv_obj_t* row_sp;
    lv_obj_t* btn_pwr[2];  // [0]=1.2 kW, [1]=2.0 kW — inside row_sp

    // A/C fan sub-controls (cool/eco modes)
    lv_obj_t* btnmx_ac_fan;     // Auto | Man
    lv_obj_t* btn_acfan_dn;
    lv_obj_t* btn_acfan_up;
    lv_obj_t* lbl_acfan_lvl;

    // VENTILADOR panel
    lv_obj_t* btnmx_fan_heat;
    lv_obj_t* btnmx_fan_off;
    lv_obj_t* btn_fan_dn;
    lv_obj_t* btn_fan_up;
    lv_obj_t* lbl_fan_lvl;

    // AGUA CALIENTE panel
    lv_obj_t* lbl_water_temp;
    lv_obj_t* water_track;   // pill outline
    lv_obj_t* water_fill;    // bottom-anchored fill, clipped to the pill
    lv_obj_t* btnmx_boiler;

    // SOLAR panel
    lv_obj_t* lbl_solar_status;
    lv_obj_t* lbl_solar_current;   lv_obj_t* lbl_solar_current_u;
    lv_obj_t* lbl_solar_power;     lv_obj_t* lbl_solar_power_u;
    lv_obj_t* lbl_solar_yield;     lv_obj_t* lbl_solar_yield_u;
    lv_obj_t* bar_batt;
    lv_obj_t* lbl_batt_soc;
    // Battery power port (CARGA/DESCARGA) below the SOC bar + its flow line
    lv_obj_t* box_bpwr;   lv_obj_t* hdr_bpwr;   lv_obj_t* hdr_bpwr_lbl;
    lv_obj_t* lbl_bpwr_w;
    lv_obj_t* flow_bpwr;       // vertical ghost line bar ↔ port
    lv_obj_t* flow_bpwr_str;   // zebra overlay (animated when power flows)
    int8_t    flow_bpwr_dir;   // 0=idle, +1=charging (up), -1=discharging (down)

    // AGUA LIMPIA panel (fresh-water tank, BTHome)
    lv_obj_t* bar_tank;
    lv_obj_t* lbl_tank_pct;

    // INVERSOR panel (Victron VE.Bus / Multiplus)
    lv_obj_t* lbl_inv_state;       // "Inverting" / "Charging" / "Off" / …
    lv_obj_t* lbl_inv_mains_w;     // shore power
    lv_obj_t* lbl_inv_load_w;      // AC out power
    lv_obj_t* lbl_inv_batt_w;      // computed battery W (V*A)
    lv_obj_t* flow_mains;          // ghost line MAINS → INV
    lv_obj_t* flow_load;           // ghost line INV → LOAD
    lv_obj_t* flow_batt;           // ghost line INV ↔ BATT
    // Zebra-stripe overlays (animated when power is flowing).
    lv_obj_t* flow_mains_str;
    lv_obj_t* flow_load_str;
    lv_obj_t* flow_batt_str;
    int8_t    flow_mains_dir;      // 0=idle, +1=right, -1=left
    int8_t    flow_load_dir;
    int8_t    flow_batt_dir;
    // Port box/header refs for dynamic coloring
    lv_obj_t* box_mains;  lv_obj_t* hdr_mains;  lv_obj_t* hdr_mains_lbl;
    lv_obj_t* box_load;   lv_obj_t* hdr_load;
    lv_obj_t* box_batt;   lv_obj_t* hdr_batt;   lv_obj_t* hdr_batt_lbl;

    // Status bar
    lv_obj_t* lbl_conn;
    lv_obj_t* lbl_status;

} ui;

// ── Topbar status-icon state machine ──────────────────────────────────────
// The four status glyphs (WiFi, LIN, BLE, cloud/tunnel) share one all-white
// visual vocabulary: DISABLED = dim, CONNECTING = blink, CONNECTED = solid,
// FAILED = solid with a diagonal slash struck over the glyph (no FontAwesome
// glyph exists for a crossed-out icon).  Icon indices: 0 WiFi, 1 LIN, 2 BLE,
// 3 cloud.
enum IconSt : uint8_t { IST_DISABLED, IST_CONNECTING, IST_CONNECTED, IST_FAILED };
static IconSt s_iconSt[4] = { IST_DISABLED, IST_DISABLED, IST_DISABLED, IST_DISABLED };
static bool   s_iconBlink = false;   // CONNECTING blink phase, toggled by the 500 ms timer

static lv_obj_t* icon_obj(int i)
{
    switch (i) { case 0: return ui.icon_wifi; case 1: return ui.icon_lin;
                 case 2: return ui.icon_bt;   default: return ui.icon_cloud; }
}
static lv_obj_t* slash_obj(int i)
{
    switch (i) { case 0: return ui.slash_wifi; case 1: return ui.slash_lin;
                 case 2: return ui.slash_bt;   default: return ui.slash_cloud; }
}

// Diagonal slash bar over an icon: a white core with a C_BG border, so the
// border "cuts" a gap out of the white glyph while the white core stays
// visible against the dark topbar — readable on both.
static lv_obj_t* make_slash(lv_obj_t* parent, lv_obj_t* over)
{
    lv_obj_t* s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, 30, 6);
    lv_obj_set_style_radius(s, 1, 0);
    lv_obj_set_style_bg_color(s, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s, C_BG, 0);      // dark edges "cut" the glyph;
    lv_obj_set_style_border_width(s, 2, 0);         // 2 px white core stays visible
    lv_obj_set_style_border_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(s, 15, 0);   // centre of the 30×6 bar
    lv_obj_set_style_transform_pivot_y(s, 3, 0);
    lv_obj_set_style_transform_rotation(s, 450, 0); // 45.0°
    lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(s, over, LV_ALIGN_CENTER, 0, 0);
    return s;
}

static void repaint_icon(int i)
{
    lv_obj_t* ic = icon_obj(i);
    if (!ic) return;
    lv_obj_t* sl = slash_obj(i);
    lv_opa_t opa = LV_OPA_COVER;
    bool slash   = false;
    switch (s_iconSt[i]) {
    case IST_DISABLED:   opa = LV_OPA_40; break;
    case IST_CONNECTING: opa = s_iconBlink ? LV_OPA_COVER : LV_OPA_20; break;
    case IST_CONNECTED:  opa = LV_OPA_COVER; break;
    case IST_FAILED:     opa = LV_OPA_COVER; slash = true; break;
    }
    lv_obj_set_style_text_color(ic, lv_color_white(), 0);
    lv_obj_set_style_text_opa(ic, opa, 0);
    if (sl) {
        if (slash) lv_obj_clear_flag(sl, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(sl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_icon_state(int i, IconSt s)
{
    if (s_iconSt[i] == s) return;
    s_iconSt[i] = s;
    repaint_icon(i);
}

// Map a subsystem's boolean health to the icon vocabulary: dim until it is
// attempting, blinking while it connects, struck-through once the grace window
// elapses without a connection.  `downSince` (caller-owned, per icon) records
// the lv_tick when the attempt began; 0 means "not currently attempting".
static const uint32_t ICON_GRACE_MS = 15000;
static IconSt derive_icon_state(bool ok, bool attempting, uint32_t& downSince,
                                uint32_t now)
{
    if (ok)         { downSince = 0; return IST_CONNECTED; }
    if (!attempting){ downSince = 0; return IST_DISABLED;  }
    if (!downSince) downSince = now;
    return (now - downSince < ICON_GRACE_MS) ? IST_CONNECTING : IST_FAILED;
}

// ── Forward declarations ──────────────────────────────────────────────────────
static void refresh_controls();
static void on_sp_dn(lv_event_t* e);
static void on_sp_up(lv_event_t* e);
static void on_heat_changed(lv_event_t* e);
static void on_fan_heat_changed(lv_event_t* e);
static void on_fan_off_changed(lv_event_t* e);
static void on_fan_dn(lv_event_t* e);
static void on_fan_up(lv_event_t* e);
static void on_boiler_changed(lv_event_t* e);
static void on_ac_btn_clicked(lv_event_t* e);
static void on_pwr_clicked(lv_event_t* e);
static void on_ac_fan_changed(lv_event_t* e);
static void on_acfan_dn(lv_event_t* e);
static void on_acfan_up(lv_event_t* e);
static void on_conf_clicked(lv_event_t* e);

// ── Font loader ───────────────────────────────────────────────────────────────
static void load_fonts()
{
    const size_t reg_sz  = font_regular_ttf_end - font_regular_ttf_start;
    const size_t bold_sz = font_bold_ttf_end    - font_bold_ttf_start;
    const size_t ico_sz  = font_icons_ttf_end   - font_icons_ttf_start;
    s_font_14    = lv_tiny_ttf_create_data(font_regular_ttf_start, reg_sz, 14);
    s_font_18    = lv_tiny_ttf_create_data(font_regular_ttf_start, reg_sz, 18);
    s_font_20    = lv_tiny_ttf_create_data(font_regular_ttf_start, reg_sz, 20);
    s_font_22    = lv_tiny_ttf_create_data(font_regular_ttf_start, reg_sz, 22);
    s_font_24    = lv_tiny_ttf_create_data(font_regular_ttf_start, reg_sz, 24);
    s_font_28    = lv_tiny_ttf_create_data(font_regular_ttf_start, reg_sz, 28);
    s_font_title  = lv_tiny_ttf_create_data(font_bold_ttf_start,    bold_sz, 26);
    s_font_logo   = lv_tiny_ttf_create_data(font_bold_ttf_start,    bold_sz, 28);
    s_font_splash = lv_tiny_ttf_create_data(font_bold_ttf_start,    bold_sz, 52);
    s_font_icons14 = lv_tiny_ttf_create_data(font_icons_ttf_start, ico_sz, 14);
    s_font_icons22 = lv_tiny_ttf_create_data(font_icons_ttf_start, ico_sz, 22);
    s_font_icons24 = lv_tiny_ttf_create_data(font_icons_ttf_start, ico_sz, 24);
    s_font_icons28 = lv_tiny_ttf_create_data(font_icons_ttf_start, ico_sz, 28);
    s_font_icons36 = lv_tiny_ttf_create_data(font_icons_ttf_start, ico_sz, 36);

    // Set the icon font as fallback so labels can mix Latin text + FA glyphs
    // (e.g. "FA_COG  Conf." renders correctly in a single label).
    if (s_font_14)    s_font_14->fallback    = s_font_icons14;
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

// Rounded I/O port box: a coloured header strip over a body with a centred
// value label.  Shared by the INVERSOR ports and the battery power port; the
// caller recolours `head`/`box` and rewrites `head_lbl`/`val` at runtime.
struct IoBoxOut { lv_obj_t* box; lv_obj_t* head; lv_obj_t* head_lbl; lv_obj_t* val; };
static IoBoxOut make_io_box(lv_obj_t* parent, int x, int y, int w, int h,
                            const char* hdr, int hdr_h = 22)
{
    // Wrapper with clip_corner provides the rounded outline shared by head
    // (top corners) and box (bottom corners) — LVGL has no per-corner radius
    // so we round the parent and let it clip.
    lv_obj_t* wrap = lv_obj_create(parent);
    lv_obj_set_pos(wrap, x, y - hdr_h);
    lv_obj_set_size(wrap, w, hdr_h + h);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_radius(wrap, 5, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_set_style_clip_corner(wrap, true, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* head = lv_obj_create(wrap);
    lv_obj_set_pos(head, 0, 0);
    lv_obj_set_size(head, w, hdr_h);
    lv_obj_set_style_bg_color(head, C_PORT_GREY_HDR, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_radius(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* hl = lv_label_create(head);
    lv_label_set_text(hl, hdr);
    lv_obj_set_style_text_color(hl, C_TEXT, 0);
    lv_obj_set_style_text_font(hl, s_font_14, 0);
    lv_obj_center(hl);

    lv_obj_t* box = lv_obj_create(wrap);
    lv_obj_set_pos(box, 0, hdr_h);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, C_PORT_GREY_BODY, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* v = lv_label_create(box);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, C_TEXT, 0);
    lv_obj_set_style_text_font(v, s_font_20, 0);
    lv_obj_center(v);
    return {box, head, hl, v};
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

// Cached LIN status from the last p4DisplayUpdate() call; used by
// refresh_flame_icon() so the topbar updates immediately on LCD taps.
static bool s_linOk = false;

// Cached OpenAir A/C "confirmed by BLE" flag (see P4DisplayData::acConnected);
// gates the topbar snowflake brightness in refresh_flame_icon().
static bool s_acConnected = false;

// Btnmatrix maps filled from i18n strings each time build_main_screen() runs.
// Stored as module-level arrays so LVGL's pointer reference stays valid.
static const char* s_heat_map[3]     = {};
static const char* s_fan_heat_map[3] = {};
static const char* s_fan_off_map[3]  = {};
static const char* s_boiler_map[6]   = {};
static const char* s_ac_fan_map[3]   = {};   // [Auto][Man]

static void build_main_screen()
{
    // Fill btnmatrix maps with current-language strings.
    s_heat_map[0]     = t(TK::OFF);
    s_heat_map[1]     = t(TK::ON);
    s_heat_map[2]     = "";
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
    s_ac_fan_map[0]   = t(TK::FAN_AUTO);
    s_ac_fan_map[1]   = t(TK::FAN_MAN);
    s_ac_fan_map[2]   = "";

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

    // The four status glyphs are painted by repaint_icon() through the white
    // DISABLED/CONNECTING/CONNECTED/FAILED vocabulary; creation only places them.
    ui.icon_lin = lv_label_create(topbar);
    lv_label_set_text(ui.icon_lin, FA_RANDOM);
    lv_obj_set_style_text_font(ui.icon_lin, s_font_icons24, 0);
    lv_obj_align(ui.icon_lin, LV_ALIGN_RIGHT_MID, -16, 0);

    ui.icon_wifi = lv_label_create(topbar);
    lv_label_set_text(ui.icon_wifi, FA_WIFI);
    lv_obj_set_style_text_font(ui.icon_wifi, s_font_icons24, 0);
    lv_obj_align_to(ui.icon_wifi, ui.icon_lin, LV_ALIGN_OUT_LEFT_MID, -16, 0);

    ui.icon_bt = lv_label_create(topbar);
    lv_label_set_text(ui.icon_bt, FA_BLUETOOTH);
    lv_obj_set_style_text_font(ui.icon_bt, s_font_icons24, 0);
    lv_obj_align_to(ui.icon_bt, ui.icon_wifi, LV_ALIGN_OUT_LEFT_MID, -16, 0);

    ui.icon_cloud = lv_label_create(topbar);
    lv_label_set_text(ui.icon_cloud, FA_CLOUD);
    lv_obj_set_style_text_font(ui.icon_cloud, s_font_icons24, 0);
    lv_obj_align_to(ui.icon_cloud, ui.icon_bt, LV_ALIGN_OUT_LEFT_MID, -16, 0);

    // Strike-through bars for the FAILED state, one per icon (hidden otherwise).
    ui.slash_lin   = make_slash(topbar, ui.icon_lin);
    ui.slash_wifi  = make_slash(topbar, ui.icon_wifi);
    ui.slash_bt    = make_slash(topbar, ui.icon_bt);
    ui.slash_cloud = make_slash(topbar, ui.icon_cloud);
    for (int i = 0; i < 4; i++) repaint_icon(i);

    // Firmware-update reminder icon, in a reserved slot between the cloud
    // status icon and the Config button.  Hidden until p4SetUpdateAvailable(true)
    // reveals it; the slot is kept clear either way so nothing reflows.
    ui.icon_ota = lv_label_create(topbar);
    lv_label_set_text(ui.icon_ota, FA_DOWNLOAD);
    lv_obj_set_style_text_font(ui.icon_ota, s_font_icons24, 0);
    // White to stand apart from the blue/grey tunnel cloud icon next to it.
    lv_obj_set_style_text_color(ui.icon_ota, C_TEXT, 0);
    lv_obj_align_to(ui.icon_ota, ui.icon_cloud, LV_ALIGN_OUT_LEFT_MID, -16, 0);
    lv_obj_add_flag(ui.icon_ota, LV_OBJ_FLAG_HIDDEN);

    ui.btn_conf = lv_button_create(topbar);
    lv_obj_set_size(ui.btn_conf, 158, 40);
    lv_obj_align_to(ui.btn_conf, ui.icon_ota, LV_ALIGN_OUT_LEFT_MID, -16, 0);
    style_button(ui.btn_conf);
    lv_obj_t* lbl_conf = lv_label_create(ui.btn_conf);
    lv_label_set_text(lbl_conf, FA_COG "  Config");
    lv_obj_set_style_text_font(lbl_conf, s_font_20, 0);
    lv_obj_center(lbl_conf);
    lv_obj_add_event_cb(ui.btn_conf, on_conf_clicked, LV_EVENT_CLICKED, NULL);

    // ── CALEFACCIÓN panel (col1 row1) ─────────────────────────────────────────
    lv_obj_t* p_heat = make_section(scr, 0, CONTENT_Y, HEAT_W, ROW1_H);

    // Title: "CALEFACCIÓN" normally, swapped to "CLIMATIZACIÓN" by
    // refresh_controls() when the OpenAir A/C is configured.
    ui.lbl_heat_title = make_label(p_heat, t(TK::HEATING), s_font_title, C_DIM, 12, 8);

    // Off | On toggle as a 2-button matrix, mirroring the FAN standby row.
    ui.btnmx_heat = lv_buttonmatrix_create(p_heat);
    lv_obj_set_pos(ui.btnmx_heat, 12, 52);
    lv_obj_set_size(ui.btnmx_heat, HEAT_W - 24, 56);
    lv_buttonmatrix_set_map(ui.btnmx_heat, s_heat_map);
    lv_buttonmatrix_set_button_ctrl_all(ui.btnmx_heat, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(ui.btnmx_heat, true);
    lv_buttonmatrix_set_selected_button(ui.btnmx_heat, 0);
    style_btnmatrix(ui.btnmx_heat, s_font_20);   // match the FAN buttons
    lv_obj_add_event_cb(ui.btnmx_heat, on_heat_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // A/C mode row — four individual buttons so each can have its own font.
    // [❄️ cool][❄️ECO][☀️ heat][Apag.]  Hidden by default; replaces btnmx_heat
    // when the OpenAir A/C is configured.
    {
        static const char* s_ac_icons[3] = { FA_SNOWFLAKE, FA_SNOWLEAF, FA_FIRE };
        ui.row_ac = lv_obj_create(p_heat);
        lv_obj_remove_style_all(ui.row_ac);
        lv_obj_set_pos(ui.row_ac, 12, 52);
        lv_obj_set_size(ui.row_ac, HEAT_W - 24, 56);
        lv_obj_set_style_bg_opa(ui.row_ac, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(ui.row_ac, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(ui.row_ac, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(ui.row_ac, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(ui.row_ac, 6, 0);
        lv_obj_set_style_pad_all(ui.row_ac, 0, 0);
        for (int i = 0; i < 4; ++i) {
            ui.btn_ac[i] = lv_button_create(ui.row_ac);
            lv_obj_set_flex_grow(ui.btn_ac[i], 1);
            lv_obj_set_height(ui.btn_ac[i], LV_PCT(100));
            style_button(ui.btn_ac[i]);
            lv_obj_t* lbl = lv_label_create(ui.btn_ac[i]);
            if (i < 3) {
                lv_label_set_text(lbl, s_ac_icons[i]);
                lv_obj_set_style_text_font(lbl, s_font_icons28, 0);
            } else {
                lv_label_set_text(lbl, t(TK::OFF));
                lv_obj_set_style_text_font(lbl, s_font_20, 0);
                lv_obj_set_style_text_letter_space(lbl, -1, 0);
            }
            lv_obj_set_style_text_color(lbl, C_TEXT, 0);
            lv_obj_center(lbl);
            lv_obj_add_event_cb(ui.btn_ac[i], on_ac_btn_clicked, LV_EVENT_CLICKED, NULL);
        }
        lv_obj_add_state(ui.btn_ac[3], LV_STATE_CHECKED);  // start with "off" selected
        lv_obj_add_flag(ui.row_ac, LV_OBJ_FLAG_HIDDEN);
    }

    ui.row_sp = lv_obj_create(p_heat);
    lv_obj_remove_style_all(ui.row_sp);
    lv_obj_set_pos(ui.row_sp, 12, 115);
    lv_obj_set_size(ui.row_sp, HEAT_W - 24, 60);
    lv_obj_set_style_bg_opa(ui.row_sp, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ui.row_sp, LV_OBJ_FLAG_SCROLLABLE);

    // Layout: [▼ 46px] [temp label — centered in its slot] [▲ 46px] [4px] [1.2/2.0 col 48px]
    static constexpr int SP_BTN_W  = 46;   // ▼/▲ button width
    static constexpr int PWR_W     = 48;   // power button column width
    static constexpr int PWR_H     = 26;   // each power button height
    static constexpr int ROW_W     = HEAT_W - 24;                    // 252
    static constexpr int PWR_X     = ROW_W - PWR_W;                  // 204
    static constexpr int SP_UP_X   = PWR_X - 8 - SP_BTN_W;          // 150
    // Temperature label centred in [SP_BTN_W .. SP_UP_X], i.e. [46..150] = 104 px slot
    static constexpr int LBL_CTR_X = SP_BTN_W + (SP_UP_X - SP_BTN_W) / 2; // 98

    ui.btn_sp_dn = lv_button_create(ui.row_sp);
    lv_obj_set_size(ui.btn_sp_dn, SP_BTN_W, 60);
    lv_obj_set_pos(ui.btn_sp_dn, 0, 0);
    style_button(ui.btn_sp_dn);
    lv_obj_t* l_dn = lv_label_create(ui.btn_sp_dn);
    lv_label_set_text(l_dn, FA_CARET_U);
    lv_obj_set_style_text_font(l_dn, s_font_icons24, 0);
    lv_obj_center(l_dn);
    lv_obj_add_event_cb(ui.btn_sp_dn, on_sp_dn, LV_EVENT_CLICKED, NULL);

    ui.lbl_room_sp = lv_label_create(ui.row_sp);
    lv_label_set_text(ui.lbl_room_sp, "20°C");
    lv_obj_set_style_text_font(ui.lbl_room_sp, s_font_24, 0);
    lv_obj_set_style_text_color(ui.lbl_room_sp, C_TEXT, 0);
    // Centre the label within its slot, offset from the full-row centre.
    lv_obj_align(ui.lbl_room_sp, LV_ALIGN_CENTER,
                 LBL_CTR_X - ROW_W / 2, 0);  // = 100 - 126 = -26 px

    // Power buttons (1.2 kW / 2.0 kW): column to the right of the temp label.
    static const char* pwr_labels[2] = { "1.2", "2.0" };
    for (int i = 0; i < 2; ++i) {
        ui.btn_pwr[i] = lv_button_create(ui.row_sp);
        lv_obj_set_size(ui.btn_pwr[i], PWR_W, PWR_H);
        lv_obj_set_pos(ui.btn_pwr[i], PWR_X, i == 0 ? 2 : 60 - PWR_H - 2);
        style_button(ui.btn_pwr[i]);
        lv_obj_t* lbl = lv_label_create(ui.btn_pwr[i]);
        lv_label_set_text(lbl, pwr_labels[i]);
        lv_obj_set_style_text_font(lbl, s_font_18, 0);
        lv_obj_set_style_text_color(lbl, C_TEXT, 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(ui.btn_pwr[i], on_pwr_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(i));
    }
    lv_obj_add_state(ui.btn_pwr[0], LV_STATE_CHECKED);  // 1.2 kW default

    ui.btn_sp_up = lv_button_create(ui.row_sp);
    lv_obj_set_size(ui.btn_sp_up, SP_BTN_W, 60);
    lv_obj_set_pos(ui.btn_sp_up, SP_UP_X, 0);
    style_button(ui.btn_sp_up);
    lv_obj_t* l_up = lv_label_create(ui.btn_sp_up);
    lv_label_set_text(l_up, FA_CARET_D);
    lv_obj_set_style_text_font(l_up, s_font_icons24, 0);
    lv_obj_center(l_up);
    lv_obj_add_event_cb(ui.btn_sp_up, on_sp_up, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(ui.row_sp, LV_OBJ_FLAG_HIDDEN);

    // ── VENTILADOR panel (col1 row2) ──────────────────────────────────────────
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

    // Fan level: −/+ buttons replace old slider.  Anchored so the left edge
    // of fan_dn matches "Apag." and the right edge of fan_up matches "Enc.":
    //   btnmx_fan_off lives at x=12, w=FAN_W−24=179 → right edge x=191.
    ui.btn_fan_dn = lv_button_create(p_fan);
    lv_obj_set_size(ui.btn_fan_dn, 53, 50);
    lv_obj_set_pos(ui.btn_fan_dn, 12, 109);
    style_button(ui.btn_fan_dn);
    lv_obj_t* l_fdn = lv_label_create(ui.btn_fan_dn);
    lv_label_set_text(l_fdn, FA_CARET_U);
    lv_obj_set_style_text_font(l_fdn, s_font_icons24, 0);
    lv_obj_center(l_fdn);
    lv_obj_add_event_cb(ui.btn_fan_dn, on_fan_dn, LV_EVENT_CLICKED, NULL);

    ui.lbl_fan_lvl = make_label(p_fan, "5", s_font_28, C_TEXT, 71, 117);
    lv_obj_set_width(ui.lbl_fan_lvl, 50);
    lv_obj_set_style_text_align(ui.lbl_fan_lvl, LV_TEXT_ALIGN_CENTER, 0);

    ui.btn_fan_up = lv_button_create(p_fan);
    lv_obj_set_size(ui.btn_fan_up, 53, 50);
    lv_obj_set_pos(ui.btn_fan_up, 138, 109);   // 191 (Enc. right) − 53 = 138
    style_button(ui.btn_fan_up);
    lv_obj_t* l_fup = lv_label_create(ui.btn_fan_up);
    lv_label_set_text(l_fup, FA_CARET_D);
    lv_obj_set_style_text_font(l_fup, s_font_icons24, 0);
    lv_obj_center(l_fup);
    lv_obj_add_event_cb(ui.btn_fan_up, on_fan_up, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(ui.btn_fan_dn,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.btn_fan_up,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.lbl_fan_lvl, LV_OBJ_FLAG_HIDDEN);

    // A/C fan controls (cool / eco modes).  Auto | Man toggle; in Man a 1–6
    // blower-speed selector appears.  In eco/auto the toggle is locked to Auto
    // and disabled.  All hidden until refresh_controls() reveals them.
    ui.btnmx_ac_fan = lv_buttonmatrix_create(p_fan);
    lv_obj_set_pos(ui.btnmx_ac_fan, 12, 54);
    lv_obj_set_size(ui.btnmx_ac_fan, FAN_W - 24, 48);
    lv_buttonmatrix_set_map(ui.btnmx_ac_fan, s_ac_fan_map);
    lv_buttonmatrix_set_button_ctrl_all(ui.btnmx_ac_fan, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(ui.btnmx_ac_fan, true);
    lv_buttonmatrix_set_selected_button(ui.btnmx_ac_fan, 0);
    style_btnmatrix(ui.btnmx_ac_fan, s_font_20);
    lv_obj_add_event_cb(ui.btnmx_ac_fan, on_ac_fan_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(ui.btnmx_ac_fan, LV_OBJ_FLAG_HIDDEN);

    ui.btn_acfan_dn = lv_button_create(p_fan);
    lv_obj_set_size(ui.btn_acfan_dn, 53, 50);
    lv_obj_set_pos(ui.btn_acfan_dn, 12, 109);
    style_button(ui.btn_acfan_dn);
    lv_obj_t* l_acdn = lv_label_create(ui.btn_acfan_dn);
    lv_label_set_text(l_acdn, FA_CARET_U);
    lv_obj_set_style_text_font(l_acdn, s_font_icons24, 0);
    lv_obj_center(l_acdn);
    lv_obj_add_event_cb(ui.btn_acfan_dn, on_acfan_dn, LV_EVENT_CLICKED, NULL);

    ui.lbl_acfan_lvl = make_label(p_fan, "3", s_font_28, C_TEXT, 71, 117);
    lv_obj_set_width(ui.lbl_acfan_lvl, 50);
    lv_obj_set_style_text_align(ui.lbl_acfan_lvl, LV_TEXT_ALIGN_CENTER, 0);

    ui.btn_acfan_up = lv_button_create(p_fan);
    lv_obj_set_size(ui.btn_acfan_up, 53, 50);
    lv_obj_set_pos(ui.btn_acfan_up, 138, 109);
    style_button(ui.btn_acfan_up);
    lv_obj_t* l_acup = lv_label_create(ui.btn_acfan_up);
    lv_label_set_text(l_acup, FA_CARET_D);
    lv_obj_set_style_text_font(l_acup, s_font_icons24, 0);
    lv_obj_center(l_acup);
    lv_obj_add_event_cb(ui.btn_acfan_up, on_acfan_up, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(ui.btn_acfan_dn,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.btn_acfan_up,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.lbl_acfan_lvl, LV_OBJ_FLAG_HIDDEN);

    // ── AGUA CALIENTE panel (col2 row1) ───────────────────────────────────────
    lv_obj_t* p_water = make_section(scr, WATER_X, CONTENT_Y, WATER_W, ROW1_H);

    make_label(p_water, t(TK::HOT_WATER), s_font_title, C_DIM, 12, 8);

    ui.lbl_water_temp = lv_label_create(p_water);
    lv_label_set_text(ui.lbl_water_temp, "--°C");
    lv_obj_set_style_text_font(ui.lbl_water_temp, s_font_22, 0);
    lv_obj_set_style_text_color(ui.lbl_water_temp, C_TEXT, 0);
    lv_obj_set_pos(ui.lbl_water_temp, 232, 53);   // top aligned with the boiler buttons
    lv_obj_set_width(ui.lbl_water_temp, TANK_W);
    lv_obj_set_style_text_align(ui.lbl_water_temp, LV_TEXT_ALIGN_CENTER, 0);

    // Thermometer pill, centred under the temperature label (inside the 64 px
    // column at x=232).
    constexpr int WP_X = 232 + (TANK_W - WATER_PILL_W) / 2;
    constexpr int WP_Y = 85;   // below the temp label; bottom (169) matches the buttons

    ui.water_track = lv_obj_create(p_water);
    lv_obj_set_pos(ui.water_track, WP_X, WP_Y);
    lv_obj_set_size(ui.water_track, WATER_PILL_W, TANK_H_WATER);
    lv_obj_set_style_radius(ui.water_track, WATER_PILL_W / 2, 0);
    lv_obj_set_style_clip_corner(ui.water_track, true, 0);
    lv_obj_set_style_bg_color(ui.water_track, C_BG, 0);
    lv_obj_set_style_bg_opa(ui.water_track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ui.water_track, C_BORDER_BAT, 0);
    lv_obj_set_style_border_width(ui.water_track, 2, 0);
    lv_obj_set_style_pad_all(ui.water_track, 0, 0);
    lv_obj_clear_flag(ui.water_track, LV_OBJ_FLAG_SCROLLABLE);

    // Bottom-anchored rectangular fill.  The track's clip_corner rounds it to
    // the pill outline: round at the bottom, flat on top while rising, and
    // rounding off at the top once it reaches the capsule's upper end.
    ui.water_fill = lv_obj_create(ui.water_track);
    lv_obj_set_size(ui.water_fill, WATER_FILL_W, 0);
    lv_obj_set_align(ui.water_fill, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_style_radius(ui.water_fill, 0, 0);
    lv_obj_set_style_bg_color(ui.water_fill, C_WATER_COLD, 0);
    lv_obj_set_style_bg_opa(ui.water_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.water_fill, 0, 0);
    lv_obj_set_style_pad_all(ui.water_fill, 0, 0);
    lv_obj_clear_flag(ui.water_fill, LV_OBJ_FLAG_SCROLLABLE);

    // Scale ticks (6 marks) down the right side, inside the pill.  Created
    // after the fill so they draw on top of it; coloured like the panel
    // background so they read as notches cut into the fill, and inset 2 px
    // from the right edge so they never touch the border.
    for (int k = 1; k <= 6; k++) {
        lv_obj_t* tk = lv_obj_create(ui.water_track);
        lv_obj_set_pos(tk, WATER_FILL_W - WATER_TICK_W - 2,
                       WATER_FILL_H - (WATER_FILL_H * k) / 7 - 1);
        lv_obj_set_size(tk, WATER_TICK_W, 2);
        lv_obj_set_style_bg_color(tk, C_BG, 0);
        lv_obj_set_style_bg_opa(tk, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tk, 0, 0);
        lv_obj_set_style_radius(tk, 1, 0);
        lv_obj_set_style_pad_all(tk, 0, 0);
        lv_obj_clear_flag(tk, LV_OBJ_FLAG_SCROLLABLE);
    }

    ui.btnmx_boiler = lv_buttonmatrix_create(p_water);
    lv_obj_set_pos(ui.btnmx_boiler, 12, 53);
    lv_obj_set_size(ui.btnmx_boiler, 214, 116);
    lv_buttonmatrix_set_map(ui.btnmx_boiler, s_boiler_map);
    lv_buttonmatrix_set_button_ctrl_all(ui.btnmx_boiler, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(ui.btnmx_boiler, true);
    lv_buttonmatrix_set_selected_button(ui.btnmx_boiler, 0);
    style_btnmatrix(ui.btnmx_boiler, s_font_20);
    lv_obj_add_event_cb(ui.btnmx_boiler, on_boiler_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // ── SOLAR panel (col2 row2) ───────────────────────────────────────────────
    lv_obj_t* p_sol = make_section(scr, SOLAR_X, ROW2_Y, SOLAR_W, ROW2_H);

    static constexpr int BATT_X = 150;
    static constexpr int BATT_Y = 46;
    static constexpr int NUB_W  = 12, NUB_H = 7;

    make_label(p_sol, "SOLAR", s_font_title, C_DIM, 12, 12);

    ui.lbl_batt_soc = lv_label_create(p_sol);
    lv_label_set_text(ui.lbl_batt_soc, "--%");
    lv_obj_set_style_text_font(ui.lbl_batt_soc, s_font_22, 0);
    lv_obj_set_style_text_color(ui.lbl_batt_soc, C_TEXT, 0);
    lv_obj_set_pos(ui.lbl_batt_soc, BATT_X, 8);
    lv_obj_set_width(ui.lbl_batt_soc, TANK_W);
    lv_obj_set_style_text_align(ui.lbl_batt_soc, LV_TEXT_ALIGN_CENTER, 0);

    ui.lbl_solar_status  = make_label(p_sol, "--", s_font_22, C_TEXT, 12, 52);

    // Each reading is a flex row [value (s_font_20) | unit (s_font_14)] whose
    // children are bottom-aligned so the small unit sits on the value baseline.
    // The smaller unit narrows the readout, freeing room for the battery column.
    auto make_reading = [&](int y, lv_color_t color,
                            lv_obj_t** val_out, lv_obj_t** unit_out) {
        lv_obj_t* row = lv_obj_create(p_sol);
        lv_obj_set_pos(row, 12, y);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 3, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        *val_out  = make_label(row, "--", s_font_20, color, 0, 0);
        *unit_out = make_label(row, "",   s_font_14, color, 0, 0);
        // Nudge the unit up 3 px: bottom-aligned boxes don't share a baseline
        // because the two fonts have different descender heights.
        lv_obj_set_style_translate_y(*unit_out, -3, 0);
    };
    make_reading( 82, C_CYAN,    &ui.lbl_solar_current, &ui.lbl_solar_current_u);
    make_reading(111, C_CYAN_BR, &ui.lbl_solar_power,   &ui.lbl_solar_power_u);
    make_reading(140, C_YELLOW,  &ui.lbl_solar_yield,   &ui.lbl_solar_yield_u);
    // Per-unit baseline tweaks (make_reading lifts every unit by 3 px).
    lv_obj_set_style_translate_y(ui.lbl_solar_current_u, -2, 0);  // "A"
    lv_obj_set_style_translate_y(ui.lbl_solar_yield_u,   -2, 0);  // "kWh"

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

    // ── Battery power port (CARGA / DESCARGA) ─────────────────────────────────
    // Mirrors the web BATERÍA panel: a green/red port below the SOC bar whose
    // header and body recolour with charge direction, fed by a vertical flow
    // line that animates while power moves.  Driven by Ultimatron V × A.
    constexpr int BFLOW_X = BATT_X + TANK_W / 2 - 3;   // centred under the bar
    constexpr int BFLOW_Y = BATT_Y + TANK_H_BATT;      // bar bottom
    constexpr int BFLOW_W = 6;
    constexpr int BFLOW_H = 18;

    ui.flow_bpwr = lv_obj_create(p_sol);
    lv_obj_set_pos(ui.flow_bpwr, BFLOW_X, BFLOW_Y);
    lv_obj_set_size(ui.flow_bpwr, BFLOW_W, BFLOW_H);
    lv_obj_set_style_bg_color(ui.flow_bpwr, lv_color_hex(0xaed7f2), 0);
    lv_obj_set_style_bg_opa(ui.flow_bpwr, LV_OPA_30, 0);
    lv_obj_set_style_border_width(ui.flow_bpwr, 0, 0);
    lv_obj_set_style_radius(ui.flow_bpwr, 0, 0);
    lv_obj_set_style_pad_all(ui.flow_bpwr, 0, 0);
    lv_obj_clear_flag(ui.flow_bpwr, LV_OBJ_FLAG_SCROLLABLE);

    // Stripe overlay — rows every 8 px (4 px stripe + 4 px gap); the timer
    // below translates the container by phase to animate vertical motion.
    ui.flow_bpwr_str = lv_obj_create(p_sol);
    lv_obj_set_pos(ui.flow_bpwr_str, BFLOW_X, BFLOW_Y);
    lv_obj_set_size(ui.flow_bpwr_str, BFLOW_W, BFLOW_H);
    lv_obj_set_style_bg_opa(ui.flow_bpwr_str, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.flow_bpwr_str, 0, 0);
    lv_obj_set_style_pad_all(ui.flow_bpwr_str, 0, 0);
    lv_obj_clear_flag(ui.flow_bpwr_str, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui.flow_bpwr_str, LV_OBJ_FLAG_HIDDEN);
    for (int sy = -8; sy <= BFLOW_H; sy += 8) {
        lv_obj_t* r = lv_obj_create(ui.flow_bpwr_str);
        lv_obj_set_pos(r, 0, sy);
        lv_obj_set_size(r, BFLOW_W, 4);
        lv_obj_set_style_bg_color(r, lv_color_hex(0xaed7f2), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_radius(r, 0, 0);
        lv_obj_set_style_pad_all(r, 0, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    }

    {
        // Port spans 24 px wider than the bar so "DESCARGA" fits; stays
        // centred on the bar (and thus on the flow line) above it.
        auto r = make_io_box(p_sol, BATT_X - 12, 142, TANK_W + 24, 28,
                             t(TK::BATT_CHARGE), 18);
        ui.lbl_bpwr_w = r.val;
        ui.box_bpwr = r.box; ui.hdr_bpwr = r.head; ui.hdr_bpwr_lbl = r.head_lbl;
    }

    // Vertical zebra driver: +dir = charging (stripes climb toward the bar),
    // −dir = discharging (stripes fall toward the port).  60 ms × 8 phases.
    lv_timer_create([](lv_timer_t*) {
        if (!ui.flow_bpwr_str || ui.flow_bpwr_dir == 0) return;
        static uint8_t phase = 0;
        phase = (phase + 1) & 7;
        lv_obj_set_style_translate_y(ui.flow_bpwr_str,
            ui.flow_bpwr_dir > 0 ? -(int)phase : (int)phase, 0);
    }, 60, NULL);

    // ── INVERSOR panel (Victron VE.Bus / Multiplus dongle) ───────────────────
    // Right slot of row 2 (283 × ROW2_H).  Layout copied from the EEZ
    // project: MAINS box on the left, Multiplus icon in the middle, LOAD
    // box top-right and BAT box bottom-right, plus three flow lines.
    // Read-only for now — ON/OFF requires VE.Bus GATT, not advertising.
    {
        lv_obj_t* p_inv = make_section(scr, INV_X, ROW2_Y, INV_W, ROW2_H);

        make_label(p_inv, t(TK::INVERTER), s_font_title, C_DIM, 10, 12);

        {
            auto r = make_io_box(p_inv, 15, 103, 95, 29, t(TK::INV_MAINS));
            ui.lbl_inv_mains_w = r.val;
            ui.box_mains = r.box; ui.hdr_mains = r.head; ui.hdr_mains_lbl = r.head_lbl;
        }
        {
            auto r = make_io_box(p_inv, 248, 56, 98, 29, t(TK::INV_LOADS));
            ui.lbl_inv_load_w = r.val;
            ui.box_load = r.box; ui.hdr_load = r.head;
        }
        {
            auto r = make_io_box(p_inv, 248, 120, 98, 29, "BAT.");
            ui.lbl_inv_batt_w = r.val;
            ui.box_batt = r.box; ui.hdr_batt = r.head; ui.hdr_batt_lbl = r.head_lbl;
        }

        // Multiplus body — matches the web redesign: blue body with corner
        // ears, centered orange bar, dark display window, dark foot trim.
        constexpr int MPX_X = 140, MPX_Y = 60, MPX_W = 78, MPX_H = 96;
        constexpr int FOOT_H = (int)(MPX_H * 0.18f);
        constexpr int BLUE_H = MPX_H - FOOT_H;

        lv_obj_t* mp = lv_obj_create(p_inv);
        lv_obj_set_pos(mp, MPX_X, MPX_Y);
        lv_obj_set_size(mp, MPX_W, MPX_H);
        lv_obj_set_style_bg_color(mp, lv_color_hex(0x0077c5), 0);
        lv_obj_set_style_bg_opa(mp, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(mp, lv_color_hex(0x3a3a4a), 0);
        lv_obj_set_style_border_width(mp, 2, 0);
        lv_obj_set_style_radius(mp, 3, 0);
        lv_obj_set_style_pad_all(mp, 0, 0);
        lv_obj_clear_flag(mp, LV_OBJ_FLAG_SCROLLABLE);

        // Corner ears (dark rectangles at top-left and top-right)
        auto make_ear = [&](int x) {
            lv_obj_t* e = lv_obj_create(mp);
            lv_obj_set_pos(e, x, 0);
            lv_obj_set_size(e, (int)(MPX_W * 0.22f), 12);  // EAR_W
            lv_obj_set_style_bg_color(e, C_BG, 0);
            lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(e, 0, 0);
            lv_obj_set_style_radius(e, 0, 0);
            lv_obj_clear_flag(e, LV_OBJ_FLAG_SCROLLABLE);
        };
        // mp has border_width=2 + pad_all=0, so the content area is
        // (MPX_W − 4) px wide.  Placing the right ear at MPX_W − ear_w would
        // overflow the content box by 4 px and LVGL clips that excess, making
        // the right ear render visibly narrower than the left.  Subtract the
        // two border widths (4 px) so the right edge sits on the content
        // boundary and the two ears measure the same on screen.
        constexpr int EAR_W = (int)(MPX_W * 0.22f);
        make_ear(0);
        make_ear(MPX_W - 4 - EAR_W);

        // Orange accent bar (centered in the blue area)
        lv_obj_t* ob = lv_obj_create(mp);
        int barW = (int)(MPX_W * 0.84f);
        lv_obj_set_pos(ob, (MPX_W - barW) / 2 - 1, BLUE_H / 2 - 8);
        lv_obj_set_size(ob, barW, 5);
        lv_obj_set_style_bg_color(ob, lv_color_hex(0xf96432), 0);
        lv_obj_set_style_bg_opa(ob, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ob, 0, 0);
        lv_obj_set_style_radius(ob, 1, 0);
        lv_obj_clear_flag(ob, LV_OBJ_FLAG_SCROLLABLE);

        // Dark display window (below the bar, left-aligned)
        lv_obj_t* dw = lv_obj_create(mp);
        lv_obj_set_pos(dw, (int)(MPX_W * 0.12f), BLUE_H / 2 + 2);
        lv_obj_set_size(dw, (int)(MPX_W * 0.30f), 8);
        lv_obj_set_style_bg_color(dw, lv_color_hex(0x1a3a5c), 0);
        lv_obj_set_style_bg_opa(dw, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dw, 0, 0);
        lv_obj_set_style_radius(dw, 1, 0);
        lv_obj_clear_flag(dw, LV_OBJ_FLAG_SCROLLABLE);

        // Dark foot trim (bottom ~18%)
        lv_obj_t* ft = lv_obj_create(mp);
        lv_obj_set_pos(ft, 0, BLUE_H);
        lv_obj_set_size(ft, MPX_W, FOOT_H);
        lv_obj_set_style_bg_color(ft, C_BG, 0);
        lv_obj_set_style_bg_opa(ft, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ft, 0, 0);
        lv_obj_set_style_radius(ft, 0, 0);
        lv_obj_clear_flag(ft, LV_OBJ_FLAG_SCROLLABLE);

        // State label centered in the empty band below the dark display
        // window and above the foot trim — the visual "dead space" inside
        // the blue body where the value reads most clearly.
        ui.lbl_inv_state = lv_label_create(mp);
        lv_label_set_text(ui.lbl_inv_state, "--");
        lv_obj_set_style_text_color(ui.lbl_inv_state, C_TEXT, 0);
        lv_obj_set_style_text_font(ui.lbl_inv_state, s_font_14, 0);
        constexpr int DW_BOTTOM = BLUE_H / 2 + 2 + 8;     // dark display window bottom
        constexpr int STATE_H   = 14;                     // ~ s_font_14 height
        // LV_ALIGN_TOP_MID centers the label horizontally against the
        // parent's geometric center (border-aware), which a plain x=0 +
        // width=MPX_W + TEXT_ALIGN_CENTER would not, since the 2 px
        // border shifts the visible interior.
        lv_obj_align(ui.lbl_inv_state, LV_ALIGN_TOP_MID, 0,
                     DW_BOTTOM + (BLUE_H - DW_BOTTOM - STATE_H) / 2);

        auto make_flow = [&](int x, int y, int w,
                             lv_obj_t** out, lv_obj_t** stripes_out) {
            lv_obj_t* f = lv_obj_create(p_inv);
            lv_obj_set_pos(f, x, y);
            lv_obj_set_size(f, w, 5);
            lv_obj_set_style_bg_color(f, lv_color_hex(0xaed7f2), 0);
            lv_obj_set_style_bg_opa(f, LV_OPA_30, 0);
            lv_obj_set_style_border_width(f, 0, 0);
            lv_obj_set_style_radius(f, 0, 0);
            lv_obj_set_style_pad_all(f, 0, 0);
            lv_obj_clear_flag(f, LV_OBJ_FLAG_SCROLLABLE);
            *out = f;

            // Stripe overlay used to animate the flow when power is moving.
            // Children anchored every 8 px (4 px stripe + 4 px gap); a global
            // LVGL timer translates the container by phase = 0..7 each tick.
            lv_obj_t* s = lv_obj_create(p_inv);
            lv_obj_set_pos(s, x, y);
            lv_obj_set_size(s, w, 5);
            lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(s, 0, 0);
            lv_obj_set_style_pad_all(s, 0, 0);
            lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
            for (int sx = -8; sx <= w; sx += 8) {
                lv_obj_t* r = lv_obj_create(s);
                lv_obj_set_pos(r, sx, 0);
                lv_obj_set_size(r, 4, 5);
                lv_obj_set_style_bg_color(r, lv_color_hex(0xaed7f2), 0);
                lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(r, 0, 0);
                lv_obj_set_style_radius(r, 0, 0);
                lv_obj_set_style_pad_all(r, 0, 0);
                lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
            }
            *stripes_out = s;
        };
        // MAINS box ends at x=110, MPX starts at MPX_X=140 → gap 30px
        make_flow(110, 106, 30, &ui.flow_mains, &ui.flow_mains_str);
        // MPX ends at MPX_X+MPX_W=218, LOAD/BATT boxes start at x=248 → gap 30px
        make_flow(218, 69,  30, &ui.flow_load,  &ui.flow_load_str);
        make_flow(218, 132, 30, &ui.flow_batt,  &ui.flow_batt_str);

        // Zebra animation driver — translates each active stripe container
        // by phase px on every tick.  Period 60 ms × 8 phases = 480 ms / cycle.
        lv_timer_create([](lv_timer_t*) {
            static uint8_t phase = 0;
            phase = (phase + 1) & 7;
            auto step = [](lv_obj_t* s, int8_t dir) {
                if (!s || dir == 0) return;
                lv_obj_set_style_translate_x(s, dir > 0 ? phase : -(int)phase, 0);
            };
            step(ui.flow_mains_str, ui.flow_mains_dir);
            step(ui.flow_load_str,  ui.flow_load_dir);
            step(ui.flow_batt_str,  ui.flow_batt_dir);
        }, 60, nullptr);
    }

    // ── AGUA LIMPIA panel (fresh-water tank, BTHome) ──────────────────────────
    // Tank graphic centered horizontally so it leaves the same gap against
    // the left separator and the right panel border.  AGUA_W=224, body w=146
    // → (224 − 146) / 2 = 39 px gap on each side.  Neck and percent label
    // shift with the body to keep their relative alignment.
    {
        lv_obj_t* p_tank = make_section(scr, AGUA_X, CONTENT_Y, AGUA_W, ROW1_H);

        make_label(p_tank, t(TK::FRESH_WATER), s_font_title, C_DIM, 17, 12);

        ui.lbl_tank_pct = lv_label_create(p_tank);
        lv_label_set_text(ui.lbl_tank_pct, "-- %");
        lv_obj_set_style_text_font(ui.lbl_tank_pct, s_font_22, 0);
        lv_obj_set_style_text_color(ui.lbl_tank_pct, C_TEXT, 0);
        lv_obj_set_pos(ui.lbl_tank_pct, 80, 59);          // centered above body (body center 112, label w=64)
        lv_obj_set_width(ui.lbl_tank_pct, 64);
        lv_obj_set_style_text_align(ui.lbl_tank_pct, LV_TEXT_ALIGN_CENTER, 0);

        // Neck cap on top-right of the tank body — small grey rectangle
        // protruding 6 px above the body top, like a screw-cap.
        lv_obj_t* neck = lv_obj_create(p_tank);
        lv_obj_set_size(neck, 17, 6);
        lv_obj_set_pos(neck, 161, 92);                     // body right (185) − neck w (17) − 7 px gap
        lv_obj_set_style_bg_color(neck, C_BORDER_BAT, 0);
        lv_obj_set_style_bg_opa(neck, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(neck, 0, 0);
        lv_obj_set_style_radius(neck, 1, 0);
        lv_obj_clear_flag(neck, LV_OBJ_FLAG_SCROLLABLE);

        // Tank body — vertical bar that owns its own border/background so we
        // don't need a separate container.  Vertical orientation: indicator
        // rises from bottom proportional to pct.
        ui.bar_tank = lv_bar_create(p_tank);
        lv_obj_set_pos(ui.bar_tank, 39, 96);
        lv_obj_set_size(ui.bar_tank, 146, 71);
        lv_bar_set_range(ui.bar_tank, 0, 100);
        lv_bar_set_value(ui.bar_tank, 0, LV_ANIM_OFF);
        lv_bar_set_orientation(ui.bar_tank, LV_BAR_ORIENTATION_VERTICAL);
        lv_obj_set_style_bg_color(ui.bar_tank, C_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ui.bar_tank, C_WATER_COLD, LV_PART_INDICATOR);
        lv_obj_set_style_radius(ui.bar_tank, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(ui.bar_tank, 0, LV_PART_INDICATOR);
        lv_obj_set_style_border_color(ui.bar_tank, C_BORDER_BAT, LV_PART_MAIN);
        lv_obj_set_style_border_width(ui.bar_tank, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ui.bar_tank, 2, 0);
    }

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
    // Start empty: this slot is for actions/errors only.  The main screen is
    // built after the 2 s splash, by which point the boot "Iniciando…" phase
    // is over — baking it in here left it stuck (the clear in main.cpp fires
    // its -1→0 transition before this label exists).
    lv_label_set_text(ui.lbl_status, "");
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
    make_sep(scr, WATER_X,             CONTENT_Y,    1,  ROW1_H);  // CALEFACCIÓN | AGUA CALIENTE
    make_sep(scr, WATER_X + WATER_W + 3, CONTENT_Y,  1,  ROW1_H);  // AGUA CALIENTE right edge (overlaps the AGUA separator at AGUA_X+3)
    make_sep(scr, SOLAR_X,             ROW2_Y,        1,  ROW2_H);  // VENTILADOR | SOLAR
    make_sep(scr, SOLAR_X + SOLAR_W,   ROW2_Y,        1,  ROW2_H);  // SOLAR right edge
    make_sep(scr, AGUA_X + 3,          CONTENT_Y,     1,  ROW1_H);  // AGUA CALIENTE | AGUA LIMPIA
    make_sep(scr, INV_X,               ROW2_Y,        1,  ROW2_H);  // SOLAR | INVERSOR
    make_sep(scr, 0,                   ROW2_Y - 1,    HEAT_W,  1);  // CALEFACCIÓN / VENTILADOR
    make_sep(scr, WATER_X,             ROW2_Y - 1,    WATER_W, 1);  // AGUA CALIENTE / SOLAR
    make_sep(scr, AGUA_X,              ROW2_Y - 1,    AGUA_W,  1);  // AGUA LIMPIA / INVERSOR
    make_sep(scr, 0,       H - STATUS_H - 1, W,   1);   // above status bar

    lv_screen_load(scr);
    if (s_main_scr) lv_obj_delete(s_main_scr);
    s_main_scr = scr;
    refresh_controls();
}

// ── Refresh widgets from local state ──────────────────────────────────────────

// Update the topbar heat/cool indicator.  Shows a snowflake (blue) whenever
// the A/C cooling mode is selected; otherwise shows the flame in amber.  Both
// glyphs follow the selected mode but stay dim until the appliance confirms it
// is active: the flame on (s_linOk && heatingOn), the snowflake on the A/C's
// BLE telemetry (s_acConnected) — mirrors the web refreshIndicators() gating.
static void refresh_flame_icon()
{
    bool cooling = st.openairConfigured && (st.acMode == 1 || st.acMode == 2);
    if (cooling) {
        lv_label_set_text(ui.icon_flame, FA_SNOWFLAKE);
        lv_obj_set_style_text_color(ui.icon_flame, lv_color_hex(0x44aaff), 0);
        lv_obj_set_style_text_opa(ui.icon_flame,
            s_acConnected ? LV_OPA_COVER : LV_OPA_30, 0);
    } else {
        lv_label_set_text(ui.icon_flame, FA_FIRE);
        lv_obj_set_style_text_color(ui.icon_flame, C_AMBER, 0);
        lv_obj_set_style_text_opa(ui.icon_flame,
            (s_linOk && st.heatingOn) ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

// Set a one-of-N selection on a button matrix via the CHECKED ctrl bit.
static void bm_select(lv_obj_t* bm, int sel, int count)
{
    for (int i = 0; i < count; ++i) {
        if (i == sel)
            lv_buttonmatrix_set_button_ctrl(bm, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        else
            lv_buttonmatrix_clear_button_ctrl(bm, i, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
}

static void ac_btn_select(int sel)  // sel 0–3: cool/eco/heat/off
{
    for (int i = 0; i < 4; ++i) {
        if (i == sel) lv_obj_add_state(ui.btn_ac[i], LV_STATE_CHECKED);
        else          lv_obj_clear_state(ui.btn_ac[i], LV_STATE_CHECKED);
    }
}

static void refresh_controls()
{
    char buf[20];
    snprintf(buf, sizeof(buf), "%.1f°C", st.roomSetpoint);
    lv_label_set_text(ui.lbl_room_sp, buf);

    // ── Heat-source panel: CALEFACCIÓN (Truma only) vs CLIMATIZACIÓN (A/C) ──
    if (st.openairConfigured) {
        lv_label_set_text(ui.lbl_heat_title, t(TK::CLIMATE));
        lv_obj_add_flag(ui.btnmx_heat, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui.row_ac, LV_OBJ_FLAG_HIDDEN);
        int sel = st.heatingOn ? 2 : (st.acMode == 1) ? 0 : (st.acMode == 2) ? 1 : 3;
        ac_btn_select(sel);
        // Setpoint applies whenever any mode (cool/eco/heat) is active.
        bool active = st.heatingOn || st.acMode != 0;
        if (active) lv_obj_remove_flag(ui.row_sp, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(ui.row_sp, LV_OBJ_FLAG_HIDDEN);
        // Power buttons: only in cool (1) or eco (2) — not heat, not off.
        bool showPwr = !st.heatingOn && (st.acMode == 1 || st.acMode == 2);
        for (int i = 0; i < 2; ++i) {
            if (showPwr) {
                lv_obj_remove_flag(ui.btn_pwr[i], LV_OBJ_FLAG_HIDDEN);
                if (i == st.acPower) lv_obj_add_state(ui.btn_pwr[i], LV_STATE_CHECKED);
                else                 lv_obj_clear_state(ui.btn_pwr[i], LV_STATE_CHECKED);
            } else {
                lv_obj_add_flag(ui.btn_pwr[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_label_set_text(ui.lbl_heat_title, t(TK::HEATING));
        lv_obj_add_flag(ui.row_ac, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui.btnmx_heat, LV_OBJ_FLAG_HIDDEN);
        bm_select(ui.btnmx_heat, st.heatingOn ? 1 : 0, 2);
        if (st.heatingOn) lv_obj_remove_flag(ui.row_sp, LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_add_flag(ui.row_sp, LV_OBJ_FLAG_HIDDEN);
        // Hide power buttons in Truma-only (calefacción) mode.
        for (int i = 0; i < 2; ++i)
            lv_obj_add_flag(ui.btn_pwr[i], LV_OBJ_FLAG_HIDDEN);
    }

    // ── VENTILADOR panel ──
    // A/C fan context: A/C configured and in cool or eco mode.
    // acMode==0 (off) falls through to Truma standby fan controls (Off/On + 1-10).
    if (st.openairConfigured && !st.heatingOn && st.acMode != 0) {
        lv_obj_add_flag(ui.btnmx_fan_heat, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.btnmx_fan_off,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.btn_fan_dn,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.btn_fan_up,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.lbl_fan_lvl,    LV_OBJ_FLAG_HIDDEN);

        lv_obj_remove_flag(ui.btnmx_ac_fan, LV_OBJ_FLAG_HIDDEN);
        bool cool = (st.acMode == 1);
        // Auto/Man only selectable in cool mode; eco/off lock it to Auto.
        bool autoSel = cool ? st.acFanAuto : true;
        bm_select(ui.btnmx_ac_fan, autoSel ? 0 : 1, 2);
        for (int i = 0; i < 2; ++i) {
            if (cool) lv_buttonmatrix_clear_button_ctrl(ui.btnmx_ac_fan, i, LV_BUTTONMATRIX_CTRL_DISABLED);
            else      lv_buttonmatrix_set_button_ctrl(ui.btnmx_ac_fan, i, LV_BUTTONMATRIX_CTRL_DISABLED);
        }
        lv_obj_set_style_opa(ui.btnmx_ac_fan, cool ? LV_OPA_COVER : LV_OPA_40, 0);

        // Speed selector only in cool + Man.
        if (cool && !st.acFanAuto) {
            if (st.acFanSpeed < 1) st.acFanSpeed = 1;
            if (st.acFanSpeed > 6) st.acFanSpeed = 6;
            char b[8]; snprintf(b, sizeof(b), "%d", st.acFanSpeed);
            lv_label_set_text(ui.lbl_acfan_lvl, b);
            lv_obj_remove_flag(ui.btn_acfan_dn,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui.btn_acfan_up,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui.lbl_acfan_lvl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui.btn_acfan_dn,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui.btn_acfan_up,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui.lbl_acfan_lvl, LV_OBJ_FLAG_HIDDEN);
        }
        goto boiler;
    }

    lv_obj_add_flag(ui.btnmx_ac_fan,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.btn_acfan_dn,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.btn_acfan_up,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.lbl_acfan_lvl,  LV_OBJ_FLAG_HIDDEN);

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

boiler:
    int bi = (st.boilerMode >= 0 && st.boilerMode <= 3) ? st.boilerMode : 0;
    for (int i = 0; i < 4; ++i) {
        if (i == bi)
            lv_buttonmatrix_set_button_ctrl(ui.btnmx_boiler, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        else
            lv_buttonmatrix_clear_button_ctrl(ui.btnmx_boiler, i, LV_BUTTONMATRIX_CTRL_CHECKED);
    }

    refresh_flame_icon();
}

// ── Event callbacks ───────────────────────────────────────────────────────────

static void on_sp_dn(lv_event_t*)
{
    if (st.roomSetpoint > 5.0f) st.roomSetpoint -= 0.5f;
    refresh_controls();
}

static void on_sp_up(lv_event_t*)
{
    if (st.roomSetpoint < 30.0f) st.roomSetpoint += 0.5f;
    refresh_controls();
}

static void on_heat_changed(lv_event_t* e)
{
    lv_obj_t* bm = (lv_obj_t*)lv_event_get_target(e);
    uint32_t  idx = lv_buttonmatrix_get_selected_button(bm);
    st.heatingOn = (idx == 1);
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

// Apply the Truma heat-fan rule shared by the heat toggle and the climate
// "heat" button: heat-on defaults the fan to eco; heat-off clears eco/high.
static void apply_heat_fan_rule()
{
    if (st.heatingOn && (st.fanMode < 1 || st.fanMode > 2)) st.fanMode = 1;
    if (!st.heatingOn && st.fanMode >= 1 && st.fanMode <= 2) st.fanMode = 0;
}

static void on_ac_fan_changed(lv_event_t* e)
{
    lv_obj_t* bm  = (lv_obj_t*)lv_event_get_target(e);
    uint32_t  idx = lv_buttonmatrix_get_selected_button(bm);
    if (idx <= 1) {
        st.acFanAuto = (idx == 0);
        refresh_controls();
    }
}

static void on_acfan_dn(lv_event_t*)
{
    if (st.acFanSpeed > 1) { st.acFanSpeed--; refresh_controls(); }
}

static void on_acfan_up(lv_event_t*)
{
    if (st.acFanSpeed < 6) { st.acFanSpeed++; refresh_controls(); }
}

static void on_ac_btn_clicked(lv_event_t* e)
{
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    int idx = -1;
    for (int i = 0; i < 4; ++i) {
        if (btn == ui.btn_ac[i]) { idx = i; break; }
    }
    if (idx < 0) return;
    // 0=cool(acMode=1), 1=eco(acMode=2), 2=heat(Truma), 3=off(acMode=0)
    st.heatingOn = (idx == 2);
    st.acMode    = (idx == 0) ? 1 : (idx == 1) ? 2 : 0;
    if (st.heatingOn) apply_heat_fan_rule();
    refresh_controls();
}

static void on_pwr_clicked(lv_event_t* e)
{
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx == 0 || idx == 1) {
        st.acPower = idx;
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
static lv_obj_t*     s_wake_overlay      = nullptr;  // transparent shield on
                                                     // lv_layer_top() while
                                                     // blanked — swallows the
                                                     // touch that wakes the
                                                     // panel so it doesn't
                                                     // hit any widget.

// Dim level = 20 % of normal brightness, floor 8.
static int dim_level() { int d = s_brightness_normal / 5; return d < 8 ? 8 : d; }

static void icon_blink_timer_cb(lv_timer_t*);   // defined below

static void wake_overlay_release_cb(lv_event_t*)
{
    if (s_wake_overlay) {
        lv_obj_del(s_wake_overlay);
        s_wake_overlay = nullptr;
    }
}

static void install_wake_overlay()
{
    if (s_wake_overlay) return;
    s_wake_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wake_overlay);
    lv_obj_set_size(s_wake_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_wake_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_wake_overlay, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_wake_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_CLICKABLE);
    // Remove on release OR press-lost (finger drag off the panel) so the
    // overlay can never stick around once the user is no longer touching.
    lv_obj_add_event_cb(s_wake_overlay, wake_overlay_release_cb,
                        LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_wake_overlay, wake_overlay_release_cb,
                        LV_EVENT_PRESS_LOST, nullptr);
}

static void screen_timeout_cb(lv_timer_t*) {
    if (!s_disp) return;

    // Animate brightness toward target.
    if (s_brightness != s_target) {
        const int step = (s_brightness < s_target) ? 10 : 2;
        const int diff = s_target - s_brightness;
        s_brightness  += (diff > step) ? step : (diff < -step) ? -step : diff;
        bsp_display_brightness_set(s_brightness);
    }

    // Keep the panel fully awake while a firmware update is running, so the
    // download/flash progress stays visible regardless of the screen timeout.
    if (p4OtaInstalling()) {
        s_target  = s_brightness_normal;
        s_dimmed  = false;
        s_blanked = false;
        lv_display_trigger_activity(s_disp);   // reset idle so it won't dim/blank
        return;
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
        // Shield UI from the next press so the touch that wakes the panel
        // can't also click a button underneath.  The overlay tears down on
        // its own release/press-lost event.
        install_wake_overlay();
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

    if (lvglLock(1000)) {
        load_fonts();
        build_splash(lv_screen_active());
        s_splash_tick = lv_tick_get();

        // Screen timeout + brightness — read NVS.
        uint8_t idx   = 0;   // default: 30 s
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
        // 500 ms repaint cadence for the topbar status icons — provides the
        // CONNECTING blink without coupling to main.cpp's loop frequency.
        lv_timer_create(icon_blink_timer_cb, 500, nullptr);
        if (s_timeout_ms > 0)
            ESP_LOGI(TAG, "screen timeout: %lu ms, normal brightness: %d%%",
                     (unsigned long)s_timeout_ms, (int)brite);

        lvglUnlock();
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
    out.acMode       = st.acMode;
    out.acFanAuto    = st.acFanAuto;
    out.acFanSpeed   = st.acFanSpeed;
    out.acPower      = st.acPower;
}

// Remote setters (called from the WS dispatcher).  Take the LVGL lock so the
// st mutation + widget refresh happen atomically with the LVGL refresh task.
// A short timeout keeps a stuck UI from blocking the WS task; on timeout we
// silently drop the write (the next remote update will retry).
void p4SetHeating(bool on)
{
    if (!lvglLock(50)) return;
    st.heatingOn = on;
    // Mirror the on-screen rule: turning heat off clears active heat-fan
    // modes; turning it on defaults to eco if currently in level mode.
    if (st.heatingOn && (st.fanMode < 1 || st.fanMode > 2)) st.fanMode = 1;
    if (!st.heatingOn && st.fanMode >= 1 && st.fanMode <= 2) st.fanMode = 0;
    refresh_controls();
    lvglUnlock();
}

void p4SetFanMode(int mode)
{
    if (mode < 0)  mode = 0;
    if (mode > 12) mode = 12;
    if (!lvglLock(50)) return;
    st.fanMode = mode;
    refresh_controls();
    lvglUnlock();
}

void p4SetBoilerMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    if (!lvglLock(50)) return;
    st.boilerMode = mode;
    refresh_controls();
    lvglUnlock();
}

void p4SetEnergyIdx(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 4) idx = 4;
    if (!lvglLock(50)) return;
    st.energyIdx = idx;
    refresh_controls();
    lvglUnlock();
}

void p4SetRoomSetpoint(float celsius)
{
    if (celsius < 5.0f)  celsius = 5.0f;
    if (celsius > 30.0f) celsius = 30.0f;
    if (!lvglLock(50)) return;
    st.roomSetpoint = celsius;
    refresh_controls();
    lvglUnlock();
}

void p4SetAcMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    if (!lvglLock(50)) return;
    st.acMode = mode;
    // Selecting a cooling mode clears Truma heat; "heat" arrives via p4SetHeating.
    if (mode != 0) { st.heatingOn = false; apply_heat_fan_rule(); }
    refresh_controls();
    lvglUnlock();
}

void p4SetAcFan(bool autoMode, int speed)
{
    if (speed < 1) speed = 1;
    if (speed > 6) speed = 6;
    if (!lvglLock(50)) return;
    st.acFanAuto  = autoMode;
    st.acFanSpeed = speed;
    refresh_controls();
    lvglUnlock();
}

void p4SetAcPower(int idx)
{
    if (idx < 0 || idx > 1) return;
    if (!lvglLock(50)) return;
    st.acPower = idx;
    refresh_controls();
    lvglUnlock();
}

// Latest tunnel UI state, written by main loop, read by the LVGL blink
// timer.  uint8 reads/writes are atomic on RISC-V, no lock needed.
static volatile uint8_t s_tunnel_state = 0;

void p4SetTunnelState(uint8_t state) { s_tunnel_state = state; }

// ── Firmware-update reminder icon + prompt modal ──────────────────────────
static lv_obj_t* s_ota_prompt = nullptr;   // active "update now?" modal, or null

void p4SetUpdateAvailable(bool available)
{
    if (!ui.icon_ota) return;
    if (!lvglLock(50)) return;
    if (available) lv_obj_clear_flag(ui.icon_ota, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(ui.icon_ota,   LV_OBJ_FLAG_HIDDEN);
    lvglUnlock();
}

static void ota_prompt_close()
{
    if (s_ota_prompt) {
        lv_obj_delete(s_ota_prompt);
        s_ota_prompt = nullptr;
    }
}

static void on_ota_prompt_now(lv_event_t*)
{
    ota_prompt_close();
    p4OtaInstall();
}

static void on_ota_prompt_later(lv_event_t*)
{
    ota_prompt_close();
}

bool p4DisplayShowUpdatePrompt(const char* from_ver, const char* to_ver)
{
    if (!lvglLock(50)) return false;
    // Only prompt while the main screen is up — never on top of a settings
    // screen, the OTA progress screen, or the splash.
    if (!s_main_scr || lv_screen_active() != s_main_scr || s_ota_prompt) {
        lvglUnlock();
        return false;
    }

    // Dimming backdrop covering the whole UI.
    lv_obj_t* back = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(back, 0, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_50, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);   // swallow taps behind modal
    s_ota_prompt = back;

    // Centered dialog box.
    lv_obj_t* box = lv_obj_create(back);
    lv_obj_set_size(box, 480, 260);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, C_TOPBAR, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, C_BTN_ACTIVE, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon = lv_label_create(box);
    lv_label_set_text(icon, FA_DOWNLOAD);
    lv_obj_set_style_text_font(icon, s_font_icons24 ? s_font_icons24 : s_font_20, 0);
    lv_obj_set_style_text_color(icon, C_TEXT, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t* title = lv_label_create(box);
    lv_label_set_text(title, t(TK::OTA_AVAILABLE));
    lv_obj_set_style_text_font(title, s_font_title ? s_font_title : s_font_24, 0);
    lv_obj_set_style_text_color(title, C_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 38);

    char ver_buf[48];
    snprintf(ver_buf, sizeof(ver_buf), "v%s  ->  v%s",
             from_ver ? from_ver : "?", to_ver ? to_ver : "?");
    lv_obj_t* ver_lbl = lv_label_create(box);
    lv_label_set_text(ver_lbl, ver_buf);
    lv_obj_set_style_text_font(ver_lbl, s_font_18, 0);
    lv_obj_set_style_text_color(ver_lbl, C_LABEL, 0);
    lv_obj_align(ver_lbl, LV_ALIGN_TOP_MID, 0, 78);

    lv_obj_t* prompt = lv_label_create(box);
    lv_label_set_text(prompt, t(TK::OTA_PROMPT));
    lv_obj_set_style_text_font(prompt, s_font_20, 0);
    lv_obj_set_style_text_color(prompt, C_TEXT, 0);
    lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 110);

    lv_obj_t* btn_now = lv_button_create(box);
    lv_obj_set_size(btn_now, 200, 56);
    lv_obj_align(btn_now, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    style_button(btn_now);
    lv_obj_set_style_bg_color(btn_now, C_BTN_ACTIVE, 0);
    lv_obj_t* l_now = lv_label_create(btn_now);
    lv_label_set_text(l_now, t(TK::OTA_UPDATE_NOW));
    lv_obj_set_style_text_font(l_now, s_font_20, 0);
    lv_obj_center(l_now);
    lv_obj_add_event_cb(btn_now, on_ota_prompt_now, LV_EVENT_CLICKED, NULL);

    lv_obj_t* btn_later = lv_button_create(box);
    lv_obj_set_size(btn_later, 200, 56);
    lv_obj_align(btn_later, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    style_button(btn_later);
    lv_obj_t* l_later = lv_label_create(btn_later);
    lv_label_set_text(l_later, t(TK::OTA_LATER));
    lv_obj_set_style_text_font(l_later, s_font_20, 0);
    lv_obj_center(l_later);
    lv_obj_add_event_cb(btn_later, on_ota_prompt_later, LV_EVENT_CLICKED, NULL);

    lvglUnlock();
    return true;
}

// ── Error/warning modal ───────────────────────────────────────────────────
// Pops over the main screen on a new A/C or Truma fault (driven from main.cpp),
// mirroring the web's error modal.  Single "Aceptar" button dismisses it; the
// status bar keeps the rotating alert while the fault persists.
static lv_obj_t*     s_err_modal = nullptr;
static volatile bool s_err_modal_dismissed = false;   // set on "Aceptar" tap

void p4DisplayHideErrorModal()
{
    if (!lvglLock(50)) return;
    if (s_err_modal) {
        lv_obj_delete(s_err_modal);
        s_err_modal = nullptr;
    }
    lvglUnlock();
}

// True once after the user taps "Aceptar" — consumed (cleared) by the read so
// main.cpp can attribute the dismissal to the fault the modal was showing.
bool p4DisplayErrorModalDismissed()
{
    bool d = s_err_modal_dismissed;
    s_err_modal_dismissed = false;
    return d;
}

static void on_err_modal_ok(lv_event_t*)
{
    if (s_err_modal) {
        lv_obj_delete(s_err_modal);
        s_err_modal = nullptr;
    }
    s_err_modal_dismissed = true;
}

bool p4DisplayShowErrorModal(const char* title, const char* sub,
                             const char* desc, uint32_t color)
{
    if (!lvglLock(50)) return false;
    // Only over the main screen — never on a settings/OTA screen or the splash.
    if (!s_main_scr || lv_screen_active() != s_main_scr) {
        lvglUnlock();
        return false;
    }
    if (s_err_modal) { lv_obj_delete(s_err_modal); s_err_modal = nullptr; }

    lv_obj_t* back = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(back, 0, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_50, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    s_err_modal = back;

    lv_obj_t* box = lv_obj_create(back);
    lv_obj_set_size(box, 520, 300);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, C_TOPBAR, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    // Warning-triangle icon (FA U+F071) at the top, in the severity colour.
    lv_obj_t* icon = lv_label_create(box);
    lv_label_set_text(icon, "\xEF\x81\xB1");
    lv_obj_set_style_text_font(icon, s_font_icons24 ? s_font_icons24 : s_font_20, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t* lbl_title = lv_label_create(box);
    lv_label_set_text(lbl_title, title ? title : "");
    lv_obj_set_style_text_font(lbl_title, s_font_title ? s_font_title : s_font_24, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(color), 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 40);

    if (sub && sub[0]) {
        lv_obj_t* lbl_sub = lv_label_create(box);
        lv_label_set_text(lbl_sub, sub);
        lv_obj_set_style_text_font(lbl_sub, s_font_18, 0);
        lv_obj_set_style_text_color(lbl_sub, C_LABEL, 0);
        lv_obj_align(lbl_sub, LV_ALIGN_TOP_MID, 0, 78);
    }

    lv_obj_t* lbl_desc = lv_label_create(box);
    lv_label_set_text(lbl_desc, desc ? desc : "");
    lv_label_set_long_mode(lbl_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_desc, 480);
    lv_obj_set_style_text_font(lbl_desc, s_font_18, 0);
    lv_obj_set_style_text_color(lbl_desc, C_TEXT, 0);
    lv_obj_align(lbl_desc, LV_ALIGN_TOP_MID, 0, 104);

    lv_obj_t* btn = lv_button_create(box);
    lv_obj_set_size(btn, 200, 56);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -4);
    style_button(btn);
    lv_obj_set_style_bg_color(btn, C_BTN_ACTIVE, 0);
    lv_obj_t* l_ok = lv_label_create(btn);
    lv_label_set_text(l_ok, t(TK::ACCEPT));
    lv_obj_set_style_text_font(l_ok, s_font_20, 0);
    lv_obj_center(l_ok);
    lv_obj_add_event_cb(btn, on_err_modal_ok, LV_EVENT_CLICKED, NULL);

    lvglUnlock();
    return true;
}

// LVGL timer (500 ms period): advances the CONNECTING blink phase for all
// status icons and re-derives the cloud icon from the volatile tunnel state,
// so the cloud stays responsive regardless of how often main.cpp polls
// wstunnelUiState() / calls p4DisplayUpdate().
static void icon_blink_timer_cb(lv_timer_t*)
{
    if (!ui.icon_cloud) return;
    s_iconBlink = !s_iconBlink;
    s_iconSt[3] = (s_tunnel_state == 2) ? IST_CONNECTED
                : (s_tunnel_state == 1) ? IST_CONNECTING
                : (s_tunnel_state == 3) ? IST_FAILED
                                        : IST_DISABLED;
    for (int i = 0; i < 4; i++) repaint_icon(i);
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

static const char* translate_multi_state(uint8_t mode)
{
    switch (mode) {
        case 0:   return t(TK::SOL_STATUS_OFF);
        case 1:   return t(TK::SOL_STATUS_LOW_POWER);
        case 2:   return t(TK::SOL_STATUS_FAULT);
        case 3:   return t(TK::SOL_STATUS_BULK);
        case 4:   return t(TK::SOL_STATUS_ABSORB);
        case 5:   return t(TK::SOL_STATUS_FLOAT);
        case 6:   return t(TK::MULTI_STATE_STORAGE);
        case 7:   return t(TK::MULTI_STATE_EQUALIZE);
        case 8:   return t(TK::MULTI_STATE_PASSTHRU);
        case 9:   return t(TK::MULTI_STATE_INVERTING);
        case 10:  return t(TK::MULTI_STATE_ASSIST);
        case 11:  return t(TK::MULTI_STATE_SUPPLY);
        case 252: return t(TK::MULTI_STATE_EXT_CTRL);
        default:  return "--";
    }
}

// Write the decimal of `v` into `buf` grouping thousands with a dot
// ("-1.234"), preserving the sign. Returns buf for inline use in *_fmt calls.
static const char* group_int(char* buf, size_t n, int v)
{
    char num[16];
    bool neg = v < 0;
    snprintf(num, sizeof(num), "%d", neg ? -v : v);
    int len = (int)strlen(num);
    size_t gi = 0;
    if (neg && gi + 1 < n) buf[gi++] = '-';
    for (int i = 0; i < len && gi + 1 < n; i++) {
        if (i > 0 && (len - i) % 3 == 0 && gi + 1 < n) buf[gi++] = '.';
        buf[gi++] = num[i];
    }
    buf[gi] = '\0';
    return buf;
}

void p4DisplayUpdate(const P4DisplayData& d)
{
    static char buf[40];

    if (!lvglLock(50)) return;

    if (!s_mainBuilt) {
        // Enforce minimum 1 s splash before switching to main screen.
        if (lv_tick_get() - s_splash_tick < 2000) {
            lvglUnlock();
            return;
        }
        build_main_screen();
        s_mainBuilt = true;
    }

    // st.* is the authoritative control state — only LVGL callbacks write it.
    // Do NOT overwrite it from d here; main.cpp reads it back via p4GetControlState.
    // Exception: openairConfigured is a config flag (NVS-derived), not a user
    // control; mirror it and rebuild the heat/fan panels when it flips.
    if (d.openairConfigured != st.openairConfigured) {
        st.openairConfigured = d.openairConfigured;
        refresh_controls();
    }

    if (!std::isnan(d.roomTemp))
        lv_label_set_text_fmt(ui.lbl_room_temp, "%.1f°C", d.roomTemp);
    else
        lv_label_set_text(ui.lbl_room_temp, "--°C");

    if (!std::isnan(d.outdoorTemp))
        lv_label_set_text_fmt(ui.lbl_outdoor, "%.1f°C", d.outdoorTemp);
    else
        lv_label_set_text(ui.lbl_outdoor, "--°C");

    // WiFi/LIN/BLE only report a boolean (or a 3-level BLE state), so each icon
    // stays dim until its subsystem starts in bootTask's sequence, then blinks
    // while attempting and falls to a struck-through FAILED if the grace window
    // expires without connecting.  derive_icon_state() owns that mapping.
    {
        uint32_t now = lv_tick_get();
        static uint32_t s_wifiDown = 0, s_linDown = 0, s_bleDown = 0;
        set_icon_state(0, derive_icon_state(d.wifiOk, d.wifiAttempting, s_wifiDown, now));
        set_icon_state(1, derive_icon_state(d.linOk,  d.linAttempting,  s_linDown,  now));
        // BLE: configured (bleState >= 1) is the precondition for "attempting".
        set_icon_state(2, derive_icon_state(d.bleState >= 2,
                                            d.bleAttempting && d.bleState >= 1,
                                            s_bleDown, now));
    }

    // Tint (water) and flame (heat) topbar indicators: bright only when LIN
    // is up AND the function is requested.  Without LIN the values reported
    // by the rest of the UI are stale/wishful — keep the icons dim to avoid
    // implying the appliance is actually doing something.  Matches the web
    // refreshIndicators() gating.
    lv_obj_set_style_text_opa(ui.icon_tint,
        (d.linOk && d.boilerMode != 0) ? LV_OPA_COVER : LV_OPA_30, 0);
    s_linOk = d.linOk;
    s_acConnected = d.acConnected;
    refresh_flame_icon();

    // Boiler thermometer.  The scale tops out at the selected boiler target
    // (40 °C in eco, 60 °C otherwise) so the fill rescales with the setpoint.
    if (!std::isnan(d.waterTemp)) {
        lv_label_set_text_fmt(ui.lbl_water_temp, "%.0f°C", d.waterTemp);
        float scaleMax = (d.boilerMode == 1) ? 40.0f : 60.0f;
        float frac = d.waterTemp / scaleMax;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        lv_obj_set_height(ui.water_fill, (int)(frac * WATER_FILL_H + 0.5f));
        lv_color_t wc = (d.waterTemp < 30.0f) ? C_WATER_COLD
                      : (d.waterTemp < 51.0f) ? C_WATER_WARM
                                              : C_WATER_HOT;
        lv_obj_set_style_bg_color(ui.water_fill, wc, 0);
    } else {
        lv_label_set_text(ui.lbl_water_temp, "--°C");
        lv_obj_set_height(ui.water_fill, 0);
    }

    // Solar data — when invalid show "--" everywhere
    if (d.solar.valid) {
        lv_label_set_text(ui.lbl_solar_status, translate_solar_status(d.solar.status));
        char g[20];
        snprintf(buf, sizeof(buf), "%.1f", d.solar.currentA);
        lv_label_set_text(ui.lbl_solar_current, buf);
        lv_label_set_text(ui.lbl_solar_current_u, "A");
        lv_label_set_text(ui.lbl_solar_power, group_int(g, sizeof(g), d.solar.powerW));
        lv_label_set_text(ui.lbl_solar_power_u, "W");
        snprintf(buf, sizeof(buf), "%.2f", d.solar.kWhToday);
        lv_label_set_text(ui.lbl_solar_yield, buf);
        snprintf(buf, sizeof(buf), "kWh %s", t(TK::TODAY));
        lv_label_set_text(ui.lbl_solar_yield_u, buf);
    } else {
        lv_label_set_text(ui.lbl_solar_status,  "--");
        lv_label_set_text(ui.lbl_solar_current, "--");
        lv_label_set_text(ui.lbl_solar_current_u, "");
        lv_label_set_text(ui.lbl_solar_power,   "--");
        lv_label_set_text(ui.lbl_solar_power_u,   "");
        lv_label_set_text(ui.lbl_solar_yield,   "--");
        lv_label_set_text(ui.lbl_solar_yield_u,   "");
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

        // CARGA / DESCARGA port — same V×A > 0 = charging convention as the web.
        int battW = (int)lroundf(d.batt.voltageV * d.batt.currentA);
        char gb[20];
        lv_label_set_text_fmt(ui.lbl_bpwr_w, "%s W", group_int(gb, sizeof(gb), battW));
        if (battW > 5) {
            lv_obj_set_style_bg_color(ui.box_bpwr, C_PORT_GREEN_BODY, 0);
            lv_obj_set_style_bg_color(ui.hdr_bpwr, C_PORT_GREEN_HDR, 0);
            lv_label_set_text(ui.hdr_bpwr_lbl, t(TK::BATT_CHARGE));
            ui.flow_bpwr_dir = 1;
            lv_obj_clear_flag(ui.flow_bpwr_str, LV_OBJ_FLAG_HIDDEN);
        } else if (battW < -5) {
            lv_obj_set_style_bg_color(ui.box_bpwr, C_PORT_RED_BODY, 0);
            lv_obj_set_style_bg_color(ui.hdr_bpwr, C_PORT_RED_HDR, 0);
            lv_label_set_text(ui.hdr_bpwr_lbl, t(TK::BATT_DISCHARGE));
            ui.flow_bpwr_dir = -1;
            lv_obj_clear_flag(ui.flow_bpwr_str, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_style_bg_color(ui.box_bpwr, C_PORT_GREY_BODY, 0);
            lv_obj_set_style_bg_color(ui.hdr_bpwr, C_PORT_GREY_HDR, 0);
            lv_label_set_text(ui.hdr_bpwr_lbl, t(TK::BATT_CHARGE));
            ui.flow_bpwr_dir = 0;
            lv_obj_add_flag(ui.flow_bpwr_str, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_label_set_text(ui.lbl_batt_soc, "--%");
        lv_bar_set_value(ui.bar_batt, 0, LV_ANIM_OFF);
        lv_label_set_text(ui.lbl_bpwr_w, "--");
        lv_obj_set_style_bg_color(ui.box_bpwr, C_PORT_GREY_BODY, 0);
        lv_obj_set_style_bg_color(ui.hdr_bpwr, C_PORT_GREY_HDR, 0);
        lv_label_set_text(ui.hdr_bpwr_lbl, t(TK::BATT_CHARGE));
        ui.flow_bpwr_dir = 0;
        lv_obj_add_flag(ui.flow_bpwr_str, LV_OBJ_FLAG_HIDDEN);
    }

    // Multiplus / VE.Bus inverter — render mains / load / battery flow.
    {
        char tb[24];
        char g[20];
        auto set_port_color = [](lv_obj_t* box, lv_obj_t* hdr,
                                 lv_color_t bc, lv_color_t hc) {
            lv_obj_set_style_bg_color(box, bc, 0);
            lv_obj_set_style_bg_color(hdr, hc, 0);
        };
        if (d.multi.valid) {
            lv_label_set_text(ui.lbl_inv_state, translate_multi_state(d.multi.deviceState));
            // MULTI_POWER_NA = inverter off / not reporting → show "--" instead
            // of a bogus number, and treat the port as idle below.
            bool mainsNa = (d.multi.acInW  == MULTI_POWER_NA);
            bool loadNa  = (d.multi.acOutW == MULTI_POWER_NA);
            if (mainsNa) snprintf(tb, sizeof(tb), "--");
            else         snprintf(tb, sizeof(tb), "%s W", group_int(g, sizeof(g), (int)d.multi.acInW));
            lv_label_set_text(ui.lbl_inv_mains_w, tb);
            if (loadNa) snprintf(tb, sizeof(tb), "--");
            else        snprintf(tb, sizeof(tb), "%s W", group_int(g, sizeof(g), (int)d.multi.acOutW));
            lv_label_set_text(ui.lbl_inv_load_w, tb);
            int battW = (int)lroundf((std::isnan(d.multi.battV) ? 0.0f : d.multi.battV)
                                     * d.multi.battA);
            snprintf(tb, sizeof(tb), "%s W", group_int(g, sizeof(g), battW));
            lv_label_set_text(ui.lbl_inv_batt_w, tb);

            // Drive the zebra-stripe animation.  Positive direction = flow
            // away from MPX (right) for mains/load; for batt, positive =
            // charging (MPX → BATT), negative = discharging (BATT → MPX).
            auto drive_flow = [](lv_obj_t* stripes, int8_t* dir_out, int signedW) {
                int8_t dir = (signedW > 5) ? 1 : (signedW < -5 ? -1 : 0);
                *dir_out = dir;
                if (dir != 0) lv_obj_clear_flag(stripes, LV_OBJ_FLAG_HIDDEN);
                else          lv_obj_add_flag(stripes,   LV_OBJ_FLAG_HIDDEN);
            };
            drive_flow(ui.flow_mains_str, &ui.flow_mains_dir, mainsNa ? 0 : d.multi.acInW);
            drive_flow(ui.flow_load_str,  &ui.flow_load_dir,  loadNa  ? 0 : d.multi.acOutW);
            drive_flow(ui.flow_batt_str,  &ui.flow_batt_dir,  battW);

            // RED: green when AC input connected (ac_in_state 0 or 1)
            bool acOn = d.multi.acInState < 2;
            set_port_color(ui.box_mains, ui.hdr_mains,
                           acOn ? C_PORT_GREEN_BODY : C_PORT_GREY_BODY,
                           acOn ? C_PORT_GREEN_HDR  : C_PORT_GREY_HDR);
            if (acOn) lv_label_set_text_fmt(ui.hdr_mains_lbl, "%s " FA_PLUG_BOLT, t(TK::INV_MAINS));
            else      lv_label_set_text(ui.hdr_mains_lbl, t(TK::INV_MAINS));

            // CARGA: red when delivering power, grey when idle
            bool loadOn = !loadNa && abs(d.multi.acOutW) > 5;
            set_port_color(ui.box_load, ui.hdr_load,
                           loadOn ? C_PORT_RED_BODY : C_PORT_GREY_BODY,
                           loadOn ? C_PORT_RED_HDR  : C_PORT_GREY_HDR);

            // BAT: green+arrow-right when charging, red+arrow-left when discharging
            if (battW > 5) {
                set_port_color(ui.box_batt, ui.hdr_batt,
                               C_PORT_GREEN_BODY, C_PORT_GREEN_HDR);
                lv_label_set_text(ui.hdr_batt_lbl, "BAT. " FA_ARROW_R);
            } else if (battW < -5) {
                set_port_color(ui.box_batt, ui.hdr_batt,
                               C_PORT_RED_BODY, C_PORT_RED_HDR);
                lv_label_set_text(ui.hdr_batt_lbl, FA_ARROW_L " BAT.");
            } else {
                set_port_color(ui.box_batt, ui.hdr_batt,
                               C_PORT_GREY_BODY, C_PORT_GREY_HDR);
                lv_label_set_text(ui.hdr_batt_lbl, "BAT.");
            }
        } else {
            lv_label_set_text(ui.lbl_inv_state, "--");
            lv_label_set_text(ui.lbl_inv_mains_w, "--");
            lv_label_set_text(ui.lbl_inv_load_w,  "--");
            lv_label_set_text(ui.lbl_inv_batt_w,  "--");
            ui.flow_mains_dir = ui.flow_load_dir = ui.flow_batt_dir = 0;
            lv_obj_add_flag(ui.flow_mains_str, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui.flow_load_str,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui.flow_batt_str,  LV_OBJ_FLAG_HIDDEN);
            set_port_color(ui.box_mains, ui.hdr_mains, C_PORT_GREY_BODY, C_PORT_GREY_HDR);
            set_port_color(ui.box_load,  ui.hdr_load,  C_PORT_GREY_BODY, C_PORT_GREY_HDR);
            set_port_color(ui.box_batt,  ui.hdr_batt,  C_PORT_GREY_BODY, C_PORT_GREY_HDR);
            lv_label_set_text(ui.hdr_mains_lbl, t(TK::INV_MAINS));
            lv_label_set_text(ui.hdr_batt_lbl, "BAT.");
        }
    }

    // Fresh-water tank (BTHome).  Same red→amber→green colour ramp as the
    // battery so "low" reads at a glance.
    if (d.tank.valid) {
        int pct = d.tank.pct;
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        lv_bar_set_value(ui.bar_tank, pct, LV_ANIM_ON);
        lv_color_t tc = (pct < 20) ? C_RED : (pct < 50) ? C_AMBER_BAR : C_WATER_COLD;
        lv_obj_set_style_bg_color(ui.bar_tank, tc, LV_PART_INDICATOR);
        lv_label_set_text_fmt(ui.lbl_tank_pct, "%d %%", pct);
    } else {
        lv_label_set_text(ui.lbl_tank_pct, "-- %");
        lv_bar_set_value(ui.bar_tank, 0, LV_ANIM_OFF);
    }

    if (d.wifiOk && d.ssid && d.ip) {
        lv_label_set_text_fmt(ui.lbl_conn, "%s / %s", d.ssid, d.ip);
        lv_obj_set_style_text_color(ui.lbl_conn, C_TEXT, 0);
    } else {
        lv_label_set_text(ui.lbl_conn, t(TK::STATUS_NO_WIFI));
        lv_obj_set_style_text_color(ui.lbl_conn, C_LABEL, 0);
    }

    refresh_controls();

    lvglUnlock();
}

void p4DisplaySetStatus(const char* msg, bool isError)
{
    if (!msg) return;
    if (!lvglLock(50)) return;
    if (ui.lbl_status) {
        lv_label_set_text(ui.lbl_status, msg);
        lv_obj_set_style_text_color(ui.lbl_status,
            isError ? C_RED : C_LABEL, 0);
    }
    lvglUnlock();
}

// ── OTA progress screen ───────────────────────────────────────────────────────

static lv_obj_t* s_ota_bar     = nullptr;
static lv_obj_t* s_ota_pct_lbl = nullptr;

void p4DisplayShowOtaScreen(const char* from_ver, const char* to_ver,
                            const char* title_txt, bool prefix_v)
{
    ESP_LOGI(TAG, "OTA screen: '%s' %s -> %s",
             title_txt ? title_txt : "(fw default)",
             from_ver ? from_ver : "?", to_ver ? to_ver : "?");
    if (!lvglLock(500)) return;

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
    lv_label_set_text(title, title_txt ? title_txt : t(TK::OTA_FW_UPDATING));
    lv_obj_set_style_text_font(title, s_font_title ? s_font_title : s_font_24, 0);
    lv_obj_set_style_text_color(title, C_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -75);

    // Version line: "v2.3.0 -> v2.12.7" (or bare hashes for the web sync)
    const char* v = prefix_v ? "v" : "";
    char ver_buf[48];
    snprintf(ver_buf, sizeof(ver_buf), "%s%s  ->  %s%s",
             v, from_ver ? from_ver : "?", v, to_ver ? to_ver : "?");
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
    lv_label_set_text(warn, t(TK::OTA_NO_POWER_OFF));
    lv_obj_set_style_text_font(warn, s_font_18, 0);
    lv_obj_set_style_text_color(warn, C_YELLOW, 0);
    lv_obj_align(warn, LV_ALIGN_CENTER, 0, 120);

    lv_screen_load(scr);
    lvglUnlock();
}

void p4DisplaySetOtaProgress(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    if (!s_ota_bar || !s_ota_pct_lbl) return;
    if (!lvglLock(100)) return;
    lv_bar_set_value(s_ota_bar, percent, LV_ANIM_ON);
    lv_label_set_text_fmt(s_ota_pct_lbl, "%d%%", percent);
    lvglUnlock();
}

void p4DisplayHideOtaScreen()
{
    // Restore the main screen after a *failed* self-OTA (a successful one
    // reboots, so this is only reached on download/validation errors).  The
    // running image is untouched, so returning to the normal UI is safe.
    ESP_LOGI(TAG, "OTA screen dismissed");
    if (!s_main_scr) return;
    if (!lvglLock(500)) return;
    lv_obj_t* cur = lv_screen_active();
    lv_screen_load(s_main_scr);
    if (cur && cur != s_main_scr) lv_obj_delete(cur);
    s_ota_bar     = nullptr;
    s_ota_pct_lbl = nullptr;
    lvglUnlock();
}

bool lvglLock(uint32_t timeout_ms) { return bsp_display_lock(timeout_ms); }
void lvglUnlock()                  { bsp_display_unlock(); }
