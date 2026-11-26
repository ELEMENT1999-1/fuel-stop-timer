/**
 * @file fuel_timer.c
 * @brief 加油计时状态机（消抖版：确认时间不产生计时误差）
 *
 * 传感器：板载隔离输入 DI0，棕→5V，蓝→GND，黑→DI0，DI COM→GND
 *
 * 消抖策略（关键）：
 *   不用"数采样次数"，而是记录**原始电平第一次变化的时间戳**，
 *   要求该状态稳定保持 PRESS_CONFIRM_MS / RELEASE_CONFIRM_MS 才认账，
 *   但计时的起点/终点直接用那个**时间戳**——
 *   所以确认时间只影响"判定"，不会让计时少算或多算。
 *
 *   ARMED ──确认闭合(50ms)──> RUNNING ──确认松开(150ms)──> DONE（显示本次用时）
 *                                    └──倒计时归零──> DONE（满时长）
 */

#include "fuel_timer.h"
#include "ui.h"
#include "buzzer.h"
#include "io_extension.h"
#include "lvgl_port.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "fuel_timer";

/* ---------- 可调参数 ---------- */
#define SENSOR_ACTIVE_LEVEL   0        /* NPN: 触发时输入被拉低 */

/* 本板实测：DI1 通道正常，DI0 通道按压无响应（硬件问题），所以用 DI1。
 * 如果以后修好 DI0，把 SENSOR_USE_DI0 改成 1，两路都接也能用（OR）。 */
#define SENSOR_USE_DI0        0
#define SENSOR_USE_DI1        1

#define TASK_TICK_MS          10
#define UI_REFRESH_EVERY      5

#define PRESS_CONFIRM_MS      50       /* 稳定闭合这么久才算"按下"（滤抖动） */
#define RELEASE_CONFIRM_MS    150      /* 稳定断开这么久才算"松手"（滤回弹/抖动） */
#define BOUNCE_WINDOW_MS      400      /* 结束后这段时间内的抖动一律无视 */

#define WARN_MS               3000
#define AMP_WARMUP_MS         5000     /* 最后 5 秒提前打开功放，保证 3/2/1 即时发声 */
#define BEEP_LEN_MS           250      /* 每声长度，太短会被功放唤醒延迟吞掉 */
#define AUTO_REARM_MS         5000
#define PRESET_DEFAULT_MS     20000
#define PRESET_MIN_MS         1000
#define PRESET_MAX_MS         99000
#define PRESET_STEP_MS        1000

/* ---------- 状态 ---------- */
static volatile uint32_t s_preset_ms = PRESET_DEFAULT_MS;
static volatile int32_t  s_adjust_ms = 0;
static volatile bool     s_reset_req = false;
static volatile bool     s_new_preset_req = false;
static volatile uint32_t s_new_preset_ms = PRESET_DEFAULT_MS;

static int      s_state = UI_ARMED;
static bool     s_pressed = false;      /* 确认后的状态（给 UI / 状态机用） */
static bool     s_armed_ready = false;  /* 必须先看到一次确认松开 */
static bool     s_raw_prev = false;
static uint32_t s_edges = 0;   /* 原始电平变化次数，调试用 */
static int64_t  t_active_first = 0;     /* 原始电平变低的时间戳 */
static int64_t  t_inactive_first = 0;   /* 原始电平变高的时间戳 */
static int      s_last_sec = 99;
static bool     s_past_zero = false;  /* 是否已越过 0（超时） */
static uint32_t s_result_ms = 0;

static bool read_sensor_raw(void)
{
    bool a = SENSOR_USE_DI0 && (IO_EXTENSION_Input(DI0) == SENSOR_ACTIVE_LEVEL);
    bool b = SENSOR_USE_DI1 && (IO_EXTENSION_Input(DI1) == SENSOR_ACTIVE_LEVEL);
    return a || b;
}

