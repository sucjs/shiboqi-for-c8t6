/**
 * @file ui_test_helpers.c
 * @brief test_ui_layout 用到的辅助入口(仅在宿主机测试里编译)
 *
 * 这些函数只是把 ui.c 的公开 API 组合成"初始化画布 / 清画布 / 按 mode+view 画一页",
 * 不改变任何生产逻辑, 因此不会让测试与实现漂移。
 */
#include "ui.h"
#include "app_config.h"
#include "kk_oled.h"

void UI_Init_ForTest(void)
{
  UI_Init();
}

void UI_Clear_ForTest(void)
{
  OLED_Clear();
}

void UI_DrawAny_ForTest(const UiSnapshot *s, AppMode mode, uint8_t view)
{
  if (mode == MODE_TP)
  {
    if (view == (uint8_t)TP_VIEW_WAVE) { UI_DrawTpWave(s); }
    else                               { UI_DrawTpSummary(s); }
  }
  else
  {
    if (view == (uint8_t)SC_VIEW_METER) { UI_DrawScopeMeter(s); }
    else                                { UI_DrawScopeWave(s); }
  }
}
