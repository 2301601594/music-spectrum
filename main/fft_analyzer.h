#pragma once

#include <stdint.h>
#include "esp_err.h"

// ---配置常量---
#define FFT_N 512         // FFT采样点数
#define NUM_BANDS 32       // 最终输出的频带数量
#define MATRIX_HEIGHT 16   // 可视化矩阵高度

/**
 * @brief 初始化FFT分析器
 * * 该函数会创建FFT处理任务、相关队列和互斥锁。
 * 必须在系统启动时调用一次。
 * * @return esp_err_t ESP_OK 表示成功, 其他表示失败
 */
esp_err_t fft_analyzer_init(void);

/**
 * @brief 将原始音频数据推送到FFT任务进行处理
 * * wave_player任务会调用此函数，将从I2S读取到的数据块发送过来。
 * * @param data 指向16位PCM音频数据的指针
 * @param len  数据点的数量 (不是字节数)
 * @param channels 音频的通道数 (1 for mono, 2 for stereo)
 * @return esp_err_t ESP_OK 表示数据成功入队, 其他表示失败
 */
esp_err_t fft_analyzer_push_audio_data(const int16_t* data, int len, int channels);

/**
 * @brief 获取计算好的频谱高度数据
 * * LED显示任务会调用此函数，以获取用于绘制频谱的32个柱状条的高度。
 * * @param heights 一个大小为 NUM_BANDS 的数组，用于接收高度数据 (0-15)
 */
void fft_analyzer_get_heights(uint8_t heights[NUM_BANDS]);