/**
 * @file ui.c
 * @brief 四个页面的绘制 + 刷新调度
 *
 * 版面设计原则(为什么用固定行坐标):
 *   128x64 一共 8 页(每页 8 行)。逐行固定可以保证"数值换位数时不抖动、
 *   分隔线不被压、波形不越到文字区"。坐标全部来自 app_config.h 的 UI_* 常量,
 *   宿主测试(test_ui_layout)用同一份常量逐像素校验不重叠、不越界。
 *
 * 版面(三相数值页):
 *   y= 0..11  状态行: 左 `3PH`  中 频率  右 相序(AUTO/NORMAL/SINGLE)
 *   y= 12     分隔线
 *   y=14..37  焦点通道 24px 大字 + 12px 单位
 *   y=40      分隔线
 *   y=41..52  另两相: 通道名 + 值 + 相角
 *   y=53..63  底行: 不平衡度(左) + 按键提示(右)
 *
 * 版面(波形页/示波页):
 *   y= 0..11  状态行: 模式/时基/量程/触发
 *   y=13..52  波形带(3 条或 1 条轨迹 + 分格虚线)
 *   y=53..63  底行: Vpp/Vrms/f + 冻结/编辑标记
 */
#include "ui.h"
#include "app_config.h"
#include "kk_font.h"
#include "kk_oled.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ 工具 */

/** 快照(单一实例; 单线程访问, 由 app 与 UI 在主循环里顺序使用) */
static UiSnapshot s_snap;
static uint8_t     s_inited;

/** 把有符号 mV 格式化成 "-12.34V" 之类(避免浮点) */
static void fmt_voltage(char *buf, size_t n, int32_t mv)
{
  int32_t a = (mv < 0) ? -mv : mv;
  int32_t whole = a / 1000;
  int32_t frac = (a % 1000) / 10;   /* 两位小数 */

  if (mv < 0)
  {
    (void)snprintf(buf, n, "- %u.%02u", (unsigned)whole, (unsigned)frac);
  }
  else
  {
    (void)snprintf(buf, n, "%u.%02u", (unsigned)whole, (unsigned)frac);
  }
}

/** 频率: 0.001Hz -> "1234.56" (Hz, 两位小数) */
static void fmt_freq(char *buf, size_t n, uint32_t mhz)
{
  uint32_t whole = mhz / 1000U;
  uint32_t frac = (mhz % 1000U) / 10U;
  (void)snprintf(buf, n, "%u.%02u", (unsigned)whole, (unsigned)frac);
}

/** 相角: 0.1度 -> "-120.0" */
static void fmt_phase(char *buf, size_t n, int32_t ddeg)
{
  int32_t a = (ddeg < 0) ? -ddeg : ddeg;
  uint32_t whole = (uint32_t)(a / 10);
  uint32_t frac = (uint32_t)(a % 10);

  if (ddeg < 0)
  {
    (void)snprintf(buf, n, "- %u.%u", (unsigned)whole, (unsigned)frac);
  }
  else
  {
    (void)snprintf(buf, n, "%u.%u", (unsigned)whole, (unsigned)frac);
  }
}

/** 通道字母(0/1/2 -> "UA"/"UB"/"UC") */
static const char *ch_name(uint8_t ch)
{
  static const char *names[3] = { "UA", "UB", "UC" };
  return names[(ch < 3U) ? ch : 0U];
}

/** 相序文本 */
static const char *seq_text(MeasureSeq s)
{
  switch (s)
  {
    case SEQ_ABC:     return "ABC";
    case SEQ_ACB:     return "ACB";
    case SEQ_MISSING: return "MISS";
    default:          return "---";
  }
}

/** 触发方式文本(短) */
static const char *trig_text(uint8_t m)
{
  switch (m)
  {
    case TRIG_AUTO:   return "AUTO";
    case TRIG_NORMAL: return "NORM";
    case TRIG_SINGLE: return "SING";
    default:          return "---";
  }
}

/** 时基文本 */
static const char *tbase_text(uint8_t t)
{
  return (t == (uint8_t)TBASE_CARRIER) ? "CAR" : "FUND";
}

/** 量程文本 */
static const char *range_text(uint8_t r)
{
  switch (r)
  {
    case RANGE_FULL:    return "21.5V";
    case RANGE_HALF:    return "10.8V";
    case RANGE_QUARTER: return "5.4V";
    default:            return "?";
  }
}

/* ----------------------------------------------------------- 状态行/底行 */

