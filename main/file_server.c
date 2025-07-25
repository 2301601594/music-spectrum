/* --- 您需要包含的头文件 --- */
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

// 定义文件路径和缓冲区大小
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE 8192

// 定义播放模式的枚举类型
typedef enum
{
    PLAY_MODE_REPEAT_LIST, // 列表循环
    PLAY_MODE_SHUFFLE,     // 随机播放
    PLAY_MODE_REPEAT_ONE,  // 单曲循环
} play_mode_t;

// 包含文件服务和播放器状态的上下文结构体
struct player_context_data
{
    // 文件服务相关数据
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];

    // 音乐播放状态
    bool is_playing;
    play_mode_t play_mode;
    char current_track[FILE_PATH_MAX]; // 使用更大的缓冲区以容纳完整路径
    int current_position_sec;
    int total_duration_sec;

    // 互斥锁
    SemaphoreHandle_t mutex;
};

static const char *TAG = "http_server";

/* --- 辅助函数 (get_path_from_uri, set_content_type_from_file) --- */
// (这部分代码与之前相同，此处省略以保持简洁)
static const char *get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);
    if (base_pathlen + pathlen + 1 > destsize)
    {
        return NULL;
    }
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);
    return dest + base_pathlen;
}

static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (strstr(filename, ".html"))
    {
        return httpd_resp_set_type(req, "text/html");
    }
    else if (strstr(filename, ".css"))
    {
        return httpd_resp_set_type(req, "text/css");
    }
    else if (strstr(filename, ".js"))
    {
        return httpd_resp_set_type(req, "application/javascript");
    }
    else if (strstr(filename, ".png"))
    {
        return httpd_resp_set_type(req, "image/png");
    }
    return httpd_resp_set_type(req, "text/plain");
}

/* --- API 处理器 --- */

/*
 * GET /api/playlist
 * 扫描音乐目录并返回JSON格式的文件列表
 */
static esp_err_t api_playlist_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    const char *dirpath = "/sdcard"; // 假设音乐文件在SD卡根目录
    DIR *dir = opendir(dirpath);
    if (!dir)
    {
        ESP_LOGE(TAG, "Failed to open directory: %s", dirpath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Music directory not found");
        return ESP_FAIL;
    }

    httpd_resp_send_chunk(req, "[", 1);
    struct dirent *entry;
    bool first_entry = true;
    char filename_buf[300];

    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type == DT_REG &&
            (strstr(entry->d_name, ".mp3") || strstr(entry->d_name, ".wav") ||
             strstr(entry->d_name, ".aac") || strstr(entry->d_name, ".flac")))
        {
            if (!first_entry)
            {
                httpd_resp_send_chunk(req, ",", 1);
            }
            int len = snprintf(filename_buf, sizeof(filename_buf), "\"%s\"", entry->d_name);
            httpd_resp_send_chunk(req, filename_buf, len);
            first_entry = false;
        }
    }
    closedir(dir);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "Playlist sent successfully");
    return ESP_OK;
}

/*
 * GET /api/status
 * **已实现**：返回播放器的当前状态
 */
