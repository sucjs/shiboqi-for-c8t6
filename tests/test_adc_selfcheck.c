/**
 * @file test_adc_selfcheck.c
 * @brief ADC/DMA 自检判定的宿主机单元测试
 *
 * 核心目的: 守住"漏置 ADC_CR2.DMA 导致读数冻结"这类**沉默故障**。
 * 做法: 先构造一个**完全正确**的寄存器镜像(必须判定为 OK), 再逐个破坏每一位,
 *       每一次都必须被单独报出 —— 这样一旦有人改了 adc.c 漏配某位, 测试立刻红。
 */
#include <stdio.h>
#include "adc_selfcheck.h"

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    if (cond) { ++g_pass; }                                                 \
    else { ++g_fail; printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); }\
  } while (0)

/* 期望的寄存器位组合(与固件 adc.c 的配置一一对应) */
#define CR1_SCAN   (1UL << 8)
#define CR2_ADON   (1UL << 0)
#define CR2_DMA    (1UL << 8)
#define CR2_EXTTRG (1UL << 20)
#define EXT_T3_TRGO 2UL          /* T3_TRGO 在 EXTSEL 字段中的编码 */
#define DMA_EN     (1UL << 0)
#define SQR1_LEN3  (2UL << 20)   /* L=2 -> 序列长度 3 */

/** 构造一个完全正确的镜像(三相模式) */
static AdcRegImage make_good(void)
{
  AdcRegImage r;
  r.cr1 = CR1_SCAN;
  r.cr2 = CR2_ADON | CR2_DMA | CR2_EXTTRG | (EXT_T3_TRGO << 17);
  r.sqr1 = SQR1_LEN3;

  r.dma_ccr = DMA_EN;
  r.expect_ch = 3U;
  r.expect_extsel = EXT_T3_TRGO;
  return r;
}

static void test_good_config_passes(void)
{
  AdcRegImage r = make_good();
  uint16_t e = AdcSelfCheck_Evaluate(&r);

  CHECK(e == ADC_CHK_OK, "correct config must report OK");
  printf("  [good] err=0x%04X (expect 0x0000)\n", e);
}

/** 逐个破坏每一位, 每次都必须被报出 —— 这是防"沉默故障"的核心 */
static void test_each_broken_bit_detected(void)
{
  struct { const char *name; uint16_t expect; } cases[] = {
    {"CR2.DMA missing",       ADC_CHK_ERR_CR2_DMA},
    {"DMA channel disabled",  ADC_CHK_ERR_DMA_DIS},
    {"EXTTRIG off",           ADC_CHK_ERR_CR2_EXTTRIG},
    {"wrong trigger source",  ADC_CHK_ERR_EXTI_SWSTART},
    {"SCAN off",              ADC_CHK_ERR_SCAN},
    {"wrong seq length",      ADC_CHK_ERR_LEN},
    {"CONT accidentally on",  ADC_CHK_ERR_CR2_CONT},
    {"ADON off",              ADC_CHK_ERR_ADON},
    {"CAL still busy",        ADC_CHK_ERR_CAL},
  };
  const int n = (int)(sizeof(cases) / sizeof(cases[0]));

  for (int i = 0; i < n; ++i)
  {
    AdcRegImage r = make_good();
    uint16_t e;

    switch (i)
    {
      case 0: r.cr2 &= ~CR2_DMA; break;
      case 1: r.dma_ccr &= ~DMA_EN; break;
      case 2: r.cr2 &= ~CR2_EXTTRG; break;
      case 3: r.cr2 = (r.cr2 & ~(0x7UL << 17)) | (1UL << 17); break;
      case 4: r.cr1 &= ~CR1_SCAN; break;
      case 5: r.sqr1 = (0UL << 20); break;   /* 长度 1, 与 expect 3 不符 */
      case 6: r.cr2 |= (1UL << 1); break;
      case 7: r.cr2 &= ~CR2_ADON; break;
      case 8: r.cr2 |= (1UL << 2); break;
      default: break;
    }

    e = AdcSelfCheck_Evaluate(&r);
    CHECK((e & cases[i].expect) != 0U, cases[i].name);
    printf("  [%02d] %-24s -> err=0x%04X (expect bit 0x%04X)\n",
           i, cases[i].name, e, cases[i].expect);
  }
}

/** 多位同时错必须全部报出(不是只报第一个) */
static void test_multiple_errors_all_reported(void)
{
  AdcRegImage r = make_good();
  uint16_t e;

  r.cr2 &= ~CR2_DMA;      /* 漏 DMA */
  r.cr1 &= ~CR1_SCAN;     /* 漏 SCAN */
  r.dma_ccr = 0UL;        /* DMA 未使能 */

  e = AdcSelfCheck_Evaluate(&r);
  CHECK((e & ADC_CHK_ERR_CR2_DMA) != 0U, "multi: DMA bit");
  CHECK((e & ADC_CHK_ERR_SCAN) != 0U, "multi: SCAN bit");
  CHECK((e & ADC_CHK_ERR_DMA_DIS) != 0U, "multi: DMA EN bit");
  printf("  [multi] err=0x%04X (expect 0x0111)\n", e);
}

/** 单路模式: 序列长度 1 也应通过 */
static void test_single_channel_len(void)
{
  AdcRegImage r = make_good();
  r.sqr1 = (0UL << 20);   /* L=0 -> 长度 1 */
  r.expect_ch = 1U;

  CHECK(AdcSelfCheck_Evaluate(&r) == ADC_CHK_OK, "1-channel config must pass");
  printf("  [1ch] len=1 passes\n");
}

/** 触发源编码写错但 EXTTRIG 开着 -> 必须报出 */
static void test_wrong_extsel(void)
{
  AdcRegImage r = make_good();
  r.cr2 = (r.cr2 & ~(0x7UL << 17)) | (4UL << 17);  /* 写成别的源 */

  uint16_t e = AdcSelfCheck_Evaluate(&r);
  CHECK((e & ADC_CHK_ERR_EXTI_SWSTART) != 0U, "wrong EXTSEL must be reported");
  printf("  [extsel] err=0x%04X\n", e);
}

int main(void)
{
  printf("=== test_adc_selfcheck ===\n");
  test_good_config_passes();
  test_each_broken_bit_detected();
  test_multiple_errors_all_reported();
  test_single_channel_len();
  test_wrong_extsel();

  printf("--- passed %d, failed %d ---\n", g_pass, g_fail);
  return (g_fail == 0) ? 0 : 1;
}
