/**
 * @file ui_test_helpers.h
 * @brief 供宿主机版面测试使用的辅助入口声明
 *
 * 只能被 tests/ 下的测试包含。生产固件不引用它。
 */
#ifndef __UI_TEST_HELPERS_H
#define __UI_TEST_HELPERS_H

#include "ui.h"
#include "app_mode.h"

/** 初始化画布(等价于 UI_Init) */
void UI_Init_ForTest(void);

/** 清空绘制缓冲 */
void UI_Clear_ForTest(void);

/** 按 mode+view 画一页(测试用分发器) */
void UI_DrawAny_ForTest(const UiSnapshot *s, AppMode mode, uint8_t view);

#endif /* __UI_TEST_HELPERS_H */
