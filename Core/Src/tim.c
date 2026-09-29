/**
 * @file tim.c
 * @brief TIM3(采样触发, 仅 TRGO) + TIM4(1kHz 控制节拍) 实现
 *
 * TIM3 只用于产生 TRGO, **不配置任何输出通道**:
 *   - PB0/PB1 在 LQFP48 上同时是 TIM3_CH3/CH4 的复用脚, 但本工程用它们做按键;
 *     因为不配置通道(不设 CCER/CCMR 输出位), 引脚保持 GPIO 功能, 不冲突。
 *   - TRGO 选 Update 事件(UG), 于是 ADC 每个定时器周期收到一次触发。
 *
 * 采样率与 24kHz 载波的整数倍关系(见 app_config.h):
 *   三相 载波视图: ARR=299  -> 240.0kHz = 24k x 10
 *   单路 载波视图: ARR=99   -> 720.0kHz = 24k x 30
 * 整数倍是"抽取后载波精确零点"的前提。
 */
#include "tim.h"
#include "app_config.h"

#include "main.h"   /* Error_Handler */

static TIM_HandleTypeDef htim3;   /* 采样触发 */
static TIM_HandleTypeDef htim4;   /* 控制节拍 */

static volatile uint32_t s_millis;
static uint32_t s_sample_rate_hz;

void MX_TIM_Init(void)
{
  TIM_ClockConfigTypeDef  clk = {0};
  TIM_MasterConfigTypeDef mst = {0};

  /* 定时器时钟: APB1 预分频 != 1 时, APB1 定时器时钟 = APB1CLK x2 = 72MHz */
  uint32_t tim_clk = APP_TIMER_HZ;

  /* ------------------------- TIM3: 采样触发 ------------------------- */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 0;                       /* 计数时钟 = 72MHz */
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = TRIG_ARR_CARRIER_3PH;       /* 默认三相载波视图 */
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }

  clk.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &clk) != HAL_OK)
  {
    Error_Handler();
  }

  /* TRGO = Update 事件 -> 每次溢出输出一个触发脉冲给 ADC */
  mst.MasterOutputTrigger = TIM_TRGO_UPDATE;
  mst.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &mst) != HAL_OK)
  {
    Error_Handler();
  }

  s_sample_rate_hz = tim_clk / ((uint32_t)TRIG_ARR_CARRIER_3PH + 1U);

  /* ------------------------- TIM4: 1kHz 节拍 ------------------------- */
  htim4.Instance = TIM4;
  htim4.Init.Prescaler = 0;
  htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim4.Init.Period = TIM4_ARR;                   /* 72e6/(999+1) = 1000Hz */
  htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim4) != HAL_OK)
  {
    Error_Handler();
  }

  /* 使能 TIM4 更新中断(控制节拍) */
  HAL_NVIC_SetPriority(TIM4_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(TIM4_IRQn);

  /* 启动 TIM3(产生触发) 与 TIM4(产生节拍) */
  if (HAL_TIM_Base_Start(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_Base_Start_IT(&htim4) != HAL_OK)
  {
    Error_Handler();
  }
}

void TIM_Sampling_SetRate(uint16_t arr)
{
  uint32_t tim_clk = APP_TIMER_HZ;

  /* 先停 TIM3, 避免改 ARR 期间产生半个周期; 再改; 再清标志重启 */
  (void)HAL_TIM_Base_Stop(&htim3);

  __HAL_TIM_SET_PRESCALER(&htim3, 0);
  __HAL_TIM_SET_AUTORELOAD(&htim3, (uint32_t)arr);
  __HAL_TIM_SET_COUNTER(&htim3, 0U);
  __HAL_TIM_CLEAR_FLAG(&htim3, TIM_FLAG_UPDATE);

  s_sample_rate_hz = tim_clk / ((uint32_t)arr + 1U);

  (void)HAL_TIM_Base_Start(&htim3);
}

uint32_t TIM_Sampling_GetRate(void)
{
  return s_sample_rate_hz;
}

uint32_t TIM_Millis(void)
{
  return s_millis;
}

void TIM_ControlTick(void)
{
  ++s_millis;
}

/* -------------------- 中断入口(由 stm32f1xx_it.c 调用) -------------------- */

void TIM4_IRQHandler(void)
{
  /* 只处理更新事件 */
  if (__HAL_TIM_GET_FLAG(&htim4, TIM_FLAG_UPDATE) != RESET)
  {
    if (__HAL_TIM_GET_IT_SOURCE(&htim4, TIM_IT_UPDATE) != RESET)
    {
      __HAL_TIM_CLEAR_IT(&htim4, TIM_IT_UPDATE);
      TIM_ControlTick();
    }
  }
}
