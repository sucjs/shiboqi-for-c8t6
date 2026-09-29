/**
 * @file test_buttons.c
 * @brief 4 路按键状态机的宿主机单元测试(与固件共用同一份 buttons.c)
 *
 * 覆盖: 抖动只产生一个事件、短按/长按判定、连发节奏、**多路互不串扰**。
 *
 * @note 取件时机很重要: 应用每 1ms 取一次事件(与真实控制循环一致)。
 *       若只在松手后一次性取, 队列里只会留下最后那个(权威的)LONG/LONG,
 *       中途的 REPEAT 会被覆盖 —— 这是 buttons_emit 的**设计**:
 *       松手沿判定优先于可重复的 REPEAT, 避免"按住不放刷出一堆 REPEAT
 *       把真正的 LONG 挤掉"。
 */
#include <stdio.h>

#include "buttons.h"
#include "app_config.h"

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    if (cond) { ++g_pass; }                                                 \
    else { ++g_fail; printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); }\
  } while (0)

/** 喂入 ms 毫秒的同一电平, 并像 app 一样每毫秒取走全部事件 */
typedef struct
{
  int shorts;
  int longs;
  int repeats;
} EvCount;

static void feed_collect(int ms, uint8_t mask, KeyId key, EvCount *cnt)
{
  for (int i = 0; i < ms; ++i)
  {
    Buttons_Update(mask);
    ButtonEvent ev;
    while ((ev = Buttons_Poll(key)) != BTN_EVENT_NONE)
    {
      if (ev == BTN_EVENT_SHORT)      { ++cnt->shorts; }
      else if (ev == BTN_EVENT_LONG)  { ++cnt->longs; }
      else                            { ++cnt->repeats; }
    }
  }
}

/** 抖动序列: 连续跳变应该只产生一个 SHORT */
static void test_bounce_single_event(void)
{
  EvCount c = {0, 0, 0};

  Buttons_Init();

  /* 模拟机械抖动: 前 10ms 内电平乱跳, 之后稳定按下 */
  for (int i = 0; i < 10; ++i)
  {
    Buttons_Update((uint8_t)((i & 1) ? 0x01U : 0x00U));
  }
  feed_collect(60, 0x01U, KEY_MODE, &c);   /* 稳定按下 */
  /* 确认已被识别为按下: 消抖后应处于 level=1, 但还没产生事件 */
  feed_collect(60, 0x00U, KEY_MODE, &c);   /* 松开 -> 产生 SHORT */

  CHECK(c.shorts == 1, "bounce must yield exactly one SHORT");
  CHECK(c.longs == 0, "bounce must not yield LONG");
  CHECK(c.repeats == 0, "bounce must not yield REPEAT");
  printf("  [bounce] shorts=%d longs=%d repeats=%d\n", c.shorts, c.longs, c.repeats);
}

/** 长按: >700ms 产生 LONG, 且按住期间产生若干 REPEAT */
static void test_longpress_and_repeat(void)
{
  EvCount c = {0, 0, 0};

  Buttons_Init();
  feed_collect(1000, 0x01U, KEY_MODE, &c);  /* 按住 1s */
  feed_collect(40,   0x00U, KEY_MODE, &c);  /* 松开 */

  CHECK(c.longs == 1, "1s press must yield exactly one LONG");
  CHECK(c.shorts == 0, "1s press must not yield SHORT");
  /* 700ms 之后每 150ms 一次 -> 约 (1000-700)/150 = 2 次 */
  CHECK(c.repeats >= 1, "long press must yield REPEAT(s) while held");
  printf("  [long] shorts=%d longs=%d repeats=%d\n", c.shorts, c.longs, c.repeats);
}

/** 恰好低于长按阈值 -> SHORT */
static void test_borderline_short(void)
{
  EvCount c = {0, 0, 0};

  Buttons_Init();
  feed_collect(600, 0x01U, KEY_MODE, &c);
  feed_collect(40,  0x00U, KEY_MODE, &c);

  CHECK(c.shorts == 1, "600ms must be SHORT");
  CHECK(c.longs == 0, "600ms must not be LONG");
  printf("  [border] 600ms -> shorts=%d longs=%d\n", c.shorts, c.longs);
}

