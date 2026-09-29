/**
 * @file ui.h
 * @brief 三页面绘制与刷新调度(三相数值页 / 三相波形页 / 单路示波页 / 仪表页)
 *
 * 刷新策略(对应实施方案 §5.2):
 *  - **双缓冲 + 差分刷新**: 每帧从空白重画, KK_OLED 只发脏页。
 *  - **局部裁剪**: 波形绘制限制在波形带内, 把脏页稳定在 1~2 页。
 *  - **异步发送**: OLED_UpdateDMA(); 若 OLED_IsBusy() 为真则**丢弃本帧**
 *    (不排队), 保证 UI 延迟有界、不积压。
 *  - **三档节拍**: 波形页 33ms(30fps) / 数值页 100ms(10Hz) / 按键事件当拍。
 *  - 测量结果以"只读快照 + 序号"发布, UI 读序号一致的快照, 避免撕裂。
 */
#ifndef __UI_H
#define __UI_H

#include <stdint.h>

#include "measure.h"
#include "app_mode.h"

/** 一帧波形数据(已抽取); 由 app 在产出新帧时发布 */
#define UI_WAVE_POINTS  256U

/** UI 状态快照: 测量结果 + 模式上下文 + 元信息 */
typedef struct
{
  MeasureResult meas;                 /* 测量结果 */
  uint8_t  mode;                      /* AppMode */
  uint8_t  view;                      /* 子视图 */
  uint8_t  sel_ch;                    /* 焦点通道 */
  uint8_t  range_idx;                 /* 量程档 */
  uint8_t  tbase_idx;                 /* 时基档 */
  uint8_t  trig_mode;                 /* 触发方式 */
  int16_t  trig_level_mv;             /* 触发电平(输入侧 mV) */
  uint8_t  frozen;                    /* 冻结 */
  uint8_t  editing;                   /* 编辑态 */
  uint8_t  focus;                     /* 调整焦点 */
  uint8_t  switch_phase;              /* 模式切换进度(0/1/2) */
  uint8_t  adc_err;                   /* ADC 自检错误位(非 0 时状态行报警) */
  uint32_t frame_seq;                 /* 帧序号(判撕裂) */
  uint32_t frame_rate_hz;             /* 本帧有效采样率 */
  uint32_t frame_count;               /* 累计帧数(上板验证) */

  /* 波形数据: [通道][点] 原始 ADC 码 */
  const uint16_t *wave[3];
  uint16_t wave_points;
} UiSnapshot;

/** 初始化 UI(设置字体、清屏) */
void UI_Init(void);

/** 发布一帧新波形(只存指针, 不拷贝) */
void UI_PublishWave(const uint16_t *ch0, const uint16_t *ch1, const uint16_t *ch2,
                    uint16_t points, uint32_t fs_hz, uint32_t frame_seq);

/** 发布测量结果(拷贝标量) */
void UI_PublishMeasure(const MeasureResult *m);

/** 设置模式/交互状态 */
void UI_PublishMode(const ModeContext *ctx, AppMode mode, uint8_t switch_phase);

/** 设置 ADC 自检错误位(非 0 时状态行显示 ERR) */
void UI_SetAdcError(uint8_t err);

/** 设置累计帧数 */
void UI_SetFrameCount(uint32_t n);

/**
 * @brief 按当前快照重绘并**异步**提交
 * @param force_full 1 = 强制整屏重绘(模式切换后用, 避免残影)
 * @return 1 = 已提交; 0 = 因驱动忙而丢弃本帧
 */
uint8_t UI_Render(uint8_t force_full);

/** 当前应使用的刷新周期(ms): 波形页用 UI_FAST_MS, 其余 UI_NORM_MS */
uint16_t UI_FramePeriodMs(void);

/** 只读快照(供宿主测试逐像素测量版面) */
const UiSnapshot *UI_Snapshot(void);

/* ---- 供宿主测试直接调用的绘制函数(不依赖 HAL, 只依赖 KK_OLED 画布 API) ---- */

void UI_DrawTpSummary(const UiSnapshot *s);
void UI_DrawTpWave(const UiSnapshot *s);
void UI_DrawScopeWave(const UiSnapshot *s);
void UI_DrawScopeMeter(const UiSnapshot *s);

#endif /* __UI_H */
