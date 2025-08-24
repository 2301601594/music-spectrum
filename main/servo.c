#include "servo.h"

const float SMOOTH_FACTOR = 0.75;

esp_err_t servo_init()
{
  // 舵机配置结构
  servo_config_t servo_cfg = {
      .max_angle = 180,
      .min_width_us = 500,
      .max_width_us = 2500,
      .freq = 50,
      .timer_number = LEDC_TIMER_0,
      .channels = {
          .servo_pin = {
              SERVO_CH0_PIN,
          },
          .ch = {
              LEDC_CHANNEL_0,
          },
      },
      .channel_number = 1,
  };

  // 初始化舵机
  return iot_servo_init(LEDC_LOW_SPEED_MODE, &servo_cfg);
}

// 计算频谱特征
static void calculate_spectral_features(float *spectral_centroid, float *spectral_amplitude)
{
  static float prev_spectral_centroid = 0.0;
  static float prev_spectral_amplitude = 0.0;

  // 计算频谱重心
  *spectral_centroid = 0.0;

  // 计算频谱幅度
  *spectral_amplitude = 0.0;

  // 获取频谱高度
  uint8_t heights[NUM_BANDS] = {0};
  fft_analyzer_get_heights(heights);

  // 计算频谱特征
  for (int i = 0; i < NUM_BANDS; i++)
  {
    *spectral_centroid += i * heights[i];
    *spectral_amplitude += heights[i];
  }

  // 平滑处理
  *spectral_centroid = *spectral_centroid / (*spectral_amplitude + 1e-6) * 0.05 + prev_spectral_centroid * 0.95; // 计算频谱重心，避免除以零
  *spectral_amplitude = *spectral_amplitude / (NUM_BANDS + 1e-6) * 0.25 + prev_spectral_amplitude * 0.75;        // 计算频谱幅度
  prev_spectral_centroid = *spectral_centroid;
  prev_spectral_amplitude = *spectral_amplitude;
}

// 计算角度
static float spectrum2angle(float *spectral_centroid, float *spectral_amplitude)
{
  // 舵机摇摆周期
  const float SWING_PERIOD_MS = 2000.0f;
  int64_t current_time = esp_timer_get_time() / 1000;
  float swing_phase = ((float)(current_time % (int64_t)SWING_PERIOD_MS) / SWING_PERIOD_MS) * 2 * M_PI; // 0-2pi

  // 映射重心到45-135度
  float angle = *spectral_centroid / (NUM_BANDS - 1) * 90 + 45;

  // 映射幅度
  float amplitude_factor = *spectral_amplitude / 4 * 45;
  amplitude_factor = amplitude_factor < 45 ? amplitude_factor : 45;
  // 返回舵机角度
  angle += sinf(swing_phase) * amplitude_factor;
  return angle;
}
void servo_task(void *pvParam)
{
  float spectral_centroid = 0.0;
  float spectral_amplitude = 0.0;
  float current_angle = 45.0f;
  while (1)
  {
    calculate_spectral_features(&spectral_centroid, &spectral_amplitude);
    float target_angle = spectrum2angle(&spectral_centroid, &spectral_amplitude);
    current_angle = target_angle * (1-SMOOTH_FACTOR) + current_angle * SMOOTH_FACTOR;
    //ESP_LOGI("SERVO_TASK", "Spectral Centroid: %f, Spectral Amplitude: %f, Current Angle: %f", spectral_centroid, spectral_amplitude, current_angle);
    iot_servo_write_angle(LEDC_LOW_SPEED_MODE, 0, current_angle);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
