/**
 * @file tim.h
 * @brief 定时器: TIM4 = 1kHz 控制节拍, TIM3 = ADC 采样触发(仅 TRGO)
 */
#ifndef __TIM_H
#define __TIM_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

/**
 * @brief 初始化 TIM4(1kHz 更新中断)与 TIM3(采样触发)。
 * @note  TIM3 **不使用任何通道**, 只输出 TRGO 给 ADC1 的外部触发。
 *        这样 PB0/PB1(PB0/PB1 与 TIM3_CH3/CH4 复用)可以安全地做按键。
 */
void MX_TIM_Init(void);

/**
 * @brief 重新设置采样触发率(切换时基时调用)
 * @param arr TIM3 自动重载值; 触发率 = APP_TIMER_HZ / (arr + 1)
 * @note  会先停 ADC 触发、改 ARR、再重启, 避免中途产生半个周期。
 */
void TIM_Sampling_SetRate(uint16_t arr);

/** 当前采样触发率(Hz), 供测量模块算时间轴 */
uint32_t TIM_Sampling_GetRate(void);

/** 1ms 节拍计数(SysTick 或 TIM4 更新中断推进) */
uint32_t TIM_Millis(void);

/** TIM4 更新中断入口调用 */
void TIM_ControlTick(void);

#endif /* __TIM_H */
