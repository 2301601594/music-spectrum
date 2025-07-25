// 恢复后的 main.c 应该是这样的
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h" // 用于Wi-Fi连接
#include "file_serving_example_common.h"
#include "wave_player.h"

static const char *TAG = "APP_MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Application Startup");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 挂载文件系统
    const char* base_path = "/sdcard";
    ESP_ERROR_CHECK(example_mount_storage(base_path));

    // 初始化播放器服务
    ESP_ERROR_CHECK(wave_player_init());

    // 连接到Wi-Fi网络
    ESP_ERROR_CHECK(example_connect());

    // 启动文件和API服务器
    ESP_ERROR_CHECK(start_file_and_api_server(base_path));

    ESP_LOGI(TAG, "System initialized successfully. Waiting for connections.");
    // 主任务可以结束或进入低功耗模式，因为所有工作都在其他任务中进行
}