static void draw_status_line(const UiSnapshot *s)
{
  char buf[24];

  OLED_SetFont(kk_font_small12);
  OLED_SetFontPosition(OLED_FONT_POS_TOP);

  /* 左: 模式徽标 */
  (void)OLED_DrawUTF8(0, 2, (s->mode == (uint8_t)MODE_TP) ? "3PH" : "SCOPE");

  /* 中: 频率(三相用 A 相; 单路用当前通道) */
  {
    uint8_t ch = (s->sel_ch < 3U) ? s->sel_ch : 0U;
    fmt_freq(buf, sizeof(buf), s->meas.ch[ch].freq_mhz);
    (void)OLED_DrawUTF8(40, 2, buf);
    (void)OLED_DrawUTF8(78, 2, "Hz");
  }

  /* 右: 自检错误优先报警, 否则显示相序(三相)或触发方式(单路) */
  if (s->adc_err != 0U)
  {
    (void)OLED_DrawUTF8(104, 2, "ERR");
  }
  else if (s->mode == (uint8_t)MODE_TP)
  {
    (void)OLED_DrawUTF8(100, 2, seq_text(s->meas.seq));
  }
  else
  {
    (void)OLED_DrawUTF8(100, 2, trig_text(s->trig_mode));
  }

  /* 分隔线 */
  OLED_DrawHLine(0, UI_SEP_Y, OLED_W);
}

static void draw_bottom_line(const UiSnapshot *s)
{
  char buf[24];
  OLED_SetFont(kk_font_small12);
  OLED_SetFontPosition(OLED_FONT_POS_TOP);

  if (s->mode == (uint8_t)MODE_TP)
  {
    /* 左: 不平衡度(0.1% 单位 -> 一位小数) */
    uint32_t u = s->meas.unbalance_dp;
    (void)snprintf(buf, sizeof(buf), "U%u.%u%%", (unsigned)(u / 10U), (unsigned)(u % 10U));
    (void)OLED_DrawUTF8(0, UI_BOTTOM_Y, buf);

    /* 中: 帧数/采样率状态(上板验证用; 冻结时显示 HOLD) */
    if (s->frozen != 0U)
    {
      (void)OLED_DrawUTF8(52, UI_BOTTOM_Y, "HOLD");
    }
    else
    {
      (void)OLED_DrawUTF8(52, UI_BOTTOM_Y, tbase_text(s->tbase_idx));
    }

    /* 右: 编辑态标记 */
    if (s->editing != 0U)
    {
      (void)OLED_DrawUTF8(104, UI_BOTTOM_Y, "EDIT");
    }
    else
    {
      (void)OLED_DrawUTF8(104, UI_BOTTOM_Y, "K1 M");
    }
  }
  else
  {
    /* 单路: 左 Vpp 右 Vrms */
    uint8_t ch = (s->sel_ch < 3U) ? s->sel_ch : 0U;
    fmt_voltage(buf, sizeof(buf), (int32_t)s->meas.ch[ch].vpp_mv);
    (void)OLED_DrawUTF8(0, UI_BOTTOM_Y, buf);
    (void)OLED_DrawUTF8(46, UI_BOTTOM_Y, "Vpp");

    fmt_voltage(buf, sizeof(buf), (int32_t)s->meas.ch[ch].rms_mv);
    (void)OLED_DrawUTF8(72, UI_BOTTOM_Y, buf);
    (void)OLED_DrawUTF8(118, UI_BOTTOM_Y, "V");

    if (s->frozen != 0U)
    {
      (void)OLED_DrawUTF8(60, UI_BOTTOM_Y, "HOLD");
    }
  }
}

/* ------------------------------------------------------- 三相 · 数值页 */

