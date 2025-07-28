/* mic_input.c */
#include "mic_input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "fft_analyzer.h"

static const char *TAG = "MIC_INPUT";

// --- I2S 配置 ---
#define MIC_I2S_PORT        I2S_NUM_1 // 使用I2S端口1，避免与播放器使用的I2S_NUM_0冲突
#define MIC_I2S_SAMPLE_RATE 44100
#define MIC_I2S_CHANNEL_NUM 1 // 单声道
#define MIC_I2S_BITS_PER_SAMPLE 32 // ICS43434是24位，但I2S驱动通常配置为32位读取
#define MIC_BUFFER_SIZE     (FFT_N * 4) // 缓冲区大小，FFT_N点 * 4字节/点

// --- I2S 引脚定义 (请根据你的硬件连接修改) ---
#define MIC_I2S_WS_IO       GPIO_NUM_5 // Word Select (L/R Clock)
#define MIC_I2S_SCK_IO      GPIO_NUM_4 // Serial Clock (Bit Clock)
#define MIC_I2S_SD_IO       GPIO_NUM_6 // Serial Data (Data In)

static TaskHandle_t mic_task_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;

/**
 * @brief 麦克风数据采集和处理任务
 */
static void mic_capture_task(void *pvParameters) {
    int32_t *raw_samples = (int32_t *)malloc(MIC_BUFFER_SIZE);
    int16_t *fft_samples = (int16_t *)malloc(FFT_N * sizeof(int16_t));
    if (!raw_samples || !fft_samples) {
        ESP_LOGE(TAG, "Failed to allocate memory for buffers");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        size_t bytes_read = 0;
        // 1. 从I2S读取原始数据
        esp_err_t result = i2s_channel_read(rx_handle, raw_samples, MIC_BUFFER_SIZE, &bytes_read, portMAX_DELAY);

        if (result == ESP_OK && bytes_read > 0) {
            // 2. 数据转换：将32位的I2S样本转换为16位的PCM样本
            // ICS43434的有效数据在最高位，所以我们右移来提取
            int samples_to_process = bytes_read / sizeof(int32_t);
            for (int i = 0; i < samples_to_process && i < FFT_N; i++) {
                // 右移16位，丢弃低位，保留高16位作为最终样本
                fft_samples[i] = (int16_t)(raw_samples[i] >> 16);
            }
            
            // 3. 将处理好的数据推送到FFT分析器
            fft_analyzer_push_audio_data(fft_samples, samples_to_process, MIC_I2S_CHANNEL_NUM);
        }
    }
    free(raw_samples);
    free(fft_samples);
    vTaskDelete(NULL);
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
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    
    ESP_LOGI(TAG, "Microphone I2S driver initialized.");
    return ESP_OK;
}

esp_err_t mic_input_start(void) {
    if (mic_task_handle != NULL) {
        ESP_LOGW(TAG, "Mic task is already running.");
        return ESP_OK;
    }
    xTaskCreate(mic_capture_task, "mic_capture_task", 4096, NULL, 5, &mic_task_handle);
    ESP_LOGI(TAG, "Microphone capture task started.");
    return ESP_OK;
}

esp_err_t mic_input_stop(void) {
    if (mic_task_handle != NULL) {
        vTaskDelete(mic_task_handle);
        mic_task_handle = NULL;
        ESP_LOGI(TAG, "Microphone capture task stopped.");
    }
    return ESP_OK;
}