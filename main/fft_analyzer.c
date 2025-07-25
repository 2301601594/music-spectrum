#include "fft_analyzer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_dsp.h"
#include "math.h"
#include <string.h>
#include <stdlib.h> // 新增: 用于 qsort

static const char *TAG = "FFT_ANALYZER";

#define FFT_N 512

typedef struct
{
  int16_t *data;
  int len;
  int channels;
} audio_chunk_t;

static QueueHandle_t audio_queue = NULL;
static SemaphoreHandle_t data_mutex = NULL;
static uint8_t display_heights[NUM_BANDS] = {0};

static float fft_input[FFT_N * 2];
static float hanning_window[FFT_N];

// --- 核心修改 1: 提供一组更平滑的静态EQ增益曲线 ---
// 这组增益曲线比之前的更保守，旨在提供一个良好的起点。
// 强烈建议您根据自己的调试结果来微调这些值。
static const double band_eq_gains[NUM_BANDS] = {
    // Lows (Bands 0-7) - 抑制低频
    0.8, 0.9, 1.0, 1.2, 1.5, 1.8, 2.2, 2.6,
    // Low-Mids (Bands 8-15) - 从低到中频平滑提升
    3.0, 3.5, 4.0, 4.5, 5.0, 5.5, 6.0, 6.5,
    // High-Mids (Bands 16-23) - 从中频顶峰平滑下降
    6.5, 6.0, 5.5, 5.0, 4.5, 4.0, 3.5, 3.0,
    // Highs (Bands 24-31) - 抑制高频
    2.6, 2.2, 1.8, 1.5, 1.4, 1.3, 1.2, 1.1};

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

static void fft_task(void *pvParameters)
{
  esp_err_t ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "Not possible to initialize FFT2R. Error = %d", ret);
    vTaskDelete(NULL);
    return;
  }
  dsps_wind_hann_f32(hanning_window, FFT_N);

  // --- 核心修改 2: 引入Top-N-Average AGC的变量 ---
  static float dynamic_ceiling = 100.0f; // 动态“天花板”
  const float MAGNITUDE_FLOOR = 20.0f;   // 定义一个更低的“地基”

  int16_t *audio_buffer = (int16_t *)malloc(FFT_N * sizeof(int16_t));
  int buffer_pos = 0;
  audio_chunk_t chunk;

  while (1)
  {
    if (xQueueReceive(audio_queue, &chunk, portMAX_DELAY) == pdPASS)
    {
      // (音频数据填充逻辑保持不变)
      int samples_to_copy = chunk.len;
      if (buffer_pos + samples_to_copy > FFT_N)
        samples_to_copy = FFT_N - buffer_pos;
      if (chunk.channels == 1)
      {
        memcpy(&audio_buffer[buffer_pos], chunk.data, samples_to_copy * sizeof(int16_t));
      }
      else
      {
        for (int i = 0; i < samples_to_copy; i++)
        {
          audio_buffer[buffer_pos + i] = (chunk.data[i * 2] + chunk.data[i * 2 + 1]) / 2;
        }
      }
      buffer_pos += samples_to_copy;
      free(chunk.data);

      if (buffer_pos >= FFT_N)
      {
        // (FFT计算部分保持不变)
        for (int i = 0; i < FFT_N; i++)
        {
          fft_input[i * 2] = (float)audio_buffer[i] * hanning_window[i];
          fft_input[i * 2 + 1] = 0.0f;
        }
        dsps_fft2r_fc32(fft_input, FFT_N);
        dsps_bit_rev_fc32_ansi(fft_input, FFT_N);
        static float magnitudes[FFT_N / 2];
        for (int i = 0; i < FFT_N / 2; i++)
        {
          float real = fft_input[i * 2];
          float imag = fft_input[i * 2 + 1];
          magnitudes[i] = sqrtf(real * real + imag * imag);
        }

        // --- 核心修改 3: 实现Top-5平均值动态增益算法 ---

        // 步骤1: 计算每个频段的平均幅值并应用静态EQ增益
        float eq_band_magnitudes[NUM_BANDS];
        // (频段划分逻辑保持不变)
        eq_band_magnitudes[0] = (fft_add_c(magnitudes, 3, 4)) / 2 * band_eq_gains[0];
        eq_band_magnitudes[1] = (fft_add_c(magnitudes, 4, 5)) / 2 * band_eq_gains[1];
        // ...
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

        // 步骤2: 计算Top-5平均值
        float sorted_magnitudes[NUM_BANDS];
        memcpy(sorted_magnitudes, eq_band_magnitudes, sizeof(eq_band_magnitudes));
        qsort(sorted_magnitudes, NUM_BANDS, sizeof(float), compare_floats_desc);

        float top5_avg = 0;
        for (int i = 0; i < 5; i++)
        {
          top5_avg += sorted_magnitudes[i];
        }
        top5_avg /= 5;

        // 步骤3: 更新动态“天花板”
        if (top5_avg > dynamic_ceiling)
        {
          dynamic_ceiling = top5_avg;
        }
        else
        {
          dynamic_ceiling *= 0.99; // 缓慢衰减
        }
        float current_ceiling = (dynamic_ceiling > MAGNITUDE_FLOOR) ? dynamic_ceiling : MAGNITUDE_FLOOR;

        // 步骤4: 归一化并计算最终高度
        uint8_t new_heights[NUM_BANDS];
        float dynamic_range = current_ceiling - MAGNITUDE_FLOOR;
        if (dynamic_range < 1.0f)
          dynamic_range = 1.0f;

        for (int i = 0; i < NUM_BANDS; i++)
        {
          float normalized_height = (eq_band_magnitudes[i] - MAGNITUDE_FLOOR) / dynamic_range;

          // 可选: 应用伽马校正来调整视觉曲线
          const float gamma = 0.9f; // gamma < 1.0 会提升中低范围的亮度
          float powered_height = powf(normalized_height, gamma);

          int height = (int)(powered_height * 15.0f); // 映射到0-15

          if (height > 15)
            height = 15;
          if (height < 0)
            height = 0;
          new_heights[i] = height;
        }

        // (更新显示数据逻辑保持不变)
        xSemaphoreTake(data_mutex, portMAX_DELAY);
        memcpy(display_heights, new_heights, sizeof(new_heights));
        xSemaphoreGive(data_mutex);
        buffer_pos = 0;
      }
    }
  }
}

// (公共API实现保持不变)
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
    free(data_copy);
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
