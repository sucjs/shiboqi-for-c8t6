/**
 * @file measure.c
 * @brief 测量引擎实现: 纯整数定点, 无浮点、无 printf、不引用 HAL
 *
 * 算法要点与精度控制:
 *  - RMS: 先去直流均值, 再累加偏差平方(64 位), 用整数牛顿迭代开方。
 *        偏置/分压比统一在最后一步换算, 因此中间量不会溢出 64 位。
 *  - 频率: 上升沿零交叉 + **线性插值**取亚样本过零点, 再对多个周期取平均。
 *        不插值时 1 个样本 = 360*f/fs 度误差; 24kSPS@300Hz 是 4.5 度, 插值后 <0.1 度。
 *  - 相位差: A 相首个上升过零点与其他相首个上升过零点的时间差 x 360/T。
 *        单 ADC 顺序扫描带来的固定偏斜在这里作为**已知常量显式扣除**。
 *  - 相序: 检查 B/C 相相对 A 的相位差是否接近 -120/+120 或其反序。
 *  - 不平衡度: 由三路 RMS 按负序/正序近似定义给出(0.1% 单位)。
 *
 * 单位约定: 输出 mV(输入侧)、0.001Hz、0.1 度。
 */
#include "measure.h"

#include <string.h>

/* ---------------------------------------------------------------- 基础工具 */

uint32_t Measure_Isqrt(uint32_t x)
{
  uint32_t res = 0U;
  uint32_t bit = 1UL << 30;

  if (x == 0U)
  {
    return 0U;
  }

  /* 逐位逼近: 找到最高可置位 */
  while (bit > x)
  {
    bit >>= 2;
  }

  while (bit != 0U)
  {
    uint32_t t = res + bit;
    if (x >= t)
    {
      x -= t;
      res = (res >> 1) + bit;
    }
    else
    {
      res >>= 1;
    }
    bit >>= 2;
  }

  return res;
}

/** 绝对值(避开 stdlib 的 labs, 保持可移植) */
static int32_t meas_abs32(int32_t v)
{
  return (v < 0) ? -v : v;
}

/**
 * 计算单通道 RMS/最值。
 * @note 均值按"本通道自身均值"去直流, 而不是用名义偏置码 —— 交流耦合的
 *       实际偏置会随标定/温漂偏移, 用自身均值更准, 也与"RMS 是交流量"一致。
 */

/** 输入侧 mV = 偏置侧 mV x 分压比 */
static int32_t meas_mv_to_input(int32_t mv_centered)
{
  return (int32_t)(((int64_t)mv_centered * (int64_t)AFE_RATIO_NUM) / (int64_t)AFE_RATIO_DEN);
}

/* ------------------------------------------------------------ 单通道标量 */

typedef struct
{
  uint32_t rms_mv;
  uint32_t vpp_mv;
  int32_t  vmax_in;
  int32_t  vmin_in;
  uint16_t status;
  uint8_t  valid;
} ChScalar;

/**
 * 找到用于积分/统计的"整数周期窗口" [start, end)。
 *
 * 为什么必须对齐整数周期: 256 点 @24kSPS = 10.667ms, 对 400Hz 是 4.27 个周期,
 * 首尾各有一段残缺周期。残段会让 RMS 随初相漂移(实测三路可差 3%),
 * 也会让各路相位基准不一致。做法: 取**首个上升过零点所在样本**到
 * **最后一个上升过零点所在样本**之间的区间, 该区间恰好覆盖整数个周期。
 *
 * 找不到足够过零(如无信号/贴轨)时退化为整帧, 由 status 位提示。
 */
static void meas_cycle_window(const uint16_t *buf,
                             int32_t bias_code,
                             uint16_t *out_start,
                             uint16_t *out_end)
{
  uint16_t first = 0xFFFFU;
  uint16_t last = 0U;

  for (uint16_t i = 1U; i < FRAME_POINTS; ++i)
  {
    int32_t v0 = (int32_t)buf[i - 1U] - bias_code;
    int32_t v1 = (int32_t)buf[i] - bias_code;

    if ((v0 <= 0) && (v1 > 0))
    {
      if (first == 0xFFFFU) { first = i; }
      last = i;
    }
  }

  /* 至少覆盖 2 个过零(即 >=1 个完整周期)且窗口不能太短, 否则退化整帧 */
  if ((first == 0xFFFFU) || (last <= first) || ((uint16_t)(last - first) < 16U))
  {
    *out_start = 0U;
    *out_end   = FRAME_POINTS;
  }
  else
  {
    *out_start = first;
    *out_end   = last;
  }
}

