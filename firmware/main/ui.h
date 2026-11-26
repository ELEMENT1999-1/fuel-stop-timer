/**
 * @file ui.h
 * @brief 加油计时器界面接口（800x480, LVGL 9）
 *
 * 所有 ui_* 函数都会操作 LVGL，调用前必须已经 lvgl_port_lock()。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ui_set_state() 的状态值 */
#define UI_ARMED    0
#define UI_RUNNING  1
#define UI_DONE     2

/** 创建界面 */
void ui_create(void);

/** 预设变化：刷新 TARGET 与快捷芯片高亮 */
void ui_set_preset_ms(uint32_t ms);

/** 运行中：显示剩余时间；ms_left < 0 表示超时，显示负数 */
void ui_set_countdown_ms(int32_t ms_left);

/** BEEP 指示灯（调试/提示用） */
void ui_set_beep(bool on);

/** 结束：大数字显示本次用时（向下取整到 0.1s） */
void ui_set_result_ms(uint32_t elapsed_ms);

/** 状态切换（UI_ARMED / UI_RUNNING / UI_DONE） */
void ui_set_state(int state);

/** 传感器实时电平 */
void ui_set_sensor(bool active);

/** 最后 3 秒：数字转红（不做整屏闪烁） */
void ui_set_warn(bool on);

/** 上一次成绩 */
void ui_set_last_ms(uint32_t ms, bool valid);

/** 原始电平指示（调试用，1=高 0=低） */
void ui_set_raw(int di0, int di1, uint32_t edges);

#ifdef __cplusplus
}
#endif