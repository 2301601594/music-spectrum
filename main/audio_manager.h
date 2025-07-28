/* audio_manager.h */
#pragma once

#include "esp_err.h"

/**
 * @brief 定义系统支持的音频输入模式
 */
typedef enum {
    AUDIO_MODE_PLAYER,  // 播放器模式：从SD卡WAV文件获取音频
    AUDIO_MODE_MIC,     // 麦克风模式：从ICS43434麦克风获取音频
} audio_mode_t;

/**
 * @brief 初始化音频管理器
 * * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t audio_manager_init(void);

/**
 * @brief 设置当前的音频输入模式
 * * 这是模式切换的核心函数。它会负责停止当前模式的模块并启动新模式的模块。
 * * @param new_mode 要切换到的新模式
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t audio_manager_set_mode(audio_mode_t new_mode);

/**
 * @brief 获取当前的音频输入模式
 * * @return audio_mode_t 当前的模式
 */
audio_mode_t audio_manager_get_mode(void);