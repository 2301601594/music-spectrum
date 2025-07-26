#pragma once
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"

// 从 fft_analyzer.h 引入，确保数据源和显示端配置一致
#include "fft_analyzer.h" // 包含 NUM_BANDS 定义

#define BLINK_GPIO 21

// --- 核心修改: 更新矩阵尺寸 ---
#define MATRIX_WIDTH 32
#define MATRIX_HEIGHT 16
#define LED_STRIP_MAX_LEDS (MATRIX_WIDTH * MATRIX_HEIGHT) // 总灯珠数更新为 512

// --- 颜色处理结构体 ---

/**
 * @brief RGB 颜色结构体
 */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

/**
 * @brief 调色板中的一个颜色锚点
 */
typedef struct {
    uint8_t index; // 在0-255范围内的位置
    rgb_t color;   // 该位置对应的颜色
} palette_entry_t;

// --- 函数声明 ---

/**
 * @brief 初始化LED控制器
 *
 * @param led_strip 指向 led_strip_handle_t 的指针，用于返回创建的句柄
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t led_control_init(led_strip_handle_t *led_strip);

/**
 * @brief 音乐频谱显示任务
 *
 * @param pvParm 传入的参数，这里是 led_strip_handle_t 句柄
 */
void led_spectrum_task(void *pvParm);