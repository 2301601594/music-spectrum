// 恢复后的 main.c 应该是这样的
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h" // 用于Wi-Fi连接
#include "file_serving_example_common.h"
#include "wave_player.h"
#include "fft_analyzer.h"
#include "led_control.h"
#include "audio_manager.h"
#include "mic_input.h"

static const char *TAG = "APP_MAIN";
static led_strip_handle_t led_strip_handle;

/**
 * @brief 一个临时的调试任务，用于打印FFT计算出的频谱高度
 */
void debug_print_task(void *pvParameters)
{
    uint8_t heights[NUM_BANDS];
    char line_buffer[NUM_BANDS * 3 + 1]; // 用于格式化输出

    while (1)
    {

        fft_analyzer_get_heights(heights);

        int pos = 0;
        for (int i = 0; i < NUM_BANDS; i++)
        {
            pos += sprintf(line_buffer + pos, "%02d ", heights[i]);
        }
        ESP_LOGI("FFT_DEBUG", "|%s|", line_buffer);
        vTaskDelay(pdMS_TO_TICKS(500)); // 每500ms打印一次
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Application Startup");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 挂载文件系统
    const char *base_path = "/sdcard";
    ESP_ERROR_CHECK(example_mount_storage(base_path));

 // 1. 初始化底层音频模块
    ESP_ERROR_CHECK(wave_player_init());
    ESP_ERROR_CHECK(mic_input_init());

    // 2. 初始化音频处理和显示模块
    ESP_ERROR_CHECK(fft_analyzer_init());
    ESP_ERROR_CHECK(led_control_init(&led_strip_handle));

    // 3. 初始化顶层逻辑控制器
    ESP_ERROR_CHECK(audio_manager_init());



    // 连接到Wi-Fi网络
    ESP_ERROR_CHECK(example_connect());

    // 启动文件和API服务器
    ESP_ERROR_CHECK(start_file_and_api_server(base_path));

    xTaskCreate(debug_print_task, "debug_print_task", 4096, NULL, 5, NULL);
    xTaskCreate(led_spectrum_task, "led_spectrum_task", 4096, led_strip_handle, 6, NULL);

    ESP_LOGI(TAG, "System initialized successfully. Waiting for connections.");
    // 主任务可以结束或进入低功耗模式，因为所有工作都在其他任务中进行
}