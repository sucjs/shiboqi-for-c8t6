/**
 * @file measure.h
 * @brief 三相/单路测量引擎: RMS / 频率 / 相位差 / 相序 / 不平衡度 / 最值
 *
 * @note 纯整数定点, 不引用任何 STM32 头文件, 不使用浮点与 printf,
 *       因此可在宿主机用解析生成的三相正弦做确定性单元测试。
 *
 * 输入约定: 每通道 FRAME_POINTS 个原始 ADC 码(0..4095), 已含 1.65V 偏置。
 * 输出: 全部换算为**输入侧电压**(mV), 相角单位 0.1 度, 便于定点显示。
 */
#ifndef __MEASURE_H
#define __MEASURE_H

#include <stdint.h>
#include "app_config.h"

/** 单帧最多支持的通道数(三相) */
#define MEASURE_MAX_CH 3U

/** 相角用 0.1 度为单位(定点, 避免浮点) */
#define MEASURE_DEG_SCALE 10

/** 相序判定结果 */
typedef enum
{
  SEQ_UNKNOWN = 0,  /* 数据不足/无法判定 */
  SEQ_ABC,          /* 正序 */
  SEQ_ACB,          /* 逆序 */
  SEQ_MISSING       /* 缺相 */
} MeasureSeq;

/** 通道标识(与 ADC 扫描顺序一致) */
typedef enum
{
  PHASE_A = 0,
  PHASE_B,
  PHASE_C
} PhaseId;

/** 单通道标量结果 */
typedef struct
{
  uint32_t rms_mv;      /* 有效值(输入侧 mV) */
  uint32_t vpp_mv;      /* 峰峰值(输入侧 mV) */
  int32_t  vmin_mv;     /* 最小值(相对偏置, 有符号, mV) */
  int32_t  vmax_mv;     /* 最大值(相对偏置, 有符号, mV) */
  uint32_t freq_mhz;    /* 基波频率(单位 0.001Hz) */
  int32_t  phase_ddeg;  /* 相对 A 相的相位差(0.1 度, -1800..+1800) */
  uint16_t status;      /* 位掩码: 见 MEAS_ST_* */
  uint8_t  valid;       /* 本通道数据是否可用 */
} MeasureChannel;

/** status 位 */
#define MEAS_ST_CLIPPED   (1U << 0)  /* 采样码触到两端(削顶/未接信号) */
#define MEAS_ST_NO_ZERO   (1U << 1)  /* 未检出足够过零点 */
#define MEAS_ST_RAILED    (1U << 2)  /* 长期贴轨, 视为无信号 */

/** 整帧测量结果 */
typedef struct
{
  MeasureChannel ch[MEASURE_MAX_CH];
  uint8_t  ch_count;      /* 有效通道数(3 或 1) */
  MeasureSeq seq;         /* 相序 */
  uint32_t unbalance_dp;  /* 不平衡度(0.1%) */
  uint32_t sample_rate_hz;/* 本帧有效采样率(Hz), 供界面算时基 */
  uint32_t seq_no;        /* 帧序号, 用于 UI 判撕裂 */
} MeasureResult;

/**
 * @brief 对一帧原始样本做全量测量
 * @param raw        每通道指针数组, 长度 ch_count; 每通道 FRAME_POINTS 个样本
 * @param ch_count   1(单路) 或 3(三相)
 * @param fs_hz      本帧有效采样率(Hz)
 * @param seq_no     帧序号(由调用者递增)
 * @param out        输出结果
 *
 * @note 相位差以 A 相为参考; 单路模式下 phase_ddeg 恒为 0。
 */
void Measure_Frame(const uint16_t *const raw[MEASURE_MAX_CH],
                   uint8_t ch_count,
                   uint32_t fs_hz,
                   uint32_t seq_no,
                   MeasureResult *out);

/**
 * @brief 整数牛顿迭代开方(纯逻辑, 供测试直接调用)
 * @param x 被开方数
 * @return floor(sqrt(x))
 */
uint32_t Measure_Isqrt(uint32_t x);

/**
 * @brief 由三路 RMS 判定相序(纯逻辑)
 * @param phase_ddeg 三路相对 A 的相位差(0.1 度)
 * @param valid      三路是否有效
 */
MeasureSeq Measure_ClassifySequence(const int32_t phase_ddeg[MEASURE_MAX_CH],
                                    const uint8_t valid[MEASURE_MAX_CH]);

#endif /* __MEASURE_H */
