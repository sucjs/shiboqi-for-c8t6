/**
 * @file app_mode.h
 * @brief 模式/子视图状态机 + 按键路由 + 上下文保存恢复(纯逻辑, 不依赖 HAL)
 *
 * 设计要点(对应实施方案 §3/§4):
 *  - 主切换(三相 <-> 单路)**唯一入口是 K1 长按**, 且编辑态下 K1 长按 = 取消编辑。
 *  - 子视图切换是 K1 短按(与主切换分离)。
 *  - 长按期间只发"进度"事件, 松手沿才提交; 中途松手无副作用。
 *  - 每个模式持有独立上下文, 切换时整体换用, 因此恢复瞬时且无残留。
 */
#ifndef __APP_MODE_H
#define __APP_MODE_H

#include <stdint.h>

/** 顶层模式 */
typedef enum
{
  MODE_TP = 0,     /* 三相测试 */
  MODE_SCOPE,      /* 单路示波器 */
  MODE_COUNT
} AppMode;

/** 三相加子视图 */
typedef enum
{
  TP_VIEW_SUMMARY = 0,  /* 数值页 */
  TP_VIEW_WAVE,         /* 波形页 */
  TP_VIEW_COUNT
} TpView;

/** 单路子视图 */
typedef enum
{
  SC_VIEW_WAVE = 0,     /* 波形页 */
  SC_VIEW_METER,        /* 仪表页 */
  SC_VIEW_COUNT
} ScView;

/** 时基档(载波视图 / 基波视图) */
typedef enum
{
  TBASE_CARRIER = 0,    /* 高采样率, 看 PWM 载波细节 */
  TBASE_FUND,           /* 抽取后 24kSPS, 看换相/相序 */
  TBASE_COUNT
} TimeBase;

/** 垂直量程档(相对满量程的除数) */
typedef enum
{
  RANGE_FULL = 0,       /* ±21.5V 满量程 */
  RANGE_HALF,           /* ±10.75V */
  RANGE_QUARTER,        /* ±5.4V */
  RANGE_COUNT
} VRange;

/** 触发方式 */
typedef enum
{
  TRIG_AUTO = 0,
  TRIG_NORMAL,
  TRIG_SINGLE,
  TRIG_COUNT
} TrigMode;

/** 触发/调整焦点(决定 K4 改谁) */
typedef enum
{
  FOCUS_RANGE = 0,
  FOCUS_TBASE,
  FOCUS_TRIG_LEVEL,
  FOCUS_TRIG_MODE,
  FOCUS_COUNT
} AdjustFocus;

/** 每模式独立上下文 */
typedef struct
{
  uint8_t  view;        /* 子视图 */
  uint8_t  sel_ch;      /* 焦点通道: 三相 0..2 或 3=ALL; 单路 0..2 */
  uint8_t  range_idx;
  uint8_t  tbase_idx;
  uint8_t  trig_mode;
  int16_t  trig_level;  /* 触发电平(千分比 -1000..1000) */
  uint8_t  frozen;      /* 冻结 */
  uint8_t  focus;       /* 当前调整焦点 */
  uint8_t  editing;     /* 编辑态 */
} ModeContext;

/** 模式切换的进度阶段(供 UI 画进度条) */
typedef enum
{
  SWITCH_IDLE = 0,      /* 未在长按 */
  SWITCH_ARMED,         /* 长按中, 尚未到阈值 */
  SWITCH_READY          /* 已达阈值, 松手即提交 */
} SwitchPhase;

/** 一次按键处理的结果, 供 app 决定是否立即刷新 */
typedef struct
{
  uint8_t need_redraw;   /* 需要立即重绘 */
  uint8_t need_reconfig; /* 需要重配 ADC/TIM(时基变了) */
  uint8_t switched;      /* 本次发生了模式切换 */
} ModeAction;

/** 初始化(设为三相/数值页, 默认档) */
void Mode_Init(void);

/** 当前模式 / 当前模式下的上下文(只读) */
AppMode Mode_Current(void);
const ModeContext *Mode_Context(void);
/** 取指定模式的上下文(用于切换时读另一份) */
const ModeContext *Mode_ContextOf(AppMode m);

/** 长按切换的当前阶段(UI 用) */
SwitchPhase Mode_SwitchPhase(void);

/**
 * @brief 把一次按键事件喂给状态机
 * @param key 触发事件的路由通道(KEY_MODE / KEY_SEL / KEY_ADJ / KEY_OK)
 * @param ev  事件类型(short / long / repeat)
 * @return 本次处理产生的动作(是否重绘/重配/切换)
 *
 * @note 内部通过 Buttons 的"长按保持"语义判断 K1 进度: 调用者需同时每毫秒
 *       调用 Mode_Tick() 以推进进度条(见下)。
 */
ModeAction Mode_HandleKey(uint8_t key, uint8_t ev);

/**
 * @brief 每 1ms 调用, 喂入 K1 当前是否按下(用于长按进度与中途松手检测)
 */
void Mode_Tick(uint8_t k1_down);

/** 触发电平的千分比 -> 相对偏置的 mV(供 UI 画虚线用) */
int32_t Mode_TrigLevelMv(void);

#endif /* __APP_MODE_H */
