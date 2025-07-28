#include "fft_analyzer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_dsp.h"
#include "math.h"
#include <string.h>
#include <stdlib.h> // 用于 qsort

static const char *TAG = "FFT_ANALYZER";

// --- 配置常量 ---
// #define FFT_N 512 // 已在头文件中定义
// #define NUM_BANDS 32 // 已在头文件中定义
#define AGC_TOP_N_BANDS 3 // 用于AGC计算的顶峰频段数量，建议2-5之间
#define GAMMA_CORRECTION 0.9f // 伽马校正值，<1.0会提升中低亮度的视觉效果

// 定义音频数据块结构体
typedef struct
{
  int16_t *data;
  int len;
  int channels;
} audio_chunk_t;

// --- FreeRTOS & 数据相关的静态变量 ---
static QueueHandle_t audio_queue = NULL;
static SemaphoreHandle_t data_mutex = NULL;
static uint8_t display_heights[NUM_BANDS] = {0};

// --- FFT 计算相关的静态变量 ---
static float fft_input[FFT_N * 2];
static float hanning_window[FFT_N];

// --- 核心优化 1: 设计一条新的、更激进的静态EQ增益曲线 ---
// 这组增益被设计为极大地增强高频，同时保持对低频的抑制。
// 注意：极高的增益可能会放大音频文件本身的背景噪声。
static const double band_eq_gains[NUM_BANDS] = {
    // --- 低频段 (保持抑制) ---
    0.50, 0.60, 0.70, 0.80, 0.90, 1.0, 1.1, 1.2,
    // --- 中频段 (保持平稳过渡) ---
    1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.0,
    // --- 高频段 (极为激进的增强) ---
    2.5,  3.0,  3.5,  4.0,  5.0,  6.0,  7.5,  9.0,
    11.0, 13.0, 15.0, 18.0, 22.0, 26.0, 30.0, 35.0
};


// C语言qsort所需的比较函数 (用于对浮点数降序排序)
static int compare_floats_desc(const void *a, const void *b)
{
  float fa = *(const float *)a;
  float fb = *(const float *)b;
  if (fa < fb)
    return 1;
  if (fa > fb)
    return -1;
  return 0;
}

// 累加指定范围内的FFT幅值
static double fft_add_c(const float *magnitudes_array, int from, int to)
{
  double result = 0;
  int upper_bound = (to < FFT_N / 2) ? to : (FFT_N / 2 - 1);
  for (int i = from; i <= upper_bound; i++)
  {
    result += magnitudes_array[i];
  }
  return result;
}

/**
 * @brief FFT处理核心任务
 */
