/**
 * @file kk_font.h
 * @brief KK_OLED 使用的字模(u8g2 格式)
 *
 * @note 由 LEDFont 在线 API 按"界面实际出现的字符"按需生成, 体积最小。
 *       生成信息:
 *         字体   WenQuanDengKuanWeiMiHei(等宽, 数字跳动时不会左右位移)
 *         大字形 24px, "0123456789.-+": 三相数值页焦点值 / 单路 Vpp 等
 *         小字形 12px, 全 ASCII 可见字符: 状态行/标签/单位/通道名
 *       新增任何界面文案都必须重新生成字模, 否则缺字会退化为空格的 advance
 *       (不报错但显示为空)。缺字检测必须按**像素墨迹**判定, 不能用
 *       OLED_GetGlyphAdvance(该接口缺字时回退空格度量, 永远为正)。
 */
#ifndef __KK_FONT_H
#define __KK_FONT_H

#include <stdint.h>

/** 24px 大字形(数字/小数点/正负号), 用于主数值(13 字形) */
extern const uint8_t kk_font_num24[];

/** 12px 小字形(全 ASCII 可见字符), 用于状态行/标签/单位(71 字形) */
extern const uint8_t kk_font_small12[];

#endif /* __KK_FONT_H */
