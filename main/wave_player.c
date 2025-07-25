#include "wave_player.h"
#include <string.h>
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_vfs_fat.h"

static const char *TAG = "WAVE_PLAYER";

// FreeRTOS 句柄
static TaskHandle_t player_task_handle = NULL; // 播放器任务句柄
static QueueHandle_t player_cmd_queue = NULL;  // 命令队列句柄
static SemaphoreHandle_t status_mutex = NULL;  // 状态互斥锁

// 播放器内部状态
static player_status_t internal_status;         // 播放器状态
static FILE *current_fp = NULL;                 // 当前文件指针
static i2s_chan_handle_t tx_handle = NULL;      // I2S 发送通道句柄
static fft_data_callback_t fft_callback = NULL; // FFT 数据回调函数指针
static uint32_t data_start_pos = 0;             // WAV文件数据区的起始位置
static uint32_t total_data_bytes = 0;           // 数据区总字节数

// 私有函数声明
static void wave_player_task(void *pvParameters);
static esp_err_t player_handle_play(player_cmd_msg_t *msg);
static void player_handle_pause();
static void player_handle_resume();
static void player_handle_stop();
static void cleanup_resources();

/**
 * @brief 播放器核心任务
 * 获取控制队列中的控制命令，利用状态机对播放器进行控制
 */
static void wave_player_task(void *pvParameters)
{
  player_cmd_msg_t msg;

  while (1)
  {
    // 1. 等待命令
    if (xQueueReceive(player_cmd_queue, &msg, portMAX_DELAY) == pdPASS)
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
      default:
        ESP_LOGW(TAG, "Unknown command");
        break;
      }
    }
  }
}

/**
 * @brief 处理播放命令
 */
static esp_err_t player_handle_play(player_cmd_msg_t *msg)
{
  // 先停止当前播放并清理资源
  player_handle_stop();

  // 打开新文件
  current_fp = fopen(msg->filepath, "rb");
  if (!current_fp)
  {
    ESP_LOGE(TAG, "Failed to open file: %s", msg->filepath);
    return ESP_FAIL;
  }

  // 读取并解析WAV头
  wav_header_t wav_header;
  if (fread(&wav_header, 1, sizeof(wav_header_t), current_fp) != sizeof(wav_header_t))
  {
    ESP_LOGE(TAG, "Failed to read WAV header");
    cleanup_resources();
    return ESP_FAIL;
  }

  // 寻找 "data" 块的起始位置
  fseek(current_fp, 12, SEEK_SET); // 跳过 "RIFF", size, "WAVE"
  char chunk_id[4];
  uint32_t chunk_size;
  while (1)
  {
    if (fread(chunk_id, 1, 4, current_fp) != 4)
      break;
    if (fread(&chunk_size, 1, 4, current_fp) != 4)
      break;
    if (strncmp(chunk_id, "data", 4) == 0)
    {
      data_start_pos = ftell(current_fp);
      total_data_bytes = chunk_size;
      ESP_LOGI(TAG, "Found 'data' chunk. Size: %lu, Start pos: %lu", total_data_bytes, data_start_pos);
      break;
    }
    fseek(current_fp, chunk_size, SEEK_CUR);
  }

  if (data_start_pos == 0)
  {
    ESP_LOGE(TAG, "Could not find 'data' chunk in WAV file.");
    cleanup_resources();
    return ESP_FAIL;
  }

  // 配置并初始化I2S
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

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
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

  // 更新状态
  xSemaphoreTake(status_mutex, portMAX_DELAY);
  internal_status.state = PLAYER_STATE_PLAYING;
  strlcpy(internal_status.current_track, msg->filepath, FILE_PATH_MAX);
  internal_status.total_duration_sec = total_data_bytes / wav_header.byte_rate;
  internal_status.current_position_sec = 0;
  xSemaphoreGive(status_mutex);

  // 进入播放循环
  player_handle_resume();
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
    i2s_channel_disable(tx_handle); // 暂停I2S时钟
    ESP_LOGI(TAG, "Playback paused.");
  }
}

/**
 * @brief 处理继续/恢复命令
 */
