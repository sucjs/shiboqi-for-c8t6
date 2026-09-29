/**
 * @file app.c
 * @brief 装配与 1ms 控制循环: 按键 → 模式路由 → 采集/测量 → 显示分档
 *
 * 时序与并发约定:
 *  - TIM4(1kHz) 中断只推进毫秒计数; 主循环按时间差判断节拍。
 *  - DMA 半满/满中断只置标志; 抽取与测量都在主循环做(不阻塞 ISR)。
 *  - 显示按三档节拍: 波形页 33ms / 数值页 100ms / 按键事件当拍。
 *  - 时刻比较用 (now - last) 形式, 天然处理毫秒计数回绕。
 */
#include "app.h"
#include "app_config.h"
#include "app_mode.h"
#include "buttons.h"
#include "adc.h"
#include "tim.h"
#include "measure.h"
#include "ui.h"

#include "stm32f1xx_hal.h"

#include <string.h>

/* ------------------------------------------------------------------ 状态 */

/** 按键 GPIO: K1..K4 = PB0/PB1/PB5/PB6, 内部上拉, 低有效 */
#define KEY_PORT        GPIOB
#define KEY_PIN_MODE    GPIO_PIN_0
#define KEY_PIN_SEL     GPIO_PIN_1
#define KEY_PIN_ADJ     GPIO_PIN_5
#define KEY_PIN_OK      GPIO_PIN_6

#define LED_PORT        GPIOC
#define LED_PIN         GPIO_PIN_13

/** 抽取后的帧缓冲(双缓冲: 一个给测量, 一个给 UI 画) */
static uint16_t s_frame[2][3][FRAME_POINTS];
static uint8_t  s_frame_idx;          /* 当前测量用哪个 */
static uint32_t s_frame_seq;

static MeasureResult s_meas;
static uint8_t  s_adc_err;

static uint32_t s_last_ui_ms;
static uint32_t s_last_measure_ms;
static uint32_t s_last_led_ms;

/* ------------------------------------------------------------------ 按键 */

static void app_key_gpio_init(void)
{
  GPIO_InitTypeDef g = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  g.Pin  = KEY_PIN_MODE | KEY_PIN_SEL | KEY_PIN_ADJ | KEY_PIN_OK;
  g.Mode = GPIO_MODE_INPUT;
  g.Pull = GPIO_PULLUP;               /* 低电平有效 */
  HAL_GPIO_Init(KEY_PORT, &g);

  /* 状态灯 PC13(推挽) */
  g.Pin   = LED_PIN;
  g.Mode  = GPIO_MODE_OUTPUT_PP;
  g.Pull  = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_PORT, &g);
  HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);   /* 高 = 灭 */
}

/** 读一次 4 路按键, 返回"是否按下"的位掩码(bit0..3 = K1..K4) */
static uint8_t app_read_keys(void)
{
  uint8_t m = 0U;

  if (HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN_MODE) == GPIO_PIN_RESET) { m |= 0x01U; }
  if (HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN_SEL)  == GPIO_PIN_RESET) { m |= 0x02U; }
  if (HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN_ADJ)  == GPIO_PIN_RESET) { m |= 0x04U; }
  if (HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN_OK)   == GPIO_PIN_RESET) { m |= 0x08U; }

  return m;
}

/* ---------------------------------------------------- 模式/时基 → 采集参数 */

/** 由模式与时基档推出 (扫描通道数, 触发 ARR, 抽取因子) */
static void app_acq_params(uint8_t *scan_ch, uint16_t *arr, uint8_t *decim)
{
  const ModeContext *ctx = Mode_Context();
  AppMode mode = Mode_Current();

  if (mode == MODE_TP)
  {
    *scan_ch = ADC_SCAN_CH_3PH;
    if (ctx->tbase_idx == (uint8_t)TBASE_CARRIER)
    {
      *arr = TRIG_ARR_CARRIER_3PH;   /* 240kHz */
      *decim = DECIM_CARRIER;
    }
    else
    {
      *arr = TRIG_ARR_FUND;          /* 48kHz -> 24kSPS */
      *decim = DECIM_FUND;
    }
  }
  else
  {
    *scan_ch = ADC_SCAN_CH_1CH;
    if (ctx->tbase_idx == (uint8_t)TBASE_CARRIER)
    {
      *arr = TRIG_ARR_CARRIER_1CH;   /* 720kHz */
      *decim = DECIM_CARRIER;
    }
    else
    {
      *arr = TRIG_ARR_FUND;
      *decim = DECIM_FUND;
    }
  }
}

static void app_reconfig_acq(void)
{
  uint8_t  scan_ch;
  uint16_t arr;
  uint8_t  decim;

  app_acq_params(&scan_ch, &arr, &decim);
  ADC_Configure(scan_ch, arr);
  ADC_Start();
}

/* ------------------------------------------------------------------ 初始化 */

