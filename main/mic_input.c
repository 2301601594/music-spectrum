#include "mic_input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "fft_analyzer.h"

static const char *TAG = "MIC_INPUT";

// --- I2S 配置 ---
#define MIC_I2S_PORT            I2S_NUM_1
#define MIC_I2S_SAMPLE_RATE     44100
#define MIC_I2S_CHANNEL_NUM     1
#define MIC_I2S_BITS_PER_SAMPLE 32
#define MIC_BUFFER_SIZE         (FFT_N * 4)

// --- I2S 引脚定义 (请根据你的硬件连接修改) ---
#define MIC_I2S_WS_IO       GPIO_NUM_5
#define MIC_I2S_SCK_IO      GPIO_NUM_4
#define MIC_I2S_SD_IO       GPIO_NUM_6

static TaskHandle_t mic_task_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
// **新增**: 使用volatile bool标志来安全地控制任务生命周期
static volatile bool g_is_mic_task_running = false;

/**
 * @brief 麦克风数据采集和处理任务
 */
static void mic_capture_task(void *pvParameters) {
    int32_t *raw_samples = (int32_t *)malloc(MIC_BUFFER_SIZE);
    int16_t *fft_samples = (int16_t *)malloc(FFT_N * sizeof(int16_t));
    if (!raw_samples || !fft_samples) {
        ESP_LOGE(TAG, "Failed to allocate memory for buffers");
        g_is_mic_task_running = false; // 确保标志被重置
        mic_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // **核心修改**: 任务主循环由布尔标志控制
    while (g_is_mic_task_running) {
        size_t bytes_read = 0;
        // **核心修改**: 使用超时而不是永久阻塞，以便任务可以响应停止请求
        esp_err_t result = i2s_channel_read(rx_handle, raw_samples, MIC_BUFFER_SIZE, &bytes_read, pdMS_TO_TICKS(100));

        if (result == ESP_OK && bytes_read > 0) {
            int samples_to_process = bytes_read / sizeof(int32_t);
            for (int i = 0; i < samples_to_process && i < FFT_N; i++) {
                fft_samples[i] = (int16_t)(raw_samples[i] >> 16);
            }
            fft_analyzer_push_audio_data(fft_samples, samples_to_process, MIC_I2S_CHANNEL_NUM);
        } else if (result != ESP_ERR_TIMEOUT) {
            // 如果不是超时错误，则记录下来
            ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(result));
        }
    }

    // **核心修改**: 任务退出前的清理工作
    ESP_LOGI(TAG, "Mic capture task cleaning up...");
    free(raw_samples);
    free(fft_samples);
    mic_task_handle = NULL; // 清除全局任务句柄，用于向stop函数发信号
    vTaskDelete(NULL);      // 任务自我删除
}

esp_err_t mic_input_init(void) {
    // 1. 配置I2S通道
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    // 2. 配置I2S标准模式
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(MIC_I2S_BITS_PER_SAMPLE, MIC_I2S_CHANNEL_NUM),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_SCK_IO,
            .ws = MIC_I2S_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_I2S_SD_IO,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    
    ESP_LOGI(TAG, "Microphone I2S driver configured (but not enabled).");
    return ESP_OK;
}

esp_err_t mic_input_start(void) {
    if (mic_task_handle != NULL) {
        ESP_LOGW(TAG, "Mic task is already running.");
        return ESP_OK;
    }

    // **核心修改**: 先设置运行标志，再创建任务
    g_is_mic_task_running = true;

    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "I2S channel enabled.");

    xTaskCreate(mic_capture_task, "mic_capture_task", 4096, NULL, 5, &mic_task_handle);
    if (mic_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create mic capture task.");
        g_is_mic_task_running = false; // 创建失败，重置标志
        i2s_channel_disable(rx_handle); // 关闭I2S
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Microphone capture task started.");
    return ESP_OK;
}

esp_err_t mic_input_stop(void) {
    // **核心修改**: 通过设置标志来请求任务停止
    if (g_is_mic_task_running) {
        ESP_LOGI(TAG, "Requesting microphone task to stop.");
        g_is_mic_task_running = false;

        // **核心修改**: 等待任务自行结束
        int timeout_ms = 500;
        int elapsed_ms = 0;
        while (mic_task_handle != NULL && elapsed_ms < timeout_ms) {
            vTaskDelay(pdMS_TO_TICKS(20));
            elapsed_ms += 20;
        }

        if (mic_task_handle != NULL) {
             ESP_LOGW(TAG, "Mic task did not stop in time. Forcing delete.");
             vTaskDelete(mic_task_handle); // 作为最后的保险措施
             mic_task_handle = NULL;
        } else {
             ESP_LOGI(TAG, "Mic task stopped gracefully.");
        }
    }

    ESP_ERROR_CHECK(i2s_channel_disable(rx_handle));
    ESP_LOGI(TAG, "I2S channel disabled.");
    
    return ESP_OK;
}