/**
 * 计算单通道 RMS/最值。
 * @note 统计区间对齐到整数个基波周期(见 meas_cycle_window), 避免残段导致
 *       RMS/相位随初相漂移; 用本通道自身均值去直流, 而非名义偏置码 ——
 *       交流耦合的实际偏置会随标定/温漂偏移, 用自身均值更准。
 */
static void meas_channel_scalar(const uint16_t *buf, int32_t bias_code, ChScalar *out)
{
  uint16_t start, end;
  uint32_t count;
  int64_t  sum = 0;
  int32_t  vmax = 0x80000000;
  int32_t  vmin = 0x7FFFFFFF;
  uint64_t sumsq = 0;
  uint16_t status = 0U;
  uint8_t  railed_low = 0U, railed_high = 0U;

  /* 贴轨检查始终看整帧: 只统计窗口会漏掉窗口外的故障 */
  for (uint16_t i = 0U; i < FRAME_POINTS; ++i)
  {
    uint16_t c = buf[i];
    if (c <= ADC_RAIL_LOW)  { railed_low = 1U; }
    if (c >= ADC_RAIL_HIGH) { railed_high = 1U; }
  }
  if (railed_low || railed_high)
  {
    status |= MEAS_ST_CLIPPED;
  }

  meas_cycle_window(buf, bias_code, &start, &end);
  count = (uint32_t)(end - start);
  if (count == 0U) { count = 1U; }

  for (uint16_t i = start; i < end; ++i)
  {
    sum += (int64_t)buf[i];
  }

  {
    int32_t mean_code = (int32_t)(sum / (int64_t)count);

    for (uint16_t i = start; i < end; ++i)
    {
      int32_t d = (int32_t)buf[i] - mean_code;
      sumsq += (uint64_t)((int64_t)d * (int64_t)d);

      if (d > vmax) { vmax = d; }
      if (d < vmin) { vmin = d; }
    }

    /* 除以点数 -> 码^2 方差; 再换算为 mV^2 */
    {
      uint32_t var_code = (uint32_t)(sumsq / (uint64_t)count);
      uint32_t rms_code = Measure_Isqrt(var_code);
      /* 码 -> mV: mV = code * VREF / 4096 */
      uint32_t rms_mv_centered = (uint32_t)(((uint64_t)rms_code * AFE_VREF_MV) /
                                            ((uint32_t)AFE_ADC_MAX + 1U));
      uint32_t rms_in = (uint32_t)(((uint64_t)rms_mv_centered * AFE_RATIO_NUM) / AFE_RATIO_DEN);
      uint32_t vpp_in;
      /* 峰值直接由"偏差码"换算, 不经 uint16 回绕 */
      int32_t  vmax_in = meas_mv_to_input((vmax * (int32_t)AFE_VREF_MV) / ((int32_t)AFE_ADC_MAX + 1));
      int32_t  vmin_in = meas_mv_to_input((vmin * (int32_t)AFE_VREF_MV) / ((int32_t)AFE_ADC_MAX + 1));

      /* 峰峰值 = 正峰 - 负峰(都在输入侧, 已含分压比) */
      vpp_in = (uint32_t)(vmax_in - vmin_in);

      out->rms_mv  = rms_in;
      out->vpp_mv  = vpp_in;
      out->vmax_in = vmax_in;
      out->vmin_in = vmin_in;
    }
  }

  /* 贴轨判定: 若极值本身已到两端 -> 无信号 */
  if ((vmax >= (int32_t)AFE_ADC_MAX - 2) && (vmin <= 2))
  {
    status |= MEAS_ST_RAILED;
  }

  out->status = status;
  out->valid = ((status & MEAS_ST_RAILED) == 0U) ? 1U : 0U;
}

