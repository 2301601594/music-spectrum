#include "fft_analyzer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_dsp.h"
#include "math.h"
#include <string.h>
#include <stdlib.h>

// 新增: 包含音频管理器头文件以获取当前模式
#include "audio_manager.h"

static const char *TAG = "FFT_ANALYZER";

// --- 配置常量 ---
#define AGC_TOP_N_BANDS 3
#define GAMMA_CORRECTION 0.9f

typedef struct {
    int16_t *data;
    int len;
    int channels;
} audio_chunk_t;

static QueueHandle_t audio_queue = NULL;
static SemaphoreHandle_t data_mutex = NULL;
static uint8_t display_heights[NUM_BANDS] = {0};

static float fft_input[FFT_N * 2];
static float hanning_window[FFT_N];

// --- 核心修改 1: 为不同模式定义独立的静态EQ增益曲线 ---

/**
 * @brief 播放器模式EQ曲线 (激进型)
 * 用于处理高质量、干净的WAV音源，旨在创造最佳视觉效果。
 */
static const double band_eq_gains_player[NUM_BANDS] = {
    // 低频抑制, 中频平稳, 高频大幅增强
    0.50, 0.60, 0.70, 0.80, 0.90, 1.0, 1.1, 1.2,
    1.3,  1.4,  1.5,  1.6,  1.7,  1.8,  1.9,  2.0,
    2.5,  3.0,  3.5,  4.0,  5.0,  6.0,  7.5,  9.0,
    11.0, 13.0, 15.0, 18.0, 22.0, 26.0, 30.0, 35.0
};

/**
 * @brief 麦克风模式EQ曲线 (保守型)
 * 用于处理包含环境噪声的原始音源，旨在真实反映并避免过度放大噪声。
 */
static const double band_eq_gains_mic[NUM_BANDS] = {
    // 轻微抑制极低频噪声, 中高频做适度补偿性提升
    0.8,  0.9,  1.0,  1.0,  1.1,  1.1,  1.2,  1.2,
    1.3,  1.3,  1.4,  1.4,  1.5,  1.6,  1.7,  1.8,
    1.9,  2.0,  2.1,  2.2,  2.4,  2.6,  2.8,  3.0,
    3.2,  3.5,  3.8,  4.2,  4.6,  5.0,  5.5,  0.5
};

// 辅助函数: 用于qsort的降序比较
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

