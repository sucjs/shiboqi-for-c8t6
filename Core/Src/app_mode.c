/**
 * @file app_mode.c
 * @brief 模式/子视图状态机实现: 纯逻辑, 不引用 HAL
 *
 * 防误触的实现方式(与实施方案 §3.4 对齐, 四层都在这里落地):
 *  1. 时间: 长按门槛由 buttons.c 的 BTN_LONGPRESS_MS 提供, 远高于机械抖动。
 *  2. 上下文: `s_ctx[cur].editing` 为非 0 时, K1 长按被解释为"取消编辑", 不切模式。
 *  3. 提交时机: ButtonEvent 的 LONG **只在上抬起沿产生**, 所以按住不放不会反复切换;
 *     中途松手得到的是 SHORT, 天然"完全取消"且无副作用。
 *  4. 视觉: Mode_Tick() 依据按住时长推进 SwitchPhase, UI 画进度条;
 *     未达阈值前 SwitchPhase=SWITCH_ARMED, 不改任何状态。
 */
#include "app_mode.h"
#include "buttons.h"
#include "app_config.h"

#include <string.h>

/** 触发电平千分比的上限(±1000 表示满量程) */
#define TRIG_LEVEL_MAX 1000
/** 触发电平每次加/减的步长(千分比) */
#define TRIG_LEVEL_STEP 50

static AppMode      s_mode;
static ModeContext  s_ctx[MODE_COUNT];
static SwitchPhase  s_phase;
static uint16_t     s_k1_hold_ms;   /* K1 已按住时长(ms), 用于进度条 */

/* ------------------------------------------------------------ 上下文默认值 */

/** 某模式的出厂默认上下文 */
static void mode_context_defaults(ModeContext *c, AppMode m)
{
  memset(c, 0, sizeof(*c));

  if (m == MODE_TP)
  {
    c->view      = (uint8_t)TP_VIEW_SUMMARY;
    c->sel_ch    = 0U;              /* 焦点 A 相 */
    /* 三相默认用**基波视图**: 相序/相位是三相的首要判据, 不需要载波细节 */
    c->tbase_idx = (uint8_t)TBASE_FUND;
  }
  else
  {
    c->view      = (uint8_t)SC_VIEW_WAVE;
    c->sel_ch    = 0U;
    /* 单路默认用**载波视图**: 单路模式的意义就是放大看载波 */
    c->tbase_idx = (uint8_t)TBASE_CARRIER;
  }

  c->range_idx  = (uint8_t)RANGE_FULL;
  c->trig_mode  = (uint8_t)TRIG_AUTO;
  c->trig_level = 0;                /* 0V 触发 */
  c->frozen     = 0U;
  c->focus      = (uint8_t)FOCUS_TBASE;
  c->editing    = 0U;
}

void Mode_Init(void)
{
  s_mode = MODE_TP;
  s_phase = SWITCH_IDLE;
  s_k1_hold_ms = 0U;

  for (uint8_t m = 0U; m < (uint8_t)MODE_COUNT; ++m)
  {
    mode_context_defaults(&s_ctx[m], (AppMode)m);
  }
}

AppMode Mode_Current(void) { return s_mode; }

const ModeContext *Mode_Context(void) { return &s_ctx[s_mode]; }

const ModeContext *Mode_ContextOf(AppMode m)
{
  if ((uint8_t)m >= (uint8_t)MODE_COUNT)
  {
    return &s_ctx[MODE_TP];
  }
  return &s_ctx[m];
}

SwitchPhase Mode_SwitchPhase(void) { return s_phase; }

int32_t Mode_TrigLevelMv(void)
{
  const ModeContext *c = &s_ctx[s_mode];
  /* 千分比 -> 输入侧 mV: level/1000 * 满量程 */
  return (int32_t)(((int64_t)c->trig_level * (int64_t)AFE_AC_FULLSCALE_MV) / (TRIG_LEVEL_MAX));
}

/* -------------------------------------------------------- 模式内的小工具 */

static uint8_t view_count(AppMode m)
{
  return (m == MODE_TP) ? (uint8_t)TP_VIEW_COUNT : (uint8_t)SC_VIEW_COUNT;
}

static void next_view(ModeContext *c, AppMode m)
{
  uint8_t n = view_count(m);
  c->view = (uint8_t)((c->view + 1U) % n);
}

static uint8_t ch_count_for_mode(AppMode m)
{
  return (m == MODE_TP) ? 3U : 3U; /* 单路也可切换监测 A/B/C */
}

static void next_channel(ModeContext *c, AppMode m)
{
  uint8_t n = ch_count_for_mode(m);
  uint8_t total = (m == MODE_TP) ? (uint8_t)(n + 1U) : n; /* 三相多一个 ALL */

  c->sel_ch = (uint8_t)((c->sel_ch + 1U) % total);
}

/** K4 在编辑态下的"加档": 改当前焦点参数 */
static uint8_t adjust_focus_up(ModeContext *c)
{
  uint8_t changed = 0U;

  switch ((AdjustFocus)c->focus)
  {
    case FOCUS_RANGE:
      c->range_idx = (uint8_t)((c->range_idx + 1U) % (uint8_t)RANGE_COUNT);
      changed = 1U;
      break;

    case FOCUS_TBASE:
      c->tbase_idx = (uint8_t)((c->tbase_idx + 1U) % (uint8_t)TBASE_COUNT);
      changed = 1U;  /* 时基变了 -> 需要重配采样 */
      break;

    case FOCUS_TRIG_LEVEL:
      if (c->trig_level < (TRIG_LEVEL_MAX - TRIG_LEVEL_STEP))
      {
        c->trig_level = (int16_t)(c->trig_level + TRIG_LEVEL_STEP);
      }
      break;

    case FOCUS_TRIG_MODE:
      c->trig_mode = (uint8_t)((c->trig_mode + 1U) % (uint8_t)TRIG_COUNT);
      break;

    default:
      break;
  }

  return changed;
}

