/**
 * @file adc.c
 * @brief ADC1 扫描 + TIM3_TRGO 触发 + DMA1_Channel1 循环缓冲 + 整数抽取
 *
 * 关键设计决定与理由:
 *  1. **ADCCLK 必须显式设成 /6(12MHz)**。F103 的 ADCCLK 上限是 14MHz, 而
 *     CubeMX 的 .ioc 可能把它算成 36MHz(/2) —— 那是非法的。这里强制
 *     RCC_ADCCLKConfig(RCC_PCLK2_Div6), 并在 ADC_SelfCheck 里回读验证。
 *  2. **DMA1_Channel1 是 ADC1 的硬件固定通道**(RM0008 表 78), 不可自由选择。
 *  3. **循环模式 + 半传输/传输完成中断**: 中断里只置标志, 主循环搬运并抽取,
 *     保证 ISR 极短且不丢点。
 *  4. **抽取用整数均值(boxcar)**: 因为触发率是 24kHz 的整数倍, boxcar 的零点
 *     正好压住 24k/48k/72k..., 抽取后基波视图无需额外滤波器。
 *  5. 原始缓冲按**最大**抽取因子(DECIM_FUND)分配, 所以载波视图(decim=1)只用
 *     一个半缓冲的前 FRAME_POINTS 次扫描; 帧率恒定, 与 decim 无关。
 */
#include "adc.h"
#include "app_config.h"
#include "adc_selfcheck.h"
#include "tim.h"

#include "main.h"   /* Error_Handler */

/* DMA1_Channel1 = ADC1 硬件固定映射 */
#define ADC_DMA_CHANNEL  DMA1_Channel1
#define ADC_DMA_IRQn     DMA1_Channel1_IRQn

#define ADC_NCH_MAX      3U

static ADC_HandleTypeDef hadc1;
static DMA_HandleTypeDef hdma_adc1;

/** 原始 DMA 缓冲(半字), 长度 = 2 x ADC_DMA_SCANS_PER_HALF x nch */
static uint16_t s_raw[ADC_DMA_LEN];

/** 就绪标志: [0] = 前半, [1] = 后半; 由中断置位, 主循环清位 */
static volatile uint8_t s_half_ready[2];
static volatile AcqState s_state = ACQ_IDLE;
static uint32_t s_frame_count;
static uint8_t  s_scan_ch = ADC_SCAN_CH_3PH;

/** 抽取输出帧缓冲(每通道 FRAME_POINTS 个样本) */

/** 偏置码(单路模式下给无数据的通道填这个值) */
static uint16_t adc_bias_code(void)
{
  return (uint16_t)(((uint32_t)AFE_BIAS_MV * ((uint32_t)AFE_ADC_MAX + 1U)) / AFE_VREF_MV);
}

/* ----------------------------------------------------------------- 初始化 */

void MX_ADC_Init(uint8_t scan_ch)
{
  GPIO_InitTypeDef gpio = {0};

  s_scan_ch = (scan_ch == 1U) ? ADC_SCAN_CH_1CH : ADC_SCAN_CH_3PH;

  /* APB2 = 72MHz; /6 -> 12MHz(<=14MHz 上限) */
  __HAL_RCC_ADC_CONFIG(RCC_ADCPCLK2_DIV6);

  /* --- GPIO: PA0/PA1/PA2 模拟输入 --- */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  gpio.Pin  = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* --- DMA1 Channel1 --- */
  __HAL_RCC_DMA1_CLK_ENABLE();

  hdma_adc1.Instance                 = ADC_DMA_CHANNEL;
  hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
  hdma_adc1.Init.Mode                = DMA_CIRCULAR;   /* 循环 -> 双缓冲 */
  hdma_adc1.Init.Priority            = DMA_PRIORITY_HIGH;
  if (HAL_DMA_Init(&hdma_adc1) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

  HAL_NVIC_SetPriority(ADC_DMA_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(ADC_DMA_IRQn);

  /* --- ADC1 --- */
  __HAL_RCC_ADC1_CLK_ENABLE();

  hadc1.Instance                   = ADC1;
  hadc1.Init.ScanConvMode          = (s_scan_ch > 1U) ? ADC_SCAN_ENABLE : ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode    = DISABLE;         /* 由 TIM3_TRGO 触发 */
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv      = ADC_EXTERNALTRIGCONV_T3_TRGO;
  hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion       = s_scan_ch;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  ADC_Configure(s_scan_ch, TRIG_ARR_CARRIER_3PH);
  ADC_Start();
}

void ADC_Configure(uint8_t scan_ch, uint16_t trig_arr)
{
  ADC_ChannelConfTypeDef ch = {0};

  s_scan_ch = (scan_ch == 1U) ? ADC_SCAN_CH_1CH : ADC_SCAN_CH_3PH;

  (void)HAL_ADC_Stop_DMA(&hadc1);

  /* 序列长度: SCAN 位与 SQR1.L 都要跟着改(HAL 的 Init 不会重写这两个) */
  hadc1.Instance->CR1  = (s_scan_ch > 1U) ? ADC_CR1_SCAN : 0U;
  hadc1.Instance->SQR1 = (uint32_t)((s_scan_ch - 1U) << 20);   /* L = N-1 */

  /* 采样时间: 源阻抗约 520Ω, Ts=1.5 允许 734Ω(裕量 1.41x) */
  ch.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;

  ch.Channel = ADC_CHANNEL_0; ch.Rank = 1U; (void)HAL_ADC_ConfigChannel(&hadc1, &ch);
  if (s_scan_ch > 1U)
  {
    ch.Channel = ADC_CHANNEL_1; ch.Rank = 2U; (void)HAL_ADC_ConfigChannel(&hadc1, &ch);
    ch.Channel = ADC_CHANNEL_2; ch.Rank = 3U; (void)HAL_ADC_ConfigChannel(&hadc1, &ch);
  }

  TIM_Sampling_SetRate(trig_arr);
}

void ADC_Start(void)
{
  s_half_ready[0] = 0U;
  s_half_ready[1] = 0U;

  if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)s_raw, ADC_DMA_LEN) != HAL_OK)
  {
    s_state = ACQ_ERROR;
    return;
  }
  s_state = ACQ_RUNNING;
}