// 辅助函数: 累加指定范围内的FFT幅值
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
static void fft_task(void *pvParameters) {
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Not possible to initialize FFT2R. Error = %d", ret);
        vTaskDelete(NULL);
        return;
    }
    dsps_wind_hann_f32(hanning_window, FFT_N);

    static float dynamic_ceiling = 100.0f;
    const float MAGNITUDE_FLOOR = 20.0f;
    const float CEILING_DECAY_RATE = 0.992f;

    int16_t *audio_buffer = (int16_t *)malloc(FFT_N * sizeof(int16_t));
    int buffer_pos = 0;
    audio_chunk_t chunk;

    while (1) {
        if (xQueueReceive(audio_queue, &chunk, pdMS_TO_TICKS(100)) == pdPASS) {
            // 音频数据填充逻辑
            int samples_in_chunk = chunk.len;
            int frames_to_process = 0;
            if (chunk.channels == 1) {
                frames_to_process = samples_in_chunk;
                if (buffer_pos + frames_to_process > FFT_N) frames_to_process = FFT_N - buffer_pos;
                memcpy(&audio_buffer[buffer_pos], chunk.data, frames_to_process * sizeof(int16_t));
            } else if (chunk.channels == 2) {
                frames_to_process = samples_in_chunk / 2;
                if (buffer_pos + frames_to_process > FFT_N) frames_to_process = FFT_N - buffer_pos;
                for (int i = 0; i < frames_to_process; i++) {
                    audio_buffer[buffer_pos + i] = (chunk.data[i * 2] + chunk.data[i * 2 + 1]) / 2;
                }
            }
            buffer_pos += frames_to_process;
            free(chunk.data);

            if (buffer_pos >= FFT_N) {
                // FFT准备和计算
                for (int i = 0; i < FFT_N; i++) {
                    fft_input[i * 2] = (float)audio_buffer[i] * hanning_window[i];
                    fft_input[i * 2 + 1] = 0.0f;
                }
                dsps_fft2r_fc32(fft_input, FFT_N);
                dsps_bit_rev_fc32_ansi(fft_input, FFT_N);
                static float magnitudes[FFT_N / 2];
                for (int i = 0; i < FFT_N / 2; i++) {
                    float real = fft_input[i * 2];
                    float imag = fft_input[i * 2 + 1];
                    magnitudes[i] = sqrtf(real * real + imag * imag);
                }

                // --- 核心修改 2: 根据当前模式选择EQ曲线 ---
                audio_mode_t current_mode = audio_manager_get_mode();
                const double *active_eq_gains = (current_mode == AUDIO_MODE_PLAYER) ? band_eq_gains_player : band_eq_gains_mic;

                // 使用被选中的EQ曲线进行计算
                float eq_band_magnitudes[NUM_BANDS];
                eq_band_magnitudes[0] = (fft_add_c(magnitudes, 3, 4)) / 2 * active_eq_gains[0];
                eq_band_magnitudes[1] = (fft_add_c(magnitudes, 4, 5)) / 2 * active_eq_gains[1];
                eq_band_magnitudes[2] = (fft_add_c(magnitudes, 5, 6)) / 2 * active_eq_gains[2];
                eq_band_magnitudes[3] = (fft_add_c(magnitudes, 6, 7)) / 2 * active_eq_gains[3];
                eq_band_magnitudes[4] = (fft_add_c(magnitudes, 7, 8)) / 2 * active_eq_gains[4];
                eq_band_magnitudes[5] = (fft_add_c(magnitudes, 8, 9)) / 2 * active_eq_gains[5];
                eq_band_magnitudes[6] = (fft_add_c(magnitudes, 9, 10)) / 2 * active_eq_gains[6];
                eq_band_magnitudes[7] = (fft_add_c(magnitudes, 10, 11)) / 2 * active_eq_gains[7];
                eq_band_magnitudes[8] = (fft_add_c(magnitudes, 11, 12)) / 2 * active_eq_gains[8];
                eq_band_magnitudes[9] = (fft_add_c(magnitudes, 12, 13)) / 2 * active_eq_gains[9];
                eq_band_magnitudes[10] = (fft_add_c(magnitudes, 13, 14)) / 2 * active_eq_gains[10];
                eq_band_magnitudes[11] = (fft_add_c(magnitudes, 14, 16)) / 3 * active_eq_gains[11];
                eq_band_magnitudes[12] = (fft_add_c(magnitudes, 16, 18)) / 3 * active_eq_gains[12];
                eq_band_magnitudes[13] = (fft_add_c(magnitudes, 18, 20)) / 3 * active_eq_gains[13];
                eq_band_magnitudes[14] = (fft_add_c(magnitudes, 20, 24)) / 5 * active_eq_gains[14];
                eq_band_magnitudes[15] = (fft_add_c(magnitudes, 24, 28)) / 5 * active_eq_gains[15];
                eq_band_magnitudes[16] = (fft_add_c(magnitudes, 28, 32)) / 5 * active_eq_gains[16];
                eq_band_magnitudes[17] = (fft_add_c(magnitudes, 32, 36)) / 5 * active_eq_gains[17];
                eq_band_magnitudes[18] = (fft_add_c(magnitudes, 36, 42)) / 7 * active_eq_gains[18];
                eq_band_magnitudes[19] = (fft_add_c(magnitudes, 42, 48)) / 7 * active_eq_gains[19];
                eq_band_magnitudes[20] = (fft_add_c(magnitudes, 48, 56)) / 9 * active_eq_gains[20];
                eq_band_magnitudes[21] = (fft_add_c(magnitudes, 56, 64)) / 9 * active_eq_gains[21];
                eq_band_magnitudes[22] = (fft_add_c(magnitudes, 64, 74)) / 11 * active_eq_gains[22];
                eq_band_magnitudes[23] = (fft_add_c(magnitudes, 74, 84)) / 11 * active_eq_gains[23];
                eq_band_magnitudes[24] = (fft_add_c(magnitudes, 84, 97)) / 14 * active_eq_gains[24];
                eq_band_magnitudes[25] = (fft_add_c(magnitudes, 97, 110)) / 14 * active_eq_gains[25];
                eq_band_magnitudes[26] = (fft_add_c(magnitudes, 110, 128)) / 19 * active_eq_gains[26];
                eq_band_magnitudes[27] = (fft_add_c(magnitudes, 128, 146)) / 19 * active_eq_gains[27];
                eq_band_magnitudes[28] = (fft_add_c(magnitudes, 146, 170)) / 25 * active_eq_gains[28];
                eq_band_magnitudes[29] = (fft_add_c(magnitudes, 170, 194)) / 25 * active_eq_gains[29];
                eq_band_magnitudes[30] = (fft_add_c(magnitudes, 194, 224)) / 31 * active_eq_gains[30];
                eq_band_magnitudes[31] = (fft_add_c(magnitudes, 224, 255)) / 32 * active_eq_gains[31];
                
                // AGC 和高度计算
                float sorted_magnitudes[NUM_BANDS];
                memcpy(sorted_magnitudes, eq_band_magnitudes, sizeof(eq_band_magnitudes));
                qsort(sorted_magnitudes, NUM_BANDS, sizeof(float), compare_floats_desc);
                float top_n_avg = 0;
                for (int i = 0; i < AGC_TOP_N_BANDS; i++) top_n_avg += sorted_magnitudes[i];
                top_n_avg /= AGC_TOP_N_BANDS;
                if (top_n_avg > dynamic_ceiling) dynamic_ceiling = top_n_avg;
                else dynamic_ceiling *= CEILING_DECAY_RATE;
                float current_ceiling = (dynamic_ceiling > MAGNITUDE_FLOOR) ? dynamic_ceiling : MAGNITUDE_FLOOR;
                uint8_t new_heights[NUM_BANDS];
                float dynamic_range = current_ceiling - MAGNITUDE_FLOOR;
                if (dynamic_range < 1.0f) dynamic_range = 1.0f;
                for (int i = 0; i < NUM_BANDS; i++) {
                    float normalized_height = (eq_band_magnitudes[i] - MAGNITUDE_FLOOR) / dynamic_range;
                    float powered_height = powf(fmaxf(0.0f, normalized_height), GAMMA_CORRECTION);
                    int height = (int)(powered_height * (MATRIX_HEIGHT - 1) + 0.5f);
                    if (height >= MATRIX_HEIGHT) height = MATRIX_HEIGHT - 1;
                    if (height < 0) height = 0;
                    new_heights[i] = height;
                }

                xSemaphoreTake(data_mutex, portMAX_DELAY);
                memcpy(display_heights, new_heights, sizeof(new_heights));
                xSemaphoreGive(data_mutex);
                buffer_pos = 0;
            }
        } else {
            // --- 核心修改 3: 超时处理根据模式区分 ---
            audio_mode_t current_mode = audio_manager_get_mode();
            xSemaphoreTake(data_mutex, portMAX_DELAY);
            if (current_mode == AUDIO_MODE_PLAYER) {
                // 播放器模式: 音乐结束，缓慢衰减
                bool changed = false;
                for(int i = 0; i < NUM_BANDS; i++) {
                    if(display_heights[i] > 0) {
                        display_heights[i]--;
                        changed = true;
                    }
                }
                if(!changed) {
                    dynamic_ceiling = MAGNITUDE_FLOOR;
                }
            } else { // AUDIO_MODE_MIC
                // 麦克风模式: 环境安静，立即归零
                memset(display_heights, 0, sizeof(display_heights));
                dynamic_ceiling = MAGNITUDE_FLOOR;
            }
            xSemaphoreGive(data_mutex);
        }
    }
    free(audio_buffer);
    vTaskDelete(NULL);
}

