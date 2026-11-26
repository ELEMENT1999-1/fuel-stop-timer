/**
 * @file ui.c
 * @brief 加油计时器界面：三位七段数码管（含 0.1s）+ 环形进度 + 预设/微调
 *
 *   FUEL STOP                    ● SENSOR CLOSED     LAST 18.4s
 *   PIT LANE COUNTDOWN
 *        ███  ███  .  █   s        ╭─────╮
 *                                 │ RUN │
 *   TARGET 20.0s                 ╰─────╯
 *   - / + 1s (hold 5s)
 *   [10][15][20][25][30][45]  [−] [+]  [ RE-ARM ]
 */

#include "ui.h"
#include "seg7.h"
#include "fuel_timer.h"
#include "lvgl.h"

#include <stdio.h>

#define COL_BG      lv_color_hex(0x0A0D12)
#define COL_PANEL   lv_color_hex(0x141A22)
#define COL_PANEL2  lv_color_hex(0x1B232E)
#define COL_LINE    lv_color_hex(0x1F2833)
#define COL_DIM     lv_color_hex(0x66717E)
#define COL_TEXT    lv_color_hex(0xE9EFF6)
#define COL_ACCENT  lv_color_hex(0x22D3EE)
#define COL_WARN    lv_color_hex(0xFF3B30)
#define COL_ORANGE  lv_color_hex(0xFF7A1A)
#define COL_GREEN   lv_color_hex(0x2BD97A)

static const uint32_t k_presets_ms[] = { 10000, 15000, 20000, 25000, 30000, 45000 };
#define PRESET_CNT ((int)(sizeof(k_presets_ms) / sizeof(k_presets_ms[0])))

static lv_obj_t *s_d[3];          /* 0=十位 1=个位 2=十分位 */
static lv_obj_t *s_dot;
static lv_obj_t *s_arc;
static lv_obj_t *s_lbl_arc;
static lv_obj_t *s_lbl_last;
static lv_obj_t *s_lbl_sensor;
static lv_obj_t *s_dot_sensor;
static lv_obj_t *s_lbl_target;
static lv_obj_t *s_lbl_raw;
static lv_obj_t *s_minus;      /* 超时负号 */
static lv_obj_t *s_lbl_beep;   /* 发声指示 */
static lv_obj_t *s_chip[PRESET_CNT];
static lv_obj_t *s_chip_lbl[PRESET_CNT];

static int      s_state = UI_ARMED;
static int      s_sensor_on = -1;
static int      s_arc_val = -1;
static int      s_digit_val[3] = { -1, -1, -1 };
static int      s_active_chip = -2;
static uint32_t s_preset_ms = 20000;
static uint32_t s_result_ms = 0;
static bool     s_warn = false;

/* ---------- 小工具 ---------- */

static lv_obj_t *box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                     lv_color_t bg, lv_opa_t opa, int32_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, int32_t x, int32_t y,
                       const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_color_t dim_to(lv_color_t c, lv_color_t bg, uint8_t weight)
{
    return lv_color_mix(c, bg, weight);
}

/* ---------- 数字渲染 ---------- */

static lv_color_t digit_on_color(void)
{
    if (s_warn) {
        return COL_WARN;
    }
    switch (s_state) {
    case UI_RUNNING: return COL_ACCENT;
    case UI_DONE:    return COL_GREEN;
    default:         return COL_TEXT;
    }
}

static void apply_digit_colors(void)
{
    lv_color_t on = digit_on_color();
    lv_color_t off = dim_to(on, COL_BG, 26);
    for (int i = 0; i < 3; i++) {
        seg7_set_colors(s_d[i], on, off);
    }
    lv_obj_set_style_bg_color(s_dot, on, 0);
    lv_obj_set_style_bg_color(s_minus, on, 0);
}

static void set_digit(int idx, int v)
{
    if (s_digit_val[idx] == v) {
        return;
    }
    s_digit_val[idx] = v;
    seg7_set_value(s_d[idx], (uint8_t)v);
}

