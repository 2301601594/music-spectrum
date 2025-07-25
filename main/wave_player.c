#include "wave_player.h"
#include <string.h>
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_vfs_fat.h"
#include "fft_analyzer.h"

static const char *TAG = "WAVE_PLAYER";

// FreeRTOS 句柄
static TaskHandle_t player_task_handle = NULL; // 播放器任务句柄
static QueueHandle_t player_cmd_queue = NULL;  // 命令队列句柄
static SemaphoreHandle_t status_mutex = NULL;  // 状态互斥锁

// 播放器内部状态
static player_status_t internal_status;         // 播放器状态
static FILE *current_fp = NULL;                 // 当前文件指针
static i2s_chan_handle_t tx_handle = NULL;      // I2S 发送通道句柄
static uint32_t data_start_pos = 0;             // WAV文件数据区的起始位置
static uint32_t total_data_bytes = 0;           // 数据区总字节数

// 私有函数声明
static void wave_player_task(void *pvParameters);
static esp_err_t player_handle_play(player_cmd_msg_t *msg);
static void player_handle_pause();
static void player_handle_resume();
static void player_handle_stop();
static void player_handle_seek(player_cmd_msg_t *msg); // 新增
static void cleanup_resources();

/**
 * @brief 播放器核心任务
 * 获取控制队列中的控制命令，利用状态机对播放器进行控制
 */
static void wave_player_task(void *pvParameters)
{
  player_cmd_msg_t msg;
  uint8_t *read_buf = (uint8_t *)malloc(I2S_BUFFER_SIZE);
  size_t bytes_read = 0;
  size_t bytes_written = 0;

  while (1)
  {
    // 根据播放状态决定等待命令的超时时间
    // 播放时: 非阻塞检查命令 (timeout=0)
    // 其他状态: 阻塞等待，直到新命令到来
    TickType_t wait_time = (internal_status.state == PLAYER_STATE_PLAYING) ? 0 : portMAX_DELAY;

    if (xQueueReceive(player_cmd_queue, &msg, wait_time) == pdPASS)
    {
      ESP_LOGI(TAG, "Received command: %d", msg.cmd);
      switch (msg.cmd)
      {
      case PLAYER_CMD_PLAY:
        player_handle_play(&msg);
        break;
      case PLAYER_CMD_PAUSE:
        player_handle_pause();
        break;
      case PLAYER_CMD_RESUME:
        player_handle_resume();
        break;
      case PLAYER_CMD_STOP:
        player_handle_stop();
        break;
      case PLAYER_CMD_SEEK:
        player_handle_seek(&msg);
        break;
      default:
        ESP_LOGW(TAG, "Unknown command");
        break;
      }
    }

    // 如果当前是播放状态，则执行数据泵逻辑
    if (internal_status.state == PLAYER_STATE_PLAYING && current_fp)
    {
      bytes_read = fread(read_buf, 1, I2S_BUFFER_SIZE, current_fp);
      if (bytes_read > 0)
      {
        i2s_channel_write(tx_handle, read_buf, bytes_read, &bytes_written, portMAX_DELAY);
        if (bytes_written > 0)
        {
          fft_analyzer_push_audio_data((int16_t *)read_buf, bytes_written / 2, internal_status.num_channels);
        }

        // 更新播放进度
        long current_file_pos = ftell(current_fp);
        uint32_t bytes_played = current_file_pos > data_start_pos ? current_file_pos - data_start_pos : 0;
        if (internal_status.byte_rate > 0)
        {
          uint32_t current_sec = bytes_played / internal_status.byte_rate;
          xSemaphoreTake(status_mutex, portMAX_DELAY);
          internal_status.current_position_sec = current_sec;
          xSemaphoreGive(status_mutex);
        }
      }
      else
      {
        ESP_LOGI(TAG, "End of file reached.");
        player_handle_stop();
      }
    }
  }
  free(read_buf);
}

/**
 * @brief 处理 SEEK 命令
 * 用于在播放过程中跳转到指定位置
 */
/**
 * @brief 处理跳转(Seek)命令
 */
static void player_handle_seek(player_cmd_msg_t *msg)
{
  // 只有在播放或暂停时才能Seek
  if (internal_status.state == PLAYER_STATE_STOPPED || !current_fp || total_data_bytes == 0)
  {
    ESP_LOGW(TAG, "Seek command ignored: Player is stopped or no track loaded.");
    return;
  }

  if (msg->seek_percent < 0 || msg->seek_percent > 100)
  {
    ESP_LOGE(TAG, "Invalid seek percentage: %d", msg->seek_percent);
    return;
  }

  // 1. 计算目标位置的字节偏移量
  uint32_t seek_offset_bytes = (uint32_t)((total_data_bytes / 100.0f) * msg->seek_percent);

  // 2. 字节对齐，确保从一个完整的采样点开始读取
  uint16_t block_align = (internal_status.bits_per_sample / 8) * internal_status.num_channels;
  if (block_align > 0)
  {
    seek_offset_bytes = (seek_offset_bytes / block_align) * block_align;
  }

  long target_pos = data_start_pos + seek_offset_bytes;
  ESP_LOGI(TAG, "Seeking to %d%%, byte position %ld", msg->seek_percent, target_pos);

  // 3. 记录seek前的状态
  bool was_playing = (internal_status.state == PLAYER_STATE_PLAYING);

  // 4. 如果正在播放，先禁用通道以停止并刷新DMA
  if (was_playing)
  {
    i2s_channel_disable(tx_handle);
  }

  // 5. 使用 fseek 跳转文件指针
  if (fseek(current_fp, target_pos, SEEK_SET) != 0)
  {
    ESP_LOGE(TAG, "fseek failed!");
    // 即使fseek失败，如果之前是播放状态，也应该尝试恢复硬件状态
    if (was_playing)
    {
      i2s_channel_enable(tx_handle);
    }
    return;
  }

  // 6. 如果之前是播放状态，重新使能通道以让主循环继续播放
  if (was_playing)
  {
    i2s_channel_enable(tx_handle);
  }

  // 7. 更新状态中的当前秒数
  uint32_t new_pos_sec = (uint32_t)((internal_status.total_duration_sec / 100.0f) * msg->seek_percent);
  xSemaphoreTake(status_mutex, portMAX_DELAY);
  internal_status.current_position_sec = new_pos_sec;
  xSemaphoreGive(status_mutex);
}