/* -------------------------------------------------------------- 过零检测 */

/**
 * 找到首个**上升**过零点(相对给定基准), 返回插值后的亚样本位置(x16 定点)。
 * 返回 0xFFFFFFFF 表示未找到。
 *
 * 插值: 在样本 i-1 <= 0 < 样本 i 的区段, 零点位置
 *   t = (i-1) + (-v[i-1]) / (v[i] - v[i-1])
 * 用 16 位小数的定点表示, 避免浮点。
 */
static uint32_t meas_first_cross_x16(const uint16_t *buf, int32_t bias_code)
{
  for (uint16_t i = 1U; i < FRAME_POINTS; ++i)
  {
    int32_t v0 = (int32_t)buf[i - 1U] - bias_code;
    int32_t v1 = (int32_t)buf[i] - bias_code;

    if ((v0 <= 0) && (v1 > 0))
    {
      int32_t den = v1 - v0;          /* > 0 */
      int32_t num = -v0;              /* >= 0, < den */
      /* t = (i-1) + num/den, 放大 65536 倍 */
      int64_t t = ((int64_t)(i - 1U) << 16) + (((int64_t)num << 16) / den);
      return (uint32_t)(t & 0xFFFFFFFFLL);
    }
  }

  return 0xFFFFFFFFUL;
}

/**
 * 统计上升过零次数(未插值), 用于频率估计与置信度判断。
 */
static uint16_t meas_count_rising_cross(const uint16_t *buf, int32_t bias_code)
{
  uint16_t n = 0U;

  for (uint16_t i = 1U; i < FRAME_POINTS; ++i)
  {
    int32_t v0 = (int32_t)buf[i - 1U] - bias_code;
    int32_t v1 = (int32_t)buf[i] - bias_code;

    if ((v0 <= 0) && (v1 > 0))
    {
      ++n;
    }
  }

  return n;
}

/**
 * 取第 n 个上升过零点(0 基)的亚样本位置(x16)。
 * 用于频率计算: 首末过零点跨 k 个周期, 比跨 1 个周期稳健得多。
 */
static uint32_t meas_nth_cross_x16(const uint16_t *buf, int32_t bias_code, uint16_t n)
{
  uint16_t seen = 0U;

  for (uint16_t i = 1U; i < FRAME_POINTS; ++i)
  {
    int32_t v0 = (int32_t)buf[i - 1U] - bias_code;
    int32_t v1 = (int32_t)buf[i] - bias_code;

    if ((v0 <= 0) && (v1 > 0))
    {
      if (seen == n)
      {
        int32_t den = v1 - v0;
        int32_t num = -v0;
        int64_t t = ((int64_t)(i - 1U) << 16) + (((int64_t)num << 16) / den);
        return (uint32_t)(t & 0xFFFFFFFFLL);
      }
      ++seen;
    }
  }

  return 0xFFFFFFFFUL;
}

/**
 * 由首末两个上升过零点估算基波频率(单位 0.001Hz)。
 * k 个周期跨度为 span_x16 个样本: T = span / fs / k
 *   f = fs * k / span
 * 为避免除法放大误差, 全程用 64 位中间量。
 */
static uint32_t meas_estimate_freq(const uint16_t *buf,
                                   int32_t bias_code,
                                   uint32_t fs_hz,
                                   uint16_t *out_cycles)
{
  uint16_t n = meas_count_rising_cross(buf, bias_code);

  *out_cycles = 0U;

  if (n < 2U)
  {
    return 0U; /* 不足以估计 */
  }

  {
    uint32_t first = meas_nth_cross_x16(buf, bias_code, 0U);
    uint32_t last  = meas_nth_cross_x16(buf, bias_code, (uint16_t)(n - 1U));
    uint32_t span_x16;
    uint16_t cycles = (uint16_t)(n - 1U);

    if ((first == 0xFFFFFFFFUL) || (last == 0xFFFFFFFFUL) || (last <= first))
    {
      return 0U;
    }

    span_x16 = last - first;
    *out_cycles = cycles;

    /* span_x16 = span_samples * 65536, 所以 f = fs*cycles*65536/span_x16 直接就是 Hz。
     * f_mHz = f_hz * 1000。全程 64 位: fs(72e6) * 255 * 65536 ≈ 1.2e15, 安全。 */
    {
      int64_t num = (int64_t)fs_hz * (int64_t)cycles;
      int64_t f_hz = (num << 16) / (int64_t)span_x16;
      int64_t f_mhz = f_hz * 1000LL;

      if (f_mhz < 0) { f_mhz = 0; }
      if (f_mhz > 0xFFFFFFFFLL) { f_mhz = 0xFFFFFFFFLL; }
      return (uint32_t)f_mhz;
    }
  }
}

