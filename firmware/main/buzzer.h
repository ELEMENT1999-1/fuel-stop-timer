/**
 * @file buzzer.h
 * @brief 板载 ES8311 + 扬声器 的报警音（最响音量，短促鸣叫）
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 I2S + ES8311 并启动发声任务 */
void buzzer_init(void);

/** 鸣叫 ms 毫秒（非阻塞） */
void buzzer_beep(uint32_t ms);

/**
 * @brief 功放预热：true 时提前打开功放并解除静音（不出声），
 *        这样后续 130~250ms 的短音不会被功放唤醒延迟吞掉。
 */
void buzzer_arm(bool on);

/** 当前是否正在发声（给界面指示用） */
bool buzzer_is_active(void);

/** 立即停止 */
void buzzer_stop(void);

#ifdef __cplusplus
}
#endif