/**
 * @brief 处理播放命令
 */
static esp_err_t player_handle_play(player_cmd_msg_t *msg)
{
  player_handle_stop();

  current_fp = fopen(msg->filepath, "rb");
  if (!current_fp)
  {
    ESP_LOGE(TAG, "Failed to open file: %s", msg->filepath);
    return ESP_FAIL;
  }

  wav_header_t wav_header;
  if (fread(&wav_header, 1, sizeof(wav_header_t), current_fp) != sizeof(wav_header_t))
  {
    ESP_LOGE(TAG, "Failed to read WAV header");
    cleanup_resources();
    return ESP_FAIL;
  }

  fseek(current_fp, 12, SEEK_SET);
  char chunk_id[4];
  uint32_t chunk_size;
  while (fread(chunk_id, 1, 4, current_fp) == 4 && fread(&chunk_size, 1, 4, current_fp) == 4)
  {
    if (strncmp(chunk_id, "data", 4) == 0)
    {
      data_start_pos = ftell(current_fp);
      total_data_bytes = chunk_size;
      break;
    }
    fseek(current_fp, chunk_size, SEEK_CUR);
  }

  if (data_start_pos == 0)
  {
    ESP_LOGE(TAG, "Could not find 'data' chunk.");
    cleanup_resources();
    return ESP_FAIL;
  }

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(wav_header.sample_rate),
      .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(wav_header.bits_per_sample, wav_header.num_channels),
      .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = I2S_BCK_IO, .ws = I2S_WS_IO, .dout = I2S_DO_IO, .din = I2S_GPIO_UNUSED},
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

  // 更新状态，包括新增的字段
  xSemaphoreTake(status_mutex, portMAX_DELAY);
  internal_status.state = PLAYER_STATE_PLAYING;
  strlcpy(internal_status.current_track, msg->filepath, FILE_PATH_MAX);
  internal_status.total_duration_sec = total_data_bytes / wav_header.byte_rate;
  internal_status.current_position_sec = 0;
  internal_status.num_channels = wav_header.num_channels;
  internal_status.bits_per_sample = wav_header.bits_per_sample;
  internal_status.byte_rate = wav_header.byte_rate;
  xSemaphoreGive(status_mutex);

  return ESP_OK;
}

/**
 * @brief 处理暂停命令
 */
static void player_handle_pause()
{
  if (internal_status.state == PLAYER_STATE_PLAYING)
  {
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    internal_status.state = PLAYER_STATE_PAUSED;
    xSemaphoreGive(status_mutex);
    if (tx_handle)
    {
      i2s_channel_disable(tx_handle);
    }
    ESP_LOGI(TAG, "Playback paused.");
  }
}

/**
 * @brief 处理继续/恢复命令
 */
static void player_handle_resume()
{
  if (internal_status.state == PLAYER_STATE_PAUSED)
  {
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    internal_status.state = PLAYER_STATE_PLAYING;
    xSemaphoreGive(status_mutex);
    if (tx_handle)
    {
      i2s_channel_enable(tx_handle);
    }
    ESP_LOGI(TAG, "Playback resumed.");
  }
}

/**
 * @brief 处理停止命令
 */
static void player_handle_stop()
{
  if (internal_status.state != PLAYER_STATE_STOPPED)
  {
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    internal_status.state = PLAYER_STATE_STOPPED;
    internal_status.current_position_sec = 0;
    internal_status.total_duration_sec = 0;
    strcpy(internal_status.current_track, "N/A");
    xSemaphoreGive(status_mutex);

    cleanup_resources();
    ESP_LOGI(TAG, "Playback stopped.");
  }
}

/**
 * @brief 清理所有资源
 */
static void cleanup_resources()
{
  if (tx_handle)
  {
    i2s_channel_disable(tx_handle);
    i2s_del_channel(tx_handle);
    tx_handle = NULL;
  }
  if (current_fp)
  {
    fclose(current_fp);
    current_fp = NULL;
  }
  data_start_pos = 0;
  total_data_bytes = 0;
}

// --- 公共 API 实现 ---

/**
 * @brief 初始化播放器
 */
esp_err_t wave_player_init(void)
{
  player_cmd_queue = xQueueCreate(10, sizeof(player_cmd_msg_t));
  status_mutex = xSemaphoreCreateMutex();
  player_handle_stop();
  xTaskCreate(wave_player_task, "player_task", 4096, NULL, 5, &player_task_handle);
  return ESP_OK;
}

/**
 * @brief 向播放器发送控制命令
 */
esp_err_t wave_player_send_cmd(player_cmd_msg_t *msg)
{
  if (xQueueSend(player_cmd_queue, msg, pdMS_TO_TICKS(100)) != pdPASS)
  {
    ESP_LOGE(TAG, "Failed to send command to queue");
    return ESP_FAIL;
  }
  return ESP_OK;
}


/**
 * @brief 获取播放器当前状态
 */
void wave_player_get_status(player_status_t *status)
{
  if (xSemaphoreTake(status_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
  {
    memcpy(status, &internal_status, sizeof(player_status_t));
    xSemaphoreGive(status_mutex);
  }
}