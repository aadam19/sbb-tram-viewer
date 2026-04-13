
#include <Arduino.h>
#include <lvgl.h>
#include <WiFi.h>
#include <time.h>
#include "display.h"
#include "esp_bsp.h"
#include "lv_port.h"

/**
 * Set the rotation degree:
 *      - 0: 0 degree
 *      - 90: 90 degree
 *      - 180: 180 degree
 *      - 270: 270 degree
 *
 */
#define LVGL_PORT_ROTATION_DEGREE               (90)

/**
/* To use the built-in examples and demos of LVGL uncomment the includes below respectively.
 * You also need to copy `lvgl/examples` to `lvgl/src/examples`. Similarly for the demos `lvgl/demos` to `lvgl/src/demos`.
 */
#include "ui.h"
extern "C" const lv_img_dsc_t logo_img;

static lv_obj_t *g_boot_splash = NULL;

static void boot_splash_done_cb(lv_timer_t *timer)
{
    if (timer != NULL) {
        lv_timer_del(timer);
    }
    if (g_boot_splash != NULL) {
        lv_obj_del(g_boot_splash);
        g_boot_splash = NULL;
    }
    ui_create();
}

static void show_boot_splash(void)
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x020617), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lv_scr_act(), 0, 0);
    lv_obj_set_style_radius(lv_scr_act(), 0, 0);
    lv_obj_set_style_shadow_width(lv_scr_act(), 0, 0);
    lv_obj_set_style_outline_width(lv_scr_act(), 0, 0);

    g_boot_splash = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_boot_splash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_boot_splash, lv_color_hex(0x020617), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_boot_splash, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_boot_splash, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_boot_splash, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(g_boot_splash, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(g_boot_splash, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_boot_splash, 0, LV_PART_MAIN);
    lv_obj_clear_flag(g_boot_splash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *logo = lv_img_create(g_boot_splash);
    lv_img_set_src(logo, &logo_img);
    lv_obj_set_style_img_recolor_opa(logo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_img_opa(logo, LV_OPA_COVER, 0);

    int32_t disp_w = lv_obj_get_width(g_boot_splash);
    int32_t disp_h = lv_obj_get_height(g_boot_splash);
    int32_t max_w = (disp_w * 100) / 100;
    int32_t max_h = (disp_h * 74) / 100;
    uint32_t zoom_w = (uint32_t)((max_w * 256) / (int32_t)logo_img.header.w);
    uint32_t zoom_h = (uint32_t)((max_h * 256) / (int32_t)logo_img.header.h);
    uint32_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    if (zoom > 256U) zoom = 256U;
    if (zoom < 64U) zoom = 64U;
    lv_img_set_zoom(logo, (uint16_t)zoom);
    lv_obj_center(logo);

    lv_timer_create(boot_splash_done_cb, 2000, NULL);
}

void setup()
{
    Serial.begin(115200);

    /* Try reconnecting automatically to previously saved Wi-Fi credentials. */
    WiFi.mode(WIFI_STA);
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);
    WiFi.begin();
    configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.google.com");

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
#if LVGL_PORT_ROTATION_DEGREE == 90
        .rotate = LV_DISP_ROT_90,
#elif LVGL_PORT_ROTATION_DEGREE == 270
        .rotate = LV_DISP_ROT_270,
#elif LVGL_PORT_ROTATION_DEGREE == 180
        .rotate = LV_DISP_ROT_180,
#elif LVGL_PORT_ROTATION_DEGREE == 0
        .rotate = LV_DISP_ROT_NONE,
#endif
    };

    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    /* Lock the mutex due to the LVGL APIs are not thread-safe */
    bsp_display_lock(0);

    /**
     * Try an example. Don't forget to uncomment header.
     * See all the examples online: https://docs.lvgl.io/master/examples.html
     * source codes: https://github.com/lvgl/lvgl/tree/e7f88efa5853128bf871dde335c0ca8da9eb7731/examples
     */
    //  lv_example_btn_1();

    /**
     * Or try out a demo.
     * Don't forget to uncomment header and enable the demos in `lv_conf.h`. E.g. `LV_USE_DEMOS_WIDGETS`
     */
     show_boot_splash();
//     lv_demo_benchmark();
    // lv_demo_music();
    //lv_demo_stress();

    /* Release the mutex */
    bsp_display_unlock();
}

void loop()
{
    delay(1000);
}