void UI_DrawTpSummary(const UiSnapshot *s)
{
  char buf[24];

  draw_status_line(s);

  /* --- 焦点通道: 24px 大字 --- */
  {
    uint8_t ch = (s->sel_ch < 3U) ? s->sel_ch : 0U;
    int32_t rms = (int32_t)s->meas.ch[ch].rms_mv;
    int32_t a = (rms < 0) ? -rms : rms;

    OLED_SetFont(kk_font_num24);
    OLED_SetFontPosition(OLED_FONT_POS_TOP);

    /* 只显示伏特整数 + 一位小数(24px 数字最多约 6 个字形) */
    (void)snprintf(buf, sizeof(buf), "%u.%u",
                   (unsigned)(a / 1000), (unsigned)((a % 1000) / 100));
    (void)OLED_DrawUTF8(6, UI_BIG_Y0, buf);

    /* 单位与通道名(小字) */
    OLED_SetFont(kk_font_small12);
    (void)OLED_DrawUTF8(96, UI_BIG_Y0 + 8, "V");
    (void)OLED_DrawUTF8(108, UI_BIG_Y0, ch_name(ch));

    if (s->meas.ch[ch].valid == 0U)
    {
      (void)OLED_DrawUTF8(6, UI_BIG_Y0 + 26, "no signal");
    }
  }

  OLED_DrawHLine(0, UI_SEP2_Y, OLED_W);

  /* --- 另两相: 通道名 + 值 + 相角 --- */
  {
    uint8_t ch = (s->sel_ch < 3U) ? s->sel_ch : 0U;
    uint8_t others[2];
    uint8_t n = 0U;

    for (uint8_t i = 0U; i < 3U; ++i)
    {
      if (i != ch) { others[n++] = i; }
    }

    OLED_SetFont(kk_font_small12);
    OLED_SetFontPosition(OLED_FONT_POS_TOP);

    for (uint8_t k = 0U; k < n; ++k)
    {
      uint8_t c = others[k];
      int16_t x = (k == 0U) ? 0 : 64;

      (void)OLED_DrawUTF8(x, UI_ROW2_Y, ch_name(c));

      fmt_voltage(buf, sizeof(buf), (int32_t)s->meas.ch[c].rms_mv);
      (void)OLED_DrawUTF8((int16_t)(x + 18), UI_ROW2_Y, buf);
      (void)OLED_DrawUTF8((int16_t)(x + 48), UI_ROW2_Y, "V");

      fmt_phase(buf, sizeof(buf), s->meas.ch[c].phase_ddeg);
      (void)OLED_DrawUTF8((int16_t)(x + 18), (int16_t)(UI_ROW2_Y + 12), buf);
    }
  }

  draw_bottom_line(s);
}

/* --------------------------------------------------------- 波形绘制 */

/** 把 ADC 码映射到波形带内的 y 坐标 */
static int16_t code_to_y(uint16_t code, uint8_t range_idx)
{
  /* 以 1.65V 偏置为中心, 按量程档缩放; 满量程 = 3000 码跨度 */
  int32_t span;
  int32_t d;
  int32_t half = (int32_t)(UI_WAVE_H / 2U);
  int32_t mid = (int32_t)UI_WAVE_Y0 + half;
  int32_t bias = (int32_t)(((uint32_t)AFE_BIAS_MV * ((uint32_t)AFE_ADC_MAX + 1U)) / AFE_VREF_MV);

  switch (range_idx)
  {
    case RANGE_FULL:    span = 3400; break;
    case RANGE_HALF:    span = 1700; break;
    default:            span = 850;  break;
  }

  d = (int32_t)code - bias;
  /* y = mid - d * half / (span/2) */
  return (int16_t)(mid - ((d * half * 2) / span));
}

/** 画一条波形轨迹(按点数抽样到屏宽) */
static void draw_trace(const uint16_t *buf, uint16_t points, uint8_t range_idx, uint8_t ch)
{
  int16_t x;
  int16_t prev_y = 0;
  uint8_t have_prev = 0U;

  if ((buf == 0) || (points == 0U))
  {
    return;
  }

  /* 3 通道用不同纵向偏移错开, 避免完全重叠看不清(三相共时间轴) */
  int16_t y_off = (int16_t)((int16_t)ch * 0);

  for (x = 0; x < (int16_t)OLED_W; ++x)
  {
    uint32_t idx = ((uint32_t)x * (uint32_t)points) / (uint32_t)OLED_W;
    int16_t y;

    if (idx >= points) { idx = (uint32_t)points - 1U; }
    y = (int16_t)(code_to_y(buf[idx], range_idx) + y_off);

    if (y < (int16_t)UI_WAVE_Y0) { y = (int16_t)UI_WAVE_Y0; }
    if (y > (int16_t)UI_WAVE_Y1) { y = (int16_t)UI_WAVE_Y1; }

    if (have_prev != 0U)
    {
      OLED_DrawLine((int16_t)(x - 1), prev_y, x, y);
    }
    else
    {
      OLED_DrawPixel(x, y);
      have_prev = 1U;
    }
    prev_y = y;
  }
}

/** 波形带的分格虚线 */
static void draw_grid(void)
{
  for (int16_t x = 0; x < (int16_t)OLED_W; x += 16)
  {
    for (int16_t y = (int16_t)UI_WAVE_Y0; y <= (int16_t)UI_WAVE_Y1; y += 4)
    {
      OLED_DrawPixel(x, y);
    }
  }
}

