/**
 * @file app.h
 * @brief 应用装配与 1ms 控制循环
 */
#ifndef __APP_H
#define __APP_H

#include <stdint.h>

/** 上电初始化: ADC/TIM/OLED/UI/按键/模式, 并跑 ADC 自检 */
void App_Init(void);

/** 主循环体(每轮调用一次; 内部按节拍调度) */
void App_Loop(void);

/** 最近一次 ADC 自检的错误位(0 = 通过) */
uint8_t App_GetAdcError(void);

#endif /* __APP_H */
