/**
 * @file seg7.h
 * @brief 七段数码管风格数字控件（用 LVGL 矩形拼出来，任意尺寸都锐利，无需外挂字体）
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建一个数字位（0-9）
 * @param parent 父对象
 * @param w      数字宽度(px)
 * @param h      数字高度(px)
 * @param thick  笔画粗细(px)
 */
lv_obj_t *seg7_create(lv_obj_t *parent, int32_t w, int32_t h, int32_t thick);

/** 设置显示的数字（0-9，其它值显示为空白） */
void seg7_set_value(lv_obj_t *digit, uint8_t value);

/**
 * @brief 设置亮/灭颜色
 * @param on  点亮笔画颜色
 * @param off 熄灭笔画颜色（一般是 on 压暗的版本）
 */
void seg7_set_colors(lv_obj_t *digit, lv_color_t on, lv_color_t off);

#ifdef __cplusplus
}
#endif
