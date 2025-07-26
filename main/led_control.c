#include "led_control.h"
#include "esp_log.h"
#include "esp_timer.h" // 用于获取高精度时间

static const char *TAG = "LED_CONTROL";

// --- 核心修改 1: 定义亮度和固定的16级颜色数组 ---

// 全局亮度控制 (0-255)。请从一个较低的值开始，例如 40。
// 如果闪烁问题依旧，请尝试进一步降低此值。
#define BRIGHTNESS 5

// 定义一个固定的16级颜色数组，从下(index 0)到上(index 15)
// 颜色格式为 {r, g, b}
const rgb_t spectrum_colors[MATRIX_HEIGHT] = {
    {  0,   0, 255}, // 0: Blue
    {  0,  60, 255}, // 1
    {  0, 120, 255}, // 2
    {  0, 180, 180}, // 3: Cyan
    {  0, 255, 120}, // 4
    {  0, 255,  60}, // 5
    {  0, 255,   0}, // 6: Green
    { 60, 255,   0}, // 7
    {120, 255,   0}, // 8
    {180, 255,   0}, // 9: Lime
    {240, 240,   0}, // 10
    {255, 200,   0}, // 11: Yellow
    {255, 160,   0}, // 12
    {255, 120,   0}, // 13: Orange
    {255,  60,   0}, // 14
    {255,   0,   0}  // 15: Red
};


// --- 内部函数 (保持不变) ---
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

// --- LED 控制器初始化 (保持不变) ---
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

// --- 核心修改 2: 更新后的频谱显示任务 ---
void led_spectrum_task(void *pvParm) {
    led_strip_handle_t led_strip = (led_strip_handle_t)pvParm;
    uint8_t raw_fft_heights[MATRIX_WIDTH];
    float smoothed_heights[MATRIX_WIDTH] = {0.0f};
    int peak_y[MATRIX_WIDTH] = {0};
    int64_t last_peak_fall_time = 0;
    const int PEAK_FALL_DELAY_MS = 120;

    while (1) {
        int64_t start_time = esp_timer_get_time();
        fft_analyzer_get_heights(raw_fft_heights);
        for (int i = 0; i < MATRIX_WIDTH; i++) {
            smoothed_heights[i] = (smoothed_heights[i] * 1.0f + (float)raw_fft_heights[i] * 3.0f) / 4.0f;
        }
        ESP_ERROR_CHECK(led_strip_clear(led_strip));
        int64_t current_time = esp_timer_get_time() / 1000;
        if (current_time - last_peak_fall_time > PEAK_FALL_DELAY_MS) {
            last_peak_fall_time = current_time;
            for (int i = 0; i < MATRIX_WIDTH; i++) {
                if (peak_y[i] > 0) peak_y[i]--;
            }
        }
        for (int x = 0; x < MATRIX_WIDTH; x++) {
            int display_height = (int)(smoothed_heights[x] + 0.5f);
            if (raw_fft_heights[x] > peak_y[x]) {
                peak_y[x] = raw_fft_heights[x];
            }
            for (int y = 0; y < display_height; y++) {
                rgb_t color = spectrum_colors[y];
                uint8_t r = (color.r * BRIGHTNESS) / 255;
                uint8_t g = (color.g * BRIGHTNESS) / 255;
                uint8_t b = (color.b * BRIGHTNESS) / 255;
                int index = XY_to_index(x, y);
                if (index != -1) {
                    ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, index, r, g, b));
                }
            }
            int peak_draw_y = peak_y[x];
            if (peak_draw_y >= MATRIX_HEIGHT) peak_draw_y = MATRIX_HEIGHT - 1;
            if (peak_draw_y < display_height) peak_draw_y = display_height;
            int peak_index = XY_to_index(x, peak_draw_y);
            if (peak_index != -1) {
                uint8_t peak_brightness = (200 * BRIGHTNESS) / 255;
                ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, peak_index, peak_brightness, peak_brightness, peak_brightness));
            }
        }
        ESP_ERROR_CHECK(led_strip_refresh(led_strip));
        int64_t elapsed_us = esp_timer_get_time() - start_time;
        int64_t sleep_us = 20000 - elapsed_us; // 20ms周期
        if (sleep_us > 0) {
            esp_rom_delay_us(sleep_us);
        } else {
          ;
            //ESP_LOGW(TAG, "LED refresh task overrun by %lld us", -sleep_us);
        }
    }
}