/** K3 短按: 循环移动调整焦点 */
static void adjust_focus_next(ModeContext *c)
{
  c->focus = (uint8_t)((c->focus + 1U) % (uint8_t)FOCUS_COUNT);
}

/* ------------------------------------------------------------- 模式切换 */

/**
 * 事务式切换: 目标上下文本来就在 s_ctx[] 里(活对象), 只需换索引 +
 * 清理瞬态 + 强制整屏重绘。失败不留半成品。
 */
static void mode_commit_switch(void)
{
  AppMode target = (s_mode == MODE_TP) ? MODE_SCOPE : MODE_TP;

  s_mode = target;
  s_ctx[target].editing = 0U;   /* 进来时一律不在编辑态, 避免状态含糊 */
  s_phase = SWITCH_IDLE;
  s_k1_hold_ms = 0U;
}

/* ------------------------------------------------------------------ 按键 */

ModeAction Mode_HandleKey(uint8_t key, uint8_t ev)
{
  ModeAction act;
  ModeContext *c = &s_ctx[s_mode];

  act.need_redraw = 0U;
  act.need_reconfig = 0U;
  act.switched = 0U;

  switch ((KeyId)key)
  {
    /* ---------------- K1 MODE: 短按切子视图 / 长按切模式 ---------------- */
    case KEY_MODE:
      if (ev == (uint8_t)BTN_EVENT_SHORT)
      {
        /* 编辑态下短按无意义, 忽略, 避免"手滑退出编辑" */
        if (c->editing == 0U)
        {
          next_view(c, s_mode);
          act.need_redraw = 1U;
        }
      }
      else if (ev == (uint8_t)BTN_EVENT_LONG)
      {
        if (c->editing != 0U)
        {
          /* 上下文优先级: 编辑态长按 K1 = 取消编辑(回退) */
          c->editing = 0U;
          act.need_redraw = 1U;
        }
        else
        {
          mode_commit_switch();
          act.need_redraw = 1U;
          act.need_reconfig = 1U;   /* 两种模式采样率不同 */
          act.switched = 1U;
        }
      }
      else
      {
        /* REPEAT 不参与主切换 */
      }
      break;

    /* ---------------- K2 SEL: 短按选通道 / 长按连发 ---------------- */
    case KEY_SEL:
      if ((ev == (uint8_t)BTN_EVENT_SHORT) || (ev == (uint8_t)BTN_EVENT_REPEAT))
      {
        next_channel(c, s_mode);
        act.need_redraw = 1U;
      }
      break;

    /* ---------------- K3 ADJ: 短按移焦点 / 长按进出编辑态 ------------- */
    case KEY_ADJ:
      if (ev == (uint8_t)BTN_EVENT_SHORT)
      {
        adjust_focus_next(c);
        act.need_redraw = 1U;
      }
      else if (ev == (uint8_t)BTN_EVENT_LONG)
      {
        c->editing = (uint8_t)((c->editing == 0U) ? 1U : 0U);
        act.need_redraw = 1U;
      }
      else
      {
        /* REPEAT 不参与, 避免误入编辑 */
      }
      break;

    /* ---------------- K4 OK: 编辑态加档 / 非编辑态冻结 ---------------- */
    case KEY_OK:
      if (c->editing != 0U)
      {
        if (ev == (uint8_t)BTN_EVENT_LONG)
        {
          /* 编辑态长按 = 取消并回退(此处语义为退出编辑, 不做数值回滚) */
          c->editing = 0U;
          act.need_redraw = 1U;
        }
        else if ((ev == (uint8_t)BTN_EVENT_SHORT) || (ev == (uint8_t)BTN_EVENT_REPEAT))
        {
          if (adjust_focus_up(c) != 0U)
          {
            act.need_reconfig = 1U;
          }
          act.need_redraw = 1U;
        }
        else
        {
          /* none */
        }
      }
      else
      {
        if (ev == (uint8_t)BTN_EVENT_SHORT)
        {
          c->frozen = (uint8_t)((c->frozen == 0U) ? 1U : 0U);
          act.need_redraw = 1U;
        }
        else if (ev == (uint8_t)BTN_EVENT_LONG)
        {
          /* 非编辑态长按 = 复位视图到默认页 */
          mode_context_defaults(c, s_mode);
          act.need_redraw = 1U;
          act.need_reconfig = 1U;
        }
        else
        {
          /* none */
        }
      }
      break;

    default:
      break;
  }

  return act;
}

/* ------------------------------------------------------------- 长按进度 */

void Mode_Tick(uint8_t k1_down)
{
  ModeContext *c = &s_ctx[s_mode];

  /* 编辑态下不显示切模式进度(长按 K1 是取消编辑) */
  if ((k1_down == 0U) || (c->editing != 0U))
  {
    s_phase = SWITCH_IDLE;
    s_k1_hold_ms = 0U;
    return;
  }

  if (s_k1_hold_ms < BTN_LONGPRESS_MS)
  {
    s_k1_hold_ms++;
  }

  s_phase = (s_k1_hold_ms >= BTN_LONGPRESS_MS) ? SWITCH_READY : SWITCH_ARMED;
}
