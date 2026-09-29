/**
 * @file adc.h
 * @brief 采集: ADC1 三通道扫描 + TIM3_TRGO 触发 + DMA 双缓冲 + 整数抽取
 *
 * 数据通路:
 *   TIM3_TRGO ──触发──> ADC1 扫描(PA0/PA1/PA2) ──DMA──> 原始双半缓冲
 *                                                        │
 *                                    主循环: 半满/全满 → 抽取 → 帧缓冲
 *
 * 抽取(decimation)的意义: 触发率是 24kHz 载波的整数倍, 按 DECIM 做**整数
 * 均值抽取**(boxcar), 其频响零点恰好落在 24k/48k/72k...(载波及其谐波),
 * 因此抽取后的基波视图天然干净, 不需要额外的模拟/数字滤波器。
 */
#ifndef __ADC_H
#define __ADC_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

/** 采集状态 */
typedef enum
{
  ACQ_IDLE = 0,
  ACQ_RUNNING,
  ACQ_ERROR
} AcqState;

/**
 * @brief 初始化 ADC1(扫描 + 外部触发 + DMA 循环) 与 DMA1_Channel1
 * @param scan_ch 规则序列通道数(3 = 三相, 1 = 单路)
 */
void MX_ADC_Init(uint8_t scan_ch);

/**
 * @brief 按模式/时基重配采集
 * @param scan_ch  1 或 3
 * @param trig_arr TIM3 自动重载值(决定触发率)
 */
void ADC_Configure(uint8_t scan_ch, uint16_t trig_arr);

/** 开始采集(启动 DMA) */
void ADC_Start(void);

/** 停止采集 */
void ADC_Stop(void);

/** 当前采集状态 */
AcqState ADC_GetState(void);

/**
 * @brief 在**主循环**里调用: 若某半缓冲已就绪则抽取出一帧
 * @param out_ch   输出: 每通道 FRAME_POINTS 个样本的帧缓冲指针(3 个)
 * @param decim    抽取因子(1 = 载波视图不抽取)
 * @param fs_raw   原始触发率(Hz), 用于回填有效采样率
 * @param out_fs   输出: 本帧有效采样率(Hz)
 * @return 1 = 产出新帧; 0 = 无新帧
 *
 * @note 中断里只置标志, 真正的搬运与抽取都在这里做, 保证 ISR 极短。
 */
uint8_t ADC_PollFrame(uint16_t *out_ch[3], uint8_t decim, uint32_t fs_raw, uint32_t *out_fs);

/**
 * @brief 回读 ADC/DMA 关键寄存器做自检
 * @return 0 = 通过; 非 0 = AdcSelfCheck 的错误位掩码
 */
uint16_t ADC_SelfCheck(void);

/** 累计产出的帧数(上板验证用: 持续增长说明采集真的在跑) */
uint32_t ADC_GetFrameCount(void);

#endif /* __ADC_H */