static void fft_task(void *pvParameters)
{
  // 初始化DSP库中的FFT功能
  esp_err_t ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "Not possible to initialize FFT2R. Error = %d", ret);
    vTaskDelete(NULL);
    return;
  }
  // 生成汉宁窗，用于平滑采样数据的边缘
  dsps_wind_hann_f32(hanning_window, FFT_N);

  // --- 核心优化 2: 引入更完善的自动增益控制(AGC)变量 ---
  static float dynamic_ceiling = 100.0f; // 动态“天花板”，会根据音乐响度自动调整
  const float MAGNITUDE_FLOOR = 20.0f;   // 幅值“地基”，低于此值的能量将被忽略
  const float CEILING_DECAY_RATE = 0.992f; // 天花板的衰减速率，越接近1.0衰减越慢

  int16_t *audio_buffer = (int16_t *)malloc(FFT_N * sizeof(int16_t));
  int buffer_pos = 0;
  audio_chunk_t chunk;

  while (1)
  {
    // 尝试从队列中获取音频数据，超时时间为1000ms
    if (xQueueReceive(audio_queue, &chunk, pdMS_TO_TICKS(1000)) == pdPASS)
    {
      int samples_in_chunk = chunk.len;
      int frames_to_process = 0;

      // 将接收到的立体声或单声道数据填充到FFT处理缓冲区
      if (chunk.channels == 1) { // 单声道
        frames_to_process = samples_in_chunk;
        if (buffer_pos + frames_to_process > FFT_N) {
          frames_to_process = FFT_N - buffer_pos;
        }
        memcpy(&audio_buffer[buffer_pos], chunk.data, frames_to_process * sizeof(int16_t));
      } else if (chunk.channels == 2) { // 立体声 (混合为单声道)
        frames_to_process = samples_in_chunk / 2;
        if (buffer_pos + frames_to_process > FFT_N) {
          frames_to_process = FFT_N - buffer_pos;
        }
        for (int i = 0; i < frames_to_process; i++) {
          audio_buffer[buffer_pos + i] = (chunk.data[i * 2] + chunk.data[i * 2 + 1]) / 2;
        }
      }
      
      buffer_pos += frames_to_process;
      free(chunk.data); // 释放已处理的数据块内存

      // 当缓冲区填满一个FFT_N长度时，开始处理
      if (buffer_pos >= FFT_N)
      {
        // 1. 准备FFT输入数据 (应用汉宁窗)
        for (int i = 0; i < FFT_N; i++)
        {
          fft_input[i * 2] = (float)audio_buffer[i] * hanning_window[i];
          fft_input[i * 2 + 1] = 0.0f;
        }

        // 2. 执行FFT计算
        dsps_fft2r_fc32(fft_input, FFT_N);
        dsps_bit_rev_fc32_ansi(fft_input, FFT_N);
        
        // 3. 计算每个频率点的幅值
        static float magnitudes[FFT_N / 2];
        for (int i = 0; i < FFT_N / 2; i++)
        {
          float real = fft_input[i * 2];
          float imag = fft_input[i * 2 + 1];
          magnitudes[i] = sqrtf(real * real + imag * imag);
        }

        // --- 核心优化开始 ---

        // 步骤 A: 将FFT结果分组到32个频带，并应用静态EQ增益
        float eq_band_magnitudes[NUM_BANDS];
        // (频段划分逻辑保持不变, 但现在会乘以上面新定义的 band_eq_gains)
        eq_band_magnitudes[0] = (fft_add_c(magnitudes, 3, 4)) / 2 * band_eq_gains[0];
        eq_band_magnitudes[1] = (fft_add_c(magnitudes, 4, 5)) / 2 * band_eq_gains[1];
        eq_band_magnitudes[2] = (fft_add_c(magnitudes, 5, 6)) / 2 * band_eq_gains[2];
        eq_band_magnitudes[3] = (fft_add_c(magnitudes, 6, 7)) / 2 * band_eq_gains[3];
        eq_band_magnitudes[4] = (fft_add_c(magnitudes, 7, 8)) / 2 * band_eq_gains[4];
        eq_band_magnitudes[5] = (fft_add_c(magnitudes, 8, 9)) / 2 * band_eq_gains[5];
        eq_band_magnitudes[6] = (fft_add_c(magnitudes, 9, 10)) / 2 * band_eq_gains[6];
        eq_band_magnitudes[7] = (fft_add_c(magnitudes, 10, 11)) / 2 * band_eq_gains[7];
        eq_band_magnitudes[8] = (fft_add_c(magnitudes, 11, 12)) / 2 * band_eq_gains[8];
        eq_band_magnitudes[9] = (fft_add_c(magnitudes, 12, 13)) / 2 * band_eq_gains[9];
        eq_band_magnitudes[10] = (fft_add_c(magnitudes, 13, 14)) / 2 * band_eq_gains[10];
        eq_band_magnitudes[11] = (fft_add_c(magnitudes, 14, 16)) / 3 * band_eq_gains[11];
        eq_band_magnitudes[12] = (fft_add_c(magnitudes, 16, 18)) / 3 * band_eq_gains[12];
        eq_band_magnitudes[13] = (fft_add_c(magnitudes, 18, 20)) / 3 * band_eq_gains[13];
        eq_band_magnitudes[14] = (fft_add_c(magnitudes, 20, 24)) / 5 * band_eq_gains[14];
        eq_band_magnitudes[15] = (fft_add_c(magnitudes, 24, 28)) / 5 * band_eq_gains[15];
        eq_band_magnitudes[16] = (fft_add_c(magnitudes, 28, 32)) / 5 * band_eq_gains[16];
        eq_band_magnitudes[17] = (fft_add_c(magnitudes, 32, 36)) / 5 * band_eq_gains[17];
        eq_band_magnitudes[18] = (fft_add_c(magnitudes, 36, 42)) / 7 * band_eq_gains[18];
        eq_band_magnitudes[19] = (fft_add_c(magnitudes, 42, 48)) / 7 * band_eq_gains[19];
        eq_band_magnitudes[20] = (fft_add_c(magnitudes, 48, 56)) / 9 * band_eq_gains[20];
        eq_band_magnitudes[21] = (fft_add_c(magnitudes, 56, 64)) / 9 * band_eq_gains[21];
        eq_band_magnitudes[22] = (fft_add_c(magnitudes, 64, 74)) / 11 * band_eq_gains[22];
        eq_band_magnitudes[23] = (fft_add_c(magnitudes, 74, 84)) / 11 * band_eq_gains[23];
        eq_band_magnitudes[24] = (fft_add_c(magnitudes, 84, 97)) / 14 * band_eq_gains[24];
        eq_band_magnitudes[25] = (fft_add_c(magnitudes, 97, 110)) / 14 * band_eq_gains[25];
        eq_band_magnitudes[26] = (fft_add_c(magnitudes, 110, 128)) / 19 * band_eq_gains[26];
        eq_band_magnitudes[27] = (fft_add_c(magnitudes, 128, 146)) / 19 * band_eq_gains[27];
        eq_band_magnitudes[28] = (fft_add_c(magnitudes, 146, 170)) / 25 * band_eq_gains[28];
        eq_band_magnitudes[29] = (fft_add_c(magnitudes, 170, 194)) / 25 * band_eq_gains[29];
        eq_band_magnitudes[30] = (fft_add_c(magnitudes, 194, 224)) / 31 * band_eq_gains[30];
        eq_band_magnitudes[31] = (fft_add_c(magnitudes, 224, 255)) / 32 * band_eq_gains[31];

        // 步骤 B: 计算Top-N平均值，用于更新动态天花板 (AGC)
        float sorted_magnitudes[NUM_BANDS];
        memcpy(sorted_magnitudes, eq_band_magnitudes, sizeof(eq_band_magnitudes));
        qsort(sorted_magnitudes, NUM_BANDS, sizeof(float), compare_floats_desc);

        float top_n_avg = 0;
        for (int i = 0; i < AGC_TOP_N_BANDS; i++)
        {
          top_n_avg += sorted_magnitudes[i];
        }
        top_n_avg /= AGC_TOP_N_BANDS;

        // 步骤 C: 更新动态“天花板”
        if (top_n_avg > dynamic_ceiling) {
          dynamic_ceiling = top_n_avg; // 如果当前峰值更高，天花板立刻抬升
        } else {
          dynamic_ceiling *= CEILING_DECAY_RATE; // 否则，天花板缓慢衰减
        }
        // 确保天花板不会低于设定的“地基”
        float current_ceiling = (dynamic_ceiling > MAGNITUDE_FLOOR) ? dynamic_ceiling : MAGNITUDE_FLOOR;

        // 步骤 D: 归一化并计算最终高度
        uint8_t new_heights[NUM_BANDS];
        float dynamic_range = current_ceiling - MAGNITUDE_FLOOR;
        if (dynamic_range < 1.0f) dynamic_range = 1.0f; // 避免除以零

        for (int i = 0; i < NUM_BANDS; i++)
        {
          // 将频段幅值归一化到 0.0 - 1.0 之间
          float normalized_height = (eq_band_magnitudes[i] - MAGNITUDE_FLOOR) / dynamic_range;

          // 步骤 E: (可选但推荐) 应用伽马校正来调整视觉曲线
          // gamma < 1.0 会提升中低范围的亮度，使频谱看起来更“饱满”
          float powered_height = powf(fmaxf(0.0f, normalized_height), GAMMA_CORRECTION);

          // 将结果映射到最终的显示高度 (0 到 MATRIX_HEIGHT-1)
          int height = (int)(powered_height * (MATRIX_HEIGHT - 1) + 0.5f);
          if (height >= MATRIX_HEIGHT) height = MATRIX_HEIGHT - 1;
          if (height < 0) height = 0;
          new_heights[i] = height;
        }

        // --- 核心优化结束 ---

        // 使用互斥锁安全地更新共享的显示数据
        xSemaphoreTake(data_mutex, portMAX_DELAY);
        memcpy(display_heights, new_heights, sizeof(new_heights));
        xSemaphoreGive(data_mutex);

        // 重置缓冲区指针，准备下一次FFT
        buffer_pos = 0;
      }
    }
    else
    {
      // 如果队列接收超时 (表示没有新的音频数据)，则让频谱缓慢归零
      xSemaphoreTake(data_mutex, portMAX_DELAY);
      bool changed = false;
      for(int i=0; i<NUM_BANDS; i++) {
          if(display_heights[i] > 0) {
              display_heights[i]--; // 简单的逐帧衰减效果
              changed = true;
          }
      }
      // 如果所有频段都已归零，重置动态天花板，以便音乐恢复时能快速响应
      if(!changed) {
        dynamic_ceiling = MAGNITUDE_FLOOR;
      }
      xSemaphoreGive(data_mutex);
    }
  }
  // 理论上不会执行到这里
  free(audio_buffer);
  vTaskDelete(NULL);
}

