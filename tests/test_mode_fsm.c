/**
 * @file test_mode_fsm.c
 * @brief 模式状态机的宿主机单元测试(与固件共用同一份 app_mode.c)
 *
 * 守住实施方案 §3.4 与 §7 的四条关键不变量:
 *   1. 短按 K1 只切子视图, **不切模式**;
 *   2. 编辑态下长按 K1 必须**取消编辑**, 而不是切模式;
 *   3. 长按中途松手必须**无副作用**(仍是原模式);
 *   4. 切换后目标模式上下文**完整恢复**(而不是被源模式污染)。
 * 另加: 按键路由互不串扰、REPEAT 不参与主切换。
 */
#include <stdio.h>
#include <string.h>

#include "app_mode.h"
#include "buttons.h"

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    if (cond) { ++g_pass; }                                                 \
    else { ++g_fail; printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); }\
  } while (0)

/** 模拟一次"短按": 按下->松开(均越过消抖) */
static void press_short(uint8_t key_index)
{
  /* 消抖 20ms: 按住 40ms 保证被确认, 但 < 700ms 所以是短按 */
  for (int i = 0; i < 40; ++i) { Buttons_Update((uint8_t)(1U << key_index)); }
  for (int i = 0; i < 40; ++i) { Buttons_Update(0U); }
}

/** 模拟一次"长按": 按住 over_ms 毫秒后松开 */
static void press_long(uint8_t key_index, int over_ms)
{
  for (int i = 0; i < over_ms; ++i) { Buttons_Update((uint8_t)(1U << key_index)); }
  for (int i = 0; i < 40; ++i) { Buttons_Update(0U); }
}

/** 把 buttons 队列里产生的事件喂给 FSM */
static ModeAction drain_keys(void)
{
  ModeAction total = {0, 0, 0};

  for (uint8_t k = 0U; k < (uint8_t)KEY_NUM; ++k)
  {
    ButtonEvent ev;
    while ((ev = Buttons_Poll((KeyId)k)) != BTN_EVENT_NONE)
    {
      ModeAction a = Mode_HandleKey(k, (uint8_t)ev);
      total.need_redraw   |= a.need_redraw;
      total.need_reconfig |= a.need_reconfig;
      total.switched      |= a.switched;
    }
  }
  return total;
}

static void reset_all(void)
{
  Buttons_Init();
  Mode_Init();
  (void)drain_keys();
}

/* -------------------------------------------------------------- 测试项 */

/** 1. 短按 K1 只切子视图, 不切模式 */
static void test_short_k1_view_only(void)
{
  reset_all();
  CHECK(Mode_Current() == MODE_TP, "start in TP");

  uint8_t v0 = Mode_Context()->view;
  press_short(KEY_MODE);
  ModeAction a = drain_keys();

  CHECK(Mode_Current() == MODE_TP, "short K1 must NOT switch mode");
  CHECK(a.switched == 0U, "short K1 switched flag must be 0");
  CHECK(Mode_Context()->view != v0, "short K1 must change sub-view");
  CHECK(a.need_redraw != 0U, "short K1 must request redraw");
  printf("  [1] short K1: view %u -> %u, mode still TP\n", v0, Mode_Context()->view);
}

/** 2. 非编辑态长按 K1 切模式 */
static void test_long_k1_switches_mode(void)
{
  reset_all();
  press_long(KEY_MODE, 800);
  ModeAction a = drain_keys();

  CHECK(Mode_Current() == MODE_SCOPE, "long K1 must switch TP->SCOPE");
  CHECK(a.switched != 0U, "switched flag must be set");
  CHECK(a.need_reconfig != 0U, "must request reconfig (different sample rate)");

  press_long(KEY_MODE, 800);
  (void)drain_keys();
  CHECK(Mode_Current() == MODE_TP, "long K1 must switch back to TP");
  printf("  [2] long K1 toggles: TP -> SCOPE -> TP\n");
}

