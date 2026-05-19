// Settings screens for the 800×480 P4 display.
// All screens are built and loaded with the LVGL lock already held (called
// from LVGL event callbacks). Screens are created fresh on each entry and
// deleted on exit; this keeps memory usage predictable.
//
// NVS layout (matches CLAUDE.md):
//   namespace "wifi"    : ssid, pass
//   namespace "mqtt"    : host, port, user, pass
//   namespace "solar"   : addr, key
//   namespace "batt"    : addr
//   namespace "display" : timeout_idx (u8), lang (u8: 0=ES 1=EN)

#include "p4settings.hpp"
#include "p4display.hpp"
#include "i18n.hpp"
#include "wifi_manager.hpp"
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "esp_log.h"
#include "nvs.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char* TAG = "cfg";

// ── Colours (same palette as p4display.cpp) ──────────────────────────────────
#define C_BG         lv_color_hex(0x1a1a2e)
#define C_TOPBAR     lv_color_hex(0x0f0f22)
#define C_SEP        lv_color_hex(0x444466)
#define C_LABEL      lv_color_hex(0x8888aa)
#define C_TEXT       lv_color_hex(0xffffff)
#define C_DIM        lv_color_hex(0xaaccff)
#define C_BTN        lv_color_hex(0x2a2a4a)
#define C_BTN_ACT    lv_color_hex(0x3a7bd5)
#define C_BTN_SEL    lv_color_hex(0x3a7bd5)
#define C_RED        lv_color_hex(0xff4444)
#define C_GREEN      lv_color_hex(0x44ff44)

// FontAwesome 6 codepoints — must match glyphs in font_icons.ttf.
// UTF-8 encodings of the Unicode codepoints.
#define FA_COG       "\xEF\x80\x93"   // U+F013
#define FA_WIFI      "\xEF\x87\xAB"   // U+F1EB
#define FA_ENVELOPE  "\xEF\x83\xA0"   // U+F0E0
#define FA_SUN       "\xEF\x86\x85"   // U+F185
#define FA_DISPLAY   "\xEF\x84\x88"   // U+F108
#define FA_GLOBE     "\xEF\x82\xAC"   // U+F0AC
#define FA_EYE       "\xEF\x81\xAE"   // U+F06E
#define FA_EYE_SLASH "\xEF\x81\xB0"   // U+F070
#define FA_SEARCH    "\xEF\x80\x82"   // U+F002

// Screen geometry.
static constexpr int SCR_W   = 800;
static constexpr int SCR_H   = 480;
static constexpr int TITLE_H = 55;
static constexpr int KB_H    = 220;  // LVGL keyboard height

// ── Navigation state ─────────────────────────────────────────────────────────
static lv_obj_t* s_mainScreen = nullptr;  // main screen to restore on exit

// ── NVS helpers ───────────────────────────────────────────────────────────────

static void nvs_read(const char* ns, const char* key, char* buf, size_t len) {
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_str(h, key, buf, &len);
    nvs_close(h);
}

static void nvs_write(const char* ns, const char* key, const char* val) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open %s failed", ns);
        return;
    }
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static uint8_t nvs_read_u8(const char* ns, const char* key, uint8_t def) {
    uint8_t val = def;
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return def;
    nvs_get_u8(h, key, &val);
    nvs_close(h);
    return val;
}

static void nvs_write_u8(const char* ns, const char* key, uint8_t val) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

// ── Navigate-back timer (delayed so "Config saved" message is briefly visible) ──

static void back_timer_cb(lv_timer_t* t) {
    lv_obj_t* prev = (lv_obj_t*)lv_timer_get_user_data(t);
    lv_obj_t* cur  = lv_screen_active();
    lv_screen_load(prev);
    lv_obj_delete(cur);
    lv_timer_delete(t);
}

static void schedule_back(lv_obj_t* prev, uint32_t delay_ms = 1200) {
    lv_timer_t* t = lv_timer_create(back_timer_cb, delay_ms, prev);
    lv_timer_set_repeat_count(t, 1);
}

// ── Common UI helpers ─────────────────────────────────────────────────────────

static void style_screen(lv_obj_t* scr) {
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_btn(lv_obj_t* btn, lv_color_t bg = C_BTN) {
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_text_color(btn, C_TEXT, 0);
    lv_obj_set_style_bg_color(btn, C_BTN_ACT, LV_STATE_PRESSED);
}

static lv_obj_t* make_label(lv_obj_t* par, const char* txt,
                              const lv_font_t* fnt, lv_color_t col) {
    lv_obj_t* l = lv_label_create(par);
    lv_label_set_text(l, txt);
    if (fnt) lv_obj_set_style_text_font(l, fnt, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

// Title bar shared by all settings screens.
// Returns the screen object (parent of the title bar).
// back_cb: called when the "Back" button is pressed.
static lv_obj_t* build_title_bar(const char* title, lv_event_cb_t back_cb) {
    lv_obj_t* scr = lv_obj_create(nullptr);
    style_screen(scr);

    lv_obj_t* bar = lv_obj_create(scr);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, SCR_W, TITLE_H);
    lv_obj_set_style_bg_color(bar, C_TOPBAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    const P4Fonts* f = p4GetFonts();

    lv_obj_t* lbl = make_label(bar, title, f->title, C_DIM);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t* btn = lv_button_create(bar);
    lv_obj_set_size(btn, 140, 40);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -12, 0);
    style_btn(btn);
    lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* bl = make_label(btn, t(TK::BACK), f->f20, C_TEXT);
    lv_obj_center(bl);

    // Horizontal separator below title bar.
    lv_obj_t* sep = lv_obj_create(scr);
    lv_obj_set_pos(sep, 0, TITLE_H);
    lv_obj_set_size(sep, SCR_W, 2);
    lv_obj_set_style_bg_color(sep, C_SEP, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    return scr;
}

// Custom special map: row 0 = numbers, row 1 = symbols for passwords,
// row 2 = Spanish accented chars, row 3 = nav/ok.
// Replaces the default "!@#$…" special mode with a Latin-friendly layout.
// Button count (40) matches the default spec map so the same ctrl array works.
static const char* const s_kb_map_spec[] = {
    "1","2","3","4","5","6","7","8","9","0", LV_SYMBOL_BACKSPACE, "\n",
    "abc","!","@","#","$","%","&","*","(",")","_","+", "\n",
    "\xC3\xA1","\xC3\xA9","\xC3\xAD","\xC3\xB3","\xC3\xBA","\xC3\xB1","\xC3\xBC","\xC2\xA1","\xC2\xBF","\"","'","=", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

// Cast helper: C++ won't implicitly convert int to lv_buttonmatrix_ctrl_t.
#define BMC(x) static_cast<lv_buttonmatrix_ctrl_t>(x)
#define KB_BTN(w)  BMC(LV_BUTTONMATRIX_CTRL_POPOVER | (w))
#define KB_CTRL(w) BMC(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED | (w))
#define KB_CHK(w)  BMC(LV_BUTTONMATRIX_CTRL_CHECKED | (w))

// Ctrl array mirroring default_kb_ctrl_spec_map (40 entries, no Arabic).
static const lv_buttonmatrix_ctrl_t s_kb_ctrl_spec[] = {
    // Row 0: 10 × w1 + backspace w2
    KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1),
    KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_CHK(2),
    // Row 1: "abc" ctrl-btn (w2) + 11 × w1
    KB_CTRL(2), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1),
    KB_BTN(1),  KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1),
    // Row 2: 12 × w1 (accented chars)
    KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1),
    KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1),
    // Row 3: [KB] ctrl-btn(w2), [LEFT] checked(w2), [space] w6, [RIGHT] checked(w2), [OK] ctrl-btn(w2)
    KB_CTRL(2), KB_CHK(2), BMC(6), KB_CHK(2), KB_CTRL(2),
};

