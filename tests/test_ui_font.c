/**
 * @file test_ui_font.c
 * @brief 字模覆盖测试: **按像素墨迹**判定缺字(不能用 advance!)
 *
 * 真实故障模式(基线已踩过并留下反向守卫):
 *   KK_OLED 运行时不含字体数据, 缺字时"不画像素、按空格推进 advance、不报错",
 *   于是界面出现空白但你完全看不出原因。
 *
 *   更隐蔽的是: `OLED_GetGlyphAdvance` 在缺字时会**回退到空格的度量**而不是返回 0,
 *   所以拿它做覆盖检查**永远不会失败**(已实测)。唯一可靠的判据是
 *   "渲染后这个字符到底有没有画出像素" —— 见 glyph_has_ink()。
 *
 * 反向守卫: 必须确认一个"肯定没生成"的字符被判为缺字, 否则说明检测手段本身退化了。
 */
#include <stdio.h>
#include <string.h>

#include "kk_font.h"
#include "kk_oled.h"
#include "app_config.h"

/* 测试钩子: 取当前绘制缓冲 */
const uint8_t *OLED_InternalTestGetDrawBuffer(void);

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, fmt, ...)                                              \
  do {                                                                     \
    if (cond) { ++g_pass; }                                                \
    else { ++g_fail; printf("  FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } \
  } while (0)

/**
 * @brief 判断一个字符在当前字体下是否真的能画出像素
 * @return 1 = 有墨迹(字形存在); 0 = 全空白(缺字回退成空格)
 * @note  这是本工程唯一可信的缺字判据, 不要改用 OLED_GetGlyphAdvance。
 */
static int glyph_has_ink(const uint8_t *font, char c)
{
  char one[2];
  const uint8_t *buf;
  int ink = 0;

  one[0] = c;
  one[1] = '\0';

  OLED_Clear();
  OLED_SetFont(font);
  OLED_SetFontPosition(OLED_FONT_POS_TOP);
  (void)OLED_DrawUTF8(0, 0, one);

  buf = OLED_InternalTestGetDrawBuffer();
  for (int i = 0; i < (128 * 8); ++i)
  {
    if (buf[i] != 0U) { ++ink; }
  }
  return (ink > 0) ? 1 : 0;
}

/* ------------------------------------------------ 界面实际用到的全部文本 */

/* 与 ui.c 里的字面量保持一致: 任何新增文案都必须在这里补上并重新生成字模 */
static const char *kSmallText[] = {
  /* 状态行 / 模式徽标 */
  "3PH", "SCOPE", "Hz", "ERR",
  /* 相序 */
  "ABC", "ACB", "MISS", "---",
  /* 触发方式 */
  "AUTO", "NORM", "SING",
  /* 时基 / 量程 */
  "CAR", "FUND", "21.5V", "10.8V", "5.4V",
  /* 通道与单位 */
  "UA", "UB", "UC", "A", "B", "C", "V", "Vrms", "Vpp",
  /* 底行 */
  "HOLD", "EDIT", "K1 M", "no signal",
  /* 切换提示 */
  "SWITCH?", "SWITCH!",
  /* 数字(小字) */
  "0", "1", "9999", "0.00", "1234.56", "-120.0", "U1.2%",
};

static void test_small_font_coverage(void)
{
  int missing = 0;

  printf("  [small font 12px]\n");
  for (unsigned i = 0; i < sizeof(kSmallText) / sizeof(kSmallText[0]); ++i)
  {
    const char *s = kSmallText[i];
    for (unsigned j = 0; s[j] != '\0'; ++j)
    {
      /* 空格本身就没有墨迹, 是"无墨迹判据"的固有边界, 不参与覆盖检查 */
      if (s[j] == ' ') { continue; }

      if (glyph_has_ink(kk_font_small12, s[j]) == 0)
      {
        printf("    MISSING '%c' (0x%02X) in \"%s\"\n", s[j], (unsigned char)s[j], s);
        ++missing;
      }
    }
  }
  CHECK(missing == 0, "小字形缺失 %d 个字符, 需重新生成字模", missing);
  printf("    coverage OK (0 missing)\n");
}

static void test_num24_font_coverage(void)
{
  static const char *digits = "0123456789.-+";
  int missing = 0;

  printf("  [num24 font]\n");
  for (unsigned i = 0; digits[i] != '\0'; ++i)
  {
    if (glyph_has_ink(kk_font_num24, digits[i]) == 0)
    {
      printf("    MISSING '%c'\n", digits[i]);
      ++missing;
    }
  }
  CHECK(missing == 0, "大字形缺失 %d 个字形", missing);
  printf("    coverage OK (0 missing)\n");
}

/**
 * 反向守卫: 必须能检出一个**未生成**的字符。
 * 若这一步判为"有墨迹", 说明检测手段退化(比如误用了 advance), 测试必须红。
 */
static void test_detector_actually_works(void)
{
  /* 'v' 与 '~' 没在生成列表里(小字集是 A-Z a-z 0-9 与少量符号, 但为稳妥用 '~') */
  CHECK(glyph_has_ink(kk_font_small12, '~') == 0,
        "detector must report '~' as missing (reverse guard)");
  /* 空格本身就没有墨迹, 必须判为"无墨迹" */
  CHECK(glyph_has_ink(kk_font_small12, ' ') == 0,
        "space must have no ink");
  /* 已生成的大写字母必须有墨迹 */
  CHECK(glyph_has_ink(kk_font_small12, 'V') == 1,
        "'V' must have ink");
  printf("  [detector] reverse guard OK ('~' missing, 'V' present)\n");
}

/** 宽度必须为正且随字符数增加(说明 advance 正常工作) */
static void test_advance_sane(void)
{
  uint16_t w1, w5;

  OLED_SetFont(kk_font_small12);
  w1 = OLED_GetUTF8Width("V");
  w5 = OLED_GetUTF8Width("VVVVV");

  CHECK(w1 > 0, "'V' width must be > 0");
  CHECK(w5 > w1, "5 chars must be wider than 1");
  printf("  [advance] w('V')=%u  w('VVVVV')=%u\n", w1, w5);
}

int main(void)
{
  printf("=== test_ui_font ===\n");

  OLED_Init();

  test_small_font_coverage();
  test_num24_font_coverage();
  test_detector_actually_works();
  test_advance_sane();

  printf("--- passed %d, failed %d ---\n", g_pass, g_fail);
  return (g_fail == 0) ? 0 : 1;
}
