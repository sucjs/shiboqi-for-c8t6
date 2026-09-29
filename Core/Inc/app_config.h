/**
 * @file app_config.h
 * @brief 全工程唯一调参入口: 通道数 / 采样率 / 帧长 / 按键时序 / UI 节拍 / 版面 / 标定
 *
 * @note 只放常量, 不放代码. 改量程/时基/刷新率只需改这里.
 *
 * 本文件的电气常量来自硬件设计文档 THREE_PHASE_HW_DESIGN.md (§5 前端, §6 采样时序)。
 * 与"实施方案"不同之处: 被测对象是三相三线电调(ESC)输出, 基波 300~800Hz、
 * PWM 载波 24kHz, 且要求看清载波细节 —— 因此采样率按 24kHz 载波定, 而不是工频。
 */
#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

/* ============================ 时钟 ============================ */

#define APP_HSE_HZ          8000000UL
#define APP_SYSCLK_HZ       72000000UL
#define APP_APB1_HZ         36000000UL
#define APP_APB2_HZ         72000000UL

/**
 * TIM2/TIM3/TIM4 的输入时钟.
 * STM32F103 在 APB1 预分频 != 1 时, APB1 定时器时钟 = APB1CLK x2 = HCLK = 72MHz.
 */
#define APP_TIMER_HZ        72000000UL

/**
 * ADC 时钟. F103 的 ADCCLK 上限是 14MHz, APB2=72MHz:
 *   /4 = 18MHz 超限, /6 = 12MHz 是可达的最大合规值.
 * @warning CubeMX 的 .ioc 里 ADCFreqValue 可能被填成 36MHz(/2), 那是非法的,
 *          必须在 adc.c 里显式调用 RCC_ADCCLKConfig(RCC_PCLK2_Div6) 覆盖.
 */
#define APP_ADCCLK_HZ       12000000UL

/** ADC 采样时间(周期数). HW 文档 §6.1: 源阻抗 520Ω 时 Ts=1.5 允许 734Ω, 裕量 1.41x. */
#define APP_ADC_SAMPLETIME_CYCLES   1.5f

/** 一次转换占用的 ADC 周期数 = 采样时间 + 12.5 个逐次逼近周期 */
#define APP_ADC_CONV_CYCLES         14.0f

/* ======================= 模拟前端 (AFE) ======================= */

/**
 * 分压比 1/13.05 (R1=47k, R2=3.9k), 交流耦合后叠加 1.65V 偏置.
 *   V_in = V_adc_centered_on_bias x 13.05
 * 用整数比存储, 避免浮点: mV_in = mV_adc * AFE_RATIO_NUM / AFE_RATIO_DEN
 */
#define AFE_RATIO_NUM       1305UL
#define AFE_RATIO_DEN       100UL

/** ADC 参考. LQFP48 无 VREF+, 满量程 = VDDA. */
#define AFE_VREF_MV         3300UL
#define AFE_ADC_MAX         4095UL

/** 偏置点(VDDA/2). 交流耦合, 只用于判断直流是否被拉到削顶. */
#define AFE_BIAS_MV         1650UL

/**
 * 交流满量程(输入侧峰值) = 1.65V / k = 1.65 x 13.05 = 21.5V.
 * 两电平逆变器相-中性瞬时峰值 = 2*Vbus/3 = 16V (Vbus=24V), 留 0.42V 余量.
 */
#define AFE_AC_FULLSCALE_MV 21500UL

/**
 * 削顶判定门限: ADC 原始码距离两端的最小裕量.
 * 一旦采样码落入 [0+AFE_CLIP_MARGIN, 4095-AFE_CLIP_MARGIN] 之外即认为削顶.
 * 16V 峰值对应 ADC 码 0.424V..2.876V, 折算约 526..3570, 距两端仍有 ~500 码.
 */
#define AFE_CLIP_MARGIN     20U

/* ======================= 采集: 通道与定时 ======================= */

/** 三相模式的 ADC 规则序列长度(PA0/PA1/PA2) */
#define ADC_SCAN_CH_3PH     3U
/** 单路模式只扫 1 个通道(PA0) */
#define ADC_SCAN_CH_1CH     1U

/**
 * 采样触发率 = APP_TIMER_HZ / (TRIG_ARR + 1)。
 *
 * **核心约束**: 抽取用整数均值(boxcar), 其零点在 k*fs/N。要让零点压住 24kHz
 * 载波(及其谐波), 必须满足 `fs / N = 24000`, 即 **触发率必须是 24000 的整数倍**。
 *
 *   载波视图(看 PWM 细节, 不抽取 N=1):
 *     三相 fs=240000 = 24k x 10 (ARR=299)  -> 10 点/载波周期
 *     单路 fs=720000 = 24k x 30 (ARR=99)   -> 30 点/载波周期
 *   基波视图(看换相/相序, N=2):
 *     fs=48000 (ARR=1499) -> 抽取后 24kSPS, 零点在 24k/48k, 载波被完全压掉
 *
 * 为什么基波视图不用 N=1 直接采 24k: Nyquist 恰好等于载波频率是最坏情况,
 * 载波会混叠成直流/低频假信号污染相位测量。多采一倍再平均(N=2)才干净。
 *
 * 占空比校核 (Ts=1.5 -> 14 个 ADCCLK @12MHz = 1.1667us/通道):
 *   三相载波 240k: 3 x 1.1667 = 3.50us / 4.1667us = 84%
 *   单路载波 720k: 1 x 1.1667 = 1.17us / 1.3889us = 84%
 *   基波 48k 三相: 3.50us / 20.833us = 17%(余量极大)
 * **最紧的是 84%**, 留 16% 余量; 因为是循环 DMA, CPU 不参与逐次转换。
 */
