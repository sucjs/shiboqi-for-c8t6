cmake_minimum_required(VERSION 3.22)

#
# 宿主机单元测试(不交叉编译, 不依赖 HAL)
#
# 只覆盖**纯逻辑**模块: 这些模块不引用任何 STM32 头文件, 因此可以在 PC 上
# 直接编译运行, 用于在上板之前守住模式/按键/测量/自检的正确性。
#
# 用法:
#   cmake -S tests -B build/tests
#   cmake --build build/tests
#   ctest --test-dir build/tests --output-on-failure
#
project(shiboqi_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

enable_testing()

set(CORE_INC ${CMAKE_CURRENT_SOURCE_DIR}/../Core/Inc)
set(CORE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../Core/Src)

# 被测的纯逻辑源码(与固件共用同一份文件, 避免测试与实现漂移)
set(HOST_LOGIC_SRC
    ${CORE_SRC}/buttons.c
    ${CORE_SRC}/app_mode.c
    ${CORE_SRC}/measure.c
    ${CORE_SRC}/adc_selfcheck.c
)

# ---------------------------------------------------------------------------
# 测量引擎: RMS / 频率 / 相位 / 相序 / 不平衡度
# ---------------------------------------------------------------------------
add_executable(test_measure
    ${CMAKE_CURRENT_SOURCE_DIR}/test_measure.c
    ${CORE_SRC}/measure.c
)
target_include_directories(test_measure PRIVATE ${CORE_INC})
target_compile_options(test_measure PRIVATE -Wall -Wextra)
target_link_libraries(test_measure PRIVATE m)
add_test(NAME measure COMMAND test_measure)

# ---------------------------------------------------------------------------
# 按键状态机: 消抖 / 短按 / 长按 / 连发 / 多路互不串扰
# ---------------------------------------------------------------------------
add_executable(test_buttons
    ${CMAKE_CURRENT_SOURCE_DIR}/test_buttons.c
    ${CORE_SRC}/buttons.c
)
target_include_directories(test_buttons PRIVATE ${CORE_INC})
target_compile_options(test_buttons PRIVATE -Wall -Wextra)
add_test(NAME buttons COMMAND test_buttons)

# ---------------------------------------------------------------------------
# 模式状态机: 切换语义 + 防误触 + 上下文保存恢复
#
# 守住四条不变量: 短按 K1 不切模式; 编辑态长按 K1 = 取消编辑;
# 长按中途松手无副作用; 切换后目标上下文完整恢复。
# ---------------------------------------------------------------------------
add_executable(test_mode_fsm
    ${CMAKE_CURRENT_SOURCE_DIR}/test_mode_fsm.c
    ${CORE_SRC}/app_mode.c
    ${CORE_SRC}/buttons.c
)
target_include_directories(test_mode_fsm PRIVATE ${CORE_INC})
target_compile_options(test_mode_fsm PRIVATE -Wall -Wextra)
add_test(NAME mode_fsm COMMAND test_mode_fsm)

# ---------------------------------------------------------------------------
# ADC/DMA 自检判定
#
# 守住"漏置 ADC_CR2.DMA -> 编译 0 警告、HAL 返回 OK、界面照常刷新、但读数是死的"
# 这类沉默故障: 逐个破坏每位都必须被报出。
# ---------------------------------------------------------------------------
add_executable(test_adc_selfcheck
    ${CMAKE_CURRENT_SOURCE_DIR}/test_adc_selfcheck.c
    ${CORE_SRC}/adc_selfcheck.c
)
target_include_directories(test_adc_selfcheck PRIVATE ${CORE_INC})
target_compile_options(test_adc_selfcheck PRIVATE -Wall -Wextra)
add_test(NAME adc_selfcheck COMMAND test_adc_selfcheck)

# ---------------------------------------------------------------------------
# OLED 版面: 链接**真实的 ui.c + 真实 kk_oled 画布**, 逐像素测量
#
# 版面的重叠/越界/压线编译不报错、看代码也看不出来, 只有渲染真实一帧再测量像素
# 才会暴露。driver 层(真正的 I2C)用 kk_oled_stub.c 替换。
# ---------------------------------------------------------------------------
set(KK_OLED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Core/ThirdParty/kk_oled)

set(KK_OLED_SRC
    ${KK_OLED_DIR}/graphics/kk_oled.c
    ${KK_OLED_DIR}/graphics/kk_oled_draw.c
    ${KK_OLED_DIR}/graphics/kk_oled_font.c
    ${KK_OLED_DIR}/graphics/kk_oled_bitmap.c
)
set(KK_OLED_INC
    ${KK_OLED_DIR}/include
    ${KK_OLED_DIR}/graphics
    ${KK_OLED_DIR}/driver
)

add_executable(test_ui_layout
    ${CMAKE_CURRENT_SOURCE_DIR}/test_ui_layout.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_test_helpers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kk_oled_stub.c
    ${CORE_SRC}/ui.c
    ${CORE_SRC}/kk_font.c
    ${CORE_SRC}/measure.c
    ${CORE_SRC}/app_mode.c
    ${CORE_SRC}/buttons.c
    ${KK_OLED_SRC}
)
target_include_directories(test_ui_layout PRIVATE
    ${CORE_INC} ${CMAKE_CURRENT_SOURCE_DIR} ${KK_OLED_INC})
target_compile_definitions(test_ui_layout PRIVATE KK_OLED_TEST)
target_compile_options(test_ui_layout PRIVATE -Wall -Wextra)
add_test(NAME ui_layout COMMAND test_ui_layout)

# ---------------------------------------------------------------------------
# 字模覆盖: **按像素墨迹**判定缺字(不能用 advance), 并带反向守卫
# ---------------------------------------------------------------------------
add_executable(test_ui_font
    ${CMAKE_CURRENT_SOURCE_DIR}/test_ui_font.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kk_oled_stub.c
    ${CORE_SRC}/kk_font.c
    ${KK_OLED_SRC}
)
target_include_directories(test_ui_font PRIVATE
    ${CORE_INC} ${CMAKE_CURRENT_SOURCE_DIR} ${KK_OLED_INC})
target_compile_definitions(test_ui_font PRIVATE KK_OLED_TEST)
target_compile_options(test_ui_font PRIVATE -Wall -Wextra)
add_test(NAME ui_font COMMAND test_ui_font)

# 便于 IDE/编辑器索引全部纯逻辑源码
add_library(host_logic OBJECT ${HOST_LOGIC_SRC})
target_include_directories(host_logic PRIVATE ${CORE_INC})
