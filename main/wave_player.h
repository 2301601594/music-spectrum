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

// 文件最长路径
#define FILE_PATH_MAX 256
// I2S读取/写入缓存区大小
#define I2S_BUFFER_SIZE 1024

/**
 * @brief 播放器状态枚举
 */
typedef enum
{
    PLAYER_STATE_STOPPED, // 停止
    PLAYER_STATE_PLAYING, // 播放中
    PLAYER_STATE_PAUSED,  // 暂停
} player_state_t;

/**
 * @brief 播放器控制指令枚举
 */
typedef enum
{
    PLAYER_CMD_PLAY,
    PLAYER_CMD_PAUSE,
    PLAYER_CMD_RESUME,
    PLAYER_CMD_STOP,
    PLAYER_CMD_SEEK,
} player_cmd_t;

/**
 * @brief 播放器命令消息结构体
 * 用于通过队列在任务间传递命令
 */
typedef struct
{
    player_cmd_t cmd;
    char filepath[FILE_PATH_MAX]; // 仅用于 PLAY 命令
    int seek_percent;             // <-- 新增: 用于 SEEK 命令 (0-100)
} player_cmd_msg_t;

/**
 * @brief 播放器当前状态信息结构体
 */
typedef struct
{
    player_state_t state;
    char current_track[FILE_PATH_MAX];
    uint32_t total_duration_sec;
    uint32_t current_position_sec;
    // 新增内部计算需要的字段
    uint16_t num_channels;
    uint16_t bits_per_sample;
    uint32_t byte_rate;
} player_status_t;

/**
 * @brief 初始化播放器
 *
 * 该函数会创建播放器任务和相关的FreeRTOS资源。
 * 必须在使用播放器前调用一次。
 *
 * @return esp_err_t ESP_OK 表示成功, 其他表示失败
 */
esp_err_t wave_player_init(void);

/**
 * @brief 向播放器发送控制命令
 *
 * 这是控制播放器的主要方式。
 *
 * @param msg 指向命令消息结构体的指针
 * @return esp_err_t ESP_OK 表示成功, 其他表示失败
 */
esp_err_t wave_player_send_cmd(player_cmd_msg_t *msg);

/**
 * @brief 获取播放器当前状态
 *
 * 供Web服务器等其他模块查询播放器的实时状态。
 *
 * @param status 指向一个 player_status_t 结构体的指针，用于接收状态信息
 */
void wave_player_get_status(player_status_t *status);

// WAV 文件头结构
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