/* --------------------------------------------------------------- 相序判定 */

MeasureSeq Measure_ClassifySequence(const int32_t phase_ddeg[MEASURE_MAX_CH],
                                    const uint8_t valid[MEASURE_MAX_CH])
{
  /* 容差: 标称 -1200 / +1200 (0.1 度), 允许 +/-250 (25 度) */
  const int32_t target_b = -1200;
  const int32_t target_c = 1200;
  const int32_t tol = 250;
  int32_t db, dc;

  if ((valid[PHASE_A] == 0U) || (valid[PHASE_B] == 0U) || (valid[PHASE_C] == 0U))
  {
    return SEQ_MISSING;
  }

  db = phase_ddeg[PHASE_B];
  dc = phase_ddeg[PHASE_C];

  /* 归一化到 (-1800, +1800] */
  while (db <= -1800) { db += 3600; }
  while (db >  1800) { db -= 3600; }
  while (dc <= -1800) { dc += 3600; }
  while (dc >  1800) { dc -= 3600; }

  /* 正序 ABC: B 滞后 120 (即 -1200), C 滞后 240 (即 +1200, 等价 -2400+3600) */
  if ((meas_abs32(db - target_b) <= tol) && (meas_abs32(dc - target_c) <= tol))
  {
    return SEQ_ABC;
  }

  /* 逆序 ACB: 相位关系互换 */
  if ((meas_abs32(db - target_c) <= tol) && (meas_abs32(dc - target_b) <= tol))
  {
    return SEQ_ACB;
  }

  return SEQ_UNKNOWN;
}

/* ------------------------------------------------------------------ 主流程 */