static uint32_t clamp_preset(uint32_t ms)
{
    if (ms < PRESET_MIN_MS) {
        ms = PRESET_MIN_MS;
    }
    if (ms > PRESET_MAX_MS) {
        ms = PRESET_MAX_MS;
    }
    ms = (ms / PRESET_STEP_MS) * PRESET_STEP_MS;
    if (ms < PRESET_MIN_MS) {
        ms = PRESET_MIN_MS;
    }
    return ms;
}

static void apply_preset(uint32_t ms)
{
    s_preset_ms = clamp_preset(ms);
    if (lvgl_port_lock(100)) {
        ui_set_preset_ms(s_preset_ms);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "preset = %u ms", (unsigned)s_preset_ms);
}

void fuel_timer_set_preset_ms(uint32_t ms)
{
    s_new_preset_ms = ms;
    s_new_preset_req = true;
}

void fuel_timer_adjust_ms(int32_t delta_ms)
{
    s_adjust_ms += delta_ms;
}

uint32_t fuel_timer_get_preset_ms(void)
{
    return s_preset_ms;
}

void fuel_timer_reset(void)
{
    s_reset_req = true;
}

/* ---------- 辅助 ---------- */

static void set_state(int st)
{
    if (s_state == st) {
        return;
    }
    s_state = st;
    if (lvgl_port_lock(100)) {
        ui_set_state(st);
        lvgl_port_unlock();
    }
}

static void finish(int64_t elapsed_ms)
{
    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    }
    if (elapsed_ms > 999990) {          /* 显示上限 99.9s，避免溢出 */
        elapsed_ms = 999990;
    }
    s_result_ms = (uint32_t)elapsed_ms;

    buzzer_stop();
    buzzer_arm(false);
    if (lvgl_port_lock(100)) {
        ui_set_warn(false);
        ui_set_result_ms(s_result_ms);
        ui_set_last_ms(s_result_ms, true);
        lvgl_port_unlock();
    }
    set_state(UI_DONE);
    ESP_LOGI(TAG, "DONE, elapsed = %u ms", (unsigned)s_result_ms);
}

/* ---------- 主循环 ---------- */