void App_Init(void)
{
  app_key_gpio_init();

  Buttons_Init();
  Mode_Init();

  MX_TIM_Init();                       /* TIM4 节拍 + TIM3 采样触发 */
  MX_ADC_Init(ADC_SCAN_CH_3PH);        /* 默认三相 */

  UI_Init();

  /* 上电自检: 回读 ADC/DMA 寄存器, 把"沉默故障"变成可见报警 */
  s_adc_err = (uint8_t)ADC_SelfCheck();
  UI_SetAdcError(s_adc_err);

  s_frame_idx  = 0U;
  s_frame_seq  = 0U;
  s_last_ui_ms = TIM_Millis();

  memset(&s_meas, 0, sizeof(s_meas));
  memset(s_frame, 0, sizeof(s_frame));
}

uint8_t App_GetAdcError(void) { return s_adc_err; }

/* ------------------------------------------------------------------- 循环 */

/** 处理一路按键事件 */
static void app_handle_key_event(uint8_t key)
{
  ButtonEvent ev = Buttons_Poll((KeyId)key);

  if (ev == BTN_EVENT_NONE)
  {
    return;
  }

  ModeAction a = Mode_HandleKey(key, (uint8_t)ev);
  if (a.need_reconfig != 0U)
  {
    app_reconfig_acq();
  }
  if (a.need_redraw != 0U)
  {
    /* 按键事件当拍立即刷新(force_full = 模式切换过 -> 整屏重绘防残影) */
    (void)UI_Render(a.switched != 0U);
    s_last_ui_ms = TIM_Millis();
  }
}

/** 采集 → 抽取 → 测量 → 发布给 UI */
static void app_poll_measure(void)
{
  uint8_t  scan_ch;
  uint16_t arr;
  uint8_t  decim;
  uint32_t fs = 0U;
  uint16_t *frame[3];

  /* 冻结时不再更新测量(波形与数值都停住) */
  if (Mode_Context()->frozen != 0U)
  {
    return;
  }

  app_acq_params(&scan_ch, &arr, &decim);

  (void)scan_ch;
  {
    uint8_t next = (uint8_t)(1U - s_frame_idx);
    frame[0] = s_frame[next][0];
    frame[1] = s_frame[next][1];
    frame[2] = s_frame[next][2];

    if (ADC_PollFrame(frame, decim, TIM_Sampling_GetRate(), &fs) == 0U)
    {
      return;   /* 本拍无新帧 */
    }
    s_frame_idx = next;
  }

  ++s_frame_seq;

  /* 测量(纯逻辑, 主循环执行) */
  {
    const uint16_t *raw[3];
    raw[0] = s_frame[s_frame_idx][0];
    raw[1] = s_frame[s_frame_idx][1];
    raw[2] = s_frame[s_frame_idx][2];

    Measure_Frame(raw, (Mode_Current() == MODE_TP) ? 3U : 1U,
                  fs, s_frame_seq, &s_meas);
  }

  /* 发布给 UI(只存指针, 不拷贝) */
  UI_PublishWave(s_frame[s_frame_idx][0],
                 s_frame[s_frame_idx][1],
                 s_frame[s_frame_idx][2],
                 FRAME_POINTS, fs, s_frame_seq);
  UI_PublishMeasure(&s_meas);
}

void App_Loop(void)
{
  uint32_t now = TIM_Millis();
  uint8_t  keys = app_read_keys();

  /* --- 1. 按键: 每 1ms 喂一次(消抖/长按/连发都在 buttons.c 里) --- */
  Buttons_Update(keys);
  Mode_Tick((uint8_t)((keys & 0x01U) ? 1U : 0U));   /* K1 用于长按进度提示 */

  for (uint8_t k = 0U; k < (uint8_t)KEY_NUM; ++k)
  {
    app_handle_key_event(k);
  }

  /* --- 2. 采集与测量: 每 1ms 尝试一次(有帧就处理) --- */
  {
    static uint32_t last_probe_ms;
    if ((uint32_t)(now - last_probe_ms) >= 1U)
    {
      last_probe_ms = now;
      app_poll_measure();
    }
  }

  /* --- 3. 显示: 按当前页面的节拍 --- */
  {
    static uint8_t last_mode_view = 0xFFU;
    const ModeContext *ctx = Mode_Context();
    uint8_t key_view = (uint8_t)((Mode_Current() << 4) | (ctx->view & 0x0FU));

    if ((uint32_t)(now - s_last_ui_ms) >= (uint32_t)UI_FramePeriodMs())
    {
      UI_PublishMode(ctx, Mode_Current(), (uint8_t)Mode_SwitchPhase());
      UI_SetFrameCount(ADC_GetFrameCount());
      (void)UI_Render((key_view != last_mode_view) ? 1U : 0U);
      last_mode_view = key_view;
      s_last_ui_ms = now;
    }
  }

  /* --- 4. 状态灯: 有帧在跑就每 500ms 翻转, 便于肉眼确认"没死" --- */
  if ((uint32_t)(now - s_last_led_ms) >= 500U)
  {
    s_last_led_ms = now;
    HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
  }

  (void)s_last_measure_ms;
}