/* ms → 三位数字；ceil_mode=true 用于倒计时（避免提前显示 0.0） */
static void render_ms(uint32_t ms, bool ceil_mode)
{
    uint32_t v10 = ceil_mode ? (ms + 99) / 100 : ms / 100;
    if (v10 > 999) {
        v10 = 999;
    }
    uint32_t secs = v10 / 10;
    uint32_t tenth = v10 % 10;
    if (secs > 99) {
        secs = 99;
    }
    int tens = (int)(secs / 10);
    int ones = (int)(secs % 10);

    set_digit(0, (tens == 0) ? 10 : tens);  /* 10 = 留空 */
    set_digit(1, ones);
    set_digit(2, (int)tenth);
    lv_obj_set_style_bg_opa(s_minus, LV_OPA_TRANSP, 0);   /* 非负状态收起负号 */
}

/* 有符号：负数表示超时，左侧点亮负号 */
static void render_signed_ms(int32_t ms_left)
{
    bool neg = (ms_left < 0);
    uint32_t a = neg ? (uint32_t)(-(int64_t)ms_left) : (uint32_t)ms_left;
    uint32_t v10 = (a + 99) / 100;
    if (v10 > 999) {
        v10 = 999;
    }
    uint32_t secs = v10 / 10;
    uint32_t tenth = v10 % 10;
    if (secs > 99) {
        secs = 99;
    }
    int tens = (int)(secs / 10);
    int ones = (int)(secs % 10);

    set_digit(0, (tens == 0) ? 10 : tens);
    set_digit(1, ones);
    set_digit(2, (int)tenth);
    lv_obj_set_style_bg_opa(s_minus, neg ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

/* ---------- 交互回调 ---------- */

static void chip_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    fuel_timer_set_preset_ms(k_presets_ms[idx]);
}

static void minus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        fuel_timer_adjust_ms(-5000);
    } else {
        fuel_timer_adjust_ms(-1000);
    }
}

static void plus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        fuel_timer_adjust_ms(5000);
    } else {
        fuel_timer_adjust_ms(1000);
    }
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    fuel_timer_reset();
}

/* ---------- 创建 ---------- */

