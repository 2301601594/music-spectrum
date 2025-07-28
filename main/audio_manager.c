/* audio_manager.c */
#include "audio_manager.h"
#include "esp_log.h"

// 包含需要控制的模块的头文件
#include "wave_player.h"
#include "mic_input.h" // 我们即将创建的麦克风模块

static const char *TAG = "AUDIO_MANAGER";
static audio_mode_t g_current_mode = AUDIO_MODE_PLAYER; // 系统启动时默认为播放器模式

esp_err_t audio_manager_init(void) {
    ESP_LOGI(TAG, "Audio Manager initialized, default mode is PLAYER.");
    // 可以在这里进行一些初始状态设置
    g_current_mode = AUDIO_MODE_PLAYER;
    return ESP_OK;
}

esp_err_t audio_manager_set_mode(audio_mode_t new_mode) {
    if (new_mode == g_current_mode) {
        ESP_LOGI(TAG, "Mode is already %s, no change.", new_mode == AUDIO_MODE_PLAYER ? "PLAYER" : "MIC");
        return ESP_OK; // 模式未改变，直接返回
    }

    ESP_LOGI(TAG, "Switching mode from %s to %s", g_current_mode == AUDIO_MODE_PLAYER ? "PLAYER" : "MIC", new_mode == AUDIO_MODE_PLAYER ? "PLAYER" : "MIC");

    // 根据新模式执行切换逻辑
    switch (new_mode) {
        case AUDIO_MODE_PLAYER:
            // --- 切换到播放器模式 ---
            // 1. 停止麦克风采集
            mic_input_stop();
            // 2. 播放器此时应处于待命状态，等待前端发送播放指令
            break;

        case AUDIO_MODE_MIC:
            // --- 切换到麦克风模式 ---
            // 1. 立即停止当前可能正在播放的音乐
            player_cmd_msg_t stop_msg = {.cmd = PLAYER_CMD_STOP};
            wave_player_send_cmd(&stop_msg);
            // 2. 启动麦克风采集
            mic_input_start();
            break;
        
        default:
            ESP_LOGW(TAG, "Unknown mode requested: %d", new_mode);
            return ESP_ERR_INVALID_ARG;
    }

    // 更新全局状态
    g_current_mode = new_mode;
    return ESP_OK;
}

audio_mode_t audio_manager_get_mode(void) {
    return g_current_mode;
}