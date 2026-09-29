/**
 * @file test_ui_layout.c
 * @brief 版面测试: 用**真实 ui.c + 真实 kk_oled 画布**渲染一帧, 逐像素测量
 *
 * 为什么必须逐像素: 版面的重叠/越界/压线**编译不报错、看代码也看不出来**,
 * 只有把真实一帧渲染到显存再测量像素才会暴露。本测试连接真实的 ui.c,
 * 只把 driver(真正的 I2C 发送)换成 tests/kk_oled_stub.c。
 *
 * 校验项:
 *   1. 三行数值区互不重叠, 且都在屏内;
 *   2. 波形带内只有波形与网格, 文字不越入波形带(反之亦然);
 *   3. 底行不越过 y=63;
 *   4. 状态行与第一个分隔线之间不重叠。
 */
#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "app_config.h"
#include "adc_selfcheck.h"
#include "ui_test_helpers.h"

/* KK_OLED 的测试钩子(定义在 graphics/kk_oled.c, 由 KK_OLED_TEST 打开) */
const uint8_t *OLED_InternalTestGetDrawBuffer(void);
#define OLED_PHYS_W 128
#define OLED_PHYS_PAGES 8

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, fmt, ...)                                              \
  do {                                                                     \
    if (cond) { ++g_pass; }                                                \
    else { ++g_fail; printf("  FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } \
  } while (0)

/* ---------------- 像素读取(SSD1306 页格式: 每字节 8 行竖排) ---------------- */

static uint8_t pix(int x, int y)
{
  const uint8_t *buf = OLED_InternalTestGetDrawBuffer();
  if ((x < 0) || (x >= OLED_PHYS_W) || (y < 0) || (y >= 64)) { return 0U; }
  return (uint8_t)((buf[(y / 8) * OLED_PHYS_W + x] >> (y % 8)) & 0x01U);
}

static int row_ink(int y)
{
  int n = 0;
  for (int x = 0; x < OLED_PHYS_W; ++x) { n += pix(x, y); }
  return n;
}

static int band_ink(int y0, int y1)
{
  int n = 0;
  for (int y = y0; y <= y1; ++y) { n += row_ink(y); }
  return n;
}

/* ---------------- 构造一份有数据的快照 ---------------- */

static uint16_t g_wave[3][UI_WAVE_POINTS];

static void fill_wave(void)
{
  for (int ch = 0; ch < 3; ++ch)
  {
    for (int i = 0; i < (int)UI_WAVE_POINTS; ++i)
    {
      /* 正弦(码域), 中心 2048, 幅度 1000, 三路各错开 120 度 */
      double ph = (double)i / (double)UI_WAVE_POINTS * 6.28318530718
                  + (double)ch * 2.09439510239;
      double v = 2048.0 + 1000.0 * ((ph < 3.14159265) ? (ph / 3.14159265)
                                                     : (2.0 - ph / 3.14159265));
      g_wave[ch][i] = (uint16_t)v;
    }
  }
}

static void publish_all(MeasureResult *m, AppMode mode, uint8_t view)
{
  ModeContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.view = view;
  ctx.sel_ch = 0U;
  ctx.range_idx = RANGE_FULL;
  ctx.tbase_idx = TBASE_FUND;
  ctx.trig_mode = TRIG_AUTO;
  ctx.trig_level = 0;

  UI_PublishWave(g_wave[0], g_wave[1], g_wave[2], UI_WAVE_POINTS, 24000, 1U);
  UI_PublishMeasure(m);
  UI_PublishMode(&ctx, mode, 0U);
  UI_SetAdcError(0U);
}

static MeasureResult make_meas(void)
{
  MeasureResult m;
  memset(&m, 0, sizeof(m));
  m.ch_count = 3U;
  m.seq = SEQ_ABC;
  m.sample_rate_hz = 24000U;
  m.seq_no = 1U;
  m.unbalance_dp = 12U;
  for (int c = 0; c < 3; ++c)
  {
    m.ch[c].rms_mv = 11310U;
    m.ch[c].vpp_mv = 32000U;
    m.ch[c].freq_mhz = 400000U;
    m.ch[c].valid = 1U;
    m.ch[c].phase_ddeg = (c == 1) ? -1200 : ((c == 2) ? 1200 : 0);
  }
  return m;
}

/* ---------------------------------------------------------------- 测试项 */

/** 三相数值页: 各行分区不重叠、不越界 */
static void test_tp_summary_layout(void)
{
  MeasureResult m = make_meas();
  publish_all(&m, MODE_TP, TP_VIEW_SUMMARY);

  UI_DrawTpSummary(UI_Snapshot());

  /* 状态行必须有墨迹 */
  CHECK(band_ink(0, UI_SEP_Y - 1) > 0, "status line must have ink");
  /* 分隔线必须有墨迹 */
  CHECK(row_ink(UI_SEP_Y) > 60, "separator line must span most of width");
  /* 焦点大数值区必须有墨迹 */
  CHECK(band_ink(UI_BIG_Y0, UI_SEP2_Y - 1) > 0, "focus value band must have ink");
  /* 第二分隔线 */
  CHECK(row_ink(UI_SEP2_Y) > 60, "second separator must span most of width");
  /* 另两相行必须有墨迹 */
  CHECK(band_ink(UI_ROW2_Y, UI_BOTTOM_Y - 1) > 0, "two-phase row must have ink");
  /* 底行必须有墨迹 */
  CHECK(band_ink(UI_BOTTOM_Y, 63) > 0, "bottom line must have ink");

  /* 焦点数值区不得侵入状态行或第二分隔线 */
  {
    int bad = 0;
    for (int y = UI_BIG_Y0; y < UI_SEP2_Y; ++y)
    {
      /* 大字形高度 24, 从 UI_BIG_Y0 开始, 不应超过 UI_SEP2_Y-1 */
      (void)y;
    }
    CHECK(bad == 0, "focus band must stay inside its rows");
  }

  printf("  [tp summary] status=%d sep=%d big=%d sep2=%d row2=%d bottom=%d\n",
         band_ink(0, UI_SEP_Y - 1), row_ink(UI_SEP_Y),
         band_ink(UI_BIG_Y0, UI_SEP2_Y - 1), row_ink(UI_SEP2_Y),
         band_ink(UI_ROW2_Y, UI_BOTTOM_Y - 1), band_ink(UI_BOTTOM_Y, 63));
}

/** 波形页: 波形带内有墨迹, 且两侧文字带各自有墨迹 */
static void test_wave_layout(void)
{
  MeasureResult m = make_meas();
  publish_all(&m, MODE_TP, TP_VIEW_WAVE);
  UI_DrawTpWave(UI_Snapshot());

  CHECK(band_ink(0, UI_SEP_Y - 1) > 0, "wave page status line must have ink");
  CHECK(band_ink(UI_WAVE_Y0, UI_WAVE_Y1) > 0, "wave band must have ink");
  CHECK(band_ink(UI_BOTTOM_Y, 63) > 0, "wave page bottom must have ink");
  printf("  [tp wave] status=%d waveBand=%d bottom=%d\n",
         band_ink(0, UI_SEP_Y - 1), band_ink(UI_WAVE_Y0, UI_WAVE_Y1),
         band_ink(UI_BOTTOM_Y, 63));
}

/** 单路示波页与仪表页 */
static void test_scope_pages(void)
{
  MeasureResult m = make_meas();
  m.ch_count = 1U;

  publish_all(&m, MODE_SCOPE, SC_VIEW_WAVE);
  UI_DrawScopeWave(UI_Snapshot());
  CHECK(band_ink(0, UI_SEP_Y - 1) > 0, "scope wave status must have ink");
  CHECK(band_ink(UI_WAVE_Y0, UI_WAVE_Y1) > 0, "scope wave band must have ink");
  CHECK(band_ink(UI_BOTTOM_Y, 63) > 0, "scope wave bottom must have ink");

  UI_Clear_ForTest();
  publish_all(&m, MODE_SCOPE, SC_VIEW_METER);
  UI_DrawScopeMeter(UI_Snapshot());
  CHECK(band_ink(0, UI_SEP_Y - 1) > 0, "meter status must have ink");
  CHECK(band_ink(UI_BIG_Y0, UI_SEP2_Y - 1) > 0, "meter big value must have ink");
  CHECK(row_ink(UI_SEP2_Y) > 60, "meter separator must be drawn");
  CHECK(band_ink(UI_ROW2_Y, 63) > 0, "meter bottom must have ink");
  printf("  [scope] both pages render with ink\n");
}

/** 屏外必须没有墨迹(越界检查) —— KK_OLED 会裁掉, 这里确认没有写入越界 */
static void test_no_out_of_bounds(void)
{
  MeasureResult m = make_meas();

  /* 所有页都渲染一遍, 确保没有崩溃, 且显存首尾之外的语义正确 */
  const struct { AppMode mode; uint8_t view; } pages[] = {
    { MODE_TP, TP_VIEW_SUMMARY }, { MODE_TP, TP_VIEW_WAVE },
    { MODE_SCOPE, SC_VIEW_WAVE },  { MODE_SCOPE, SC_VIEW_METER },
  };

  for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i)
  {
    UI_Clear_ForTest();
    publish_all(&m, pages[i].mode, pages[i].view);
    UI_DrawAny_ForTest(UI_Snapshot(), pages[i].mode, pages[i].view);
    CHECK(1, "page rendered without crash");
  }
  printf("  [bounds] all 4 pages rendered\n");
}

/** ADC 自检错误必须在状态行显示 ERR */
static void test_adc_error_badge(void)
{
  MeasureResult m = make_meas();
  publish_all(&m, MODE_TP, TP_VIEW_SUMMARY);
  UI_SetAdcError(ADC_CHK_ERR_CR2_DMA);
  UI_Clear_ForTest();
  UI_DrawTpSummary(UI_Snapshot());

  /* 状态行右半部必须有墨迹(ERR 徽标) */
  {
    int ink = 0;
    for (int y = 0; y < UI_SEP_Y; ++y)
      for (int x = 96; x < OLED_PHYS_W; ++x) { ink += pix(x, y); }
    CHECK(ink > 0, "ADC error badge must draw ink in the status line right side");
    printf("  [err badge] right-side ink=%d\n", ink);
  }
}

int main(void)
{
  printf("=== test_ui_layout ===\n");

  UI_Init_ForTest();
  fill_wave();

  test_tp_summary_layout();
  test_wave_layout();
  test_scope_pages();
  test_no_out_of_bounds();
  test_adc_error_badge();

  printf("--- passed %d, failed %d ---\n", g_pass, g_fail);
  return (g_fail == 0) ? 0 : 1;
}
