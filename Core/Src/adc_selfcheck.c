/**
 * @file adc_selfcheck.c
 * @brief ADC/DMA 配置回读判定实现(纯整数, 不引用 HAL)
 *
 * 位定义依据 RM0008:
 *   ADC_CR2: ADON=bit0, CONT=bit1, CAL=bit2, DMA=bit8, EXTSEL=bits[19:17], EXTTRIG=bit20
 *   ADC_CR1: SCAN=bit8
 *   ADC_SQR1: L=bits[23:20]  (= 规则序列长度 - 1)
 *   DMA_CCR: EN=bit0
 */
#include "adc_selfcheck.h"

#define CR1_SCAN_BIT       (1UL << 8)
#define CR2_ADON_BIT       (1UL << 0)
#define CR2_CONT_BIT       (1UL << 1)
#define CR2_CAL_BIT        (1UL << 2)
#define CR2_DMA_BIT        (1UL << 8)
#define CR2_EXTTRIG_BIT    (1UL << 20)
#define CR2_EXTSEL_SHIFT   17U
#define CR2_EXTSEL_MASK    (0x7UL << CR2_EXTSEL_SHIFT)
#define SQR1_L_SHIFT       20U
#define SQR1_L_MASK        (0xFUL << SQR1_L_SHIFT)
#define DMA_CCR_EN_BIT     (1UL << 0)

uint16_t AdcSelfCheck_Evaluate(const AdcRegImage *reg)
{
  uint16_t err = ADC_CHK_OK;

  if (reg == 0)
  {
    return ADC_CHK_ERR_ADON;
  }

  /* --- 上电与校准 --- */
  if ((reg->cr2 & CR2_ADON_BIT) == 0UL)
  {
    err |= ADC_CHK_ERR_ADON;
  }
  /* 校准位在启动校准后由硬件清零; 这里要求它已结束(即未在忙) */
  if ((reg->cr2 & CR2_CAL_BIT) != 0UL)
  {
    err |= ADC_CHK_ERR_CAL;
  }

  /* --- DMA: 这一位是"读数冻结"沉默故障的源头 --- */
  if ((reg->cr2 & CR2_DMA_BIT) == 0UL)
  {
    err |= ADC_CHK_ERR_CR2_DMA;
  }
  if ((reg->dma_ccr & DMA_CCR_EN_BIT) == 0UL)
  {
    err |= ADC_CHK_ERR_DMA_DIS;
  }

  /* --- 触发: 必须外部触发 + 源是 TIM3_TRGO --- */
  if ((reg->cr2 & CR2_EXTTRIG_BIT) == 0UL)
  {
    err |= ADC_CHK_ERR_CR2_EXTTRIG;
  }
  {
    uint32_t extsel = (reg->cr2 & CR2_EXTSEL_MASK) >> CR2_EXTSEL_SHIFT;
    /* 触发源字段必须等于期望值(固件传 ADC_EXTERNALTRIGCONV_T3_TRGO 的编码)。
     * 这里**不能**拿 CR2 自比 —— 那样永远相等, 等于没检。 */
    if (extsel != reg->expect_extsel)
    {
      err |= ADC_CHK_ERR_EXTI_SWSTART;
    }
  }

  /* --- 单次模式不应开 CONTINUOUS(否则采样率不再由 TIM 决定) --- */
  if ((reg->cr2 & CR2_CONT_BIT) != 0UL)
  {
    err |= ADC_CHK_ERR_CR2_CONT;
  }

  /* --- 扫描模式与序列长度 --- */
  if ((reg->cr1 & CR1_SCAN_BIT) == 0UL)
  {
    err |= ADC_CHK_ERR_SCAN;
  }
  {
    uint32_t len_m1 = (reg->sqr1 & SQR1_L_MASK) >> SQR1_L_SHIFT;
    if ((len_m1 + 1UL) != reg->expect_ch)
    {
      err |= ADC_CHK_ERR_LEN;
    }
  }

  return err;
}
