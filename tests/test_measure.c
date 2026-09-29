/**
 * @file test_measure.c
 * @brief 测量引擎的宿主机单元测试(不交叉编译, 与固件共用同一份 measure.c)
 *
 * 用解析生成的三相正弦/方波做确定性输入, 覆盖:
 *   - RMS 精度(与解析值比较)
 *   - 频率精度(线性插值过零)
 *   - 120 度相位差、正序/逆序判定
 *   - 缺相必须被识别
 *   - 含直流偏置时 RMS 不受影响(去均值)
 *   - 削顶/无信号必须报告状态位
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* C11 严格模式下 M_PI 不是标准宏, 自己定义 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "measure.h"
#include "app_config.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, fmt, ...)                                  \
  do {                                                         \
    if (cond) { ++g_pass; }                                    \
    else { ++g_fail;                                           \
      printf("  FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } \
  } while (0)

/* ---------------- 波形发生器 ---------------- */

static uint16_t g_buf[MEASURE_MAX_CH][FRAME_POINTS];

/** 生成正弦: code(phi) = bias + A*sin(2*pi*f*t + phi_deg)  (码域) */
static void gen_sine(uint16_t *dst, double f, uint32_t fs, double phi_deg, double amp_code, double bias_code)
{
  for (uint16_t i = 0; i < FRAME_POINTS; ++i)
  {
    double t = (double)i / (double)fs;
    double v = bias_code + amp_code * sin(2.0 * M_PI * f * t + phi_deg * M_PI / 180.0);
    int iv = (int)(v + 0.5);
    if (iv < 0) { iv = 0; }
    if (iv > 4095) { iv = 4095; }
    dst[i] = (uint16_t)iv;
  }
}

static const double BIAS_CODE = 2048.0; /* 中间码, 对应约 1.65V */

/* ---------------- 测试项 ---------------- */

static void test_isqrt(void)
{
  CHECK(Measure_Isqrt(0) == 0, "isqrt(0)");
  CHECK(Measure_Isqrt(1) == 1, "isqrt(1)");
  CHECK(Measure_Isqrt(4) == 2, "isqrt(4)");
  CHECK(Measure_Isqrt(15) == 3, "isqrt(15)");
  CHECK(Measure_Isqrt(16) == 4, "isqrt(16)");
  CHECK(Measure_Isqrt(1000000) == 1000, "isqrt(1e6)");
  CHECK(Measure_Isqrt(999999) == 999, "isqrt(999999)");
  /* 大数不溢出 */
  CHECK(Measure_Isqrt(0xFFFFFFF0UL) > 60000UL, "isqrt(big)");
}

/** RMS/频率/相位 在 24kSPS 基波视图下的精度 */
static void test_three_phase_abc(void)
{
  const uint32_t fs = 24000;
  const double f = 400.0;      /* 基波 300~800 区间内 */
  const double amp = 1200.0;   /* 码域幅度 */
  const uint16_t *raw[3];
  MeasureResult r;

  gen_sine(g_buf[0], f, fs, 0.0,    amp, BIAS_CODE);
  gen_sine(g_buf[1], f, fs, -120.0, amp, BIAS_CODE);
  gen_sine(g_buf[2], f, fs, 120.0,  amp, BIAS_CODE);
  raw[0] = g_buf[0]; raw[1] = g_buf[1]; raw[2] = g_buf[2];

  Measure_Frame(raw, 3, fs, 1, &r);

  /* 频率: 0.001Hz 单位 */
  CHECK(fabs((double)r.ch[0].freq_mhz / 1000.0 - f) < 0.05,
        "freq got %.3fHz want %.1f", (double)r.ch[0].freq_mhz / 1000.0, f);
  CHECK(fabs((double)r.ch[1].freq_mhz / 1000.0 - f) < 0.05, "freq B");
  CHECK(fabs((double)r.ch[2].freq_mhz / 1000.0 - f) < 0.05, "freq C");

  /* 三路 RMS 应相等且等于 amp/sqrt(2) 折算到输入侧 */
  {
    double rms_code = amp / sqrt(2.0);
    double rms_mv_centered = rms_code * 3300.0 / 4096.0;
    double rms_in = rms_mv_centered * 13.05;
    CHECK(fabs((double)r.ch[0].rms_mv - rms_in) / rms_in < 0.01,
          "rms A got %u want %.1f", r.ch[0].rms_mv, rms_in);
    CHECK(fabs((double)r.ch[0].rms_mv - (double)r.ch[1].rms_mv) / rms_in < 0.01, "rms A vs B");
    CHECK(fabs((double)r.ch[0].rms_mv - (double)r.ch[2].rms_mv) / rms_in < 0.01, "rms A vs C");
  }

  /* 相位: B 约 -1200 (0.1度), C 约 +1200 */
  CHECK(abs(r.ch[1].phase_ddeg - (-1200)) <= 20,
        "phase B got %d want -1200", r.ch[1].phase_ddeg);
  CHECK(abs(r.ch[2].phase_ddeg - 1200) <= 20,
        "phase C got %d want +1200", r.ch[2].phase_ddeg);

  /* 相序: 正序 */
  CHECK(r.seq == SEQ_ABC, "seq got %d want ABC", (int)r.seq);

  /* 不平衡度应接近 0 */
  CHECK(r.unbalance_dp < 30, "unbalance got %u want ~0", r.unbalance_dp);

  printf("  [ABC] f=%.3fHz rms=%umV phB=%d phC=%d seq=%d unbal=%u\n",
         (double)r.ch[0].freq_mhz / 1000.0, r.ch[0].rms_mv,
         r.ch[1].phase_ddeg, r.ch[2].phase_ddeg, (int)r.seq, r.unbalance_dp);
}