// Keyboard with enlarged key labels. LV_PART_ITEMS targets each button cell.
// In Spanish mode the special layout is replaced with a Latin accented-char map.
static lv_obj_t* build_keyboard(lv_obj_t* scr) {
    const P4Fonts* f = p4GetFonts();
    lv_obj_t* kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, SCR_W, KB_H);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, f->f24, LV_PART_ITEMS);
    if (currentLanguage() == Language::ES) {
        lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, s_kb_map_spec, s_kb_ctrl_spec);
    }
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    return kb;
}

// Scrollable content panel below the title bar.
// When show_kb=false the panel fills the remaining height.
// When show_kb=true the panel is shorter, leaving room for the keyboard.
static lv_obj_t* build_content_panel(lv_obj_t* scr, bool show_kb = false) {
    int panel_h = show_kb ? (SCR_H - TITLE_H - 2 - KB_H) : (SCR_H - TITLE_H - 2);
    lv_obj_t* p = lv_obj_create(scr);
    lv_obj_set_pos(p, 0, TITLE_H + 2);
    lv_obj_set_size(p, SCR_W, panel_h);
    lv_obj_set_style_bg_color(p, C_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_left(p, 20, 0);
    lv_obj_set_style_pad_right(p, 20, 0);
    lv_obj_set_style_pad_top(p, 10, 0);
    lv_obj_set_style_pad_bottom(p, 10, 0);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    return p;
}

// ── Forward declarations ──────────────────────────────────────────────────────
static void show_menu(lv_obj_t* from);
static void show_wifi(lv_obj_t* from);
#ifndef NO_MQTT
static void show_mqtt(lv_obj_t* from);
#endif
static void show_ble(lv_obj_t* from);
static void show_display(lv_obj_t* from);
static void show_language(lv_obj_t* from);

// ── "from" helpers — store prev screen pointer in the button user_data ────────
// Each "Back" callback reads the pointer and calls show_xxx(prev).
// We use lv_screen_active() at build time and store it as event user_data.

struct NavData {
    lv_obj_t* prev;
    lv_event_cb_t target_fn;  // unused for simple back; kept for extensibility
};

// ═══════════════════════════════════════════════════════════════════════════════
// SETTINGS MENU
// ═══════════════════════════════════════════════════════════════════════════════

struct MenuBtn {
    const char* icon;
    TK          label_key;
    lv_event_cb_t cb;
};

static void menu_back_cb(lv_event_t*) {
    lv_obj_t* cur = lv_screen_active();
    lv_screen_load(s_mainScreen);
    lv_obj_delete(cur);
}

static void menu_wifi_cb(lv_event_t* e)     { show_wifi(lv_screen_active()); }
#ifndef NO_MQTT
static void menu_mqtt_cb(lv_event_t* e)     { show_mqtt(lv_screen_active()); }
#endif
static void menu_ble_cb(lv_event_t* e)      { show_ble(lv_screen_active()); }
static void menu_display_cb(lv_event_t* e)  { show_display(lv_screen_active()); }
static void menu_language_cb(lv_event_t* e) { show_language(lv_screen_active()); }

static void show_menu(lv_obj_t* /*from*/) {
    const P4Fonts* f = p4GetFonts();
    lv_obj_t* scr = build_title_bar(t(TK::SETTINGS), menu_back_cb);

    // Button grid layout.
    // NO_MQTT build: 2×2 grid (4 buttons, 370×185 each, 20px gap).
    // Full build:    3+2 grid (5 buttons, 240×170 each, 20px gap).
    //
    // Icon: icons36 (36px FA glyph) centred in upper half of button.
    // Label: f24 centred in lower portion.

#ifdef NO_MQTT
    static const MenuBtn ITEMS[] = {
        { FA_WIFI,    TK::WIFI_CFG,  menu_wifi_cb     },
        { FA_SUN,     TK::SOLAR_CFG, menu_ble_cb      },
        { FA_DISPLAY, TK::DISP_CFG,  menu_display_cb  },
        { FA_GLOBE,   TK::LANGUAGE,  menu_language_cb },
    };
    static constexpr int N      = 4;
    static constexpr int COLS   = 2;
    static constexpr int BTN_W  = 370;
    static constexpr int BTN_H  = 185;
    static constexpr int GAP    = 20;
    // Centre the 2-column grid horizontally and vertically in the content area.
    static constexpr int GRID_W = COLS * BTN_W + (COLS - 1) * GAP;
    static constexpr int ROWS   = (N + COLS - 1) / COLS;  // 2
    static constexpr int GRID_H = ROWS * BTN_H + (ROWS - 1) * GAP;
    static constexpr int OX     = (SCR_W - GRID_W) / 2;
    static constexpr int OY     = TITLE_H + 2 + (SCR_H - TITLE_H - 2 - GRID_H) / 2;

    for (int i = 0; i < N; i++) {
        int col = i % COLS;
        int row = i / COLS;
        int x   = OX + col * (BTN_W + GAP);
        int y   = OY + row * (BTN_H + GAP);

        lv_obj_t* btn = lv_button_create(scr);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, BTN_W, BTN_H);
        style_btn(btn);
        lv_obj_add_event_cb(btn, ITEMS[i].cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* ico = make_label(btn, ITEMS[i].icon, f->icons36, C_DIM);
        lv_obj_align(ico, LV_ALIGN_CENTER, 0, -26);
        lv_obj_t* lbl = make_label(btn, t(ITEMS[i].label_key), f->f24, C_TEXT);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, +30);
    }
#else
    static const MenuBtn ITEMS[] = {
        { FA_WIFI,     TK::WIFI_CFG,  menu_wifi_cb     },
        { FA_ENVELOPE, TK::MQTT_CFG,  menu_mqtt_cb     },
        { FA_SUN,      TK::SOLAR_CFG, menu_ble_cb      },
        { FA_DISPLAY,  TK::DISP_CFG,  menu_display_cb  },
        { FA_GLOBE,    TK::LANGUAGE,  menu_language_cb },
    };
    static constexpr int N      = 5;
    static constexpr int BTN_W  = 240;
    static constexpr int BTN_H  = 185;
    static constexpr int GAP    = 20;
    static constexpr int CONTENT_H = SCR_H - TITLE_H - 2;
    // Row 1: 3 buttons centred; Row 2: 2 buttons centred.
    static constexpr int ROW1_X = (SCR_W - 3 * BTN_W - 2 * GAP) / 2;
    static constexpr int ROW2_X = (SCR_W - 2 * BTN_W - 1 * GAP) / 2;
    static constexpr int ROW1_Y = TITLE_H + 2 + (CONTENT_H - 2 * BTN_H - GAP) / 2;

    for (int i = 0; i < N; i++) {
        int col     = i % 3;
        int row     = i / 3;
        int base_x  = (row == 0) ? ROW1_X : ROW2_X;
        int x       = base_x + col * (BTN_W + GAP);
        int y       = ROW1_Y + row * (BTN_H + GAP);

        lv_obj_t* btn = lv_button_create(scr);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, BTN_W, BTN_H);
        style_btn(btn);
        lv_obj_add_event_cb(btn, ITEMS[i].cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* ico = make_label(btn, ITEMS[i].icon, f->icons36, C_DIM);
        lv_obj_align(ico, LV_ALIGN_CENTER, 0, -26);
        lv_obj_t* lbl = make_label(btn, t(ITEMS[i].label_key), f->f24, C_TEXT);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, +30);
    }
