#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "ui.h"

#ifndef LV_SYMBOL_STAR
/* FontAwesome "star" used by LVGL symbol font */
#define LV_SYMBOL_STAR "\xEF\x80\x85"
#endif

#ifndef LV_SYMBOL_BUS
#ifdef LV_SYMBOL_DRIVE
#define LV_SYMBOL_BUS LV_SYMBOL_DRIVE
#else
#define LV_SYMBOL_BUS "BUS"
#endif
#endif

typedef struct {
    const char *id;
    const char *designation_official;
    const char *means_of_transport;
    const char *locality_name;
    const char *municipality_name;
} station_t;

#include "stations_data.h"

static lv_obj_t *g_tileview = NULL;
static lv_obj_t *g_tile_map = NULL;
static lv_obj_t *g_tile_times = NULL;

static lv_obj_t *g_search_panel = NULL;
static lv_obj_t *g_search_ta = NULL;
static lv_obj_t *g_search_results = NULL;
static lv_obj_t *g_keyboard = NULL;
static lv_obj_t *g_kb_hide_btn = NULL;
static lv_obj_t *g_kb_show_btn = NULL;

static lv_obj_t *g_departures_panel = NULL;
static lv_obj_t *g_events_list = NULL;
static lv_obj_t *g_departures_title = NULL;
static lv_obj_t *g_fav_btn = NULL;
static lv_obj_t *g_fav_lbl = NULL;
static lv_obj_t *g_wifi_btn = NULL;
static lv_obj_t *g_wifi_panel = NULL;
static lv_obj_t *g_wifi_list = NULL;
static lv_obj_t *g_wifi_keyboard = NULL;
static lv_obj_t *g_wifi_popup = NULL;
static lv_obj_t *g_wifi_popup_pass_ta = NULL;
static lv_obj_t *g_wifi_popup_ssid_label = NULL;
static lv_obj_t *g_wifi_scan_btn_label = NULL;
static lv_obj_t *g_wifi_scan_spinner = NULL;
static lv_timer_t *g_wifi_connect_timer = NULL;
static lv_timer_t *g_wifi_scan_timer = NULL;
static uint8_t g_wifi_poll_count = 0;
static uint8_t g_wifi_scan_anim_step = 0;
static char g_wifi_selected_ssid[33] = {0};
static char g_wifi_scan_ssids[20][33];
static bool g_wifi_scan_secure[20];
static int16_t g_wifi_scan_best_rssi[20];
static uint8_t g_wifi_scan_count = 0;
static lv_timer_t *g_station_refresh_timer = NULL;
static bool g_station_fetch_inflight = false;

static const station_t *g_selected_station = NULL;
static int32_t g_selected_station_index = -1;
static bool g_station_favorites[sizeof(kStations) / sizeof(kStations[0])] = {false};
static Preferences g_prefs;
static bool g_prefs_ready = false;
static lv_obj_t *g_clock_meter = NULL;
static lv_meter_indicator_t *g_clock_indic_hour = NULL;
static lv_meter_indicator_t *g_clock_indic_min = NULL;
static lv_meter_indicator_t *g_clock_indic_sec = NULL;
static lv_obj_t *g_clock_sec_ball = NULL;
static lv_timer_t *g_clock_timer = NULL;
static uint32_t g_clock_start_tick = 0;
static int g_clock_base_seconds = 0;

static const char *kOjpEndpoint = "https://api.opentransportdata.swiss/ojp20";
static const char *kOjpRequestorRef = "API-Explorer";
#ifndef OJP_API_TOKEN
#define OJP_API_TOKEN " INSERT-TOKEN HERE "
#endif

typedef struct {
    char line[12];
    char direction[72];
    uint32_t wait_min;
} api_departure_t;

typedef struct {
    uint32_t req_id;
    char station_id[24];
    char station_name[96];
    bool ok;
    size_t count;
    api_departure_t events[12];
} stop_fetch_ctx_t;

static uint32_t g_station_request_seq = 0;

static void show_wifi_panel(bool show);
static void hide_wifi_keyboard(void);
static void update_seconds_ball(uint32_t sec_pos);
static void on_wifi_scan(lv_event_t *e);
static void populate_wifi_list_from_scan(int count);
static void render_cached_wifi_list(void);
static void populate_events_from_api_or_mock(void);
static void start_station_fetch_async(const station_t *station);
static void station_fetch_task(void *param);
static void station_fetch_apply_cb(void *param);
static void update_favorite_button_state(void);
static void refresh_search_results(void);
static lv_obj_t *add_search_result_row(const station_t *station, bool is_favorite);
static void station_refresh_timer_cb(lv_timer_t *timer);
static void sort_departures_by_wait(api_departure_t *events, size_t count);
static void load_favorites_from_nvs(void);
static void save_favorites_to_nvs(void);

static String ascii_safe_text(const char *s)
{
    String out;
    if (s == NULL) {
        return out;
    }

    const uint8_t *p = (const uint8_t *)s;
    while (*p != 0U) {
        if (*p < 0x80U) {
            out += (char)*p;
            p++;
            continue;
        }
        if (*p == 0xC3U && p[1] != 0U) {
            uint8_t b = p[1];
            if (b == 0xA4U) out += "a";       /* ä */
            else if (b == 0x84U) out += "A";  /* Ä */
            else if (b == 0xB6U) out += "o";  /* ö */
            else if (b == 0x96U) out += "O";  /* Ö */
            else if (b == 0xBCU) out += "u";  /* ü */
            else if (b == 0x9CU) out += "U";  /* Ü */
            else if (b == 0x9FU) out += "ss";  /* ß */
            p += 2;
            continue;
        }
        /* Drop unsupported glyph bytes to keep UI font-safe. */
        p++;
    }
    return out;
}

static String format_transport_text(const char *s)
{
    String in = ascii_safe_text(s);
    if (in.length() == 0) {
        return in;
    }

    String out;
    out.reserve(in.length() + 8);
    bool prev_space = false;
    for (size_t i = 0; i < in.length(); i++) {
        char c = in.charAt((unsigned int)i);
        if (c == '|') {
            if (out.length() > 0 && out.charAt(out.length() - 1) != ' ') {
                out += ' ';
            }
            out += '|';
            out += ' ';
            prev_space = true;
            continue;
        }

        if (c == ' ') {
            if (prev_space) {
                continue;
            }
            prev_space = true;
            out += c;
            continue;
        }

        prev_space = false;
        out += c;
    }

    while (out.length() > 0 && out.charAt(out.length() - 1) == ' ') {
        out.remove(out.length() - 1);
    }
    return out;
}

static void wifi_scan_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    if (g_wifi_scan_btn_label != NULL) {
        static const char *kFrames[] = {"Scanning", "Scanning.", "Scanning..", "Scanning..."};
        lv_label_set_text(g_wifi_scan_btn_label, kFrames[g_wifi_scan_anim_step % 4U]);
        g_wifi_scan_anim_step++;
    }

    int count = WiFi.scanComplete();
    if (count == WIFI_SCAN_RUNNING) {
        return;
    }

    if (g_wifi_scan_timer != NULL) {
        lv_timer_del(g_wifi_scan_timer);
        g_wifi_scan_timer = NULL;
    }
    if (g_wifi_scan_btn_label != NULL) {
        lv_label_set_text(g_wifi_scan_btn_label, "Scan");
    }
    if (g_wifi_scan_spinner != NULL) {
        lv_obj_add_flag(g_wifi_scan_spinner, LV_OBJ_FLAG_HIDDEN);
    }

    if (count < 0) {
        lv_obj_clean(g_wifi_list);
        lv_list_add_text(g_wifi_list, "Scan failed");
        WiFi.scanDelete();
        return;
    }

    populate_wifi_list_from_scan(count);
    WiFi.scanDelete();
}