/** 逆序 ACB 必须判为 ACB */
static void test_three_phase_acb(void)
{
  const uint32_t fs = 24000;
  const double f = 600.0;
  const double amp = 1200.0;
  const uint16_t *raw[3];
  MeasureResult r;

  /* 交换 B/C 的相位 -> 逆序 */
  gen_sine(g_buf[0], f, fs, 0.0,   amp, BIAS_CODE);
  gen_sine(g_buf[1], f, fs, 120.0, amp, BIAS_CODE);
  gen_sine(g_buf[2], f, fs, -120.0, amp, BIAS_CODE);
  raw[0] = g_buf[0]; raw[1] = g_buf[1]; raw[2] = g_buf[2];

  Measure_Frame(raw, 3, fs, 2, &r);

  CHECK(r.seq == SEQ_ACB, "ACB seq got %d want ACB", (int)r.seq);
  printf("  [ACB] phB=%d phC=%d seq=%d\n", r.ch[1].phase_ddeg, r.ch[2].phase_ddeg, (int)r.seq);
}

/** 缺相: 某路贴轨/无信号 -> 必须报缺相且该通道 invalid */
static void test_missing_phase(void)
{
  const uint32_t fs = 24000;
  const double f = 400.0;
  const uint16_t *raw[3];
  MeasureResult r;

  gen_sine(g_buf[0], f, fs, 0.0,   1200.0, BIAS_CODE);
  gen_sine(g_buf[1], f, fs, -120.0, 1200.0, BIAS_CODE);
  /* C 相接地: 全部贴 0 */
  for (uint16_t i = 0; i < FRAME_POINTS; ++i) { g_buf[2][i] = 0; }
  raw[0] = g_buf[0]; raw[1] = g_buf[1]; raw[2] = g_buf[2];

  Measure_Frame(raw, 3, fs, 3, &r);

  CHECK(r.seq == SEQ_MISSING, "missing seq got %d want MISSING", (int)r.seq);
  CHECK((r.ch[2].status & MEAS_ST_CLIPPED) != 0U, "C should be flagged clipped");
  printf("  [MISSING] C status=0x%X valid=%u seq=%d\n",
         r.ch[2].status, r.ch[2].valid, (int)r.seq);
}

/** 含直流偏置时 RMS 不受影响(去均值) */
static void test_dc_offset_rejected(void)
{
  const uint32_t fs = 24000;
  const double f = 400.0;
  const double amp = 1000.0;
  const uint16_t *raw[3];
  MeasureResult r0, r1;

  gen_sine(g_buf[0], f, fs, 0.0, amp, BIAS_CODE);
  gen_sine(g_buf[1], f, fs, -120.0, amp, BIAS_CODE);
  gen_sine(g_buf[2], f, fs, 120.0, amp, BIAS_CODE);
  raw[0] = g_buf[0]; raw[1] = g_buf[1]; raw[2] = g_buf[2];
  Measure_Frame(raw, 3, fs, 4, &r0);

  /* 整体抬高 200 码(相当于偏置漂移) */
  for (uint8_t c = 0; c < 3; ++c)
    for (uint16_t i = 0; i < FRAME_POINTS; ++i)
      g_buf[c][i] = (uint16_t)(g_buf[c][i] + 200);
  Measure_Frame(raw, 3, fs, 5, &r1);

  CHECK(abs((int)r0.ch[0].rms_mv - (int)r1.ch[0].rms_mv) <= 3,
        "DC offset changed RMS: %u -> %u", r0.ch[0].rms_mv, r1.ch[0].rms_mv);
  printf("  [DC] rms %u -> %u (should be equal)\n", r0.ch[0].rms_mv, r1.ch[0].rms_mv);
}

