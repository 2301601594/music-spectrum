#include "wave_player.h"
#include <inttypes.h> // 包含此头文件以使用 PRIu32 等宏

static const char *TAG = "WAVE_PLAYER";

// 播放函数
void play_wav(const char *filepath)
{
  FILE *fp = fopen(filepath, "rb");
  if (!fp)
  {
    ESP_LOGE(TAG, "Failed to open file: %s", filepath);
    return;
  }

  wav_header_t wav_header;
  fread(&wav_header, sizeof(wav_header_t), 1, fp);

  // 打印音乐信息 (已修正格式说明符)
  ESP_LOGI(TAG, "RIFF header: %.4s", wav_header.riff_header);
  ESP_LOGI(TAG, "WAVE header: %.4s", wav_header.wave_header);
  ESP_LOGI(TAG, "Audio Format: %u, Num Channels: %u, Sample Rate: %lu",
           wav_header.audio_format, wav_header.num_channels, (unsigned long)wav_header.sample_rate);
  ESP_LOGI(TAG, "Bits Per Sample: %u, Data Size: %lu",
           wav_header.bits_per_sample, (unsigned long)wav_header.data_size);

  // 检查是否为PCM格式 (已修正格式说明符)
  if (wav_header.audio_format != 1)
  {
    ESP_LOGE(TAG, "Unsupported audio format: %u", wav_header.audio_format);
    fclose(fp);
    return;
  }

  // 配置I2S
  i2s_chan_handle_t tx_handle;
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(wav_header.sample_rate),
      .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(wav_header.bits_per_sample, wav_header.num_channels),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = I2S_BCK_IO,
          .ws = I2S_WS_IO,
          .dout = I2S_DO_IO,
          .din = I2S_GPIO_UNUSED,
      },
  };

  i2s_chan_config_t chan_cfg = {
      .id = I2S_NUM,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 6,
      .dma_frame_num = 240,
  };

  // 初始化I2S通道
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

  // 循环读写
  uint8_t *read_buf = (uint8_t *)malloc(READ_BUFFER_SIZE);
  size_t bytes_read = 0;
  size_t bytes_written = 0;

  ESP_LOGI(TAG, "Starting audio playback...");
  while ((bytes_read = fread(read_buf, 1, READ_BUFFER_SIZE, fp)) > 0)
  {
    i2s_channel_write(tx_handle, read_buf, bytes_read, &bytes_written, portMAX_DELAY);
    if (bytes_read != bytes_written)
    {
      ESP_LOGW(TAG, "Mismatch in bytes read and written: %d vs %d", bytes_read, bytes_written);
    }
  }

  // 等待DMA缓冲读取完毕
  vTaskDelay(pdMS_TO_TICKS(100));

  // 清理
  ESP_LOGI(TAG, "Playback finished.");
  i2s_channel_disable(tx_handle);
  i2s_del_channel(tx_handle);
  free(read_buf);
  fclose(fp);
}