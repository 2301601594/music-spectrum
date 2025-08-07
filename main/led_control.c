#include "led_control.h"
#include "esp_log.h"
#include "esp_timer.h" // 用于获取高精度时间

static const char *TAG = "LED_CONTROL";

// 全局亮度控制 (0-255)。
#define BRIGHTNESS 10

// --- 核心修改: 重新设计颜色主题，使其更明亮、变化更丰富 ---

// 主题 0: 经典彩虹 (保持不变)
const rgb_t spectrum_colors_default[MATRIX_HEIGHT] = {
    {  0,   0, 255}, {  0,  60, 255}, {  0, 120, 255}, {  0, 180, 180},
    {  0, 255, 120}, {  0, 255,  60}, {  0, 255,   0}, { 60, 255,   0},
    {120, 255,   0}, {180, 255,   0}, {240, 240,   0}, {255, 200,   0},
    {255, 160,   0}, {255, 120,   0}, {255,  60,   0}, {255,   0,   0}
};

// 主题 1: "炽焰" (Vibrant Fire) - 新设计
const rgb_t spectrum_colors_fire[MATRIX_HEIGHT] = {
    {255, 255,  60}, // 亮黄
    {255, 220,  50},
    {255, 190,  40},
    {255, 160,  30}, // 橙色
    {255, 130,  20},
    {255, 100,  10},
    {255,  70,   0}, // 橙红
    {255,  40,   0},
    {255,   0,   0}, // 纯红
    {255,   0,  40},
    {255,   0,  80},
    {255,   0, 120}, // 洋红
    {255,  20, 140},
    {255,  40, 160},
    {255,  60, 180},
    {255,  80, 200}  // 粉紫
};

// 主题 2: "沧澜" (Ocean Wave) - 新设计
const rgb_t spectrum_colors_forest[MATRIX_HEIGHT] = {
    {200, 255, 255}, // 亮青
    {160, 255, 255},
    {120, 240, 255},
    { 80, 220, 255}, // 天蓝
    { 40, 200, 255},
    {  0, 180, 255},
    {  0, 160, 255}, // 亮蓝
    {  0, 140, 255},
    {  0, 110, 255},
    {  0,  80, 255}, // 纯蓝
    { 40,  60, 255},
    { 80,  40, 255}, // 靛蓝
    {120,  20, 255},
    {150,   0, 255}, // 紫色
    {180,   0, 255},
    {210,   0, 255}  // 品红
};

// 使用一个指针数组来方便地切换调色板
const rgb_t* color_palettes[3] = {
    spectrum_colors_default,
    spectrum_colors_fire,
    spectrum_colors_forest // 尽管文件名是forest，但现在它代表“沧澜”主题
};

// 全局变量，用于存储当前的颜色模式
static int g_color_mode = 0;

// --- 函数用于设置和获取颜色模式 (无变化) ---
void led_control_set_color_mode(int mode) {
    if (mode >= 0 && mode < 3) {
        g_color_mode = mode;
        ESP_LOGI(TAG, "Color mode set to %d", mode);
    } else {
        ESP_LOGW(TAG, "Invalid color mode: %d", mode);
    }
}

int led_control_get_color_mode(void) {
    return g_color_mode;
}


// --- 内部函数 (无变化) ---
static int XY_to_index(int x, int y) {
    if (x < 0 || x >= MATRIX_WIDTH || y < 0 || y >= MATRIX_HEIGHT) {
        return -1;
    }
    if ((x & 1) == 0) {
        return (x * MATRIX_HEIGHT) + y;
    } else {
        return (x * MATRIX_HEIGHT) + (MATRIX_HEIGHT - 1 - y);
    }
}