// --- 公共API函数 (保持不变) ---

esp_err_t fft_analyzer_init(void)
{
  audio_queue = xQueueCreate(10, sizeof(audio_chunk_t));
  if (!audio_queue)
  {
    ESP_LOGE(TAG, "Failed to create audio queue");
    return ESP_FAIL;
  }
  data_mutex = xSemaphoreCreateMutex();
  if (!data_mutex)
  {
    ESP_LOGE(TAG, "Failed to create data mutex");
    return ESP_FAIL;
  }
  xTaskCreate(fft_task, "fft_task", 4096, NULL, 5, NULL);
  ESP_LOGI(TAG, "FFT Analyzer initialized.");
  return ESP_OK;
}

esp_err_t fft_analyzer_push_audio_data(const int16_t *data, int len, int channels)
{
  // 复制数据以避免多任务冲突
  int16_t *data_copy = malloc(len * channels * sizeof(int16_t));
  if (!data_copy)
  {
    return ESP_ERR_NO_MEM;
  }
  memcpy(data_copy, data, len * channels * sizeof(int16_t));
  
  audio_chunk_t chunk = {.data = data_copy, .len = len, .channels = channels};
  
  if (xQueueSend(audio_queue, &chunk, pdMS_TO_TICKS(20)) != pdPASS)
  {
    ESP_LOGW(TAG, "Audio queue full, discarding data.");
    free(data_copy); // 发送失败时释放内存
    return ESP_FAIL;
  }
  return ESP_OK;
}

void fft_analyzer_get_heights(uint8_t heights[NUM_BANDS])
{
  if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
  {
    memcpy(heights, display_heights, NUM_BANDS * sizeof(uint8_t));
    xSemaphoreGive(data_mutex);
  }
}