static void player_handle_resume()
{
  if (internal_status.state != PLAYER_STATE_PLAYING)
  {
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    internal_status.state = PLAYER_STATE_PLAYING;
    xSemaphoreGive(status_mutex);
  }

  if (tx_handle)
  {
    i2s_channel_enable(tx_handle); // 确保I2S是使能的
  }

  // 播放循环
  uint8_t *read_buf = (uint8_t *)malloc(I2S_BUFFER_SIZE);
  size_t bytes_read = 0;
  size_t bytes_written = 0;
  player_cmd_msg_t msg;

  ESP_LOGI(TAG, "Starting playback loop...");
  while (internal_status.state == PLAYER_STATE_PLAYING && current_fp)
  {
    // 检查是否有新的命令，非阻塞
    if (xQueueReceive(player_cmd_queue, &msg, 0) == pdPASS)
    {
      // 如果在播放时收到新命令（如停止、播放另一首），则处理它
      if (msg.cmd == PLAYER_CMD_PLAY)
      {
        player_handle_play(&msg); // 递归调用处理新歌曲
      }
      else if (msg.cmd == PLAYER_CMD_PAUSE)
      {
        player_handle_pause();
      }
      else if (msg.cmd == PLAYER_CMD_STOP)
      {
        player_handle_stop();
      }
      // 处理完命令后跳出当前循环
      break;
    }

    // 从文件读取数据
    bytes_read = fread(read_buf, 1, I2S_BUFFER_SIZE, current_fp);
    if (bytes_read > 0)
    {
      // 将数据写入I2S
      i2s_channel_write(tx_handle, read_buf, bytes_read, &bytes_written, portMAX_DELAY);

      // 如果注册了回调，则将数据发送给FFT任务
      if (fft_callback)
      {
        // 假设是16位音频
        fft_callback((const int16_t *)read_buf, bytes_read / 2);
      }

      // 更新播放进度
      long current_pos = ftell(current_fp);
      uint32_t bytes_played = current_pos > data_start_pos ? current_pos - data_start_pos : 0;
      uint32_t current_sec = bytes_played / (total_data_bytes / internal_status.total_duration_sec);

      xSemaphoreTake(status_mutex, portMAX_DELAY);
      internal_status.current_position_sec = current_sec;
      xSemaphoreGive(status_mutex);
    }
    else
    {
      // 文件读取完毕
      ESP_LOGI(TAG, "End of file reached.");
      player_handle_stop();
      // 可在此处添加自动播放下一首的逻辑
      break;
    }
  }

  free(read_buf);
  ESP_LOGI(TAG, "Exited playback loop.");
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
  // 创建命令队列
  player_cmd_queue = xQueueCreate(10, sizeof(player_cmd_msg_t));
  if (!player_cmd_queue)
  {
    ESP_LOGE(TAG, "Failed to create command queue");
    return ESP_FAIL;
  }

  // 创建互斥锁
  status_mutex = xSemaphoreCreateMutex();
  if (!status_mutex)
  {
    ESP_LOGE(TAG, "Failed to create status mutex");
    return ESP_FAIL;
  }

  // 初始化状态
  player_handle_stop();

  // 创建并启动播放器任务
  BaseType_t ret = xTaskCreate(wave_player_task, "player_task", 4096, NULL, 5, &player_task_handle);
  if (ret != pdPASS)
  {
    ESP_LOGE(TAG, "Failed to create player task");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Wave player initialized successfully.");
  return ESP_OK;
}

/**
 * @brief 向播放器发送控制命令
 */
esp_err_t wave_player_send_cmd(player_cmd_msg_t *msg)
{
  if (!player_cmd_queue)
  {
    ESP_LOGE(TAG, "Command queue not initialized");
    return ESP_FAIL;
  }
  if (xQueueSend(player_cmd_queue, msg, pdMS_TO_TICKS(100)) != pdPASS)
  {
    ESP_LOGE(TAG, "Failed to send command to queue");
    return ESP_FAIL;
  }
  return ESP_OK;
}

/**
 * @brief 注册FFT数据回调函数
 */
void wave_player_register_fft_callback(fft_data_callback_t callback)
{
  fft_callback = callback;
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