static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    struct player_context_data *player_ctx = (struct player_context_data *)req->user_ctx;
    char json_response[256];

    // 获取互斥锁以安全地读取状态
    if (xSemaphoreTake(player_ctx->mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {

        // 从共享上下文中读取状态
        bool playing = player_ctx->is_playing;
        const char *track = player_ctx->current_track;
        int position = player_ctx->current_position_sec;
        int duration = player_ctx->total_duration_sec;

        // 释放锁
        xSemaphoreGive(player_ctx->mutex);

        // 构建JSON响应
        snprintf(json_response, sizeof(json_response),
                 "{\"isPlaying\":%s,\"track\":\"%s\",\"position\":%d,\"duration\":%d}",
                 playing ? "true" : "false", track, position, duration);

        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_response, strlen(json_response));
    }
    else
    {
        ESP_LOGE(TAG, "Failed to get mutex for reading status");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Player state is busy", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*
 * POST /api/control
 * **已更新**：接收并处理前端发送的控制命令
 */
static esp_err_t api_control_post_handler(httpd_req_t *req)
{
    char content[256];
    int total_len = req->content_len;
    if (total_len >= sizeof(content))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Command too long");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, content, total_len);
    if (received <= 0)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive command");
        return ESP_FAIL;
    }
    content[total_len] = '\0';
    ESP_LOGI(TAG, "Control command received: %s", content);

    cJSON *root = cJSON_Parse(content);
    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
        return ESP_FAIL;
    }

    cJSON *command_item = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(command_item))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'command' field");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    const char *command = command_item->valuestring;

    struct player_context_data *player_ctx = (struct player_context_data *)req->user_ctx;
    if (xSemaphoreTake(player_ctx->mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {

        if (strcmp(command, "play") == 0)
        {
            cJSON *track_item = cJSON_GetObjectItem(root, "track");
            if (cJSON_IsString(track_item))
            {
                const char *track_name = track_item->valuestring;
                ESP_LOGI(TAG, "Command: Play track -> %s", track_name);
                player_ctx->is_playing = true;
                strlcpy(player_ctx->current_track, track_name, sizeof(player_ctx->current_track));
                // TODO: 通知播放器任务开始播放新文件
            }
        }
        else if (strcmp(command, "pause") == 0)
        {
            ESP_LOGI(TAG, "Command: Pause");
            player_ctx->is_playing = false;
            // TODO: 通知播放器任务暂停
        }
        else if (strcmp(command, "resume") == 0)
        {
            ESP_LOGI(TAG, "Command: Resume");
            player_ctx->is_playing = true;
            // TODO: 通知播放器任务继续播放
        }
        else if (strcmp(command, "seek") == 0)
        {
            cJSON *value_item = cJSON_GetObjectItem(root, "value");
            if (cJSON_IsNumber(value_item))
            {
                int seek_percent = value_item->valueint;
                ESP_LOGI(TAG, "Command: Seek to %d%%", seek_percent);
                // TODO: 计算秒数并命令播放器跳转
            }
        }
        else if (strcmp(command, "set_mode") == 0)
        {
            cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
            if (cJSON_IsString(mode_item))
            {
                const char *mode_str = mode_item->valuestring;
                ESP_LOGI(TAG, "Command: Set mode to %s", mode_str);
                if (strcmp(mode_str, "shuffle") == 0)
                    player_ctx->play_mode = PLAY_MODE_SHUFFLE;
                else if (strcmp(mode_str, "repeat-one") == 0)
                    player_ctx->play_mode = PLAY_MODE_REPEAT_ONE;
                else
                    player_ctx->play_mode = PLAY_MODE_REPEAT_LIST;
            }
        }

        xSemaphoreGive(player_ctx->mutex);
    }
    else
    {
        ESP_LOGE(TAG, "Could not get mutex to process control command");
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "Command received.");
    return ESP_OK;
}

/*
 * GET 
 * 通用文件下载处理器
 */
static esp_err_t file_download_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;

    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0)
    {
        uri = "/index.html";
    }

    const char *filename = get_path_from_uri(filepath, ((struct player_context_data *)req->user_ctx)->base_path,
                                             uri, sizeof(filepath));
    if (!filename)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    if (stat(filepath, &file_stat) == -1)
    {
        ESP_LOGE(TAG, "Failed to stat file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    fd = fopen(filepath, "r");
    if (!fd)
    {
        ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filename);

    char *chunk = ((struct player_context_data *)req->user_ctx)->scratch;
    size_t chunksize;
    do
    {
        chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);
        if (chunksize > 0)
        {
            if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK)
            {
                fclose(fd);
                ESP_LOGE(TAG, "File sending failed!");
                httpd_resp_sendstr_chunk(req, NULL);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }
    } while (chunksize != 0);

    fclose(fd);
    ESP_LOGI(TAG, "File sending complete");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/*
 * 启动服务器的函数
 */
esp_err_t start_file_and_api_server(const char *base_path)
{
    struct player_context_data *server_data = calloc(1, sizeof(struct player_context_data));
    if (!server_data)
    {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(server_data->base_path, base_path, sizeof(server_data->base_path));
    server_data->mutex = xSemaphoreCreateMutex();
    if (server_data->mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(server_data);
        return ESP_FAIL;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start file server!");
        vSemaphoreDelete(server_data->mutex);
        free(server_data);
        return ESP_FAIL;
    }

    // 注册API处理器
    httpd_uri_t playlist_api_uri = {"/api/playlist", HTTP_GET, api_playlist_get_handler, server_data};
    httpd_register_uri_handler(server, &playlist_api_uri);

    httpd_uri_t status_api_uri = {"/api/status", HTTP_GET, api_status_get_handler, server_data};
    httpd_register_uri_handler(server, &status_api_uri);

    httpd_uri_t control_api_uri = {"/api/control", HTTP_POST, api_control_post_handler, server_data};
    httpd_register_uri_handler(server, &control_api_uri);

    // 注册通用文件处理器
    httpd_uri_t file_download_uri = {"/*", HTTP_GET, file_download_handler, server_data};
    httpd_register_uri_handler(server, &file_download_uri);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