/** 3. 编辑态下长按 K1 必须取消编辑, 不能切模式 */
static void test_long_k1_in_editing_cancels(void)
{
  reset_all();

  /* 进入编辑态: K3 长按 */
  press_long(KEY_ADJ, 800);
  (void)drain_keys();
  CHECK(Mode_Context()->editing != 0U, "K3 long must enter editing");

  /* 编辑态下长按 K1 */
  press_long(KEY_MODE, 800);
  ModeAction a = drain_keys();

  CHECK(Mode_Current() == MODE_TP, "editing + long K1 must NOT switch mode");
  CHECK(a.switched == 0U, "editing + long K1 switched flag must be 0");
  CHECK(Mode_Context()->editing == 0U, "editing + long K1 must cancel editing");
  printf("  [3] editing + long K1: cancelled editing, mode still TP\n");
}

/** 4. 长按中途松手必须无副作用 */
static void test_long_k1_abort_has_no_side_effect(void)
{
  reset_all();
  AppMode m0 = Mode_Current();
  uint8_t v0 = Mode_Context()->view;

  /* 按住 400ms (< 700ms 阈值) 后松开 -> 是 SHORT, 不切模式 */
  press_long(KEY_MODE, 400);
  ModeAction a = drain_keys();

  CHECK(Mode_Current() == m0, "aborted long press must not switch mode");
  CHECK(a.switched == 0U, "aborted long press switched must be 0");
  /* 因为是 SHORT, 会切子视图 —— 这是预期且无破坏性的 */
  CHECK(Mode_Context()->view != v0, "aborted press behaves as SHORT (view change)");
  printf("  [4] abort at 400ms: mode unchanged (%d), view changed as SHORT\n", (int)Mode_Current());
}

/** 5. 切换后目标模式上下文完整恢复 */
static void test_context_restored_after_switch(void)
{
  reset_all();

  /* 在三相模式里改一些东西: 切到波形页 + 移焦点 */
  press_short(KEY_MODE);  (void)drain_keys();   /* SUMMAR<->WAVE */
  press_short(KEY_ADJ);   (void)drain_keys();   /* focus */

  uint8_t tp_view = Mode_ContextOf(MODE_TP)->view;
  uint8_t tp_focus = Mode_ContextOf(MODE_TP)->focus;

  /* 切到单路, 再切回来 */
  press_long(KEY_MODE, 800); (void)drain_keys();
  CHECK(Mode_Current() == MODE_SCOPE, "now in SCOPE");
  CHECK(Mode_ContextOf(MODE_SCOPE)->view == (uint8_t)SC_VIEW_WAVE, "SCOPE default view");

  /* 单路模式有自己的默认时基(载波) */
  CHECK(Mode_ContextOf(MODE_SCOPE)->tbase_idx == (uint8_t)TBASE_CARRIER,
        "SCOPE default tbase = CARRIER");
  CHECK(Mode_ContextOf(MODE_TP)->tbase_idx == (uint8_t)TBASE_FUND,
        "TP default tbase = FUND");

  press_long(KEY_MODE, 800); (void)drain_keys();
  CHECK(Mode_Current() == MODE_TP, "back in TP");
  CHECK(Mode_ContextOf(MODE_TP)->view == tp_view, "TP view restored");
  CHECK(Mode_ContextOf(MODE_TP)->focus == tp_focus, "TP focus restored");
  printf("  [5] context restored: TP view=%u focus=%u\n", tp_view, tp_focus);
}

/** 6. REPEAT 不参与主切换, 但可用于选通道 */
static void test_repeat_semantics(void)
{
  reset_all();
  ModeAction a;

  a = Mode_HandleKey((uint8_t)KEY_MODE, (uint8_t)BTN_EVENT_REPEAT);
  CHECK(a.switched == 0U, "K1 REPEAT must not switch mode");
  CHECK(Mode_Current() == MODE_TP, "mode unchanged on K1 REPEAT");

  /* K2 REPEAT 应该步进通道 */
  {
    uint8_t c0 = Mode_Context()->sel_ch;
    a = Mode_HandleKey((uint8_t)KEY_SEL, (uint8_t)BTN_EVENT_REPEAT);
    CHECK(Mode_Context()->sel_ch != c0, "K2 REPEAT must step channel");
    CHECK(a.need_redraw != 0U, "K2 REPEAT must redraw");
  }
  printf("  [6] REPEAT: K1 ignored, K2 steps channel\n");
}

