/**
 * @file buttons.h
 * @brief 4 路按键消抖与短按/长按/连发识别(纯逻辑, 不依赖 HAL)
 *
 * @note 本模块只接受"某路当前是否按下"这一个布尔量, 由调用者每 1ms 喂一次,
 *       因此可在宿主机直接编译测试, 不需要任何 STM32 头文件。
 *
 * 事件语义(沿用基线 AItostm32 button.c 已验证的时序, 扩展到 4 路):
 *   - 按下并稳定 BTN_DEBOUNCE_MS 后才认为真的按下(消抖), 抖动不产生事件
 *   - 松开时按住时长 <  BTN_LONGPRESS_MS -> SHORT
 *   - 松开时按住时长 >= BTN_LONGPRESS_MS -> LONG
 *   - 按住超过 BTN_LONGPRESS_MS 期间, 每 BTN_REPEAT_MS 产生一次 REPEAT
 *
 * 每路**独立排队**且**互不串扰**: 同一时刻某路最多保留一个待取事件。
 */
#ifndef __BUTTONS_H
#define __BUTTONS_H

#include <stdint.h>

/** 按键逻辑通道 */
typedef enum
{
  KEY_MODE = 0,   /* K1: 短按切子视图 / 长按切模式 */
  KEY_SEL,        /* K2: 短按选通道 / 长按连发选通道 */
  KEY_ADJ,        /* K3: 短按移焦点 / 长按进出编辑态 */
  KEY_OK,         /* K4: 短按加档或冻结 / 长按取消或复位 */
  KEY_NUM
} KeyId;

/** 按键事件 */
typedef enum
{
  BTN_EVENT_NONE = 0,
  BTN_EVENT_SHORT,   /* 短按(在松开沿产生) */
  BTN_EVENT_LONG,    /* 长按(在松开沿产生) */
  BTN_EVENT_REPEAT   /* 长按保持期间的周期重复 */
} ButtonEvent;

/** 复位全部通道状态 */
void Buttons_Init(void);

/**
 * @brief 每 1ms 调用一次, 喂入 4 路"是否按下"(bit0..bit3 对应 KEY_MODE..KEY_OK)
 * @param mask_pressed 每位的 1 表示该路按下
 */
void Buttons_Update(uint8_t mask_pressed);

/**
 * @brief 取出并清除某一路待处理事件
 * @return 该路最早已排队的事件; 无事件时返回 BTN_EVENT_NONE
 */
ButtonEvent Buttons_Poll(KeyId key);

/**
 * @brief 是否存在任意待处理事件(用于"按键当拍立即刷新")
 */
uint8_t Buttons_HasPending(void);

#endif /* __BUTTONS_H */
