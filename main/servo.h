#pragma once
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "iot_servo.h"

// 定义舵机管脚
#define SERVO_CH0_PIN GPIO_NUM_7

// 舵机初始化
esp_err_t servo_init();

// 舵机任务
void servo_task(void *pvParam);