static void style_chip(int idx, bool active)
{
    lv_obj_t *c = s_chip[idx];
    lv_obj_t *l = s_chip_lbl[idx];
    if (active) {
        lv_obj_set_style_bg_color(c, COL_ORANGE, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_text_color(l, COL_BG, 0);
    } else {
        lv_obj_set_style_bg_color(c, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, COL_LINE, 0);
        lv_obj_set_style_text_color(l, COL_DIM, 0);
    }
}

void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 顶部 ---- */
    label(scr, "FUEL STOP", 28, 12, &lv_font_montserrat_28, COL_TEXT);
    label(scr, "PIT LANE COUNTDOWN", 30, 50, &lv_font_montserrat_14, COL_DIM);

    lv_obj_t *pill = box(scr, 508, 16, 264, 42, COL_PANEL, LV_OPA_COVER, 21);
    s_dot_sensor = box(pill, 16, 14, 14, 14, COL_DIM, LV_OPA_COVER, 7);
    s_lbl_sensor = label(pill, "SENSOR  OPEN", 40, 11, &lv_font_montserrat_16, COL_DIM);

    s_lbl_last = label(scr, "LAST  --", 508, 66, &lv_font_montserrat_14, COL_DIM);

    box(scr, 0, 94, 800, 1, COL_LINE, LV_OPA_COVER, 0);

    /* ---- 三位数码管 ---- */
    s_d[0] = seg7_create(scr, 150, 250, 34);
    s_d[1] = seg7_create(scr, 150, 250, 34);
    s_d[2] = seg7_create(scr, 96, 160, 24);
    lv_obj_set_pos(s_d[0], 44, 128);
    lv_obj_set_pos(s_d[1], 206, 128);
    lv_obj_set_pos(s_d[2], 398, 218);

    s_dot = box(scr, 366, 336, 20, 20, COL_TEXT, LV_OPA_COVER, 10);

    label(scr, "s", 516, 296, &lv_font_montserrat_48, COL_DIM);

    s_lbl_target = label(scr, "TARGET 20.0s", 46, 384, &lv_font_montserrat_14, COL_DIM);

    /* ---- 环形进度 ---- */
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 168, 168);
    lv_obj_set_pos(s_arc, 604, 142);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_arc_set_rotation(s_arc, 0);
    lv_arc_set_bg_angles(s_arc, 135, 405);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 100);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, COL_PANEL2, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_MAIN);

    s_lbl_arc = lv_label_create(s_arc);
    lv_label_set_text(s_lbl_arc, "ARM");
    lv_obj_set_style_text_font(s_lbl_arc, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_arc, COL_DIM, 0);
    lv_obj_align(s_lbl_arc, LV_ALIGN_CENTER, 0, 0);

    label(scr, "- / + 1s (hold 5s)", 604, 318, &lv_font_montserrat_14, dim_to(COL_DIM, COL_BG, 140));

    /* ---- 快捷预设 ---- */
    const int cw = 78, ch = 52, gap = 6, cy = 392, cx0 = 24;
    for (int i = 0; i < PRESET_CNT; i++) {
        int x = cx0 + i * (cw + gap);
        s_chip[i] = box(scr, x, cy, cw, ch, COL_PANEL, LV_OPA_COVER, 12);
        lv_obj_add_flag(s_chip[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_chip[i], chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)(k_presets_ms[i] / 1000));
        s_chip_lbl[i] = label(s_chip[i], buf, 0, 0, &lv_font_montserrat_20, COL_DIM);
        lv_obj_align(s_chip_lbl[i], LV_ALIGN_CENTER, 0, 0);
    }

    /* ---- 微调 ±1s ---- */
    lv_obj_t *mn = box(scr, 534, cy, 56, ch, COL_PANEL, LV_OPA_COVER, 12);
    lv_obj_add_flag(mn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mn, minus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(mn, minus_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *mnl = label(mn, "-", 0, 0, &lv_font_montserrat_28, COL_TEXT);
    lv_obj_align(mnl, LV_ALIGN_CENTER, 0, -2);

    lv_obj_t *pl = box(scr, 596, cy, 56, ch, COL_PANEL, LV_OPA_COVER, 12);
    lv_obj_add_flag(pl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pl, plus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(pl, plus_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *pll = label(pl, "+", 0, 0, &lv_font_montserrat_28, COL_TEXT);
    lv_obj_align(pll, LV_ALIGN_CENTER, 0, -2);

    /* ---- RE-ARM ---- */
    lv_obj_t *rst = box(scr, 664, cy, 108, ch, COL_PANEL, LV_OPA_COVER, 12);
    lv_obj_add_flag(rst, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rst, reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_border_width(rst, 1, 0);
    lv_obj_set_style_border_color(rst, COL_LINE, 0);
    lv_obj_t *rl = label(rst, "RE-ARM", 0, 0, &lv_font_montserrat_16, COL_DIM);
    lv_obj_align(rl, LV_ALIGN_CENTER, 0, 0);

    label(scr, "TAP PRESET • CLOSE = START • RELEASE = STOP • BEEP 3/2/1s",
          24, 458, &lv_font_montserrat_14, dim_to(COL_DIM, COL_BG, 130));
    s_lbl_raw = label(scr, "DI 1/1 #0", 700, 458, &lv_font_montserrat_14, dim_to(COL_DIM, COL_BG, 160));

    /* 超时负号：只在倒计时为负时点亮 */
    s_minus = box(scr, 6, 246, 32, 10, COL_TEXT, LV_OPA_TRANSP, 5);

    /* 发声指示：鸣叫时变橙色 */
    s_lbl_beep = label(scr, "BEEP", 452, 30, &lv_font_montserrat_14, dim_to(COL_DIM, COL_BG, 130));

    apply_digit_colors();
    render_ms(s_preset_ms, false);
    ui_set_preset_ms(fuel_timer_get_preset_ms());
}

/* ---------- 更新接口 ---------- */

void ui_set_preset_ms(uint32_t ms)
{
    s_preset_ms = ms;

    unsigned tenth = (ms % 1000) / 100;
    char buf[28];
    snprintf(buf, sizeof(buf), "TARGET %u.%us", (unsigned)(ms / 1000), tenth);
    lv_label_set_text(s_lbl_target, buf);

    int idx = -1;
    for (int i = 0; i < PRESET_CNT; i++) {
        if (k_presets_ms[i] == ms) {
            idx = i;
            break;
        }
    }
    if (idx != s_active_chip) {
        s_active_chip = idx;
        for (int i = 0; i < PRESET_CNT; i++) {
            style_chip(i, i == idx);
        }
    }
    if (s_state == UI_ARMED) {
        render_ms(ms, false);
        s_arc_val = -1;
        lv_arc_set_value(s_arc, 100);
    }
}

void ui_set_countdown_ms(int32_t ms_left)
{
    render_signed_ms(ms_left);

    int pct = 0;
    if (s_preset_ms > 0 && ms_left > 0) {
        if ((uint32_t)ms_left >= s_preset_ms) {
            pct = 100;
        } else {
            pct = (int)(((int64_t)ms_left * 100) / (int64_t)s_preset_ms);
        }
    }
    if (pct != s_arc_val) {
        s_arc_val = pct;
        lv_arc_set_value(s_arc, pct);
    }
}

void ui_set_beep(bool on)
{
    static int last = -1;
    int v = on ? 1 : 0;
    if (v == last) {
        return;
    }
    last = v;
    lv_obj_set_style_text_color(s_lbl_beep, v ? COL_ORANGE : dim_to(COL_DIM, COL_BG, 130), 0);
}

void ui_set_result_ms(uint32_t elapsed_ms)
{
    lv_obj_set_style_bg_opa(s_minus, LV_OPA_TRANSP, 0);
    s_result_ms = elapsed_ms;
    render_ms(elapsed_ms, false);
    s_arc_val = -1;
    lv_arc_set_value(s_arc, 0);
}

void ui_set_state(int state)
{
    int prev = s_state;
    s_state = state;
    if (prev == state) {
        return;
    }

    if (state == UI_RUNNING) {
        lv_label_set_text(s_lbl_arc, "RUN");
        lv_obj_set_style_text_color(s_lbl_arc, COL_ACCENT, 0);
        lv_obj_set_style_arc_color(s_arc, COL_ACCENT, LV_PART_INDICATOR);
    } else if (state == UI_DONE) {
        lv_label_set_text(s_lbl_arc, "DONE");
        lv_obj_set_style_text_color(s_lbl_arc, COL_GREEN, 0);
        lv_obj_set_style_arc_color(s_arc, COL_GREEN, LV_PART_INDICATOR);
        render_ms(s_result_ms, false);
    } else {
        lv_label_set_text(s_lbl_arc, "ARM");
        lv_obj_set_style_text_color(s_lbl_arc, COL_DIM, 0);
        lv_obj_set_style_arc_color(s_arc, COL_ACCENT, LV_PART_INDICATOR);
        render_ms(s_preset_ms, false);
        s_arc_val = -1;
        lv_arc_set_value(s_arc, 100);
    }
    apply_digit_colors();
}

void ui_set_sensor(bool active)
{
    int v = active ? 1 : 0;
    if (v == s_sensor_on) {
        return;
    }
    s_sensor_on = v;
    lv_obj_set_style_bg_color(s_dot_sensor, active ? COL_GREEN : COL_DIM, 0);
    lv_label_set_text(s_lbl_sensor, active ? "SENSOR  CLOSED" : "SENSOR  OPEN");
    lv_obj_set_style_text_color(s_lbl_sensor, active ? COL_GREEN : COL_DIM, 0);
}

void ui_set_warn(bool on)
{
    if (on == s_warn) {
        return;
    }
    s_warn = on;
    if (on) {
        lv_obj_set_style_arc_color(s_arc, COL_WARN, LV_PART_INDICATOR);
    } else if (s_state == UI_RUNNING) {
        lv_obj_set_style_arc_color(s_arc, COL_ACCENT, LV_PART_INDICATOR);
    }
    apply_digit_colors();
}

void ui_set_last_ms(uint32_t ms, bool valid)
{
    if (!valid) {
        lv_label_set_text(s_lbl_last, "LAST  --");
        return;
    }
    char buf[28];
    snprintf(buf, sizeof(buf), "LAST  %u.%us", (unsigned)(ms / 1000), (unsigned)((ms % 1000) / 100));
    lv_label_set_text(s_lbl_last, buf);
}


void ui_set_raw(int di0, int di1, uint32_t edges)
{
    static int last0 = -1, last1 = -1;
    if (di0 == last0 && di1 == last1) {
        return;
    }
    last0 = di0;
    last1 = di1;
    char buf[24];
    snprintf(buf, sizeof(buf), "DI %d/%d #%u", di0, di1, (unsigned)edges);
    lv_label_set_text(s_lbl_raw, buf);
    lv_obj_set_style_text_color(s_lbl_raw,
        (di0 == 0) ? COL_GREEN : dim_to(COL_DIM, COL_BG, 160), 0);
}
