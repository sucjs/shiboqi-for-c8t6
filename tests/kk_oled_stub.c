/**
 * @file kk_oled_stub.c
 * @brief 在宿主机上替代 KK_OLED 的 **driver 层**, 保留 graphics 层真实渲染
 *
 * 这样 test_ui_layout / test_ui_font 就能用**真实的 ui.c + 真实 kk_oled 画布**
 * 渲染一帧, 然后逐像素测量版面 —— 而不是只检查"函数被调用了"。
 * 只有 I2C 发送被替换成记录到一个标志里。
 */
#include "kk_oled_driver.h"

#include <string.h>

/** 最近一次提交是否发生过(供测试断言"确实提交了") */
static int s_written;
static int s_busy;

OLED_Status OLED_DriverInit(void)
{
  return OLED_OK;
}

OLED_Status OLED_DriverWriteBlocking(void)
{
  s_written = 1;
  return OLED_OK;
}

OLED_Status OLED_DriverWriteIT(void)
{
  s_written = 1;
  return OLED_OK;
}

OLED_Status OLED_DriverWriteDMA(void)
{
  s_written = 1;
  return OLED_OK;
}

bool OLED_DriverIsBusy(void)
{
  return (s_busy != 0) ? true : false;
}

OLED_Status OLED_DriverSetContrast(uint8_t value)
{
  (void)value;
  return OLED_OK;
}

OLED_Status OLED_DriverSetPowerSave(bool enable)
{
  (void)enable;
  return OLED_OK;
}

void OLED_DriverHandleMemTxComplete(void) { }
void OLED_DriverHandleError(void) { }

/* ---- 测试辅助 ---- */
int  KkOledStub_WasWritten(void) { return s_written; }
void KkOledStub_Reset(void)      { s_written = 0; s_busy = 0; }
void KkOledStub_SetBusy(int b)   { s_busy = b; }