static void timer_task(void *arg)
{
    (void)arg;

    int tick = 0;
    int64_t t0 = 0;
    int64_t done_at = 0;
    int64_t lockout_until = 0;
    int64_t last_raw_change = 0;

    if (lvgl_port_lock(1000)) {
        ui_set_preset_ms(s_preset_ms);
        ui_set_state(UI_ARMED);
        ui_set_last_ms(0, false);
        lvgl_port_unlock();
    }

    while (1) {
        int64_t now = esp_timer_get_time();

        /* --- 外部请求 --- */
        if (s_new_preset_req) {
            s_new_preset_req = false;
            if (s_state != UI_RUNNING) {
                apply_preset(s_new_preset_ms);
            }
        }
        if (s_adjust_ms != 0) {
            int32_t d = s_adjust_ms;
            s_adjust_ms = 0;
            if (s_state != UI_RUNNING) {
                apply_preset((uint32_t)((int32_t)s_preset_ms + d));
            }
        }
        if (s_reset_req) {
            s_reset_req = false;
            buzzer_stop();
    buzzer_arm(false);
            if (lvgl_port_lock(100)) {
                ui_set_warn(false);
                lvgl_port_unlock();
            }
            set_state(UI_ARMED);
            s_armed_ready = false;
        }

        /* --- 原始电平 + 边沿时间戳 --- */
        bool raw = read_sensor_raw();
        if (raw != s_raw_prev) {
            if (raw) {
                t_active_first = now;
            } else {
                t_inactive_first = now;
            }
            s_raw_prev = raw;
            s_edges++;
            ESP_LOGI(TAG, "raw %d (+%lld ms)", raw, (long long)((now - last_raw_change) / 1000));
            last_raw_change = now;
        }

        bool press_confirmed = raw && ((now - t_active_first) >= (int64_t)PRESS_CONFIRM_MS * 1000);
        bool release_confirmed = !raw && ((now - t_inactive_first) >= (int64_t)RELEASE_CONFIRM_MS * 1000);

        /* 结束后的一小段窗口内，任何电平变化都不认（滤机械回弹） */
        if (now < lockout_until) {
            press_confirmed = false;
        }
        if (release_confirmed) {
            s_armed_ready = true;
        }

        if (press_confirmed != s_pressed) {
            s_pressed = press_confirmed;
            if (lvgl_port_lock(100)) {
                ui_set_sensor(s_pressed);
                lvgl_port_unlock();
            }
            ESP_LOGI(TAG, "sensor %s", s_pressed ? "CLOSED" : "OPEN");
        }

        /* --- 状态机 --- */
        switch (s_state) {
        case UI_ARMED:
            if (s_pressed && s_armed_ready) {
                t0 = t_active_first;   /* 用原始边沿时间，消抖不引入误差 */
                s_last_sec = 99;
                s_past_zero = false;
                set_state(UI_RUNNING);
                if (lvgl_port_lock(100)) {
                    ui_set_countdown_ms((int32_t)s_preset_ms);
                    ui_set_beep(false);
                    lvgl_port_unlock();
                }
                ESP_LOGI(TAG, "START preset=%u (edge based)", (unsigned)s_preset_ms);
            }
            break;

        case UI_RUNNING: {
            int64_t elapsed_ms = (now - t0) / 1000;
            int64_t rem = (int64_t)s_preset_ms - elapsed_ms;

            /* 最后 3 秒：剩余整秒变化时各响一声（3/2/1），负数不会误触发 */
            int sec_left = (int)((rem + 999) / 1000);
            if (sec_left != s_last_sec) {
                s_last_sec = sec_left;
                if (sec_left >= 1 && sec_left <= 3) {
                    buzzer_beep(BEEP_LEN_MS);
                    ESP_LOGI(TAG, "BEEP at %d s left (rem=%lld ms)", sec_left, (long long)rem);
                }
            }
            buzzer_arm(rem <= AMP_WARMUP_MS);
            if (rem < 0 && !s_past_zero) {
                s_past_zero = true;
                ESP_LOGI(TAG, "OVERRUN: past zero, still counting up to total");
            }
            if (rem <= WARN_MS) {
                if (lvgl_port_lock(100)) {
                    ui_set_warn(true);
                    lvgl_port_unlock();
                }
            }

            /* 确认松开 = 结束；终点用原始松开边沿，避免回弹造成误差 */
            if (release_confirmed) {
                finish((t_inactive_first - t0) / 1000);
                done_at = now;
                lockout_until = now + (int64_t)BOUNCE_WINDOW_MS * 1000;
                ESP_LOGI(TAG, "STOP by release");
                break;
            }


            if ((tick % UI_REFRESH_EVERY) == 0) {
                if (lvgl_port_lock(100)) {
                    ui_set_countdown_ms((int32_t)rem);
                    ui_set_beep(buzzer_is_active());
                    lvgl_port_unlock();
                }
            }
            break;
        }

        case UI_DONE:
            if (!s_pressed && (now - done_at) / 1000 >= AUTO_REARM_MS) {
                set_state(UI_ARMED);
            }
            break;

        default:
            break;
        }

        if ((tick % 500) == 0) {
            ESP_LOGI(TAG, "DI0=%d DI1=%d pressed=%d state=%d",
                     IO_EXTENSION_Input(DI0), IO_EXTENSION_Input(DI1),
                     (int)s_pressed, s_state);
        }

        /* 每 100ms 把原始电平刷到屏幕右下角，方便现场判断板子到底收到没有 */
        if ((tick % 10) == 0) {
            int d0 = IO_EXTENSION_Input(DI0);
            int d1 = IO_EXTENSION_Input(DI1);
            if (lvgl_port_lock(50)) {
                ui_set_raw(d0, d1, s_edges);
                lvgl_port_unlock();
            }
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(TASK_TICK_MS));
    }
}

void fuel_timer_start(void)
{
    xTaskCreatePinnedToCore(timer_task, "fuel_timer", 4096, NULL, 6, NULL, 0);
}