// --- 公共API函数 ---
esp_err_t fft_analyzer_init(void) {
    audio_queue = xQueueCreate(10, sizeof(audio_chunk_t));
    if (!audio_queue) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        return ESP_FAIL;
    }
    data_mutex = xSemaphoreCreateMutex();
    if (!data_mutex) {
        ESP_LOGE(TAG, "Failed to create data mutex");
        return ESP_FAIL;
    }
    xTaskCreate(fft_task, "fft_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "FFT Analyzer initialized.");
    return ESP_OK;
}

esp_err_t fft_analyzer_push_audio_data(const int16_t *data, int len, int channels) {
    int16_t *data_copy = malloc(len * channels * sizeof(int16_t));
    if (!data_copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(data_copy, data, len * channels * sizeof(int16_t));
    
    audio_chunk_t chunk = {.data = data_copy, .len = len, .channels = channels};
    
    if (xQueueSend(audio_queue, &chunk, pdMS_TO_TICKS(20)) != pdPASS) {
        ESP_LOGW(TAG, "Audio queue full, discarding data.");
        free(data_copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void fft_analyzer_get_heights(uint8_t heights[NUM_BANDS]) {
    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(heights, display_heights, NUM_BANDS * sizeof(uint8_t));
        xSemaphoreGive(data_mutex);
    }
}
