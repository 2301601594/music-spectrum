#include "fft_analyzer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_dsp.h"
#include "math.h"
#include <string.h>

static const char *TAG = "FFT_ANALYZER";

// --- 内部数据结构和变量 ---
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

static const uint16_t band_cutoffs[NUM_BANDS + 1] = {
    2, 3, 4, 5, 6, 7, 8, 9, 11, 13, 15, 18, 21, 25, 30, 35, 42, 50, 60,
    71, 85, 101, 120, 143, 170, 202, 240, 285, 339, 403, 479, 511};

/**
 * @brief FFT核心处理任务
 */
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

  int16_t *audio_buffer = (int16_t *)malloc(FFT_N * sizeof(int16_t) * 2);
  int buffer_pos = 0;
  audio_chunk_t chunk;

  while (1)
  {
    if (xQueueReceive(audio_queue, &chunk, portMAX_DELAY) == pdPASS)
    {

      int samples_to_copy = chunk.len;
      if (buffer_pos + samples_to_copy > FFT_N)
      {
        samples_to_copy = FFT_N - buffer_pos;
      }

      if (chunk.channels == 1)
      {
        for (int i = 0; i < samples_to_copy; i++)
        {
          audio_buffer[buffer_pos + i] = chunk.data[i];
        }
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
        // --- 修改开始 ---
        // 原来的错误代码:
        // for (int i = 0; i < FFT_N; i++) {
        //     fft_input[i] = (float)audio_buffer[i] * hanning_window[i];
        // }

        // 正确准备复数输入 (实部为音频数据，虚部为0)
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

        uint8_t new_heights[NUM_BANDS];
        for (int i = 0; i < NUM_BANDS; i++)
        {
          float avg_magnitude = 0;
          uint16_t start_bin = (i == 0) ? 0 : band_cutoffs[i - 1];
          uint16_t end_bin = band_cutoffs[i];
          for (int j = start_bin; j < end_bin; j++)
          {
            avg_magnitude += magnitudes[j];
          }
          avg_magnitude /= (end_bin - start_bin);

          int height = (int)(avg_magnitude / 20000.0f * (MATRIX_HEIGHT - 1));
          if (height >= MATRIX_HEIGHT)
            height = MATRIX_HEIGHT - 1;
          if (height < 0)
            height = 0;

          new_heights[i] = height;
        }

        xSemaphoreTake(data_mutex, portMAX_DELAY);
        memcpy(display_heights, new_heights, sizeof(new_heights));
        xSemaphoreGive(data_mutex);

        buffer_pos = 0;
      }
    }
  }
}

// --- 公共API实现 ---
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

  audio_chunk_t chunk = {
      .data = data_copy,
      .len = len,
      .channels = channels};

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
    // **核心修正**: 释放正确的互斥锁
    xSemaphoreGive(data_mutex);
  }
}
