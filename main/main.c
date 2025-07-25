/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* HTTP File Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "file_serving_example_common.h"
#include "wave_player.h"

/* This example demonstrates how to create file server
 * using esp_http_server. This file has only startup code.
 * Look in file_server.c for the implementation.
 */

static const char *TAG = "example";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting example");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Initialize file storage */
    const char *base_path = "/sdcard";
    // 初始化SD卡
    ESP_ERROR_CHECK(example_mount_storage(base_path));

    // 5. 初始化我们的波形播放器服务
    ESP_LOGI(TAG, "Initializing Wave Player service...");
    ESP_ERROR_CHECK(wave_player_init());

    // --- 核心测试逻辑 ---
    ESP_LOGI(TAG, "Sending PLAY command in 3 seconds...");
    vTaskDelay(pdMS_TO_TICKS(3000)); // 等待3秒，确保所有服务都已启动

    // 构造一个播放命令
    player_cmd_msg_t play_cmd;
    play_cmd.cmd = PLAYER_CMD_PLAY;
    // 请确保你的SD卡根目录下有这个文件！
    snprintf(play_cmd.filepath, FILE_PATH_MAX, "%s/周传雄 - 青花.wav", base_path);

    // 发送命令给播放器任务
    wave_player_send_cmd(&play_cmd);
    ESP_LOGI(TAG, "PLAY command sent for track: %s", play_cmd.filepath);

    play_cmd.cmd = PLAYER_CMD_PAUSE;
    vTaskDelay(pdMS_TO_TICKS(2000)); // 等待2秒后
    wave_player_send_cmd(&play_cmd);
    ESP_LOGI(TAG, "PAUSE command sent");

    play_cmd.cmd = PLAYER_CMD_RESUME;
    vTaskDelay(pdMS_TO_TICKS(2000)); // 等待2秒后
    wave_player_send_cmd(&play_cmd);
    ESP_LOGI(TAG, "RESUME command sent");

    // --- 监控播放状态 ---
    // 创建一个循环，每2秒获取并打印一次播放器状态
    player_status_t current_status;
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));

        wave_player_get_status(&current_status);

        const char *state_str = "UNKNOWN";
        if (current_status.state == PLAYER_STATE_PLAYING)
            state_str = "PLAYING";
        else if (current_status.state == PLAYER_STATE_PAUSED)
            state_str = "PAUSED";
        else if (current_status.state == PLAYER_STATE_STOPPED)
            state_str = "STOPPED";

        ESP_LOGI(TAG, "Player Status: [%s] | Track: %s | Progress: %lu / %lu sec",
                 state_str,
                 current_status.current_track,
                 current_status.current_position_sec,
                 current_status.total_duration_sec);
    }
    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    // 连接网络
    // ESP_ERROR_CHECK(example_connect());

    /* Start the file server */
    // 开始HTTP服务器，将页面发送给到浏览器
    // ESP_ERROR_CHECK(start_file_and_api_server(base_path));
    // ESP_LOGI(TAG, "File server started");
}
