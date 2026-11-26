/**
 * @file fuel_timer.h
 * @brief 加油计时逻辑
 *
 * 行为：预设倒计时 → 传感器闭合开始 → 传感器松开即停并显示本次用时
 *       最后 3 秒（3/2/1）各鸣叫一次，数字转红。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 启动计时任务 */
void fuel_timer_start(void);

/** 直接设置预设（毫秒，会被夹到 1~99s 并对齐到整秒） */
void fuel_timer_set_preset_ms(uint32_t ms);

/** 相对调整预设（±1000 / ±5000 之类） */
void fuel_timer_adjust_ms(int32_t delta_ms);

/** 读取当前预设（毫秒） */
uint32_t fuel_timer_get_preset_ms(void);

/** 立即回到待触发并停止鸣叫 */
void fuel_timer_reset(void);

#ifdef __cplusplus
}
#endif