/** 多路互不串扰: 同时按 K1 与 K3, 各自只在自己的队列产生事件 */
static void test_no_crosstalk(void)
{
  EvCount c1 = {0, 0, 0}, c3 = {0, 0, 0};
  uint8_t both = (uint8_t)(0x01U | 0x04U);   /* K1=bit0, K3=bit2 */

  Buttons_Init();

  for (int i = 0; i < 1000; ++i)
  {
    Buttons_Update(both);
    ButtonEvent ev;
    while ((ev = Buttons_Poll(KEY_MODE)) != BTN_EVENT_NONE)
    { if (ev == BTN_EVENT_SHORT) { ++c1.shorts; } else if (ev == BTN_EVENT_LONG) { ++c1.longs; } else { ++c1.repeats; } }
    while ((ev = Buttons_Poll(KEY_ADJ)) != BTN_EVENT_NONE)
    { if (ev == BTN_EVENT_SHORT) { ++c3.shorts; } else if (ev == BTN_EVENT_LONG) { ++c3.longs; } else { ++c3.repeats; } }
  }
  for (int i = 0; i < 40; ++i)
  {
    Buttons_Update(0x00U);
    ButtonEvent ev;
    while ((ev = Buttons_Poll(KEY_MODE)) != BTN_EVENT_NONE)
    { if (ev == BTN_EVENT_SHORT) { ++c1.shorts; } else if (ev == BTN_EVENT_LONG) { ++c1.longs; } else { ++c1.repeats; } }
    while ((ev = Buttons_Poll(KEY_ADJ)) != BTN_EVENT_NONE)
    { if (ev == BTN_EVENT_SHORT) { ++c3.shorts; } else if (ev == BTN_EVENT_LONG) { ++c3.longs; } else { ++c3.repeats; } }
  }

  CHECK(c1.longs == 1, "K1 LONG while K3 also pressed");
  CHECK(c1.shorts == 0, "K1 must not also produce SHORT");
  CHECK(c3.longs == 1, "K3 LONG independent of K1");
  CHECK(c3.shorts == 0, "K3 must not also produce SHORT");
  /* K2/K4 完全没动, 必须没有任何事件 */
  CHECK(Buttons_Poll(KEY_SEL) == BTN_EVENT_NONE, "K2 must be silent");
  CHECK(Buttons_Poll(KEY_OK)  == BTN_EVENT_NONE, "K4 must be silent");
  printf("  [crosstalk] K1(%d,%d) K3(%d,%d) K2/K4 silent\n",
         c1.shorts, c1.longs, c3.shorts, c3.longs);
}

/** 事件只发一次: 取走后必须为空 */
static void test_poll_clears(void)
{
  EvCount c = {0, 0, 0};

  Buttons_Init();
  feed_collect(60, 0x01U, KEY_MODE, &c);
  feed_collect(60, 0x00U, KEY_MODE, &c);

  /* feed_collect 已经取走, 此刻队列必须为空 */
  CHECK(c.shorts == 1, "exactly one SHORT collected");
  CHECK(Buttons_Poll(KEY_MODE) == BTN_EVENT_NONE, "queue empty after drain");
  CHECK(Buttons_HasPending() == 0U, "no pending after drain");
  printf("  [clear] poll is destructive\n");
}

/** HasPending 能反映待处理事件 */
static void test_has_pending(void)
{
  Buttons_Init();
  CHECK(Buttons_HasPending() == 0U, "initially no pending");

  /* 直接喂电平而不取件 */
  for (int i = 0; i < 60; ++i) { Buttons_Update(0x01U); }
  for (int i = 0; i < 60; ++i) { Buttons_Update(0x00U); }

  CHECK(Buttons_HasPending() != 0U, "pending after press");
  (void)Buttons_Poll(KEY_MODE);
  CHECK(Buttons_HasPending() == 0U, "clear after drain");
  printf("  [pending] flag tracks queue\n");
}

int main(void)
{
  printf("=== test_buttons ===\n");
  test_bounce_single_event();
  test_longpress_and_repeat();
  test_borderline_short();
  test_no_crosstalk();
  test_poll_clears();
  test_has_pending();

  printf("--- passed %d, failed %d ---\n", g_pass, g_fail);
  return (g_fail == 0) ? 0 : 1;
}