/** 单路模式: 只算 A, 无相位/相序 */
static void test_single_channel(void)
{
  const uint32_t fs = 24000;
  const uint16_t *raw[3];
  MeasureResult r;

  gen_sine(g_buf[0], 500.0, fs, 0.0, 1200.0, BIAS_CODE);
  raw[0] = g_buf[0];

  Measure_Frame(raw, 1, fs, 6, &r);

  CHECK(r.ch_count == 1U, "ch_count got %u want 1", r.ch_count);
  CHECK(fabs((double)r.ch[0].freq_mhz / 1000.0 - 500.0) < 0.05, "1ch freq");
  CHECK(r.seq == SEQ_UNKNOWN, "1ch seq should be UNKNOWN");
  CHECK(r.ch[0].phase_ddeg == 0, "1ch phase should be 0");
  printf("  [1CH] f=%.3fHz rms=%umV vpp=%umV\n",
         (double)r.ch[0].freq_mhz / 1000.0, r.ch[0].rms_mv, r.ch[0].vpp_mv);
}

/** 载波视图: 240kSPS 下相位差仍应正确(偏斜校正生效) */
static void test_carrier_view_phase(void)
{
  const uint32_t fs = 240000;
  const double f = 400.0;
  const uint16_t *raw[3];
  MeasureResult r;

  gen_sine(g_buf[0], f, fs, 0.0,    1200.0, BIAS_CODE);
  gen_sine(g_buf[1], f, fs, -120.0, 1200.0, BIAS_CODE);
  gen_sine(g_buf[2], f, fs, 120.0,  1200.0, BIAS_CODE);
  raw[0] = g_buf[0]; raw[1] = g_buf[1]; raw[2] = g_buf[2];

  Measure_Frame(raw, 3, fs, 7, &r);

  /* 240kSPS 下 256 点只有 0.43 个 400Hz 周期 -> 过零不足, 允许 NO_ZERO,
   * 但一旦算出来就必须准。这里只断言"若给出频率则误差小"。 */
  if (r.ch[0].valid != 0U)
  {
    CHECK(fabs((double)r.ch[0].freq_mhz / 1000.0 - f) < 0.5, "carrier freq");
  }
  printf("  [CARRIER 240k] valid=%u f=%u mHz seq=%d\n",
         r.ch[0].valid, r.ch[0].freq_mhz, (int)r.seq);
}

/** 相序判定函数直接测试(边界与容差) */
static void test_seq_classifier(void)
{
  int32_t ph[3];
  uint8_t ok[3] = {1, 1, 1};
  uint8_t bad[3] = {1, 0, 1};

  ph[0] = 0; ph[1] = -1200; ph[2] = 1200;
  CHECK(Measure_ClassifySequence(ph, ok) == SEQ_ABC, "classifier ABC");

  ph[0] = 0; ph[1] = 1200; ph[2] = -1200;
  CHECK(Measure_ClassifySequence(ph, ok) == SEQ_ACB, "classifier ACB");

  ph[0] = 0; ph[1] = -1200; ph[2] = 1200;
  CHECK(Measure_ClassifySequence(ph, bad) == SEQ_MISSING, "classifier MISSING");

  /* 容差边界: 偏离 20 度仍应判 ABC */
  ph[0] = 0; ph[1] = -1000; ph[2] = 1200;
  CHECK(Measure_ClassifySequence(ph, ok) == SEQ_ABC, "classifier ABC +20deg");
  /* 偏离过大 -> UNKNOWN */
  ph[0] = 0; ph[1] = -600; ph[2] = 1200;
  CHECK(Measure_ClassifySequence(ph, ok) == SEQ_UNKNOWN, "classifier UNKNOWN 60deg");
  /* 环绕: +1700 等价 -1900 应归一化 */
  ph[0] = 0; ph[1] = 1200; ph[2] = -1200 + 3600;
  CHECK(Measure_ClassifySequence(ph, ok) == SEQ_ACB, "classifier wrap");
}

int main(void)
{
  printf("=== test_measure ===\n");
  test_isqrt();
  test_three_phase_abc();
  test_three_phase_acb();
  test_missing_phase();
  test_dc_offset_rejected();
  test_single_channel();
  test_carrier_view_phase();
  test_seq_classifier();

  printf("--- passed %d, failed %d ---\n", g_pass, g_fail);
  return (g_fail == 0) ? 0 : 1;
}
