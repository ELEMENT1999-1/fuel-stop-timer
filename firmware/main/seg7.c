/**
 * @file seg7.c
 * @brief 七段数码管控件实现
 *
 * 段位排布：
 *        a
 *      f   b
 *        g
 *      e   c
 *        d
 * 每个段是带圆角的矩形，重叠处自然融合，视觉上接近真实数码管。
 */

#include "seg7.h"
#include <stdlib.h>

#define SEG_A 0
#define SEG_B 1
#define SEG_C 2
#define SEG_D 3
#define SEG_E 4
#define SEG_F 5
#define SEG_G 6

typedef struct {
    lv_obj_t *seg[7];
    lv_color_t on;
    lv_color_t off;
    int8_t value; /* -1 表示尚未设置 */
} seg7_t;

/* bit0=a bit1=b bit2=c bit3=d bit4=e bit5=f bit6=g */
static const uint8_t k_seg_map[10] = {
    0x3F, /* 0: a b c d e f     */
    0x06, /* 1: b c             */
    0x5B, /* 2: a b g e d       */
    0x4F, /* 3: a b g c d       */
    0x66, /* 4: f g b c         */
    0x6D, /* 5: a f g c d       */
    0x7D, /* 6: a f g e c d     */
    0x07, /* 7: a b c           */
    0x7F, /* 8: 全亮            */
    0x6F, /* 9: a b c d f g     */
};

static void seg_box(lv_obj_t *parent, lv_obj_t *seg, int32_t x, int32_t y,
                    int32_t w, int32_t h, int32_t radius)
{
    lv_obj_remove_style_all(seg);
    lv_obj_set_parent(seg, parent);
    lv_obj_set_pos(seg, x, y);
    lv_obj_set_size(seg, w, h);
    lv_obj_set_style_radius(seg, radius, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(seg, lv_color_black(), 0);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *seg7_create(lv_obj_t *parent, int32_t w, int32_t h, int32_t thick)
{
    seg7_t *d = (seg7_t *)calloc(1, sizeof(seg7_t));
    if (!d) {
        return NULL;
    }
    d->value = -1;
    d->on = lv_color_white();
    d->off = lv_color_make(0x1A, 0x20, 0x28);

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);

    const int32_t t = thick;
    const int32_t r = thick / 2;

    /* 横向笔画：宽度略收，留出转角缝 */
    const int32_t hw = w - (t * 6) / 5;
    const int32_t hx = (t * 3) / 5;
    /* 纵向笔画长度 */
    const int32_t vh = (h - (t * 6) / 5) / 2 - (t * 3) / 5;
    const int32_t vy_top = (t * 3) / 5;
    const int32_t vy_bot = h / 2 + t / 5;

    for (int i = 0; i < 7; i++) {
        d->seg[i] = lv_obj_create(root);
    }

    seg_box(root, d->seg[SEG_A], hx, 0, hw, t, r);
    seg_box(root, d->seg[SEG_G], hx, (h - t) / 2, hw, t, r);
    seg_box(root, d->seg[SEG_D], hx, h - t, hw, t, r);
    seg_box(root, d->seg[SEG_F], 0, vy_top, t, vh, r);
    seg_box(root, d->seg[SEG_B], w - t, vy_top, t, vh, r);
    seg_box(root, d->seg[SEG_E], 0, vy_bot, t, vh, r);
    seg_box(root, d->seg[SEG_C], w - t, vy_bot, t, vh, r);

    lv_obj_set_user_data(root, d);

    for (int i = 0; i < 7; i++) {
        lv_obj_set_style_bg_color(d->seg[i], d->off, 0);
    }

    return root;
}

void seg7_set_value(lv_obj_t *digit, uint8_t value)
{
    if (!digit) {
        return;
    }
    seg7_t *d = (seg7_t *)lv_obj_get_user_data(digit);
    if (!d) {
        return;
    }
    if (value > 9) {
        value = 10; /* 空白 */
    }
    if (d->value == (int8_t)value) {
        return;
    }
    d->value = (int8_t)value;

    uint8_t mask = (value <= 9) ? k_seg_map[value] : 0x00;
    for (int i = 0; i < 7; i++) {
        bool on = (mask >> i) & 0x01;
        lv_obj_set_style_bg_color(d->seg[i], on ? d->on : d->off, 0);
    }
}

void seg7_set_colors(lv_obj_t *digit, lv_color_t on, lv_color_t off)
{
    if (!digit) {
        return;
    }
    seg7_t *d = (seg7_t *)lv_obj_get_user_data(digit);
    if (!d) {
        return;
    }
    d->on = on;
    d->off = off;
    /* 强制重刷 */
    int8_t v = d->value;
    d->value = -1;
    seg7_set_value(digit, (v < 0) ? 10 : (uint8_t)v);
}