void UI_DrawTpWave(const UiSnapshot *s)
{
  draw_status_line(s);

  OLED_SetClipWindow(0, (int16_t)UI_WAVE_Y0, OLED_W, UI_WAVE_H);

  draw_grid();
  for (uint8_t ch = 0U; ch < 3U; ++ch)
  {
    if (s->wave[ch] != 0)
    {
      draw_trace(s->wave[ch], s->wave_points, s->range_idx, ch);
    }
  }

  OLED_ResetClipWindow();

  /* 波形带内左下角标通道名, 便于区分三条轨迹 */
  OLED_SetFont(kk_font_small12);
  OLED_SetFontPosition(OLED_FONT_POS_TOP);
  (void)OLED_DrawUTF8(0, (int16_t)(UI_WAVE_Y0 + 1), "A");
  (void)OLED_DrawUTF8(6, (int16_t)(UI_WAVE_Y0 + 1), "B");
  (void)OLED_DrawUTF8(12, (int16_t)(UI_WAVE_Y0 + 1), "C");
  (void)OLED_DrawUTF8(88, (int16_t)(UI_WAVE_Y0 + 1), range_text(s->range_idx));

  draw_bottom_line(s);
}

void UI_DrawScopeWave(const UiSnapshot *s)
{
  uint8_t ch = (s->sel_ch < 3U) ? s->sel_ch : 0U;

  draw_status_line(s);

  OLED_SetClipWindow(0, (int16_t)UI_WAVE_Y0, OLED_W, UI_WAVE_H);
  draw_grid();

  if (s->wave[ch] != 0)
  {
    draw_trace(s->wave[ch], s->wave_points, s->range_idx, 0U);
  }

  OLED_ResetClipWindow();

  /* 触发电平虚线(相对偏置的 mV -> y) */
  {
    int32_t bias_mv = (int32_t)AFE_BIAS_MV;
    int32_t level = s->trig_level_mv;
    int32_t span;
    int32_t half = (int32_t)(UI_WAVE_H / 2U);
    int32_t mid = (int32_t)UI_WAVE_Y0 + half;
    int32_t y;

    switch (s->range_idx)
    {
      case RANGE_FULL:    span = 3400; break;
      case RANGE_HALF:    span = 1700; break;
      default:            span = 850;  break;
    }

    (void)bias_mv;
    y = mid - ((level * half * 2) / span);
    if ((y >= (int32_t)UI_WAVE_Y0) && (y <= (int32_t)UI_WAVE_Y1))
    {
      for (int16_t x = 0; x < (int16_t)OLED_W; x += 4)
      {
        OLED_DrawHLine(x, (int16_t)y, 2);
      }
    }
  }

  /* 状态行下方补一行: 通道/时基/量程 */
  OLED_SetFont(kk_font_small12);
  OLED_SetFontPosition(OLED_FONT_POS_TOP);
  (void)OLED_DrawUTF8(0, (int16_t)(UI_WAVE_Y0 + 1),
                      (ch == 0U) ? "A" : ((ch == 1U) ? "B" : "C"));
  (void)OLED_DrawUTF8(78, (int16_t)(UI_WAVE_Y0 + 1), tbase_text(s->tbase_idx));
  (void)OLED_DrawUTF8(104, (int16_t)(UI_WAVE_Y0 + 1), range_text(s->range_idx));

  draw_bottom_line(s);
}

void UI_DrawScopeMeter(const UiSnapshot *s)
{
  char buf[24];
  uint8_t ch = (s->sel_ch < 3U) ? s->sel_ch : 0U;

  draw_status_line(s);

  /* 左上: 通道名 */
  OLED_SetFont(kk_font_small12);
  OLED_SetFontPosition(OLED_FONT_POS_TOP);
  (void)OLED_DrawUTF8(0, UI_BIG_Y0, ch_name(ch));

  /* 中间: Vrms 大字 */
  {
    int32_t rms = (int32_t)s->meas.ch[ch].rms_mv;
    int32_t a = (rms < 0) ? -rms : rms;

    OLED_SetFont(kk_font_num24);
    (void)snprintf(buf, sizeof(buf), "%u.%u",
                   (unsigned)(a / 1000), (unsigned)((a % 1000) / 100));
    (void)OLED_DrawUTF8(24, UI_BIG_Y0, buf);
    OLED_SetFont(kk_font_small12);
    (void)OLED_DrawUTF8(104, (int16_t)(UI_BIG_Y0 + 8), "Vrms");
  }

  OLED_DrawHLine(0, UI_SEP2_Y, OLED_W);

  /* 下面: Vpp / Vmin / Vmax / 频率 */
  OLED_SetFont(kk_font_small12);
  OLED_SetFontPosition(OLED_FONT_POS_TOP);

  fmt_voltage(buf, sizeof(buf), (int32_t)s->meas.ch[ch].vpp_mv);
  (void)OLED_DrawUTF8(0, UI_ROW2_Y, "Vpp");
  (void)OLED_DrawUTF8(24, UI_ROW2_Y, buf);

  fmt_freq(buf, sizeof(buf), s->meas.ch[ch].freq_mhz);
  (void)OLED_DrawUTF8(76, UI_ROW2_Y, buf);
  (void)OLED_DrawUTF8(114, UI_ROW2_Y, "Hz");

  draw_bottom_line(s);
}

