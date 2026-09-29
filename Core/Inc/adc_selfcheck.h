/**
 * @file adc_selfcheck.h
 * @brief ADC/DMA 配置回读自检(纯判定逻辑, 可通过钩子注入寄存器值)
 *
 * 为什么要自检: 原工程踩过一类**沉默故障** —— 漏置 ADC_CR2.DMA 或触发源选错时,
 *   编译 0 警告、HAL 返回 HAL_OK、OLED 界面照常刷新, 但读数全是上电初值(死的)。
 *   只有回读寄存器才能暴露。本模块把"该置哪些位"写成可测试的纯逻辑,
 *   固件侧传真实寄存器值, 宿主机侧传构造好的假值。
 */
#ifndef __ADC_SELFCHECK_H
#define __ADC_SELFCHECK_H

#include <stdint.h>

/** 自检失败原因位 */
#define ADC_CHK_OK              0x0000U
#define ADC_CHK_ERR_CR2_DMA     0x0001U  /* CR2.DMA 未置位 -> 读数冻结 */
#define ADC_CHK_ERR_CR2_CONT    0x0002U  /* 单次模式误开 CONTINUOUS */
#define ADC_CHK_ERR_CR2_EXTTRIG 0x0004U  /* 未开外部触发 -> 不受 TIM 控制 */
#define ADC_CHK_ERR_EXTI_SWSTART 0x0008U /* 触发源不是 TIM3_TRGO */
#define ADC_CHK_ERR_SCAN        0x0010U  /* 扫描模式未开 */
#define ADC_CHK_ERR_LEN         0x0020U  /* 序列长度与通道数不符 */
#define ADC_CHK_ERR_CAL         0x0040U  /* 未完成校准 */
#define ADC_CHK_ERR_ADON        0x0080U  /* 未上电 */
#define ADC_CHK_ERR_DMA_DIS     0x0100U  /* DMA 通道未使能 */

/**
 * @brief 输入: 需要回读的关键寄存器镜像(由固件用真实寄存器填充)
 *
 * @note 全用 uint32 便于宿主机构造; 字段语义与 STM32F1 ADC 寄存器一致。
 */
typedef struct
{
  uint32_t cr2;          /* ADC1->CR2 */
  uint32_t cr1;          /* ADC1->CR1 */
  uint32_t sqr1;         /* ADC1->SQR1 (bits[23:20] = L, 序列长度-1) */
  uint32_t dma_ccr;      /* 对应 DMA 通道的 CCR (bit0 = EN) */
  uint32_t expect_ch;    /* 期望的规则序列通道数 */
  uint32_t expect_extsel;/* 期望的触发源编码(应为 T3_TRGO) */
} AdcRegImage;

/**
 * @brief 判定寄存器配置是否正确
 * @return ADC_CHK_OK 或错误位掩码(可多位并存)
 */
uint16_t AdcSelfCheck_Evaluate(const AdcRegImage *reg);

#endif /* __ADC_SELFCHECK_H */