#define TRIG_ARR_CARRIER_3PH    299U    /* 240.0 kHz = 24k x 10 */
#define TRIG_ARR_CARRIER_1CH    99U     /* 720.0 kHz = 24k x 30 */
#define TRIG_ARR_FUND           1499U   /*  48.0 kHz = 24k x 2  */

/** 载波频率与基波范围(用于界面刻度与自检) */
#define CARRIER_HZ          24000UL
#define FUND_MIN_HZ         300UL
#define FUND_MAX_HZ         800UL

/**
 * 抽取因子: 有效采样率 = 触发率 / 抽取因子。
 *   载波视图: decim=1 -> 240kHz / 720kHz
 *   基波视图: decim=2 -> 48k / 2 = 24kSPS(载波零点)
 */
#define DECIM_CARRIER       1U
#define DECIM_FUND          2U

/* ======================= 采集: 帧与缓冲 ======================= */

/**
 * 每通道每帧点数。由 RAM 反推上限, 不要随意加大。
 *   载波视图(三相 240k): 256 点 = 1.067ms = 25.6 个载波周期
 *   载波视图(单路 720k): 256 点 = 0.356ms = 8.5 个载波周期
 *   基波视图(24kSPS)   : 256 点 = 10.67ms = 3.2~8.5 个基波周期
 */
#define FRAME_POINTS        256U

/**
 * DMA 原始缓冲: 2 个半缓冲 x (FRAME_POINTS x DECIM_FUND) 次扫描 x 3 通道 x uint16。
 *
 * **必须按最大抽取因子(基波视图 decim=2)算大小**, 因为缓冲是共用的:
 *   2 x (256 x 2) x 3 x 2 B = 6144 B = 6.0 KB
 * 若以后把 DECIM_FUND 调大(比如 4), 这里会线性增长, 注意 20KB RAM 上限。
 */
#define ADC_DMA_SCANS_PER_HALF  (FRAME_POINTS * DECIM_FUND)
#define ADC_DMA_LEN             (2U * ADC_DMA_SCANS_PER_HALF * ADC_SCAN_CH_3PH)

/** 抽取后的帧缓冲: 3 通道 x FRAME_POINTS x uint16 = 1536 B */
#define DEC_BUF_POINTS      FRAME_POINTS

/** 每通道最多显示的波形点数(大于屏宽, 绘制时按需抽样) */
#define WAVE_COLS           128U

/** 抽取后的有效采样率(基波视图) */
#define SAMPLE_RATE_FUND_HZ 24000UL

/* ======================= 按键 ======================= */

/** 消抖时间(ms): 连续稳定该时长才确认电平 */
#define BTN_DEBOUNCE_MS     20U
/** 长按判定时间(ms) */
#define BTN_LONGPRESS_MS    700U
/** 长按后重复触发短按的间隔(ms) */
#define BTN_REPEAT_MS       150U
/** 按键有效电平: 0 = 低电平有效(内部上拉) */
#define BTN_ACTIVE_LEVEL    0U

#define KEY_COUNT           4U

/* ======================= TIM4 控制节拍 ======================= */

#define APP_CONTROL_HZ      1000U
#define TIM4_ARR            ((APP_TIMER_HZ / APP_CONTROL_HZ) - 1UL)

/* ======================= UI 节拍 ======================= */

/** 波形页刷新周期(ms) ≈ 30fps */
#define UI_FAST_MS          33U
/** 数值页/仪表页刷新周期(ms) = 10Hz */
#define UI_NORM_MS          100U
/** 模式切换提示显示时长(ms) */
#define UI_TOAST_MS         1500U

/* ======================= 版面 (128x64) ======================= */

#define OLED_W              128U
#define OLED_H              64U

/** 状态行 0..11, 分隔线 12 */
#define UI_SEP_Y            12
/** 三相数值页: 焦点数值 14..37, 分隔线 40, 另两相 41..52, 底行 53..63 */
#define UI_BIG_Y0           14
#define UI_BIG_H            24
#define UI_SEP2_Y           40
#define UI_ROW2_Y           41
#define UI_BOTTOM_Y         53
/** 波形带 */
#define UI_WAVE_Y0          13
#define UI_WAVE_Y1          52
#define UI_WAVE_H           (UI_WAVE_Y1 - UI_WAVE_Y0 + 1U)

/* ======================= 标定(24C02S) ======================= */

/** EEPROM I2C 7 位地址(地址引脚接地) */
#define CAL_EEPROM_ADDR7    0x50U
/** 标定数据版本, 不匹配则用 AFE_* 默认值 */
#define CAL_VERSION         1U
/** 每通道零点(mV)与增益(千分比) */
#define CAL_SLOT_BYTES      16U

/* ======================= 自检与保护 ======================= */

/** 采样码有效范围: 落入此区间外视为未接信号/削顶 */
#define ADC_RAIL_LOW        4U
#define ADC_RAIL_HIGH       4091U

#endif /* __APP_CONFIG_H */
