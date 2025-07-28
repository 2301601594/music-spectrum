/* mic_input.h */
#pragma once

#include "esp_err.h"

/**
 * @brief 初始化麦克风输入模块
 * * 配置并安装I2S驱动，但不启动数据采集任务。
 * * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t mic_input_init(void);

/**
 * @brief 启动麦克风数据采集
 * * 创建一个任务，该任务会持续从I2S麦克风读取数据并送入FFT分析器。
 * * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t mic_input_start(void);

/**
 * @brief 停止麦克风数据采集
 * * 删除采集任务并释放相关资源。
 * * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t mic_input_stop(void);