static void on_wifi_popup_close(lv_event_t *e)
{
    LV_UNUSED(e);
    hide_wifi_keyboard();
    if (g_wifi_popup != NULL) {
        lv_obj_add_flag(g_wifi_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_wifi_popup_password_focus(lv_event_t *e)
{
    if (g_wifi_keyboard == NULL) {
        return;
    }
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(g_wifi_keyboard, ta);
    lv_obj_clear_flag(g_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void populate_wifi_list_from_scan(int count)
{
    g_wifi_scan_count = 0;

    for (int i = 0; i < count; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) {
            continue;
        }

        char ssid_buf[33];
        ssid.toCharArray(ssid_buf, sizeof(ssid_buf));
        int16_t rssi = WiFi.RSSI(i);
        bool is_secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);

        int existing = -1;
        for (uint8_t j = 0; j < g_wifi_scan_count; j++) {
            if (strcmp(g_wifi_scan_ssids[j], ssid_buf) == 0) {
                existing = (int)j;
                break;
            }
        }

        if (existing >= 0) {
            if (rssi > g_wifi_scan_best_rssi[existing]) {
                g_wifi_scan_best_rssi[existing] = rssi;
            }
            if (is_secure) {
                g_wifi_scan_secure[existing] = true;
            }
            continue;
        }

        if (g_wifi_scan_count >= 20U) {
            break;
        }

        strncpy(g_wifi_scan_ssids[g_wifi_scan_count], ssid_buf, sizeof(g_wifi_scan_ssids[g_wifi_scan_count]) - 1);
        g_wifi_scan_ssids[g_wifi_scan_count][sizeof(g_wifi_scan_ssids[g_wifi_scan_count]) - 1] = '\0';
        g_wifi_scan_secure[g_wifi_scan_count] = is_secure;
        g_wifi_scan_best_rssi[g_wifi_scan_count] = rssi;
        g_wifi_scan_count++;
    }

    if (g_wifi_scan_count == 0U) {
        lv_obj_clean(g_wifi_list);
        lv_list_add_text(g_wifi_list, "No networks found");
        return;
    }

    render_cached_wifi_list();
}

static void render_cached_wifi_list(void)
{
    lv_obj_clean(g_wifi_list);
    if (g_wifi_scan_count == 0U) {
        lv_list_add_text(g_wifi_list, "Tap Scan to search networks");
        return;
    }

    String connected_ssid = WiFi.SSID();
    bool has_connected = (WiFi.status() == WL_CONNECTED && connected_ssid.length() > 0);

    for (uint8_t i = 0; i < g_wifi_scan_count; i++) {
        lv_obj_t *btn = lv_list_add_btn(g_wifi_list, LV_SYMBOL_WIFI, g_wifi_scan_ssids[i]);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x060B16), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_right(btn, 88, LV_PART_MAIN);

        if (has_connected && strcmp(g_wifi_scan_ssids[i], connected_ssid.c_str()) == 0) {
            lv_obj_t *tag = lv_label_create(btn);
            lv_label_set_text(tag, "Connected");
            lv_obj_set_style_text_color(tag, lv_color_hex(0x22C55E), 0);
            lv_obj_set_style_text_font(tag, &lv_font_montserrat_14, 0);
            lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -8, 0);
        }

        lv_obj_add_event_cb(btn, [](lv_event_t *evt) {
            uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(evt);
            if (idx >= g_wifi_scan_count) {
                return;
            }
            strncpy(g_wifi_selected_ssid, g_wifi_scan_ssids[idx], sizeof(g_wifi_selected_ssid) - 1);
            g_wifi_selected_ssid[sizeof(g_wifi_selected_ssid) - 1] = '\0';
            String ssid_line = String("Network: ") + String(g_wifi_selected_ssid);
            lv_label_set_text(g_wifi_popup_ssid_label, ssid_line.c_str());
            lv_textarea_set_text(g_wifi_popup_pass_ta, "");
            lv_obj_clear_flag(g_wifi_popup, LV_OBJ_FLAG_HIDDEN);
        }, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}

static void on_wifi_scan(lv_event_t *e)
{
    LV_UNUSED(e);

    if (g_wifi_scan_timer != NULL) {
        return;
    }

    lv_obj_clean(g_wifi_list);
    if (g_wifi_scan_spinner != NULL) {
        lv_obj_clear_flag(g_wifi_scan_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_wifi_scan_spinner);
    }

    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    int started = WiFi.scanNetworks(true, true);
    if (started == WIFI_SCAN_FAILED) {
        if (g_wifi_scan_spinner != NULL) {
            lv_obj_add_flag(g_wifi_scan_spinner, LV_OBJ_FLAG_HIDDEN);
        }
        lv_list_add_text(g_wifi_list, "Scan failed");
        return;
    }

    g_wifi_scan_anim_step = 0;
    if (g_wifi_scan_btn_label != NULL) {
        lv_label_set_text(g_wifi_scan_btn_label, "Scanning");
    }
    g_wifi_scan_timer = lv_timer_create(wifi_scan_timer_cb, 180, NULL);
}

static void wifi_connect_poll_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
        if (g_wifi_connect_timer != NULL) {
            lv_timer_del(g_wifi_connect_timer);
            g_wifi_connect_timer = NULL;
        }
        render_cached_wifi_list();
        return;
    }

    g_wifi_poll_count++;
    if (g_wifi_poll_count >= 20U) {
        if (g_wifi_connect_timer != NULL) {
            lv_timer_del(g_wifi_connect_timer);
            g_wifi_connect_timer = NULL;
        }
        render_cached_wifi_list();
        lv_list_add_text(g_wifi_list, "Connection failed");
    }
}

static void on_wifi_connect(lv_event_t *e)
{
    LV_UNUSED(e);

    if (g_wifi_selected_ssid[0] == '\0') {
        lv_label_set_text(g_wifi_popup_ssid_label, "Pick a network first");
        return;
    }
    const char *pass = lv_textarea_get_text(g_wifi_popup_pass_ta);

    hide_wifi_keyboard();
    if (g_wifi_popup != NULL) {
        lv_obj_add_flag(g_wifi_popup, LV_OBJ_FLAG_HIDDEN);
    }

    if (g_wifi_connect_timer != NULL) {
        lv_timer_del(g_wifi_connect_timer);
        g_wifi_connect_timer = NULL;
    }
    if (g_wifi_scan_timer != NULL) {
        lv_timer_del(g_wifi_scan_timer);
        g_wifi_scan_timer = NULL;
    }
    if (g_wifi_scan_btn_label != NULL) {
        lv_label_set_text(g_wifi_scan_btn_label, "Scan");
    }
    if (g_wifi_scan_spinner != NULL) {
        lv_obj_add_flag(g_wifi_scan_spinner, LV_OBJ_FLAG_HIDDEN);
    }

    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.disconnect(false, false);
    WiFi.begin(g_wifi_selected_ssid, pass);
    g_wifi_poll_count = 0;
    lv_obj_clean(g_wifi_list);
    lv_list_add_text(g_wifi_list, "Connecting...");
    g_wifi_connect_timer = lv_timer_create(wifi_connect_poll_cb, 500, NULL);
}

static void on_wifi_open(lv_event_t *e)
{
    LV_UNUSED(e);
    show_wifi_panel(true);
}

static void on_wifi_close(lv_event_t *e)
{
    LV_UNUSED(e);
    show_wifi_panel(false);
}

static void hide_wifi_keyboard(void)
{
    if (g_wifi_keyboard == NULL) {
        return;
    }
    lv_keyboard_set_textarea(g_wifi_keyboard, NULL);
    lv_obj_add_flag(g_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void on_wifi_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        hide_wifi_keyboard();
    }
}

static void show_wifi_panel(bool show)
{
    if (g_wifi_panel == NULL) {
        return;
    }
    if (show) {
        lv_obj_clear_flag(g_wifi_panel, LV_OBJ_FLAG_HIDDEN);
        if (g_wifi_scan_timer != NULL || WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
            if (g_wifi_scan_btn_label != NULL) {
                lv_label_set_text(g_wifi_scan_btn_label, "Scanning...");
            }
            if (g_wifi_scan_spinner != NULL) {
                lv_obj_clear_flag(g_wifi_scan_spinner, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(g_wifi_scan_spinner);
            }
            if (lv_obj_get_child_cnt(g_wifi_list) == 0) {
                lv_list_add_text(g_wifi_list, "Scanning...");
            }
        } else {
            render_cached_wifi_list();
            if (g_wifi_scan_btn_label != NULL) {
                lv_label_set_text(g_wifi_scan_btn_label, "Scan");
            }
            if (g_wifi_scan_spinner != NULL) {
                lv_obj_add_flag(g_wifi_scan_spinner, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        hide_wifi_keyboard();
        if (g_wifi_popup != NULL) {
            lv_obj_add_flag(g_wifi_popup, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_wifi_scan_timer != NULL) {
            lv_timer_del(g_wifi_scan_timer);
            g_wifi_scan_timer = NULL;
        }
        WiFi.scanDelete();
        if (g_wifi_scan_btn_label != NULL) {
            lv_label_set_text(g_wifi_scan_btn_label, "Scan");
        }
        if (g_wifi_scan_spinner != NULL) {
            lv_obj_add_flag(g_wifi_scan_spinner, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(g_wifi_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_seconds_ball(uint32_t sec_pos)
{
    if (g_clock_meter == NULL || g_clock_sec_ball == NULL) {
        return;
    }

    int32_t meter_w = lv_obj_get_content_width(g_clock_meter);
    int32_t meter_h = lv_obj_get_content_height(g_clock_meter);
    int32_t ball_w = lv_obj_get_width(g_clock_sec_ball);
    int32_t ball_h = lv_obj_get_height(g_clock_sec_ball);
    int32_t cx = meter_w / 2;
    int32_t cy = meter_h / 2;
    /* Use content radius and match seconds needle r_mod (-16). */
    int32_t radius = (meter_w / 2) - 16;

    int32_t angle_deg_i = lv_map((int32_t)sec_pos, 0, 59, 0, 360) + 270;
    float angle_deg = (float)(angle_deg_i % 360);
    float angle_rad = angle_deg * 0.01745329252f;
    int32_t bx = cx + (int32_t)lroundf(cosf(angle_rad) * (float)radius) - (ball_w / 2);
    int32_t by = cy + (int32_t)lroundf(sinf(angle_rad) * (float)radius) - (ball_h / 2);
    lv_obj_set_pos(g_clock_sec_ball, bx, by);
}

static void update_departures_layout(void)
{
    int32_t h = lv_obj_get_height(g_departures_panel);
    if (h < 220) {
        h = 320;
    }
    int32_t list_h = h - 62;
    if (list_h < 120) {
        list_h = 120;
    }
    lv_obj_set_size(g_events_list, LV_PCT(100), list_h);
    lv_obj_align(g_events_list, LV_ALIGN_TOP_LEFT, 0, 62);
}

static void update_sbb_clock(void)
{
    if (g_clock_meter == NULL) {
        return;
    }

    uint32_t hour = 0U;
    uint32_t min = 0U;
    uint32_t sec = 0U;

    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        hour = (uint32_t)ti.tm_hour;
        min = (uint32_t)ti.tm_min;
        sec = (uint32_t)ti.tm_sec;
    } else {
        uint32_t elapsed_s = lv_tick_elaps(g_clock_start_tick) / 1000U;
        uint32_t now_s = ((uint32_t)g_clock_base_seconds + elapsed_s) % 86400U;
        hour = (now_s / 3600U) % 24U;
        min = (now_s / 60U) % 60U;
        sec = now_s % 60U;
    }

    uint32_t hour_pos = ((hour % 12U) * 5U) + (min / 12U);

    lv_meter_set_indicator_value(g_clock_meter, g_clock_indic_hour, hour_pos);
    lv_meter_set_indicator_value(g_clock_meter, g_clock_indic_min, min);
    lv_meter_set_indicator_value(g_clock_meter, g_clock_indic_sec, sec);
    update_seconds_ball(sec);
}

static void clock_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    update_sbb_clock();
}

static void create_sbb_clock(lv_obj_t *parent)
{
    int hh = 12, mm = 0, ss = 0;
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    g_clock_base_seconds = (hh * 3600) + (mm * 60) + ss;
    g_clock_start_tick = lv_tick_get();

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x020617), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(parent, 0, LV_PART_MAIN);

    g_clock_meter = lv_meter_create(parent);
    lv_obj_set_size(g_clock_meter, 236, 236);
    lv_obj_center(g_clock_meter);
    lv_obj_set_style_bg_color(g_clock_meter, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_clock_meter, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_clock_meter, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_clock_meter, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_clock_meter, 6, LV_PART_MAIN);

    lv_meter_scale_t *scale = lv_meter_add_scale(g_clock_meter);
    lv_meter_set_scale_range(g_clock_meter, scale, 0, 59, 360, 270);
    lv_meter_set_scale_ticks(g_clock_meter, scale, 60, 2, 8, lv_color_hex(0x000000));
    lv_meter_set_scale_major_ticks(g_clock_meter, scale, 5, 4, 14, lv_color_hex(0x000000), 10);

    g_clock_indic_hour = lv_meter_add_needle_line(g_clock_meter, scale, 8, lv_color_hex(0x000000), -48);
    g_clock_indic_min = lv_meter_add_needle_line(g_clock_meter, scale, 7, lv_color_hex(0x000000), -24);
    g_clock_indic_sec = lv_meter_add_needle_line(g_clock_meter, scale, 3, lv_color_hex(0xE10600), -16);

    lv_obj_t *center_dot = lv_obj_create(g_clock_meter);
    lv_obj_set_size(center_dot, 10, 10);
    lv_obj_center(center_dot);
    lv_obj_set_style_radius(center_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(center_dot, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(center_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(center_dot, 0, LV_PART_MAIN);

    g_clock_sec_ball = lv_obj_create(g_clock_meter);
    lv_obj_set_size(g_clock_sec_ball, 14, 14);
    lv_obj_set_style_radius(g_clock_sec_ball, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_clock_sec_ball, lv_color_hex(0xE10600), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_clock_sec_ball, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_clock_sec_ball, 0, LV_PART_MAIN);

    g_wifi_btn = lv_btn_create(parent);
    lv_obj_set_size(g_wifi_btn, 44, 44);
    lv_obj_align(g_wifi_btn, LV_ALIGN_TOP_LEFT, 0, 6);
    lv_obj_set_style_bg_color(g_wifi_btn, lv_color_hex(0x2563EB), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_wifi_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_wifi_btn, 22, LV_PART_MAIN);
    lv_obj_add_event_cb(g_wifi_btn, on_wifi_open, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wifi_lbl = lv_label_create(g_wifi_btn);
    lv_label_set_text(wifi_lbl, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(wifi_lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(wifi_lbl);

    g_wifi_panel = lv_obj_create(parent);
    lv_obj_set_size(g_wifi_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_wifi_panel, lv_color_hex(0x020617), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_wifi_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_wifi_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_wifi_panel, 10, LV_PART_MAIN);
    lv_obj_add_flag(g_wifi_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *wifi_title = lv_label_create(g_wifi_panel);
    lv_label_set_text(wifi_title, "Wi-Fi setup");
    lv_obj_set_style_text_color(wifi_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(wifi_title, &lv_font_montserrat_22, 0);
    lv_obj_align(wifi_title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *wifi_close_btn = lv_btn_create(g_wifi_panel);
    lv_obj_set_size(wifi_close_btn, 44, 44);
    lv_obj_align(wifi_close_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(wifi_close_btn, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_close_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_close_btn, 22, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_close_btn, on_wifi_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wifi_close_lbl = lv_label_create(wifi_close_btn);
    lv_label_set_text(wifi_close_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(wifi_close_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(wifi_close_lbl);

    lv_obj_t *wifi_scan_btn = lv_btn_create(g_wifi_panel);
    lv_obj_set_size(wifi_scan_btn, 100, 36);
    lv_obj_align(wifi_scan_btn, LV_ALIGN_TOP_RIGHT, 0, 6);
    lv_obj_set_style_bg_color(wifi_scan_btn, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_scan_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_scan_btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_scan_btn, on_wifi_scan, LV_EVENT_CLICKED, NULL);
    g_wifi_scan_btn_label = lv_label_create(wifi_scan_btn);
    lv_label_set_text(g_wifi_scan_btn_label, "Scan");
    lv_obj_set_style_text_color(g_wifi_scan_btn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(g_wifi_scan_btn_label);

    g_wifi_list = lv_list_create(g_wifi_panel);
    lv_obj_set_size(g_wifi_list, LV_PCT(100), 206);
    lv_obj_align(g_wifi_list, LV_ALIGN_TOP_LEFT, 0, 52);
    lv_obj_set_style_bg_color(g_wifi_list, lv_color_hex(0x020617), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_wifi_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_wifi_list, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_wifi_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(g_wifi_list, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_wifi_list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(g_wifi_list, 6, LV_PART_MAIN);

    g_wifi_scan_spinner = lv_spinner_create(g_wifi_panel, 1000, 60);
    lv_obj_set_size(g_wifi_scan_spinner, 36, 36);
    lv_obj_align(g_wifi_scan_spinner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_arc_width(g_wifi_scan_spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_wifi_scan_spinner, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_wifi_scan_spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_wifi_scan_spinner, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_add_flag(g_wifi_scan_spinner, LV_OBJ_FLAG_HIDDEN);

    g_wifi_popup = lv_obj_create(g_wifi_panel);
    lv_obj_set_size(g_wifi_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_wifi_popup, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_wifi_popup, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_wifi_popup, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_wifi_popup, 0, LV_PART_MAIN);
    lv_obj_add_flag(g_wifi_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *wifi_popup_card = lv_obj_create(g_wifi_popup);
    lv_obj_set_size(wifi_popup_card, 210, 150);
    lv_obj_center(wifi_popup_card);
    lv_obj_set_style_bg_color(wifi_popup_card, lv_color_hex(0x060B16), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifi_popup_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(wifi_popup_card, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_popup_card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_popup_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wifi_popup_card, 10, LV_PART_MAIN);

    g_wifi_popup_ssid_label = lv_label_create(wifi_popup_card);
    lv_label_set_text(g_wifi_popup_ssid_label, "Network:");
    lv_obj_set_width(g_wifi_popup_ssid_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_wifi_popup_ssid_label, lv_color_hex(0xE5E7EB), 0);
    lv_label_set_long_mode(g_wifi_popup_ssid_label, LV_LABEL_LONG_DOT);
    lv_obj_align(g_wifi_popup_ssid_label, LV_ALIGN_TOP_LEFT, 0, 0);

    g_wifi_popup_pass_ta = lv_textarea_create(wifi_popup_card);
    lv_obj_set_size(g_wifi_popup_pass_ta, LV_PCT(100), 38);
    lv_obj_align(g_wifi_popup_pass_ta, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_textarea_set_placeholder_text(g_wifi_popup_pass_ta, "Password");
    lv_textarea_set_password_mode(g_wifi_popup_pass_ta, true);
    lv_obj_set_style_bg_color(g_wifi_popup_pass_ta, lv_color_hex(0x020617), LV_PART_MAIN);
    lv_obj_set_style_text_color(g_wifi_popup_pass_ta, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_border_color(g_wifi_popup_pass_ta, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_wifi_popup_pass_ta, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(g_wifi_popup_pass_ta, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(g_wifi_popup_pass_ta, on_wifi_popup_password_focus, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *wifi_popup_cancel_btn = lv_btn_create(wifi_popup_card);
    lv_obj_set_size(wifi_popup_cancel_btn, 84, 34);
    lv_obj_align(wifi_popup_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(wifi_popup_cancel_btn, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_popup_cancel_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_popup_cancel_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_popup_cancel_btn, on_wifi_popup_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wifi_popup_cancel_lbl = lv_label_create(wifi_popup_cancel_btn);
    lv_label_set_text(wifi_popup_cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(wifi_popup_cancel_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(wifi_popup_cancel_lbl);

    lv_obj_t *wifi_popup_connect_btn = lv_btn_create(wifi_popup_card);
    lv_obj_set_size(wifi_popup_connect_btn, 84, 34);
    lv_obj_align(wifi_popup_connect_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(wifi_popup_connect_btn, lv_color_hex(0x16A34A), LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_popup_connect_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_popup_connect_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_popup_connect_btn, on_wifi_connect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wifi_popup_connect_lbl = lv_label_create(wifi_popup_connect_btn);
    lv_label_set_text(wifi_popup_connect_lbl, "Connect");
    lv_obj_set_style_text_color(wifi_popup_connect_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(wifi_popup_connect_lbl);

    g_wifi_keyboard = lv_keyboard_create(g_wifi_panel);
    lv_obj_set_size(g_wifi_keyboard, LV_PCT(100), 140);
    lv_obj_align(g_wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(g_wifi_keyboard, lv_color_hex(0x060B16), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_wifi_keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(g_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_wifi_keyboard, on_wifi_kb_event, LV_EVENT_ALL, NULL);

    /* Start scanning once in background so list is ready when panel opens. */
    on_wifi_scan(NULL);

    update_sbb_clock();
    g_clock_timer = lv_timer_create(clock_timer_cb, 100, NULL);
}

static void set_keyboard_visible(bool visible)
{
    if (visible) {
        lv_obj_clear_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(g_keyboard, g_search_ta);
        lv_obj_set_height(g_search_results, 190);
        lv_obj_clear_flag(g_kb_hide_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_kb_show_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_keyboard_set_textarea(g_keyboard, NULL);
        lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(g_search_ta, LV_STATE_FOCUSED);
        lv_obj_set_height(g_search_results, 355);
        lv_obj_add_flag(g_kb_hide_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_kb_show_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool is_favorite_index(uint32_t idx)
{
    if (idx >= (sizeof(kStations) / sizeof(kStations[0]))) {
        return false;
    }
    return g_station_favorites[idx];
}

static void load_favorites_from_nvs(void)
{
    memset(g_station_favorites, 0, sizeof(g_station_favorites));

    if (!g_prefs.begin("ui_state", false)) {
        g_prefs_ready = false;
        return;
    }
    g_prefs_ready = true;

    const size_t station_count = sizeof(kStations) / sizeof(kStations[0]);
    const size_t bit_bytes = (station_count + 7U) / 8U;
    if (bit_bytes == 0U) {
        return;
    }

    uint8_t *buf = (uint8_t *)calloc(bit_bytes, 1);
    if (buf == NULL) {
        return;
    }

    size_t got = g_prefs.getBytes("fav_bits", buf, bit_bytes);
    if (got == 0U) {
        free(buf);
        return;
    }

    for (size_t i = 0; i < station_count; i++) {
        size_t byte_idx = i >> 3;
        uint8_t bit_mask = (uint8_t)(1U << (i & 7U));
        g_station_favorites[i] = ((buf[byte_idx] & bit_mask) != 0U);
    }

    free(buf);
}

static void save_favorites_to_nvs(void)
{
    if (!g_prefs_ready) {
        return;
    }

    const size_t station_count = sizeof(kStations) / sizeof(kStations[0]);
    const size_t bit_bytes = (station_count + 7U) / 8U;
    if (bit_bytes == 0U) {
        return;
    }

    uint8_t *buf = (uint8_t *)calloc(bit_bytes, 1);
    if (buf == NULL) {
        return;
    }

    for (size_t i = 0; i < station_count; i++) {
        if (!g_station_favorites[i]) {
            continue;
        }
        size_t byte_idx = i >> 3;
        uint8_t bit_mask = (uint8_t)(1U << (i & 7U));
        buf[byte_idx] |= bit_mask;
    }

    g_prefs.putBytes("fav_bits", buf, bit_bytes);
    free(buf);
}

static int32_t station_index_from_ptr(const station_t *station)
{
    if (station == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < (sizeof(kStations) / sizeof(kStations[0])); i++) {
        if (&kStations[i] == station) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void update_favorite_button_state(void)
{
    if (g_fav_btn == NULL || g_fav_lbl == NULL) {
        return;
    }

    bool active = false;
    if (g_selected_station_index >= 0) {
        active = is_favorite_index((uint32_t)g_selected_station_index);
    }

    lv_label_set_text(g_fav_lbl, LV_SYMBOL_STAR);
    lv_obj_set_style_text_color(
        g_fav_lbl,
        active ? lv_color_hex(0xFACC15) : lv_color_hex(0x334155),
        0
    );
    lv_obj_set_style_bg_color(
        g_fav_btn,
        active ? lv_color_hex(0x111827) : lv_color_hex(0x0B1220),
        LV_PART_MAIN
    );
    lv_obj_set_style_border_color(
        g_fav_btn,
        active ? lv_color_hex(0xFACC15) : lv_color_hex(0x1E293B),
        LV_PART_MAIN
    );
}

static void normalize_line_label(const char *line, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    if (line == NULL) {
        strncpy(out, "?", out_size - 1U);
        out[out_size - 1U] = '\0';
        return;
    }

    size_t j = 0;
    for (size_t i = 0; line[i] != '\0' && j < (out_size - 1U); i++) {
        if (line[i] >= '0' && line[i] <= '9') {
            out[j++] = line[i];
        } else if (j > 0U) {
            break;
        }
    }

    if (j == 0U) {
        strncpy(out, line, out_size - 1U);
        out[out_size - 1U] = '\0';
        return;
    }
    out[j] = '\0';
}

static lv_color_t color_for_line(const char *line)
{
    static const uint32_t palette[] = {
        0xDC2626, 0xD97706, 0xCA8A04, 0x16A34A, 0x059669, 0x0891B2,
        0x2563EB, 0x4F46E5, 0x7C3AED, 0x9333EA, 0xBE185D, 0xC2410C
    };

    if (line != NULL && line[0] != '\0') {
        uint32_t hash = 2166136261u;
        for (const char *p = line; *p != '\0'; p++) {
            hash ^= (uint8_t)(*p);
            hash *= 16777619u;
        }
        uint32_t idx = hash % (sizeof(palette) / sizeof(palette[0]));
        return lv_color_hex(palette[idx]);
    }
    return lv_color_hex(0x475569);
}

static void add_event_row(const char *line, const char *direction, uint32_t wait_min)
{
    char line_label[12];
    normalize_line_label(line, line_label, sizeof(line_label));

    lv_obj_t *item = lv_obj_create(g_events_list);
    lv_obj_set_size(item, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(item, color_for_line(line_label), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(item, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(item, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_left(item, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_right(item, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_top(item, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(item, 10, LV_PART_MAIN);

    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *nr = lv_label_create(item);
    lv_label_set_text(nr, line_label);
    lv_obj_set_width(nr, 42);
    lv_obj_set_style_text_align(nr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(nr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(nr, &lv_font_montserrat_22, 0);

    lv_obj_t *dir = lv_label_create(item);
    String safe_dir = ascii_safe_text(direction);
    lv_label_set_text(dir, safe_dir.c_str());
    lv_obj_set_flex_grow(dir, 1);
    lv_obj_set_style_text_align(dir, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(dir, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(dir, &lv_font_montserrat_18, 0);

    lv_obj_t *min = lv_label_create(item);
    if (wait_min == 0U) {
        lv_label_set_text(min, LV_SYMBOL_OK);
        lv_obj_set_width(min, 76);
        lv_obj_set_style_text_align(min, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(min, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(min, &lv_font_montserrat_22, 0);
    } else {
        char min_txt[16];
        ultoa((unsigned long)wait_min, min_txt, 10);
        strncat(min_txt, " min", sizeof(min_txt) - strlen(min_txt) - 1);
        lv_label_set_text(min, min_txt);
        lv_obj_set_width(min, 76);
        lv_obj_set_style_text_align(min, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(min, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(min, &lv_font_montserrat_18, 0);
    }
}

static bool contains_case_insensitive(const char *haystack, const char *needle)
{
    if (needle == NULL || needle[0] == '\0') {
        return true;
    }

    size_t needle_len = strlen(needle);
    for (size_t i = 0; haystack[i] != '\0'; i++) {
        size_t j = 0;
        while (j < needle_len && haystack[i + j] != '\0') {
            char a = (char)tolower((unsigned char)haystack[i + j]);
            char b = (char)tolower((unsigned char)needle[j]);
            if (a != b) {
                break;
            }
            j++;
        }
        if (j == needle_len) {
            return true;
        }
    }
    return false;
}

static bool extract_tag_value(const char *start, const char *limit, const char *tag, char *out, size_t out_size)
{
    if (start == NULL || out == NULL || out_size == 0U) {
        return false;
    }
    out[0] = '\0';

    const char *pos = strstr(start, tag);
    if (pos == NULL || (limit != NULL && pos >= limit)) {
        return false;
    }

    const char *gt = strchr(pos, '>');
    if (gt == NULL || (limit != NULL && gt >= limit)) {
        return false;
    }
    gt++;

    const char *end = strchr(gt, '<');
    if (end == NULL || (limit != NULL && end > limit)) {
        return false;
    }

    size_t len = (size_t)(end - gt);
    if (len >= out_size) {
        len = out_size - 1U;
    }
    memcpy(out, gt, len);
    out[len] = '\0';
    return len > 0U;
}

static bool extract_nested_text_value(const char *start, const char *limit, const char *container_tag, char *out, size_t out_size)
{
    const char *container = strstr(start, container_tag);
    if (container == NULL || (limit != NULL && container >= limit)) {
        return false;
    }

    const char *text_open = strstr(container, "<Text");
    if (text_open == NULL || (limit != NULL && text_open >= limit)) {
        return false;
    }

    const char *gt = strchr(text_open, '>');
    if (gt == NULL || (limit != NULL && gt >= limit)) {
        return false;
    }
    gt++;

    const char *end = strchr(gt, '<');
    if (end == NULL || (limit != NULL && end > limit)) {
        return false;
    }

    size_t len = (size_t)(end - gt);
    if (len >= out_size) {
        len = out_size - 1U;
    }
    memcpy(out, gt, len);
    out[len] = '\0';
    return len > 0U;
}

static int extract_hhmm(const char *iso_ts)
{
    if (iso_ts == NULL) {
        return -1;
    }
    const char *t = strchr(iso_ts, 'T');
    if (t == NULL) {
        return -1;
    }
    int hh = 0;
    int mm = 0;
    if (sscanf(t + 1, "%2d:%2d", &hh, &mm) != 2) {
        return -1;
    }
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        return -1;
    }
    return (hh * 60) + mm;
}

static int extract_iso_tz_offset_min(const char *iso_ts, bool *is_utc_z)
{
    if (is_utc_z != NULL) {
        *is_utc_z = false;
    }
    if (iso_ts == NULL) {
        return 0;
    }

    const char *t = strchr(iso_ts, 'T');
    if (t == NULL) {
        return 0;
    }

    const char *z = strchr(t, 'Z');
    if (z != NULL) {
        if (is_utc_z != NULL) {
            *is_utc_z = true;
        }
        return 0;
    }

    const char *plus = strrchr(t, '+');
    const char *minus = strrchr(t, '-');
    const char *sign = plus;
    if (minus != NULL && (plus == NULL || minus > plus)) {
        sign = minus;
    }
    if (sign == NULL) {
        return 0;
    }

    int off_h = 0;
    int off_m = 0;
    if (sscanf(sign + 1, "%2d:%2d", &off_h, &off_m) != 2) {
        return 0;
    }

    int off = (off_h * 60) + off_m;
    return (*sign == '-') ? -off : off;
}

static int local_utc_offset_min_now(void)
{
    time_t now = time(NULL);
    struct tm lt;
    struct tm gt;
    localtime_r(&now, &lt);
    gmtime_r(&now, &gt);

    int day_delta = (lt.tm_yday - gt.tm_yday) + ((lt.tm_year - gt.tm_year) * 365);
    int minute_delta = (lt.tm_hour * 60 + lt.tm_min) - (gt.tm_hour * 60 + gt.tm_min);
    return (day_delta * 1440) + minute_delta;
}

static uint32_t minutes_until_from_response(const char *response_ts, const char *departure_ts, uint32_t fallback_min)
{
    LV_UNUSED(response_ts);

    int base = -1;
    struct tm now_tm;
    if (getLocalTime(&now_tm, 0)) {
        base = (now_tm.tm_hour * 60) + now_tm.tm_min;
    } else {
        base = extract_hhmm(response_ts);
    }
    int dep = extract_hhmm(departure_ts);
    if (base < 0 || dep < 0) {
        return fallback_min;
    }

    bool is_utc_z = false;
    int dep_tz_off = extract_iso_tz_offset_min(departure_ts, &is_utc_z);
    int local_off = local_utc_offset_min_now();
    /* Convert departure minute-of-day into local minute-of-day. */
    dep = dep + (local_off - dep_tz_off);
    while (dep < 0) {
        dep += 24 * 60;
    }
    while (dep >= 24 * 60) {
        dep -= 24 * 60;
    }

    int delta = dep - base;
    if (delta < 0) {
        delta += 24 * 60;
    }
    return (uint32_t)delta;
}

static bool parse_ojp_stop_events(const String &xml, api_departure_t *events, size_t max_events, size_t *out_count)
{
    if (events == NULL || max_events == 0U || out_count == NULL) {
        return false;
    }
    *out_count = 0U;

    const char *buf = xml.c_str();
    if (buf == NULL || buf[0] == '\0') {
        return false;
    }

    char response_ts[40] = {0};
    if (!extract_tag_value(buf, NULL, "ResponseTimestamp>", response_ts, sizeof(response_ts))) {
        extract_tag_value(buf, NULL, "RequestTimestamp>", response_ts, sizeof(response_ts));
    }

    const char *cursor = buf;
    const char *kEventOpen = "<StopEventResult>";
    const char *kEventClose = "</StopEventResult>";
    while (*out_count < max_events) {
        const char *event_start = strstr(cursor, kEventOpen);
        if (event_start == NULL) {
            break;
        }
        const char *event_end = strstr(event_start, kEventClose);
        if (event_end == NULL) {
            break;
        }

        api_departure_t dep = {0};
        char line[20] = {0};
        char direction[80] = {0};
        char departure_ts[40] = {0};

        if (!extract_nested_text_value(event_start, event_end, "PublishedServiceName", line, sizeof(line)) &&
            !extract_nested_text_value(event_start, event_end, "PublishedLineName", line, sizeof(line)) &&
            !extract_tag_value(event_start, event_end, "PublicCode>", line, sizeof(line))) {
            strncpy(line, "?", sizeof(line) - 1);
        }
        if (!extract_tag_value(event_start, event_end, "DestinationText>", direction, sizeof(direction)) &&
            !extract_nested_text_value(event_start, event_end, "DestinationText", direction, sizeof(direction))) {
            if (!extract_tag_value(event_start, event_end, "DestinationStopPointName>", direction, sizeof(direction)) &&
                !extract_nested_text_value(event_start, event_end, "DestinationStopPointName", direction, sizeof(direction))) {
                strncpy(direction, "Unknown", sizeof(direction) - 1);
            }
        }
        if (!extract_tag_value(event_start, event_end, "EstimatedTime>", departure_ts, sizeof(departure_ts))) {
            extract_tag_value(event_start, event_end, "TimetabledTime>", departure_ts, sizeof(departure_ts));
        }

        strncpy(dep.line, line, sizeof(dep.line) - 1);
        dep.line[sizeof(dep.line) - 1] = '\0';
        strncpy(dep.direction, direction, sizeof(dep.direction) - 1);
        dep.direction[sizeof(dep.direction) - 1] = '\0';
        dep.wait_min = minutes_until_from_response(response_ts, departure_ts, (uint32_t)((*out_count * 2U) + 1U));
        events[*out_count] = dep;
        Serial.printf("[OJP] parsed[%u] line='%s' dir='%s' dep='%s' wait=%lu\n",
                      (unsigned)*out_count,
                      dep.line,
                      dep.direction,
                      departure_ts,
                      (unsigned long)dep.wait_min);
        (*out_count)++;

        cursor = event_end + strlen(kEventClose);
    }

    return *out_count > 0U;
}

static String xml_escape(const char *s)
{
    String out;
    if (s == NULL) {
        return out;
    }
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += *p; break;
        }
    }
    return out;
}

static String iso8601_utc_now()
{
    time_t now = time(NULL);
    struct tm gmt_now;
    gmtime_r(&now, &gmt_now);

    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &gmt_now);
    return String(ts);
}

static String build_ojp_stop_event_request_xml(const station_t *station)
{
    String station_id = xml_escape(station->id);
    String station_name = xml_escape(station->designation_official);
    String req_ts = iso8601_utc_now();

    String xml;
    xml.reserve(1200);
    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    xml += "<OJP xmlns=\"http://www.vdv.de/ojp\" xmlns:siri=\"http://www.siri.org.uk/siri\" version=\"2.0\">";
    xml += "<OJPRequest><siri:ServiceRequest>";
    xml += "<siri:RequestTimestamp>";
    xml += req_ts;
    xml += "</siri:RequestTimestamp>";
    xml += "<siri:RequestorRef>";
    xml += kOjpRequestorRef;
    xml += "</siri:RequestorRef>";
    xml += "<OJPStopEventRequest>";
    xml += "<siri:RequestTimestamp>";
    xml += req_ts;
    xml += "</siri:RequestTimestamp>";
    xml += "<siri:MessageIdentifier>SER-";
    xml += station_id;
    xml += "-";
    xml += String((unsigned long)millis());
    xml += "</siri:MessageIdentifier>";
    xml += "<Location><PlaceRef>";
    xml += "<siri:StopPointRef>";
    xml += station_id;
    xml += "</siri:StopPointRef>";
    xml += "<Name><Text>";
    xml += station_name;
    xml += "</Text></Name>";
    xml += "</PlaceRef></Location>";
    xml += "<Params><NumberOfResults>8</NumberOfResults><StopEventType>departure</StopEventType></Params>";
    xml += "</OJPStopEventRequest>";
    xml += "</siri:ServiceRequest></OJPRequest></OJP>";
    return xml;
}

static bool fetch_stop_events(const station_t *station, api_departure_t *events, size_t max_events, size_t *out_count)
{
    if (station == NULL || station->id == NULL || station->designation_official == NULL || events == NULL || out_count == NULL) {
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, kOjpEndpoint)) {
        return false;
    }

    http.addHeader("Content-Type", "application/xml; charset=utf-8");
    http.addHeader("Accept", "application/xml");
    if (strlen(OJP_API_TOKEN) > 0U) {
        String auth = "Bearer ";
        auth += OJP_API_TOKEN;
        http.addHeader("Authorization", auth);
    }
    http.setTimeout(9000);

    String request_xml = build_ojp_stop_event_request_xml(station);
    Serial.println("[OJP] ===== REQUEST XML BEGIN =====");
    Serial.println(request_xml);
    Serial.println("[OJP] ===== REQUEST XML END =====");
    int code = http.POST((uint8_t *)request_xml.c_str(), request_xml.length());
    if (code == -11) {
        Serial.println("[OJP] transient timeout (-11), retrying once...");
        delay(200);
        code = http.POST((uint8_t *)request_xml.c_str(), request_xml.length());
    }
    Serial.printf("[OJP] HTTP code: %d\n", code);
    if (code <= 0) {
        http.end();
        return false;
    }

    String response = http.getString();
    Serial.println("[OJP] ===== RESPONSE XML BEGIN =====");
    Serial.println(response);
    Serial.println("[OJP] ===== RESPONSE XML END =====");
    http.end();
    return parse_ojp_stop_events(response, events, max_events, out_count);
}

static void populate_no_data_events(const char *reason)
{
    lv_obj_clean(g_events_list);

    if (g_selected_station == NULL) {
        lv_label_set_text(g_departures_title, "Trams from");
        lv_list_add_text(g_events_list, "No station selected");
        return;
    }

    String title = String("Trams from ") + ascii_safe_text(g_selected_station->designation_official);
    lv_label_set_text(g_departures_title, title.c_str());
    if (reason != NULL && reason[0] != '\0') {
        lv_list_add_text(g_events_list, reason);
    } else {
        lv_list_add_text(g_events_list, "No live departures available");
    }
    lv_obj_scroll_to_y(g_events_list, 0, LV_ANIM_OFF);
}

static void populate_events_from_api_or_mock(void)
{
    if (g_selected_station == NULL) {
        populate_no_data_events("No station selected");
        return;
    }

    api_departure_t events[12];
    size_t count = 0U;
    if (!fetch_stop_events(g_selected_station, events, sizeof(events) / sizeof(events[0]), &count) || count == 0U) {
        populate_no_data_events("API request failed or returned no departures");
        return;
    }

    lv_obj_clean(g_events_list);
    String title = String("Trams from ") + ascii_safe_text(g_selected_station->designation_official);
    lv_label_set_text(g_departures_title, title.c_str());
    sort_departures_by_wait(events, count);

    for (size_t i = 0; i < count; i++) {
        add_event_row(events[i].line, events[i].direction, events[i].wait_min);
    }
    lv_obj_scroll_to_y(g_events_list, 0, LV_ANIM_OFF);
}

static void show_search_panel(void)
{
    lv_obj_clear_flag(g_search_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_departures_panel, LV_OBJ_FLAG_HIDDEN);
    set_keyboard_visible(false);
}

static void show_departures_panel(void)
{
    lv_obj_add_flag(g_search_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_departures_panel, LV_OBJ_FLAG_HIDDEN);
    set_keyboard_visible(false);
    update_departures_layout();
}

static void on_station_selected(lv_event_t *e)
{
    const station_t *station = (const station_t *)lv_event_get_user_data(e);
    if (station == NULL) {
        return;
    }

    g_selected_station = station;
    g_selected_station_index = station_index_from_ptr(station);
    update_favorite_button_state();
    show_departures_panel();
    lv_obj_clean(g_events_list);
    lv_list_add_text(g_events_list, "Loading departures...");
    start_station_fetch_async(station);
    if (g_station_refresh_timer == NULL) {
        g_station_refresh_timer = lv_timer_create(station_refresh_timer_cb, 30000, NULL);
    } else {
        lv_timer_set_period(g_station_refresh_timer, 30000);
        lv_timer_reset(g_station_refresh_timer);
    }
}

static void on_toggle_favorite(lv_event_t *e)
{
    LV_UNUSED(e);
    if (g_selected_station_index < 0) {
        return;
    }

    uint32_t idx = (uint32_t)g_selected_station_index;
    g_station_favorites[idx] = !g_station_favorites[idx];
    save_favorites_to_nvs();
    update_favorite_button_state();
    refresh_search_results();
}

static void start_station_fetch_async(const station_t *station)
{
    if (station == NULL) {
        return;
    }
    if (g_station_fetch_inflight) {
        return;
    }

    stop_fetch_ctx_t *ctx = (stop_fetch_ctx_t *)calloc(1, sizeof(stop_fetch_ctx_t));
    if (ctx == NULL) {
        populate_no_data_events("Out of memory for request");
        return;
    }

    g_station_request_seq++;
    ctx->req_id = g_station_request_seq;
    strncpy(ctx->station_id, station->id, sizeof(ctx->station_id) - 1);
    strncpy(ctx->station_name, station->designation_official, sizeof(ctx->station_name) - 1);

    BaseType_t ok = xTaskCreatePinnedToCore(
        station_fetch_task,
        "station_fetch",
        8192,
        ctx,
        2,
        NULL,
        1
    );
    if (ok != pdPASS) {
        free(ctx);
        populate_no_data_events("Failed to start background request");
        g_station_fetch_inflight = false;
        return;
    }
    g_station_fetch_inflight = true;
}

static void station_fetch_task(void *param)
{
    stop_fetch_ctx_t *ctx = (stop_fetch_ctx_t *)param;
    if (ctx == NULL) {
        vTaskDelete(NULL);
        return;
    }

    station_t st = {ctx->station_id, ctx->station_name, NULL, NULL, NULL};
    ctx->ok = fetch_stop_events(&st, ctx->events, sizeof(ctx->events) / sizeof(ctx->events[0]), &ctx->count);
    lv_async_call(station_fetch_apply_cb, ctx);
    vTaskDelete(NULL);
}

static void station_fetch_apply_cb(void *param)
{
    stop_fetch_ctx_t *ctx = (stop_fetch_ctx_t *)param;
    if (ctx == NULL) {
        return;
    }
    g_station_fetch_inflight = false;

    if (ctx->req_id != g_station_request_seq) {
        free(ctx);
        return;
    }

    if (!ctx->ok || ctx->count == 0U) {
        populate_no_data_events("No departures parsed from API response");
        free(ctx);
        return;
    }

    lv_obj_clean(g_events_list);
    String title = String("Trams from ") + ascii_safe_text(ctx->station_name);
    lv_label_set_text(g_departures_title, title.c_str());
    sort_departures_by_wait(ctx->events, ctx->count);
    for (size_t i = 0; i < ctx->count; i++) {
        add_event_row(ctx->events[i].line, ctx->events[i].direction, ctx->events[i].wait_min);
    }
    lv_obj_scroll_to_y(g_events_list, 0, LV_ANIM_OFF);
    free(ctx);
}

static void sort_departures_by_wait(api_departure_t *events, size_t count)
{
    if (events == NULL || count < 2U) {
        return;
    }

    for (size_t i = 0; i + 1U < count; i++) {
        size_t min_idx = i;
        for (size_t j = i + 1U; j < count; j++) {
            if (events[j].wait_min < events[min_idx].wait_min) {
                min_idx = j;
            }
        }
        if (min_idx != i) {
            api_departure_t tmp = events[i];
            events[i] = events[min_idx];
            events[min_idx] = tmp;
        }
    }
}

static void station_refresh_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (g_selected_station == NULL) {
        return;
    }
    start_station_fetch_async(g_selected_station);
}

static void refresh_search_results(void)
{
    static const uint32_t kMaxSearchResults = 10U;
    const char *query = lv_textarea_get_text(g_search_ta);
    if (query == NULL) {
        query = "";
    }
    lv_obj_clean(g_search_results);

    bool has_query = (query[0] != '\0');

    uint32_t matches = 0;
    for (uint32_t pass = 0; pass < 2 && matches < kMaxSearchResults; pass++) {
        bool want_favorites = (pass == 0);
        for (uint32_t i = 0; i < (sizeof(kStations) / sizeof(kStations[0])); i++) {
            bool is_fav = is_favorite_index(i);
            if (want_favorites != is_fav) {
                continue;
            }
            if (has_query && !contains_case_insensitive(kStations[i].designation_official, query)) {
                continue;
            }
            add_search_result_row(&kStations[i], is_fav);
            matches++;
            if (matches >= kMaxSearchResults) {
                break;
            }
        }
    }
    if (matches == 0) {
        lv_obj_t *txt = lv_list_add_text(g_search_results, "No matching station");
        lv_obj_set_style_text_color(txt, lv_color_hex(0x94A3B8), 0);
    } else if (has_query) {
        lv_obj_t *txt = lv_list_add_text(g_search_results, "Showing top 10 results");
        lv_obj_set_style_text_color(txt, lv_color_hex(0x64748B), 0);
    }
}

static lv_obj_t *add_search_result_row(const station_t *station, bool is_favorite)
{
    if (station == NULL) {
        return NULL;
    }

    lv_obj_t *btn = lv_btn_create(g_search_results);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, 46);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x060B16), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_left(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(btn, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(btn, on_station_selected, LV_EVENT_CLICKED, (void *)station);

    lv_obj_t *left = lv_obj_create(btn);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(left, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(left, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_CLICKABLE);

    if (is_favorite) {
        lv_obj_t *star = lv_label_create(left);
        lv_label_set_text(star, LV_SYMBOL_STAR);
        lv_obj_set_style_text_color(star, lv_color_hex(0xFACC15), 0);
        lv_obj_set_style_text_font(star, &lv_font_montserrat_14, 0);
        lv_obj_clear_flag(star, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *name = lv_label_create(left);
    String safe_name = ascii_safe_text(station->designation_official);
    lv_label_set_text(name, safe_name.c_str());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(name, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *mot = lv_label_create(btn);
    String safe_mot = format_transport_text(station->means_of_transport);
    lv_label_set_text(mot, safe_mot.c_str());
    lv_obj_set_style_text_color(mot, lv_color_hex(0x93C5FD), 0);
    lv_obj_set_style_text_font(mot, &lv_font_montserrat_14, 0);
    lv_obj_clear_flag(mot, LV_OBJ_FLAG_CLICKABLE);

    return btn;
}

static void on_search_changed(lv_event_t *e)
{
    LV_UNUSED(e);
    refresh_search_results();
}

static void on_search_focus(lv_event_t *e)
{
    LV_UNUSED(e);
    set_keyboard_visible(true);
}

static void on_keyboard_toggle(lv_event_t *e)
{
    LV_UNUSED(e);
    bool is_hidden = lv_obj_has_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    set_keyboard_visible(is_hidden);
}

static void on_change_station(lv_event_t *e)
{
    LV_UNUSED(e);
    g_station_request_seq++;
    g_station_fetch_inflight = false;
    if (g_station_refresh_timer != NULL) {
        lv_timer_del(g_station_refresh_timer);
        g_station_refresh_timer = NULL;
    }
    g_selected_station = NULL;
    g_selected_station_index = -1;
    update_favorite_button_state();
    lv_textarea_set_text(g_search_ta, "");
    refresh_search_results();
    show_search_panel();
}

void ui_create()
{
    load_favorites_from_nvs();

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x020617), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    g_tileview = lv_tileview_create(lv_scr_act());
    lv_obj_set_size(g_tileview, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_tileview, lv_color_hex(0x020617), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_tileview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_tileview, 0, LV_PART_MAIN);

    g_tile_map = lv_tileview_add_tile(g_tileview, 0, 0, LV_DIR_RIGHT);
    g_tile_times = lv_tileview_add_tile(g_tileview, 1, 0, LV_DIR_LEFT);
    create_sbb_clock(g_tile_map);

    g_search_panel = lv_obj_create(g_tile_times);
    lv_obj_set_size(g_search_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(g_search_panel, 8, 0);
    lv_obj_set_style_bg_color(g_search_panel, lv_color_hex(0x020617), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_search_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_search_panel, 0, LV_PART_MAIN);

    g_search_ta = lv_textarea_create(g_search_panel);
    lv_obj_set_width(g_search_ta, LV_PCT(100));
    lv_textarea_set_placeholder_text(g_search_ta, "Type stop name...");
    lv_obj_align(g_search_ta, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(g_search_ta, on_search_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_search_ta, on_search_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_set_style_bg_color(g_search_ta, lv_color_hex(0x060B16), LV_PART_MAIN);
    lv_obj_set_style_text_color(g_search_ta, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_border_color(g_search_ta, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_search_ta, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(g_search_ta, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_left(g_search_ta, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(g_search_ta, 12, LV_PART_MAIN);

    g_search_results = lv_list_create(g_search_panel);
    lv_obj_set_size(g_search_results, LV_PCT(100), 190);
    lv_obj_align(g_search_results, LV_ALIGN_TOP_LEFT, 0, 48);
    lv_obj_set_style_bg_color(g_search_results, lv_color_hex(0x020617), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_search_results, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_search_results, lv_color_hex(0x0F172A), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_search_results, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(g_search_results, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_search_results, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(g_search_results, 6, LV_PART_MAIN);

    g_keyboard = lv_keyboard_create(g_search_panel);
    lv_obj_set_size(g_keyboard, LV_PCT(100), 160);
    lv_obj_align(g_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(g_keyboard, lv_color_hex(0x060B16), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_COVER, LV_PART_MAIN);

    g_kb_hide_btn = lv_btn_create(g_search_panel);
    lv_obj_set_size(g_kb_hide_btn, 64, 30);
    lv_obj_align(g_kb_hide_btn, LV_ALIGN_BOTTOM_RIGHT, 0, -166);
    lv_obj_set_style_bg_color(g_kb_hide_btn, lv_color_hex(0x0F172A), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_kb_hide_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_kb_hide_btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(g_kb_hide_btn, on_keyboard_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_t *kb_hide_lbl = lv_label_create(g_kb_hide_btn);
    lv_label_set_text(kb_hide_lbl, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(kb_hide_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(kb_hide_lbl);

    g_kb_show_btn = lv_btn_create(g_search_panel);
    lv_obj_set_size(g_kb_show_btn, 42, 42);
    lv_obj_align(g_kb_show_btn, LV_ALIGN_TOP_RIGHT, 0, 20);
    lv_obj_set_style_bg_color(g_kb_show_btn, lv_color_hex(0x0F172A), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_kb_show_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_kb_show_btn, 21, LV_PART_MAIN);
    lv_obj_add_event_cb(g_kb_show_btn, on_keyboard_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_t *kb_show_lbl = lv_label_create(g_kb_show_btn);
    lv_label_set_text(kb_show_lbl, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_color(kb_show_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(kb_show_lbl);

    set_keyboard_visible(true);

    g_departures_panel = lv_obj_create(g_tile_times);
    lv_obj_set_size(g_departures_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(g_departures_panel, 8, 0);
    lv_obj_add_flag(g_departures_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(g_departures_panel, lv_color_hex(0x020617), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_departures_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_departures_panel, 0, LV_PART_MAIN);

    g_departures_title = lv_label_create(g_departures_panel);
    lv_label_set_text(g_departures_title, "Trams from");
    lv_obj_set_width(g_departures_title, LV_PCT(76));
    lv_label_set_long_mode(g_departures_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(g_departures_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_departures_title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(g_departures_title, &lv_font_montserrat_22, 0);
    lv_obj_align(g_departures_title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *change_btn = lv_btn_create(g_departures_panel);
    lv_obj_set_size(change_btn, 44, 44);
    lv_obj_align(change_btn, LV_ALIGN_TOP_LEFT, 0, 6);
    lv_obj_set_style_bg_color(change_btn, lv_color_hex(0x2563EB), LV_PART_MAIN);
    lv_obj_set_style_border_width(change_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(change_btn, 22, LV_PART_MAIN);
    lv_obj_add_event_cb(change_btn, on_change_station, LV_EVENT_CLICKED, NULL);
    lv_obj_t *change_lbl = lv_label_create(change_btn);
    lv_label_set_text(change_lbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(change_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(change_lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(change_lbl);

    g_fav_btn = lv_btn_create(g_departures_panel);
    lv_obj_set_size(g_fav_btn, 40, 40);
    lv_obj_align(g_fav_btn, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_bg_color(g_fav_btn, lv_color_hex(0x0B1220), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_fav_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_fav_btn, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_radius(g_fav_btn, 20, LV_PART_MAIN);
    lv_obj_add_event_cb(g_fav_btn, on_toggle_favorite, LV_EVENT_CLICKED, NULL);
    g_fav_lbl = lv_label_create(g_fav_btn);
    lv_label_set_text(g_fav_lbl, LV_SYMBOL_STAR);
    lv_obj_set_style_text_font(g_fav_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(g_fav_lbl);

    g_events_list = lv_list_create(g_departures_panel);
    lv_obj_set_size(g_events_list, LV_PCT(100), 200);
    lv_obj_align(g_events_list, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_obj_set_style_bg_color(g_events_list, lv_color_hex(0x020617), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_events_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_events_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(g_events_list, 10, LV_PART_MAIN);
    update_departures_layout();

    g_selected_station = NULL;
    g_selected_station_index = -1;
    update_favorite_button_state();
    refresh_search_results();
    show_search_panel();
    set_keyboard_visible(false);

    /* Start on clock tile. */
    lv_obj_set_tile(g_tileview, g_tile_map, LV_ANIM_OFF);
}