void Measure_Frame(const uint16_t *const raw[MEASURE_MAX_CH],
                   uint8_t ch_count,
                   uint32_t fs_hz,
                   uint32_t seq_no,
                   MeasureResult *out)
{
  ChScalar sc[MEASURE_MAX_CH];
  int32_t  bias_code = (int32_t)(((uint32_t)AFE_BIAS_MV *
                                 ((uint32_t)AFE_ADC_MAX + 1U)) / AFE_VREF_MV);
  int32_t  phase_ddeg_local[MEASURE_MAX_CH];
  uint8_t  valid[MEASURE_MAX_CH];

  if (ch_count > MEASURE_MAX_CH)
  {
    ch_count = MEASURE_MAX_CH;
  }

  memset(sc, 0, sizeof(sc));

  for (uint8_t c = 0U; c < ch_count; ++c)
  {
    meas_channel_scalar(raw[c], bias_code, &sc[c]);
    valid[c] = sc[c].valid;
  }

  /* --- 频率与相位: 单路只算 A --- */
  for (uint8_t c = 0U; c < ch_count; ++c)
  {
    uint16_t cycles = 0U;
    uint32_t f = 0U;

    if (sc[c].valid != 0U)
    {
      f = meas_estimate_freq(raw[c], bias_code, fs_hz, &cycles);
      if (f == 0U)
      {
        sc[c].status |= MEAS_ST_NO_ZERO;
        sc[c].valid = 0U;
        valid[c] = 0U;
      }
    }

    out->ch[c].rms_mv     = sc[c].rms_mv;
    out->ch[c].vpp_mv     = sc[c].vpp_mv;
    out->ch[c].vmax_mv    = sc[c].vmax_in;
    out->ch[c].vmin_mv    = sc[c].vmin_in;
    out->ch[c].freq_mhz   = f;
    out->ch[c].phase_ddeg = 0;   /* 相位差随后按 A 为参考填; 单路保持 0 */
    out->ch[c].status     = sc[c].status;
    out->ch[c].valid      = sc[c].valid;
    phase_ddeg_local[c] = 0;
  }

  for (uint8_t c = ch_count; c < MEASURE_MAX_CH; ++c)
  {
    memset(&out->ch[c], 0, sizeof(out->ch[c]));
    valid[c] = 0U;
  }

  /* --- 相位差(以 A 为参考) + 单 ADC 顺序扫描偏斜校正 --- */
  if (ch_count == 3U)
  {
    uint32_t tA = meas_first_cross_x16(raw[PHASE_A], bias_code);

    for (uint8_t c = 1U; c < 3U; ++c)
    {
      uint32_t tc = meas_first_cross_x16(raw[c], bias_code);

      if ((tA != 0xFFFFFFFFUL) && (tc != 0xFFFFFFFFUL) && (valid[c] != 0U) && (valid[PHASE_A] != 0U))
      {
        int64_t d = (int64_t)tc - (int64_t)tA;

        /* 校正单 ADC 顺序扫描带来的固定偏斜: 第 c 通道比 A 晚 c 个通道转换时间。
         * 转换时间 = 14 个 ADCCLK; 折算成"样本"单位再乘 65536 与 d 同尺度。
         *   dt_samples = c * 14 / ADCCLK  (秒)  x fs (样本/秒)
         *   全定点: dt_x16 = c * 14 * fs * 65536 / ADCCLK */
        d += ((int64_t)c * 14LL * (int64_t)fs_hz << 16) / (int64_t)APP_ADCCLK_HZ;

        /* 相位(0.1 度) = d_samples_x16 / span_period_x16 * 3600 */
        {
          uint16_t n = meas_count_rising_cross(raw[PHASE_A], bias_code);
          if (n >= 2U)
          {
            uint32_t first = meas_nth_cross_x16(raw[PHASE_A], bias_code, 0U);
            uint32_t last  = meas_nth_cross_x16(raw[PHASE_A], bias_code, (uint16_t)(n - 1U));
            int64_t span = (int64_t)last - (int64_t)first;
            if (span > 0)
            {
              int64_t per = span / (int64_t)(n - 1U);
              int32_t ph;
              if (per > 0)
              {
                /* 符号约定: 过零点"更晚"表示该相**滞后**, 相位取负。
                 * 故 ph = -(d / per) * 3600。 */
                ph = (int32_t)(-(d * 3600LL) / per);
              }
              else
              {
                ph = 0;
              }
              while (ph <= -1800) { ph += 3600; }
              while (ph >  1800) { ph -= 3600; }
              out->ch[c].phase_ddeg = ph;
              phase_ddeg_local[c] = ph;
            }
          }
        }
      }
    }
  }

  out->ch_count      = ch_count;
  out->sample_rate_hz = fs_hz;
  out->seq_no        = seq_no;
  out->seq           = (ch_count == 3U)
                       ? Measure_ClassifySequence(phase_ddeg_local, valid)
                       : SEQ_UNKNOWN;

  /* --- 不平衡度: |负序| / |正序| 的常用近似, 用 RMS 极差替代以省算力 ---
   * 简易定义: unbalance = (max_rms - min_rms) / avg_rms, 单位 0.1%。 */
  if (ch_count == 3U)
  {
    uint32_t mx = 0U, mn = 0xFFFFFFFFUL, sum = 0U;
    uint8_t  ok = 1U;
    for (uint8_t c = 0U; c < 3U; ++c)
    {
      if (valid[c] == 0U) { ok = 0U; break; }
      if (out->ch[c].rms_mv > mx) { mx = out->ch[c].rms_mv; }
      if (out->ch[c].rms_mv < mn) { mn = out->ch[c].rms_mv; }
      sum += out->ch[c].rms_mv;
    }
    if ((ok != 0U) && (sum > 0U))
    {
      uint32_t avg = sum / 3U;
      if (avg > 0U)
      {
        out->unbalance_dp = (uint32_t)(((uint64_t)(mx - mn) * 1000ULL) / avg);
      }
    }
    else
    {
      out->unbalance_dp = 0U;
    }
  }
  else
  {
    out->unbalance_dp = 0U;
  }
}