#endif

    lv_screen_load(scr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SCAN OVERLAY  (shared by WiFi and BLE scan)
// ═══════════════════════════════════════════════════════════════════════════════

static void scan_cancel_cb(lv_event_t* e) {
    lv_obj_t* ov   = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_t* prev = (lv_obj_t*)lv_obj_get_user_data(ov);
    lv_screen_load(prev);
    lv_obj_delete(ov);
}

// Creates a full-screen scan overlay loaded as the active LVGL screen.
// prev_screen: the screen to restore when the overlay is closed (stored as user_data).
// *list_out receives the flex-column scrollable results list.
// *spinner_out (optional) receives the animated spinner placed in the title bar.
static lv_obj_t* make_scan_overlay(lv_obj_t* prev_screen, const char* title,
                                    lv_obj_t** list_out, lv_obj_t** spinner_out = nullptr)
{
    const P4Fonts* f = p4GetFonts();

    lv_obj_t* ov = lv_obj_create(nullptr);   // full LVGL screen, not a child widget
    lv_obj_set_style_bg_color(ov, C_BG, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(ov, prev_screen);   // remember where to return on close

    // Title bar
    lv_obj_t* bar = lv_obj_create(ov);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, SCR_W, TITLE_H);
    lv_obj_set_style_bg_color(bar, C_TOPBAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_t = lv_label_create(bar);
    lv_label_set_text(lbl_t, title);
    lv_obj_set_style_text_font(lbl_t, f->title, 0);
    lv_obj_set_style_text_color(lbl_t, C_TEXT, 0);
    lv_obj_align(lbl_t, LV_ALIGN_LEFT_MID, 20, 0);

    // Spinner between title and cancel button — animated while scan runs.
    lv_obj_t* sp = lv_spinner_create(bar);
    lv_obj_set_size(sp, 36, 36);
    // Cancel btn: RIGHT_MID x=-10, w=130.  Spinner goes 10px left of that.
    lv_obj_align(sp, LV_ALIGN_RIGHT_MID, -(10 + 130 + 10 + 18), 0);
    // Dark palette on the (very dark) title bar: track matches screen bg,
    // indicator matches button bg one shade lighter so the motion is visible
    // without being bright.
    lv_obj_set_style_arc_color(sp, C_BG,  LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, C_BTN, LV_PART_INDICATOR);
    // Thinner arc (default is ~8 px and looks too heavy at this size).
    lv_obj_set_style_arc_width(sp, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_INDICATOR);
    if (spinner_out) *spinner_out = sp;

    lv_obj_t* cancel_btn = lv_button_create(bar);
    lv_obj_set_size(cancel_btn, 130, 40);
    lv_obj_align(cancel_btn, LV_ALIGN_RIGHT_MID, -10, 0);
    style_btn(cancel_btn);
    lv_obj_add_event_cb(cancel_btn, scan_cancel_cb, LV_EVENT_CLICKED, ov);
    lv_obj_t* cl = make_label(cancel_btn, t(TK::CANCEL), f->f20, C_TEXT);
    lv_obj_center(cl);

    // Separator below title
    lv_obj_t* sep = lv_obj_create(ov);
    lv_obj_set_pos(sep, 0, TITLE_H);
    lv_obj_set_size(sep, SCR_W, 2);
    lv_obj_set_style_bg_color(sep, C_SEP, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    // Scrollable list with flex column layout
    lv_obj_t* list = lv_obj_create(ov);
    lv_obj_set_pos(list, 0, TITLE_H + 2);
    lv_obj_set_size(list, SCR_W, SCR_H - TITLE_H - 2);
    lv_obj_set_style_bg_color(list, C_BG, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 10, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    if (list_out) *list_out = list;
    lv_screen_load(ov);
    return ov;
}

// Appends one selectable row to a scan overlay list.
// value: persisted string stored as user_data on the button (must outlive the overlay).
// detail: right-aligned secondary text (RSSI, MAC, …) — may be null.
// The select_cb can retrieve value via lv_obj_get_user_data(target_obj).
// It can delete the overlay via lv_obj_get_parent(lv_obj_get_parent(target_obj)).
static void add_scan_row(lv_obj_t* list, const char* name, const char* detail,
                          const char* value, lv_event_cb_t select_cb)
{
    const P4Fonts* f = p4GetFonts();

    lv_obj_t* btn = lv_button_create(list);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 60);
    style_btn(btn);
    lv_obj_set_user_data(btn, (void*)value);
    lv_obj_add_event_cb(btn, select_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, f->f22, 0);
    lv_obj_set_style_text_color(lbl, C_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);

    if (detail) {
        lv_obj_t* lbl_d = lv_label_create(btn);
        lv_label_set_text(lbl_d, detail);
        lv_obj_set_style_text_font(lbl_d, f->f18, 0);
        lv_obj_set_style_text_color(lbl_d, C_LABEL, 0);
        lv_obj_align(lbl_d, LV_ALIGN_RIGHT_MID, -8, 0);
    }
}

// Forward declarations needed by async callbacks defined before their callers.
static void wifi_ap_select_cb(lv_event_t* e);

// ═══════════════════════════════════════════════════════════════════════════════
// ASYNC SCAN HELPERS
// Callbacks run on FreeRTOS tasks; they post to LVGL via lv_async_call.
// Static string storage avoids dangling pointers in add_scan_row (which stores
// value pointer raw — no copy).
// ═══════════════════════════════════════════════════════════════════════════════

#define MAX_SCAN_ROWS 20
static char s_wifi_ssids[MAX_SCAN_ROWS][33];
static char s_ble_macs [MAX_SCAN_ROWS][13];
static char s_ble_names[MAX_SCAN_ROWS][64];

struct WifiScanCtx {
    lv_obj_t* list;
    lv_obj_t* spinner;
};

struct WifiScanResult {
    lv_obj_t* list;
    lv_obj_t* spinner;
    WifiAP*   aps;
    int       count;
};

static void wifi_populate_cb(void* arg) {
    auto* r = (WifiScanResult*)arg;
    if (r->spinner && lv_obj_is_valid(r->spinner)) lv_obj_delete(r->spinner);
    if (lv_obj_is_valid(r->list)) {
        const P4Fonts* f = p4GetFonts();
        lv_obj_clean(r->list);
        if (r->count == 0) {
            lv_obj_t* lbl = lv_label_create(r->list);
            lv_label_set_text(lbl, t(TK::WIFI_NO_NETS));
            lv_obj_set_style_text_font(lbl, f->f20, 0);
            lv_obj_set_style_text_color(lbl, C_LABEL, 0);
        } else {
            int n = r->count < MAX_SCAN_ROWS ? r->count : MAX_SCAN_ROWS;
            for (int i = 0; i < n; i++) {
                snprintf(s_wifi_ssids[i], sizeof(s_wifi_ssids[0]), "%s", r->aps[i].ssid);
                char rssi[12];
                snprintf(rssi, sizeof(rssi), "%d dBm", (int)r->aps[i].rssi);
                add_scan_row(r->list, s_wifi_ssids[i], rssi, s_wifi_ssids[i], wifi_ap_select_cb);
            }
        }
    }
    free(r->aps);
    free(r);
}

static void wifi_scan_done(const WifiAP* aps, int count, void* user) {
    auto* ctx = (WifiScanCtx*)user;
    auto* r   = (WifiScanResult*)malloc(sizeof(WifiScanResult));
    r->list    = ctx->list;
    r->spinner = ctx->spinner;
    r->count   = count;
    r->aps     = count > 0 ? (WifiAP*)malloc(count * sizeof(WifiAP)) : nullptr;
    if (r->aps) memcpy(r->aps, aps, count * sizeof(WifiAP));
    free(ctx);
    lv_async_call(wifi_populate_cb, r);
}

struct BleScanResult {
    lv_obj_t*     list;
    lv_obj_t*     spinner;
    BleDevice*    devs;
    int           count;
    lv_event_cb_t select_cb;
};

static void ble_populate_cb(void* arg) {
    auto* r = (BleScanResult*)arg;
    if (r->spinner && lv_obj_is_valid(r->spinner)) lv_obj_delete(r->spinner);
    if (lv_obj_is_valid(r->list)) {
        const P4Fonts* f = p4GetFonts();
        lv_obj_clean(r->list);
        if (r->count == 0) {
            lv_obj_t* lbl = lv_label_create(r->list);
            lv_label_set_text(lbl, t(TK::NO_BLE_DEVS));
            lv_obj_set_style_text_font(lbl, f->f20, 0);
            lv_obj_set_style_text_color(lbl, C_LABEL, 0);
        } else {
            int n = r->count < MAX_SCAN_ROWS ? r->count : MAX_SCAN_ROWS;
            for (int i = 0; i < n; i++) {
                snprintf(s_ble_macs[i],  sizeof(s_ble_macs[0]),  "%s", r->devs[i].mac);
                snprintf(s_ble_names[i], sizeof(s_ble_names[0]), "%s", r->devs[i].name);
                add_scan_row(r->list, s_ble_names[i], s_ble_macs[i],
                             s_ble_macs[i], r->select_cb);
            }
        }
    }
    free(r->devs);
    free(r);
}

struct BleScanCtx {
    lv_obj_t*     list;
    lv_obj_t*     spinner;
    lv_event_cb_t select_cb;
};

static void ble_scan_done(const BleDevice* devs, int count, void* user) {
    auto* ctx = (BleScanCtx*)user;
    auto* r   = (BleScanResult*)malloc(sizeof(BleScanResult));
    r->list      = ctx->list;
    r->spinner   = ctx->spinner;
    r->count     = count;
    r->select_cb = ctx->select_cb;
    r->devs      = count > 0 ? (BleDevice*)malloc(count * sizeof(BleDevice)) : nullptr;
    if (r->devs) memcpy(r->devs, devs, count * sizeof(BleDevice));
    free(ctx);
    lv_async_call(ble_populate_cb, r);
}

// ═══════════════════════════════════════════════════════════════════════════════
// WIFI SCREEN
// ═══════════════════════════════════════════════════════════════════════════════
// Manual SSID entry + WiFi scan overlay.  Save to NVS "wifi" then reboot.

struct WifiCtx {
    lv_obj_t* scr;
    lv_obj_t* panel;
    lv_obj_t* ta_ssid;
    lv_obj_t* ta_pass;
    lv_obj_t* lbl_pass_eye;
    lv_obj_t* kb;
    lv_obj_t* lbl_status;
    lv_obj_t* prev;
    bool pass_visible;
};
static WifiCtx wf;

static void wifi_back_cb(lv_event_t*) {
    lv_obj_t* cur = lv_screen_active();
    lv_obj_t* prev = wf.prev;
    lv_screen_load(prev);
    lv_obj_delete(cur);
}

static void wifi_eye_cb(lv_event_t*) {
    wf.pass_visible = !wf.pass_visible;
    lv_textarea_set_password_mode(wf.ta_pass, !wf.pass_visible);
    lv_label_set_text(wf.lbl_pass_eye,
        wf.pass_visible ? FA_EYE_SLASH : FA_EYE);
}

static void wifi_kb_show(lv_obj_t* ta) {
    lv_keyboard_set_textarea(wf.kb, ta);
    lv_obj_remove_flag(wf.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(wf.panel, SCR_H - TITLE_H - 2 - KB_H);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

static void wifi_kb_hide_cb(lv_event_t*) {
    lv_obj_add_flag(wf.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(wf.panel, SCR_H - TITLE_H - 2);
}

static void wifi_focus_ssid_cb(lv_event_t*) { wifi_kb_show(wf.ta_ssid); }
static void wifi_focus_pass_cb(lv_event_t*) { wifi_kb_show(wf.ta_pass); }

static void wifi_ap_select_cb(lv_event_t* e) {
    lv_obj_t* btn    = lv_event_get_target_obj(e);
    const char* ssid = (const char*)lv_obj_get_user_data(btn);
    lv_textarea_set_text(wf.ta_ssid, ssid ? ssid : "");
    lv_obj_t* ov   = lv_obj_get_parent(lv_obj_get_parent(btn));  // btn → list → scan scr
    lv_obj_t* prev = (lv_obj_t*)lv_obj_get_user_data(ov);
    lv_screen_load(prev);
    lv_obj_delete(ov);
}

static void wifi_scan_btn_cb(lv_event_t*) {
    lv_obj_t* list = nullptr;
    lv_obj_t* sp   = nullptr;
    make_scan_overlay(wf.scr, t(TK::WIFI_SCANNING), &list, &sp);
    auto* ctx    = (WifiScanCtx*)malloc(sizeof(WifiScanCtx));
    ctx->list    = list;
    ctx->spinner = sp;
    wifi_manager_scan_async(wifi_scan_done, ctx);
}

static void wifi_save_cb(lv_event_t*) {
    const char* ssid = lv_textarea_get_text(wf.ta_ssid);
    const char* pass = lv_textarea_get_text(wf.ta_pass);
    if (strlen(ssid) == 0) {
        lv_label_set_text(wf.lbl_status, "Introduce el nombre de la red (SSID)");
        return;
    }
    wifi_manager_connect(ssid, pass);   // saves to NVS and (re)connects
    lv_label_set_text(wf.lbl_status, t(TK::CFG_SAVED));
    schedule_back(wf.prev);
}

static void show_wifi(lv_obj_t* from) {
    const P4Fonts* f = p4GetFonts();
    wf.prev         = from;
    wf.pass_visible = false;

    wf.scr   = build_title_bar(t(TK::WIFI_TITLE), wifi_back_cb);
    wf.panel = build_content_panel(wf.scr);

    // SSID
    lv_obj_t* lbl_ssid = make_label(wf.panel, t(TK::WIFI_NETWORK), f->f22, C_LABEL);
    lv_obj_set_pos(lbl_ssid, 0, 10);

    wf.ta_ssid = lv_textarea_create(wf.panel);
    lv_obj_set_pos(wf.ta_ssid, 0, 40);
    lv_obj_set_size(wf.ta_ssid, 698, 54);
    lv_obj_set_style_text_font(wf.ta_ssid, f->f22, 0);
    lv_textarea_set_one_line(wf.ta_ssid, true);
    lv_textarea_set_placeholder_text(wf.ta_ssid, "MyNetwork");
    // Pre-fill saved value.
    char buf[64] = {};
    nvs_read("wifi", "ssid", buf, sizeof(buf));
    if (buf[0]) lv_textarea_set_text(wf.ta_ssid, buf);
    lv_obj_add_event_cb(wf.ta_ssid, wifi_focus_ssid_cb, LV_EVENT_FOCUSED, nullptr);

    // WiFi scan button
    {
        lv_obj_t* scan_btn = lv_button_create(wf.panel);
        lv_obj_set_pos(scan_btn, 704, 40);
        lv_obj_set_size(scan_btn, 56, 54);
        style_btn(scan_btn);
        lv_obj_add_event_cb(scan_btn, wifi_scan_btn_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* sl = make_label(scan_btn, FA_SEARCH, f->icons22, C_TEXT);
        lv_obj_center(sl);
    }

    // Password + eye toggle
    lv_obj_t* lbl_pass = make_label(wf.panel, t(TK::PASSWORD), f->f22, C_LABEL);
    lv_obj_set_pos(lbl_pass, 0, 112);

    wf.ta_pass = lv_textarea_create(wf.panel);
    lv_obj_set_pos(wf.ta_pass, 0, 142);
    lv_obj_set_size(wf.ta_pass, SCR_W - 40 - 62, 54);
    lv_obj_set_style_text_font(wf.ta_pass, f->f22, 0);
    lv_textarea_set_one_line(wf.ta_pass, true);
    lv_textarea_set_password_mode(wf.ta_pass, true);
    lv_textarea_set_placeholder_text(wf.ta_pass, t(TK::PASSWORD_PH));
    lv_obj_add_event_cb(wf.ta_pass, wifi_focus_pass_cb, LV_EVENT_FOCUSED, nullptr);

    lv_obj_t* eye_btn = lv_button_create(wf.panel);
    lv_obj_set_pos(eye_btn, SCR_W - 40 - 56, 142);
    lv_obj_set_size(eye_btn, 56, 54);
    style_btn(eye_btn);
    lv_obj_add_event_cb(eye_btn, wifi_eye_cb, LV_EVENT_CLICKED, nullptr);
    wf.lbl_pass_eye = make_label(eye_btn, FA_EYE, f->icons22, C_TEXT);
    lv_obj_center(wf.lbl_pass_eye);

    // Save button
    lv_obj_t* save_btn = lv_button_create(wf.panel);
    lv_obj_set_pos(save_btn, (SCR_W - 40 - 280) / 2, 220);
    lv_obj_set_size(save_btn, 280, 54);
    style_btn(save_btn, C_BTN_ACT);
    lv_obj_add_event_cb(save_btn, wifi_save_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* slbl = make_label(save_btn, t(TK::SAVE), f->f22, C_TEXT);
    lv_obj_center(slbl);

    // Status label
    wf.lbl_status = make_label(wf.panel, "", f->f20, C_LABEL);
    lv_obj_set_pos(wf.lbl_status, 0, 292);
    lv_obj_set_width(wf.lbl_status, SCR_W - 40);
    lv_label_set_long_mode(wf.lbl_status, LV_LABEL_LONG_WRAP);

    // Keyboard (child of screen, not panel — stays fixed at bottom)
    wf.kb = build_keyboard(wf.scr);
    lv_obj_add_event_cb(wf.kb, wifi_kb_hide_cb, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(wf.kb, wifi_kb_hide_cb, LV_EVENT_CANCEL, nullptr);

    lv_screen_load(wf.scr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// MQTT SCREEN
// ═══════════════════════════════════════════════════════════════════════════════
#ifndef NO_MQTT

struct MqttCtx {
    lv_obj_t* scr;
    lv_obj_t* ta_host;
    lv_obj_t* ta_port;
    lv_obj_t* ta_user;
    lv_obj_t* ta_pass;
    lv_obj_t* kb;
    lv_obj_t* lbl_status;
    lv_obj_t* prev;
};
static MqttCtx mq;

static void mqtt_back_cb(lv_event_t*) {
    lv_obj_t* cur = lv_screen_active();
    lv_screen_load(mq.prev);
    lv_obj_delete(cur);
}

static void mqtt_kb_hide_cb(lv_event_t*) {
    lv_obj_add_flag(mq.kb, LV_OBJ_FLAG_HIDDEN);
}

static void mqtt_alpha_focus_cb(lv_event_t* e) {
    lv_keyboard_set_textarea(mq.kb, lv_event_get_target_obj(e));
    lv_keyboard_set_mode(mq.kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_flag(mq.kb, LV_OBJ_FLAG_HIDDEN);
}

static void mqtt_num_focus_cb(lv_event_t*) {
    lv_keyboard_set_textarea(mq.kb, mq.ta_port);
    lv_keyboard_set_mode(mq.kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_remove_flag(mq.kb, LV_OBJ_FLAG_HIDDEN);
}

static void mqtt_skip_cb(lv_event_t*) {
    // Sentinel value so loadMqttConfig() returns true and skips this screen on
    // next boot.
    nvs_write("mqtt", "host", "_skip_");
    nvs_write("mqtt", "port", "1883");
    nvs_write("mqtt", "user", "");
    nvs_write("mqtt", "pass", "");
    lv_label_set_text(mq.lbl_status, t(TK::CFG_SAVED));
    schedule_back(mq.prev);
}

static void mqtt_save_cb(lv_event_t*) {
    const char* host = lv_textarea_get_text(mq.ta_host);
    const char* port = lv_textarea_get_text(mq.ta_port);
    if (strlen(host) == 0) {
        lv_label_set_text(mq.lbl_status, t(TK::MQTT_ENTER_BROKER));
        return;
    }
    if (strlen(port) == 0) {
        lv_label_set_text(mq.lbl_status, t(TK::MQTT_ENTER_PORT));
        return;
    }
    nvs_write("mqtt", "host", host);
    nvs_write("mqtt", "port", port);
    nvs_write("mqtt", "user", lv_textarea_get_text(mq.ta_user));
    nvs_write("mqtt", "pass", lv_textarea_get_text(mq.ta_pass));
    lv_label_set_text(mq.lbl_status, t(TK::CFG_SAVED));
    schedule_back(mq.prev);
}

static void show_mqtt(lv_obj_t* from) {
    const P4Fonts* f = p4GetFonts();
    mq.prev = from;

    mq.scr = build_title_bar(t(TK::MQTT_TITLE), mqtt_back_cb);

    // With 800×480 and KB at bottom (220px), the form area is 800×(480-55-2-220)=203px.
    // We don't use a scrollable panel — layout is flat on the screen.
    // Y coordinates are from screen top (below title bar).

    char buf[128] = {};
    const int PAD = 20;
    const int FULL_W = SCR_W - 2 * PAD;  // 760
    const int HOST_W = FULL_W - 10 - 140; // 610
    const int PORT_W = 140;

    // Broker row
    lv_obj_t* lbl_broker = make_label(mq.scr, t(TK::MQTT_BROKER), f->f22, C_LABEL);
    lv_obj_set_pos(lbl_broker, PAD, 68);
    lv_obj_t* lbl_port = make_label(mq.scr, t(TK::MQTT_PORT), f->f22, C_LABEL);
    lv_obj_set_pos(lbl_port, PAD + HOST_W + 10, 68);

    mq.ta_host = lv_textarea_create(mq.scr);
    lv_obj_set_pos(mq.ta_host, PAD, 94);
    lv_obj_set_size(mq.ta_host, HOST_W, 52);
    lv_obj_set_style_text_font(mq.ta_host, f->f22, 0);
    lv_textarea_set_one_line(mq.ta_host, true);
    lv_textarea_set_placeholder_text(mq.ta_host, "192.168.1.100");
    nvs_read("mqtt", "host", buf, sizeof(buf));
    if (buf[0] && strcmp(buf, "_skip_") != 0) lv_textarea_set_text(mq.ta_host, buf);
    lv_obj_add_event_cb(mq.ta_host, mqtt_alpha_focus_cb, LV_EVENT_FOCUSED, nullptr);

    mq.ta_port = lv_textarea_create(mq.scr);
    lv_obj_set_pos(mq.ta_port, PAD + HOST_W + 10, 94);
    lv_obj_set_size(mq.ta_port, PORT_W, 52);
    lv_obj_set_style_text_font(mq.ta_port, f->f22, 0);
    lv_textarea_set_one_line(mq.ta_port, true);
    lv_textarea_set_placeholder_text(mq.ta_port, "1883");
    nvs_read("mqtt", "port", buf, sizeof(buf));
    lv_textarea_set_text(mq.ta_port, buf[0] ? buf : "1883");
    lv_obj_add_event_cb(mq.ta_port, mqtt_num_focus_cb, LV_EVENT_FOCUSED, nullptr);

    // User row
    lv_obj_t* lbl_user = make_label(mq.scr, t(TK::USER_OPT), f->f22, C_LABEL);
    lv_obj_set_pos(lbl_user, PAD, 160);

    mq.ta_user = lv_textarea_create(mq.scr);
    lv_obj_set_pos(mq.ta_user, PAD, 186);
    lv_obj_set_size(mq.ta_user, FULL_W, 52);
    lv_obj_set_style_text_font(mq.ta_user, f->f22, 0);
    lv_textarea_set_one_line(mq.ta_user, true);
    lv_textarea_set_placeholder_text(mq.ta_user, t(TK::USER_PH));
    nvs_read("mqtt", "user", buf, sizeof(buf));
    if (buf[0]) lv_textarea_set_text(mq.ta_user, buf);
    lv_obj_add_event_cb(mq.ta_user, mqtt_alpha_focus_cb, LV_EVENT_FOCUSED, nullptr);

    // Password row
    lv_obj_t* lbl_ppass = make_label(mq.scr, t(TK::PASSWORD), f->f22, C_LABEL);
    lv_obj_set_pos(lbl_ppass, PAD, 252);

    mq.ta_pass = lv_textarea_create(mq.scr);
    lv_obj_set_pos(mq.ta_pass, PAD, 278);
    lv_obj_set_size(mq.ta_pass, FULL_W, 52);
    lv_obj_set_style_text_font(mq.ta_pass, f->f22, 0);
    lv_textarea_set_one_line(mq.ta_pass, true);
    lv_textarea_set_password_mode(mq.ta_pass, true);
    lv_textarea_set_placeholder_text(mq.ta_pass, t(TK::PASSWORD_OPT_PH));
    lv_obj_add_event_cb(mq.ta_pass, mqtt_alpha_focus_cb, LV_EVENT_FOCUSED, nullptr);

    // Skip + Save buttons
    lv_obj_t* skip_btn = lv_button_create(mq.scr);
    lv_obj_set_pos(skip_btn, PAD, 348);
    lv_obj_set_size(skip_btn, (FULL_W - 10) / 2, 52);
    style_btn(skip_btn);
    lv_obj_add_event_cb(skip_btn, mqtt_skip_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* sl = make_label(skip_btn, t(TK::SKIP), f->f22, C_TEXT);
    lv_obj_center(sl);

    lv_obj_t* save_btn = lv_button_create(mq.scr);
    lv_obj_set_pos(save_btn, PAD + (FULL_W - 10) / 2 + 10, 348);
    lv_obj_set_size(save_btn, (FULL_W - 10) / 2, 52);
    style_btn(save_btn, C_BTN_ACT);
    lv_obj_add_event_cb(save_btn, mqtt_save_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* svl = make_label(save_btn, t(TK::SAVE), f->f22, C_TEXT);
    lv_obj_center(svl);

    // Status
    mq.lbl_status = make_label(mq.scr, t(TK::MQTT_INSTR), f->f20, C_LABEL);
    lv_obj_set_pos(mq.lbl_status, PAD, 412);
    lv_obj_set_width(mq.lbl_status, FULL_W);
    lv_label_set_long_mode(mq.lbl_status, LV_LABEL_LONG_WRAP);

    // Keyboard
    mq.kb = build_keyboard(mq.scr);
    lv_obj_add_event_cb(mq.kb, mqtt_kb_hide_cb, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(mq.kb, mqtt_kb_hide_cb, LV_EVENT_CANCEL, nullptr);

    lv_screen_load(mq.scr);
}

#endif // !NO_MQTT

// ═══════════════════════════════════════════════════════════════════════════════
// SOLAR / BLE SCREEN
// ═══════════════════════════════════════════════════════════════════════════════

struct BleCtx {
    lv_obj_t* scr;
    lv_obj_t* panel;
    lv_obj_t* ta_solar_mac;
    lv_obj_t* ta_solar_key;
    lv_obj_t* ta_batt_mac;
    lv_obj_t* kb;
    lv_obj_t* lbl_status;
    lv_obj_t* prev;
};
static BleCtx bl_ctx;

static void ble_back_cb(lv_event_t*) {
    lv_obj_t* cur = lv_screen_active();
    lv_screen_load(bl_ctx.prev);
    lv_obj_delete(cur);
}

static void ble_kb_hide_cb(lv_event_t*) {
    lv_obj_add_flag(bl_ctx.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(bl_ctx.panel, SCR_H - TITLE_H - 2 - 80);
}

static void ble_alpha_focus_cb(lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(bl_ctx.kb, ta);
    lv_keyboard_set_mode(bl_ctx.kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_remove_flag(bl_ctx.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(bl_ctx.panel, SCR_H - TITLE_H - 2 - KB_H);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

static void ble_victron_select_cb(lv_event_t* e) {
    lv_obj_t* btn   = lv_event_get_target_obj(e);
    const char* mac = (const char*)lv_obj_get_user_data(btn);
    lv_textarea_set_text(bl_ctx.ta_solar_mac, mac ? mac : "");
    lv_obj_t* ov   = lv_obj_get_parent(lv_obj_get_parent(btn));
    lv_obj_t* prev = (lv_obj_t*)lv_obj_get_user_data(ov);
    lv_screen_load(prev);
    lv_obj_delete(ov);
}

static void ble_batt_select_cb(lv_event_t* e) {
    lv_obj_t* btn   = lv_event_get_target_obj(e);
    const char* mac = (const char*)lv_obj_get_user_data(btn);
    lv_textarea_set_text(bl_ctx.ta_batt_mac, mac ? mac : "");
    lv_obj_t* ov   = lv_obj_get_parent(lv_obj_get_parent(btn));
    lv_obj_t* prev = (lv_obj_t*)lv_obj_get_user_data(ov);
    lv_screen_load(prev);
    lv_obj_delete(ov);
}

static void ble_scan_victron_cb(lv_event_t*) {
    lv_obj_t* list = nullptr;
    lv_obj_t* sp   = nullptr;
    make_scan_overlay(bl_ctx.scr, t(TK::SCAN_VICTRON), &list, &sp);
    auto* ctx      = (BleScanCtx*)malloc(sizeof(BleScanCtx));
    ctx->list      = list;
    ctx->spinner   = sp;
    ctx->select_cb = ble_victron_select_cb;
    // Show all BLE devices (no manufacturer filter); user picks the right one.
    bleDiscoveryScan(false, ble_scan_done, ctx);
}

static void ble_scan_batt_cb(lv_event_t*) {
    lv_obj_t* list = nullptr;
    lv_obj_t* sp   = nullptr;
    make_scan_overlay(bl_ctx.scr, t(TK::SCAN_BATT), &list, &sp);
    auto* ctx      = (BleScanCtx*)malloc(sizeof(BleScanCtx));
    ctx->list      = list;
    ctx->spinner   = sp;
    ctx->select_cb = ble_batt_select_cb;
    bleDiscoveryScan(false, ble_scan_done, ctx);
}

static void ble_save_cb(lv_event_t*) {
    nvs_write("solar", "addr", lv_textarea_get_text(bl_ctx.ta_solar_mac));
    nvs_write("solar", "key",  lv_textarea_get_text(bl_ctx.ta_solar_key));
    nvs_write("batt",  "addr", lv_textarea_get_text(bl_ctx.ta_batt_mac));
    victronBleReloadConfig();
    ultimatronBleReloadConfig();
    lv_label_set_text(bl_ctx.lbl_status, t(TK::CFG_SAVED));
    schedule_back(bl_ctx.prev);
}

static void show_ble(lv_obj_t* from) {
    const P4Fonts* f = p4GetFonts();
    bl_ctx.prev = from;

    bl_ctx.scr   = build_title_bar(t(TK::BLE_TITLE), ble_back_cb);
    // Scrollable panel — shortened so fixed save button at bottom is visible.
    bl_ctx.panel = build_content_panel(bl_ctx.scr);
    lv_obj_set_height(bl_ctx.panel, SCR_H - TITLE_H - 2 - 80);  // 343 px

    // Two-column layout inside panel.
    // Panel has pad_left=20, pad_top=10.
    // Child x=0 → screen x=20 ; child y=0 → screen y=TITLE_H+12.
    // COL2_X was 440 screen → child_x = 420.
    static constexpr int COL1_W  = 380;
    static constexpr int COL2_X  = 420;  // panel-relative
    static constexpr int COL2_W  = 340;  // 760 - 420

    char buf[128] = {};

    // Vertical separator — child of scr so it starts right at topbar edge and
    // is not affected by panel scrolling.
    {
        lv_obj_t* vsep = lv_obj_create(bl_ctx.scr);
        lv_obj_set_pos(vsep, 420, TITLE_H + 2);
        lv_obj_set_size(vsep, 2, SCR_H - TITLE_H - 2 - 80);
        lv_obj_set_style_bg_color(vsep, C_SEP, 0);
        lv_obj_set_style_bg_opa(vsep, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(vsep, 0, 0);
        lv_obj_set_style_radius(vsep, 0, 0);
        lv_obj_clear_flag(vsep, LV_OBJ_FLAG_SCROLLABLE);
    }

    // ── Victron column ──
    lv_obj_t* lbl_vic = make_label(bl_ctx.panel, "Victron SmartSolar", f->title, C_DIM);
    lv_obj_set_pos(lbl_vic, 0, 2);

    lv_obj_t* lbl_mac = make_label(bl_ctx.panel, t(TK::SOLAR_ADDR), f->f22, C_LABEL);
    lv_obj_set_pos(lbl_mac, 0, 44);

    bl_ctx.ta_solar_mac = lv_textarea_create(bl_ctx.panel);
    lv_obj_set_pos(bl_ctx.ta_solar_mac, 0, 72);
    lv_obj_set_size(bl_ctx.ta_solar_mac, 326, 52);
    lv_obj_set_style_text_font(bl_ctx.ta_solar_mac, f->f22, 0);
    lv_textarea_set_one_line(bl_ctx.ta_solar_mac, true);
    nvs_read("solar", "addr", buf, sizeof(buf));
    if (buf[0]) lv_textarea_set_text(bl_ctx.ta_solar_mac, buf);
    lv_obj_add_event_cb(bl_ctx.ta_solar_mac, ble_alpha_focus_cb, LV_EVENT_FOCUSED, nullptr);

    {
        lv_obj_t* sb = lv_button_create(bl_ctx.panel);
        lv_obj_set_pos(sb, 334, 72);
        lv_obj_set_size(sb, 46, 52);
        style_btn(sb);
        lv_obj_add_event_cb(sb, ble_scan_victron_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* sl = make_label(sb, FA_SEARCH, f->icons22, C_TEXT);
        lv_obj_center(sl);
    }

    lv_obj_t* lbl_key = make_label(bl_ctx.panel, t(TK::SOLAR_KEY), f->f22, C_LABEL);
    lv_obj_set_pos(lbl_key, 0, 136);

    bl_ctx.ta_solar_key = lv_textarea_create(bl_ctx.panel);
    lv_obj_set_pos(bl_ctx.ta_solar_key, 0, 164);
    lv_obj_set_size(bl_ctx.ta_solar_key, COL1_W, 52);
    lv_obj_set_style_text_font(bl_ctx.ta_solar_key, f->f22, 0);
    lv_textarea_set_one_line(bl_ctx.ta_solar_key, true);
    nvs_read("solar", "key", buf, sizeof(buf));
    if (buf[0]) lv_textarea_set_text(bl_ctx.ta_solar_key, buf);
    lv_obj_add_event_cb(bl_ctx.ta_solar_key, ble_alpha_focus_cb, LV_EVENT_FOCUSED, nullptr);

    // ── Battery column ──
    lv_obj_t* lbl_batt_title = make_label(bl_ctx.panel, t(TK::BATT_SECTION), f->title, C_DIM);
    lv_obj_set_pos(lbl_batt_title, COL2_X, 2);

    lv_obj_t* lbl_bmac = make_label(bl_ctx.panel, t(TK::MAC_BLE_LABEL), f->f22, C_LABEL);
    lv_obj_set_pos(lbl_bmac, COL2_X, 44);

    bl_ctx.ta_batt_mac = lv_textarea_create(bl_ctx.panel);
    lv_obj_set_pos(bl_ctx.ta_batt_mac, COL2_X, 72);
    lv_obj_set_size(bl_ctx.ta_batt_mac, 286, 52);
    lv_obj_set_style_text_font(bl_ctx.ta_batt_mac, f->f22, 0);
    lv_textarea_set_one_line(bl_ctx.ta_batt_mac, true);
    nvs_read("batt", "addr", buf, sizeof(buf));
    if (buf[0]) lv_textarea_set_text(bl_ctx.ta_batt_mac, buf);
    lv_obj_add_event_cb(bl_ctx.ta_batt_mac, ble_alpha_focus_cb, LV_EVENT_FOCUSED, nullptr);

    {
        lv_obj_t* sb = lv_button_create(bl_ctx.panel);
        lv_obj_set_pos(sb, COL2_X + 294, 72);
        lv_obj_set_size(sb, 46, 52);
        style_btn(sb);
        lv_obj_add_event_cb(sb, ble_scan_batt_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* sl = make_label(sb, FA_SEARCH, f->icons22, C_TEXT);
        lv_obj_center(sl);
    }

    lv_obj_t* lbl_binfo = make_label(bl_ctx.panel, t(TK::BATT_INFO), f->f20, C_LABEL);
    lv_obj_set_pos(lbl_binfo, COL2_X, 134);
    lv_obj_set_width(lbl_binfo, COL2_W);
    lv_label_set_long_mode(lbl_binfo, LV_LABEL_LONG_WRAP);

    // ── Status + Save button — fixed children of scr (always visible at bottom) ──
    bl_ctx.lbl_status = make_label(bl_ctx.scr, "", f->f20, C_LABEL);
    lv_obj_set_pos(bl_ctx.lbl_status, 20, SCR_H - 52 - 20 - 28);
    lv_obj_set_width(bl_ctx.lbl_status, SCR_W - 40);

    lv_obj_t* save_btn = lv_button_create(bl_ctx.scr);
    lv_obj_set_pos(save_btn, (SCR_W - 280) / 2, SCR_H - 52 - 20);
    lv_obj_set_size(save_btn, 280, 52);
    style_btn(save_btn, C_BTN_ACT);
    lv_obj_add_event_cb(save_btn, ble_save_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* svl = make_label(save_btn, t(TK::SAVE), f->f22, C_TEXT);
    lv_obj_center(svl);

    // Keyboard — child of screen (fixed at bottom, not scrollable).
    bl_ctx.kb = build_keyboard(bl_ctx.scr);
    lv_obj_add_event_cb(bl_ctx.kb, ble_kb_hide_cb, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(bl_ctx.kb, ble_kb_hide_cb, LV_EVENT_CANCEL, nullptr);

    lv_screen_load(bl_ctx.scr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// DISPLAY SETTINGS SCREEN
// ═══════════════════════════════════════════════════════════════════════════════

static lv_obj_t* s_disp_prev = nullptr;

static void brightness_slider_cb(lv_event_t* e) {
    int        val = lv_slider_get_value(lv_event_get_target_obj(e));
    lv_obj_t*  lbl = (lv_obj_t*)lv_event_get_user_data(e);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", val);
    lv_label_set_text(lbl, buf);
    p4SetNormalBrightness(val);
}

static void disp_back_cb(lv_event_t*) {
    lv_obj_t* cur = lv_screen_active();
    lv_screen_load(s_disp_prev);
    lv_obj_delete(cur);
}

static void disp_timeout_cb(lv_event_t* e) {
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    // Setter persists to NVS AND updates the live s_timeout_ms so the new
    // timeout takes effect immediately without rebooting.
    p4SetScreenTimeoutIdx(idx);
    lv_obj_t* prev = s_disp_prev;
    lv_obj_t* cur  = lv_screen_active();
    lv_screen_load(prev);
    lv_obj_delete(cur);
}

static void show_display(lv_obj_t* from) {
    const P4Fonts* f = p4GetFonts();
    s_disp_prev = from;

    lv_obj_t* scr = build_title_bar(t(TK::DISPLAY_TITLE), disp_back_cb);

    uint8_t cur_idx = nvs_read_u8("display", "timeout_idx", 3);  // default: never

    lv_obj_t* lbl_timeout = make_label(scr, t(TK::TIMEOUT_LABEL), f->f22, C_LABEL);
    lv_obj_set_pos(lbl_timeout, 20, TITLE_H + 24);

    static const TK OPTS[4] = {
        TK::TIMEOUT_30S, TK::TIMEOUT_1M, TK::TIMEOUT_3M, TK::TIMEOUT_NEVER
    };
    static constexpr int BTN_W = 170;
    static constexpr int BTN_H = 80;
    static constexpr int GAP   = 20;
    const int total_w = 4 * BTN_W + 3 * GAP;
    const int start_x = (SCR_W - total_w) / 2;

    for (int i = 0; i < 4; i++) {
        lv_obj_t* btn = lv_button_create(scr);
        lv_obj_set_pos(btn, start_x + i * (BTN_W + GAP), TITLE_H + 80);
        lv_obj_set_size(btn, BTN_W, BTN_H);
        style_btn(btn, (i == cur_idx) ? C_BTN_SEL : C_BTN);
        lv_obj_add_event_cb(btn, disp_timeout_cb, LV_EVENT_CLICKED,
                            (void*)(uintptr_t)i);
        lv_obj_t* l = make_label(btn, t(OPTS[i]), f->f20, C_TEXT);
        lv_obj_center(l);
    }

    // Brightness slider
    const int SL_Y = TITLE_H + 200;
    lv_obj_t* lbl_bright = make_label(scr, t(TK::BRIGHTNESS_LABEL), f->f22, C_LABEL);
    lv_obj_set_pos(lbl_bright, 20, SL_Y);

    uint8_t cur_bright = nvs_read_u8("display", "brightness", 100);
    char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%d%%", (int)cur_bright);
    lv_obj_t* lbl_val = make_label(scr, vbuf, f->f22, C_TEXT);
    lv_obj_set_pos(lbl_val, SCR_W - 20 - 60, SL_Y);

    lv_obj_t* slider = lv_slider_create(scr);
    lv_obj_set_pos(slider, 20, SL_Y + 38);
    lv_obj_set_size(slider, SCR_W - 40, 44);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, (int)cur_bright, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, C_BTN,     LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, C_BTN_ACT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, C_DIM,     LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 6, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, lbl_val);

    lv_screen_load(scr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// LANGUAGE SCREEN
// ═══════════════════════════════════════════════════════════════════════════════

static lv_obj_t* s_lang_prev = nullptr;

static void lang_back_cb(lv_event_t*) {
    lv_obj_t* cur = lv_screen_active();
    lv_screen_load(s_lang_prev);
    lv_obj_delete(cur);
}

static void apply_lang_change() {
    lv_obj_t* lang_scr = lv_screen_active();
    lv_obj_t* menu_scr = s_lang_prev;
    // Go back to main screen and rebuild it with the new language.
    lv_screen_load(s_mainScreen);
    lv_obj_delete(lang_scr);
    if (menu_scr && menu_scr != s_mainScreen) lv_obj_delete(menu_scr);
    p4DisplayRebuild();
}

static void lang_es_cb(lv_event_t*) {
    setLanguage(Language::ES);
    apply_lang_change();
}

static void lang_en_cb(lv_event_t*) {
    setLanguage(Language::EN);
    apply_lang_change();
}

static void show_language(lv_obj_t* from) {
    const P4Fonts* f = p4GetFonts();
    s_lang_prev = from;

    lv_obj_t* scr = build_title_bar(t(TK::SELECT_LANG), lang_back_cb);

    Language cur = currentLanguage();

    static constexpr int BTN_W = 300;
    static constexpr int BTN_H = 120;
    const int center_y = (SCR_H + TITLE_H) / 2 - BTN_H / 2;
    const int gap      = 30;
    const int total_w  = 2 * BTN_W + gap;
    const int start_x  = (SCR_W - total_w) / 2;

    lv_obj_t* es_btn = lv_button_create(scr);
    lv_obj_set_pos(es_btn, start_x, center_y);
    lv_obj_set_size(es_btn, BTN_W, BTN_H);
    style_btn(es_btn, (cur == Language::ES) ? C_BTN_SEL : C_BTN);
    lv_obj_add_event_cb(es_btn, lang_es_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* es_l = make_label(es_btn, "Espa\xC3\xB1" "ol", f->f24, C_TEXT);
    lv_obj_center(es_l);

    lv_obj_t* en_btn = lv_button_create(scr);
    lv_obj_set_pos(en_btn, start_x + BTN_W + gap, center_y);
    lv_obj_set_size(en_btn, BTN_W, BTN_H);
    style_btn(en_btn, (cur == Language::EN) ? C_BTN_SEL : C_BTN);
    lv_obj_add_event_cb(en_btn, lang_en_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* en_l = make_label(en_btn, "English", f->f24, C_TEXT);
    lv_obj_center(en_l);

    lv_screen_load(scr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PUBLIC ENTRY POINT
// ═══════════════════════════════════════════════════════════════════════════════

void p4SettingsShow() {
    s_mainScreen = lv_screen_active();
    show_menu(nullptr);
}
