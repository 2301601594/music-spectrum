#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "esp_vfs_fat.h"

// I2S pins
#define I2S_BCK_IO GPIO_NUM_13
#define I2S_WS_IO GPIO_NUM_14
#define I2S_DO_IO GPIO_NUM_12
#define I2S_NUM I2S_NUM_0

// WAV file path in SD card root
#define WAV_FILE_PATH "/sdcard/周杰伦 - 夜曲.wav"
#define READ_BUFFER_SIZE 1024
// WAV header Structure
typedef struct
{
  char riff_header[4];     // "RIFF"
  uint32_t wav_size;       // size of entire file
  char wave_header[4];     // "WAVE"
  char fmt_header[4];      // "fmt "
  uint32_t fmt_chunk_size; // 16 for PCM
  uint16_t audio_format;   // 1 for PCM
  uint16_t num_channels;
  uint32_t sample_rate;
  uint32_t byte_rate;
  uint16_t block_align;
  uint16_t bits_per_sample;
  char data_header[4]; // "data"
  uint32_t data_size;  // size of data section
} wav_header_t;
// 播放函数
void play_wav(const char *filepath);