// --- LED 控制器初始化 (无变化) ---
esp_err_t led_control_init(led_strip_handle_t *led_strip) {
  led_strip_config_t strip_config = {
      .strip_gpio_num = BLINK_GPIO,
      .max_leds = LED_STRIP_MAX_LEDS,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
      .flags.invert_out = false,
  };
  led_strip_rmt_config_t rmt_config = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 65 * 100 * 1000,
      .mem_block_symbols = 256,
      .flags.with_dma = true,
  };
  if (led_strip_new_rmt_device(&strip_config, &rmt_config, led_strip) == ESP_OK) {
    ESP_LOGI(TAG, "LED strip (32x16) initialized successfully");
    led_strip_clear(*led_strip);
    return ESP_OK;
  } else {
    ESP_LOGE(TAG, "Failed to initialize LED strip");
    return ESP_FAIL;
  }
}

// --- 频谱显示任务 (逻辑无变化, 但会使用新的颜色) ---
void led_spectrum_task(void *pvParm) {
    led_strip_handle_t led_strip = (led_strip_handle_t)pvParm;
    uint8_t raw_fft_heights[MATRIX_WIDTH];
    float smoothed_heights[MATRIX_WIDTH] = {0.0f};
    int peak_y[MATRIX_WIDTH] = {0};
    int64_t last_peak_fall_time = 0;
    const int PEAK_FALL_DELAY_MS = 150;

    while (1) {
        int64_t start_time = esp_timer_get_time();

        // 1. 从FFT分析器获取最新的频段高度数据
        fft_analyzer_get_heights(raw_fft_heights);

        // 2. 更新每个LED列的平滑值和峰值
        for (int x = 0; x < MATRIX_WIDTH; x++) {
            int fft_data_index = (MATRIX_WIDTH - 1) - x;
            smoothed_heights[x] = (smoothed_heights[x] * 0.75f) + ((float)raw_fft_heights[fft_data_index] * 0.25f);
            if (raw_fft_heights[fft_data_index] > peak_y[x]) {
                peak_y[x] = raw_fft_heights[fft_data_index];
            }
        }

        // 3. 清空屏幕，准备绘制新的一帧
        ESP_ERROR_CHECK(led_strip_clear(led_strip));

        // 4. 处理峰值点的自然下落
        int64_t current_time = esp_timer_get_time() / 1000;
        if (current_time - last_peak_fall_time > PEAK_FALL_DELAY_MS) {
            last_peak_fall_time = current_time;
            for (int i = 0; i < MATRIX_WIDTH; i++) {
                if (peak_y[i] > 0) peak_y[i]--;
            }
        }

        // 5. 绘制所有灯柱和峰值点
        for (int x = 0; x < MATRIX_WIDTH; x++) {
            int display_height = (int)(smoothed_heights[x] + 0.5f);
            if (display_height > MATRIX_HEIGHT) display_height = MATRIX_HEIGHT;

            // 绘制灯柱
            for (int y = 0; y < display_height; y++) {
                // *** 使用当前选择的颜色主题 ***
                rgb_t color = color_palettes[g_color_mode][y];
                uint8_t r = (color.r * BRIGHTNESS) / 255;
                uint8_t g = (color.g * BRIGHTNESS) / 255;
                uint8_t b = (color.b * BRIGHTNESS) / 255;
                int index = XY_to_index(x, y);
                if (index != -1) {
                    ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, index, r, g, b));
                }
            }

            // 绘制峰值点
            int peak_draw_y = peak_y[x];
            if (peak_draw_y >= MATRIX_HEIGHT) peak_draw_y = MATRIX_HEIGHT - 1;
            if (peak_draw_y < display_height) peak_draw_y = display_height;
            int peak_index = XY_to_index(x, peak_draw_y);
            if (peak_index != -1) {
                uint8_t peak_brightness = (200 * BRIGHTNESS) / 255;
                ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, peak_index, peak_brightness, peak_brightness, peak_brightness));
            }
        }

        // 6. 刷新屏幕以显示新的一帧
        ESP_ERROR_CHECK(led_strip_refresh(led_strip));

        // 7. 控制帧率
        int64_t elapsed_us = esp_timer_get_time() - start_time;
        int64_t sleep_us = 20000 - elapsed_us; // 目标帧率 50fps (20ms)
        if (sleep_us > 0) {
            vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
        }
    }
}
