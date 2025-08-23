#include "servo.h"

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

void servo_task(void *pvParam)
{
  float angle = 0;
  while (1)
  {
    angle = 0;
    // 旋转到0度
    iot_servo_write_angle(LEDC_LOW_SPEED_MODE, 0, angle);
    vTaskDelay(pdMS_TO_TICKS(1000));

    angle = 90;
    // 旋转到90度
    iot_servo_write_angle(LEDC_LOW_SPEED_MODE, 0, angle);
    vTaskDelay(pdMS_TO_TICKS(1000));

    angle = 180;
    // 旋转到180度
    iot_servo_write_angle(LEDC_LOW_SPEED_MODE, 0, angle);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