/** 7. 编辑态下 K4 加档; 改时基必须请求重配 */
static void test_editing_k4_adjusts(void)
{
  reset_all();

  /* 进入编辑态 */
  press_long(KEY_ADJ, 800); (void)drain_keys();
  CHECK(Mode_Context()->editing != 0U, "in editing");

  /* 把焦点对准时基(TP 默认 focus 就是 TBASE) */
  CHECK(Mode_Context()->focus == (uint8_t)FOCUS_TBASE, "default focus = TBASE");

  uint8_t t0 = Mode_Context()->tbase_idx;
  ModeAction a = Mode_HandleKey((uint8_t)KEY_OK, (uint8_t)BTN_EVENT_SHORT);

  CHECK(Mode_Context()->tbase_idx != t0, "K4 must change tbase under TBASE focus");
  CHECK(a.need_reconfig != 0U, "changing tbase must request reconfig");
  printf("  [7] editing K4: tbase %u -> %u, reconfig=%u\n",
         t0, Mode_Context()->tbase_idx, a.need_reconfig);
}

/** 8. 非编辑态 K4 短按 = 冻结 */
static void test_k4_freeze(void)
{
  reset_all();
  CHECK(Mode_Context()->frozen == 0U, "not frozen initially");

  (void)Mode_HandleKey((uint8_t)KEY_OK, (uint8_t)BTN_EVENT_SHORT);
  CHECK(Mode_Context()->frozen != 0U, "K4 short must freeze");

  (void)Mode_HandleKey((uint8_t)KEY_OK, (uint8_t)BTN_EVENT_SHORT);
  CHECK(Mode_Context()->frozen == 0U, "K4 short again must unfreeze");
  printf("  [8] K4 freeze toggle works\n");
}

/** 9. 长按进度阶段(UI 用) */
static void test_switch_phase_progress(void)
{
  reset_all();
  CHECK(Mode_SwitchPhase() == SWITCH_IDLE, "phase idle at start");

  /* 按下 100ms -> ARMED, 未到 READY */
  for (int i = 0; i < 100; ++i) { Mode_Tick(1U); }
  CHECK(Mode_SwitchPhase() == SWITCH_ARMED, "phase ARMED before threshold");

  /* 再到 800ms -> READY */
  for (int i = 0; i < 700; ++i) { Mode_Tick(1U); }
  CHECK(Mode_SwitchPhase() == SWITCH_READY, "phase READY at threshold");

  /* 松开 -> IDLE */
  Mode_Tick(0U);
  CHECK(Mode_SwitchPhase() == SWITCH_IDLE, "phase IDLE after release");
  printf("  [9] switch phase progresses ARMED -> READY -> IDLE\n");
}

/** 10. 编辑态下不显示切模式进度(避免误导) */
static void test_phase_suppressed_in_editing(void)
{
  reset_all();
  press_long(KEY_ADJ, 800); (void)drain_keys();
  CHECK(Mode_Context()->editing != 0U, "in editing");

  for (int i = 0; i < 800; ++i) { Mode_Tick(1U); }
  CHECK(Mode_SwitchPhase() == SWITCH_IDLE,
        "phase must stay IDLE in editing (K1 long = cancel editing)");
  printf("  [10] phase suppressed in editing\n");
}

int main(void)
{
  printf("=== test_mode_fsm ===\n");
  test_short_k1_view_only();
  test_long_k1_switches_mode();
  test_long_k1_in_editing_cancels();
  test_long_k1_abort_has_no_side_effect();
  test_context_restored_after_switch();
  test_repeat_semantics();
  test_editing_k4_adjusts();
  test_k4_freeze();
  test_switch_phase_progress();
  test_phase_suppressed_in_editing();

  printf("--- passed %d, failed %d ---\n", g_pass, g_fail);
  return (g_fail == 0) ? 0 : 1;
}