/* --------------------------------------------------------------- 调度 */

void UI_Init(void)
{
  memset(&s_snap, 0, sizeof(s_snap));
  s_snap.wave_points = UI_WAVE_POINTS;

  OLED_Init();
  OLED_SetFont(kk_font_small12);
  OLED_SetFontPosition(OLED_FONT_POS_TOP);
  OLED_Clear();

  s_inited = 1U;
}

void UI_PublishWave(const uint16_t *ch0, const uint16_t *ch1, const uint16_t *ch2,
                    uint16_t points, uint32_t fs_hz, uint32_t frame_seq)
{
  s_snap.wave[0] = ch0;
  s_snap.wave[1] = ch1;
  s_snap.wave[2] = ch2;
  s_snap.wave_points = points;
  s_snap.frame_rate_hz = fs_hz;
  s_snap.frame_seq = frame_seq;
}

void UI_PublishMeasure(const MeasureResult *m)
{
  if (m != 0)
  {
    s_snap.meas = *m;
  }
}

void UI_PublishMode(const ModeContext *ctx, AppMode mode, uint8_t switch_phase)
{
  if (ctx != 0)
  {
    s_snap.view      = ctx->view;
    s_snap.sel_ch    = ctx->sel_ch;
    s_snap.range_idx = ctx->range_idx;
    s_snap.tbase_idx = ctx->tbase_idx;
    s_snap.trig_mode = ctx->trig_mode;
    s_snap.frozen    = ctx->frozen;
    s_snap.editing   = ctx->editing;
    s_snap.focus     = ctx->focus;
  }
  s_snap.mode = (uint8_t)mode;
  s_snap.switch_phase = switch_phase;
}

void UI_SetAdcError(uint8_t err)   { s_snap.adc_err = err; }
void UI_SetFrameCount(uint32_t n)  { s_snap.frame_count = n; }

int32_t Mode_TrigLevelMv(void);   /* 来自 app_mode.c */

const UiSnapshot *UI_Snapshot(void) { return &s_snap; }

uint16_t UI_FramePeriodMs(void)
{
  /* 波形页用快节拍, 其余用慢节拍 */
  int is_wave = 0;

  if (s_snap.mode == (uint8_t)MODE_TP)
  {
    is_wave = (s_snap.view == (uint8_t)TP_VIEW_WAVE);
  }
  else
  {
    is_wave = (s_snap.view == (uint8_t)SC_VIEW_WAVE);
  }

  return (is_wave != 0) ? (uint16_t)UI_FAST_MS : (uint16_t)UI_NORM_MS;
}

uint8_t UI_Render(uint8_t force_full)
{
  if (s_inited == 0U)
  {
    return 0U;
  }

  /* 驱动忙则丢弃本帧(不排队), 保证 UI 延迟有界 */
  if ((force_full == 0U) && (OLED_IsBusy()))
  {
    return 0U;
  }

  /* 触发电平从模式上下文取(由 app 在 PublishMode 前写入快照) */
  s_snap.trig_level_mv = (int16_t)Mode_TrigLevelMv();

  /* 每帧从空白重画: KK_OLED 只发脏页, 所以这是"差分刷新"的前提 */
  OLED_Clear();

  if (s_snap.mode == (uint8_t)MODE_TP)
  {
    if (s_snap.view == (uint8_t)TP_VIEW_WAVE) { UI_DrawTpWave(&s_snap); }
    else                                      { UI_DrawTpSummary(&s_snap); }
  }
  else
  {
    if (s_snap.view == (uint8_t)SC_VIEW_METER) { UI_DrawScopeMeter(&s_snap); }
    else                                       { UI_DrawScopeWave(&s_snap); }
  }

  /* 模式切换提示(长按进度 / 已切换) */
  if (s_snap.switch_phase != 0U)
  {
    OLED_DrawBox(30, 24, 68, 16);
    OLED_SetFont(kk_font_small12);
    (void)OLED_DrawUTF8(36, 28, (s_snap.switch_phase == 2U) ? "SWITCH!" : "SWITCH?");
  }

  return (OLED_UpdateDMA() == OLED_OK) ? 1U : 0U;
}
