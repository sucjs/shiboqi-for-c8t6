/**
 * @file buttons.c
 * @brief 4 路按键消抖与事件识别实现
 * @note  纯整数、纯逻辑, 不引用任何 STM32 头文件, 便于宿主机单元测试。
 *
 * 消抖策略(逐路独立):
 *   原始电平每次变化就把该路的"稳定计时"清零, 只有连续 BTN_DEBOUNCE_MS 毫秒
 *   电平不变, 才把它认作有效电平。因此机械抖动的每次跳变都重置计时, 抖动结束后
 *   只产生一次状态迁移, 一次真实的按下-松开只产生一个 SHORT/LONG 事件。
 */
#include "buttons.h"
#include "app_config.h"

/* 消抖计时上限: 稳定到该值后不再累加, 避免无意义增长 */
#define BTN_STABLE_CAP_MS BTN_DEBOUNCE_MS

typedef struct
{
  uint8_t     level;      /* 消抖后的确认电平, 1 = 按下 */
  uint8_t     raw_level;  /* 最近一次喂入的原始电平 */
  uint16_t    stable_ms;  /* raw_level 已保持的时长(ms) */
  uint16_t    hold_ms;    /* 确认按下后已持续的时长(ms) */
  uint16_t    repeat_ms;  /* 距上一次 REPEAT 的时长(ms) */
  ButtonEvent pending;    /* 待取走的事件 */
} KeyState;

static KeyState s_key[KEY_NUM];

void Buttons_Init(void)
{
  for (uint8_t i = 0U; i < (uint8_t)KEY_NUM; ++i)
  {
    s_key[i].level     = 0U;
    s_key[i].raw_level = 0U;
    s_key[i].stable_ms = BTN_STABLE_CAP_MS;
    s_key[i].hold_ms   = 0U;
    s_key[i].repeat_ms = 0U;
    s_key[i].pending   = BTN_EVENT_NONE;
  }
}

/**
 * 排队一个事件。
 *
 * 优先级规则: SHORT/LONG 是**松手沿的权威判定**, 绝不能因为队列里还压着一个
 * 可重复的 REPEAT 而丢失(调用者可能正在重绘, 无法每毫秒及时取走事件)。
 * 因此:
 *   - 空队列 -> 直接入队;
 *   - 队列里的旧事件是 REPEAT -> 被 SHORT/LONG 覆盖;
 *   - 其余情况(已有 SHORT/LONG, 或又来了一个 REPEAT) -> 丢弃新事件。
 * 这样既保证"一次按下-松开恰好一个 SHORT/LONG", 又不像素级依赖调用者及时取件。
 */
static void buttons_emit(KeyId key, ButtonEvent ev)
{
  KeyState *s = &s_key[key];

  if (s->pending == BTN_EVENT_NONE)
  {
    s->pending = ev;
  }
  else if ((s->pending == BTN_EVENT_REPEAT) &&
           ((ev == BTN_EVENT_SHORT) || (ev == BTN_EVENT_LONG)))
  {
    s->pending = ev;
  }
  else
  {
    /* 保留已有的高优先级事件, 丢弃新事件 */
  }
}

/** 单路状态机推进 1ms */
static void buttons_update_one(KeyId key, uint8_t raw)
{
  KeyState *s = &s_key[key];

  /* --- 原始电平变化: 重新开始消抖计时 --- */
  if (raw != s->raw_level)
  {
    s->raw_level = raw;
    s->stable_ms = 0U;
  }
  else if (s->stable_ms < BTN_STABLE_CAP_MS)
  {
    s->stable_ms++;
  }
  else
  {
    /* 已稳定, 无需处理 */
  }

  /* --- 稳定足够久才把原始电平提升为确认电平 --- */
  if ((s->stable_ms >= BTN_DEBOUNCE_MS) && (s->level != s->raw_level))
  {
    s->level = s->raw_level;

    if (s->level != 0U)
    {
      /* 按下沿: 开始计时长按 */
      s->hold_ms   = 0U;
      s->repeat_ms = 0U;
    }
    else
    {
      /* 松开沿: 依据按住时长判定短按/长按 */
      if (s->hold_ms >= BTN_LONGPRESS_MS)
      {
        buttons_emit(key, BTN_EVENT_LONG);
      }
      else
      {
        buttons_emit(key, BTN_EVENT_SHORT);
      }
      s->hold_ms   = 0U;
      s->repeat_ms = 0U;
    }
  }

  /* --- 保持按下: 累计时长, 越过长按阈值后周期性产生 REPEAT --- */
  if (s->level != 0U)
  {
    if (s->hold_ms < 0xFFFFU)
    {
      s->hold_ms++;
    }

    if (s->hold_ms > BTN_LONGPRESS_MS)
    {
      s->repeat_ms++;
      if (s->repeat_ms >= BTN_REPEAT_MS)
      {
        s->repeat_ms = 0U;
        buttons_emit(key, BTN_EVENT_REPEAT);
      }
    }
  }
}

void Buttons_Update(uint8_t mask_pressed)
{
  for (uint8_t i = 0U; i < (uint8_t)KEY_NUM; ++i)
  {
    uint8_t raw = (uint8_t)((mask_pressed >> i) & 0x01U);
    buttons_update_one((KeyId)i, raw);
  }
}

ButtonEvent Buttons_Poll(KeyId key)
{
  ButtonEvent ev;

  if ((uint8_t)key >= (uint8_t)KEY_NUM)
  {
    return BTN_EVENT_NONE;
  }

  ev = s_key[key].pending;
  s_key[key].pending = BTN_EVENT_NONE;
  return ev;
}

uint8_t Buttons_HasPending(void)
{
  for (uint8_t i = 0U; i < (uint8_t)KEY_NUM; ++i)
  {
    if (s_key[i].pending != BTN_EVENT_NONE)
    {
      return 1U;
    }
  }
  return 0U;
}