void ADC_Stop(void)
{
  (void)HAL_ADC_Stop_DMA(&hadc1);
  s_state = ACQ_IDLE;
}

AcqState ADC_GetState(void)          { return s_state; }
uint32_t ADC_GetFrameCount(void)     { return s_frame_count; }

/* ------------------------------------------------------- 中断: 半缓冲交接 */

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1) { s_half_ready[0] = 1U; }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1) { s_half_ready[1] = 1U; }
}

/* --------------------------------------------------------- 抽取与帧产出 */

/** 单通道整数均值抽取: 把 src 的 decim 个样本平均成一个 */
static uint16_t decimate_avg(const uint16_t *src, uint8_t decim)
{
  uint32_t sum = 0U;
  uint8_t  i;

  for (i = 0U; i < decim; ++i)
  {
    sum += (uint32_t)src[i];
  }
  return (uint16_t)(sum / (uint32_t)decim);
}

uint8_t ADC_PollFrame(uint16_t *out_ch[3], uint8_t decim, uint32_t fs_raw, uint32_t *out_fs)
{
  uint8_t  half;
  uint32_t base;
  uint8_t  ch;
  uint16_t i;

  if (decim == 0U) { decim = 1U; }

  /* 优先取后半(延迟更小); 都没有则无新帧 */
  if (s_half_ready[1] != 0U)      { half = 1U; }
  else if (s_half_ready[0] != 0U) { half = 0U; }
  else { return 0U; }

  base = (half == 0U) ? 0U : (uint32_t)ADC_DMA_SCANS_PER_HALF;

  /* 交错布局 raw[scan * nch + ch]; 每个输出样本由 decim 个连续扫描平均而来。
   * 直接写入调用者的缓冲, 不再经内部中转数组 —— 省 1.5KB RAM。 */
  for (ch = 0U; ch < ADC_NCH_MAX; ++ch)
  {
    if (out_ch[ch] == 0)
    {
      continue;    /* 调用者不需要这一路 */
    }

    if (ch < s_scan_ch)
    {
      for (i = 0U; i < FRAME_POINTS; ++i)
      {
        const uint16_t *src = &s_raw[(base + ((uint32_t)i * (uint32_t)decim)) *
                                    (uint32_t)s_scan_ch + (uint32_t)ch];
        out_ch[ch][i] = decimate_avg(src, decim);
      }
    }
    else
    {
      /* 单路模式下 B/C 无数据: 填偏置码, 避免被误判为"贴轨无信号" */
      uint16_t b = adc_bias_code();
      for (i = 0U; i < FRAME_POINTS; ++i)
      {
        out_ch[ch][i] = b;
      }
    }
  }

  s_half_ready[half] = 0U;
  ++s_frame_count;

  if (out_fs != 0)
  {
    *out_fs = fs_raw / (uint32_t)decim;
  }
  return 1U;
}

/* ------------------------------------------------------------------ 自检 */

uint16_t ADC_SelfCheck(void)
{
  AdcRegImage img;

  img.cr1        = hadc1.Instance->CR1;
  img.cr2        = hadc1.Instance->CR2;
  img.sqr1       = hadc1.Instance->SQR1;
  img.dma_ccr    = hdma_adc1.Instance->CCR;
  img.expect_ch  = s_scan_ch;
  /* ADC_EXTERNALTRIGCONV_T3_TRGO 的编码就是 CR2.EXTSEL 字段的值 */
  img.expect_extsel = (uint32_t)ADC_EXTERNALTRIGCONV_T3_TRGO;

  return AdcSelfCheck_Evaluate(&img);
}

/* ------------------------------------------------- DMA1_Channel1 中断入口 */

void DMA1_Channel1_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_adc1);
}
