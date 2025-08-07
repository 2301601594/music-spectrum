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
#include "cJSON.h"

// 包含我们重构后的播放器头文件
#include "wave_player.h"
#include "audio_manager.h"
// 包含led_control头文件以调用其函数
#include "led_control.h"

static const char *TAG = "HTTP_SERVER";

// 定义文件路径和缓冲区大小
#define FILE_PATH_MAX_LOCAL (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE 8192

/**
 * @brief HTTP服务器上下文结构
 */
typedef struct
{
    char base_path[FILE_PATH_MAX_LOCAL + 1];
    char scratch[SCRATCH_BUFSIZE];
} http_server_context_t;

/* ------------------- 辅助函数 (无变化) ------------------- */
static const char *get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest)
    {
        pathlen = quest - uri;
    }
    const char *hash = strchr(uri, '#');
    if (hash)
    {
        pathlen = hash - uri;
    }

    if (base_pathlen + pathlen + 1 > destsize)
    {
        return NULL;
    }

    strcpy(dest, base_path);
    // **重要修正**: 使用 strlcpy 保证安全
    strlcpy(dest + base_pathlen, uri, destsize - base_pathlen);

    // 返回文件名部分
    return dest + base_pathlen;
}

static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (strstr(filename, ".html"))
        return httpd_resp_set_type(req, "text/html");
    if (strstr(filename, ".css"))
        return httpd_resp_set_type(req, "text/css");
    if (strstr(filename, ".js"))
        return httpd_resp_set_type(req, "application/javascript");
    if (strstr(filename, ".png"))
        return httpd_resp_set_type(req, "image/png");
    if (strstr(filename, ".ico"))
        return httpd_resp_set_type(req, "image/x-icon");
    return httpd_resp_set_type(req, "text/plain");
}

/* ------------------- API 处理器 (与上次相同，无需修改) ------------------- */

static esp_err_t api_playlist_get_handler(httpd_req_t *req)
{
    http_server_context_t *ctx = (http_server_context_t *)req->user_ctx;
    const char *dirpath = ctx->base_path;
    DIR *dir = opendir(dirpath);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Music directory not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    struct dirent *entry;
    bool first_entry = true;
    char filename_buf[300];
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG && strstr(entry->d_name, ".wav")) {
            if (!first_entry) {
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
    return ESP_OK;
}

static esp_err_t api_status_get_handler(httpd_req_t *req) {
    player_status_t status;
    wave_player_get_status(&status);
    audio_mode_t current_mode = audio_manager_get_mode();
    const char *mode_str = (current_mode == AUDIO_MODE_PLAYER) ? "player" : "mic";
    int color_mode = led_control_get_color_mode();
    char json_response[512];
    char *track_basename = strrchr(status.current_track, '/');
    if (track_basename == NULL) {
        track_basename = status.current_track;
    } else {
        track_basename++;
    }
    snprintf(json_response, sizeof(json_response),
             "{\"mode\":\"%s\",\"isPlaying\":%s,\"track\":\"%s\",\"position\":%lu,\"duration\":%lu,\"colorMode\":%d}",
             mode_str,
             (status.state == PLAYER_STATE_PLAYING) ? "true" : "false",
             track_basename,
             status.current_position_sec,
             status.total_duration_sec,
             color_mode);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, strlen(json_response));
    return ESP_OK;
}

static esp_err_t api_control_post_handler(httpd_req_t *req)
{
    http_server_context_t *ctx = (http_server_context_t *)req->user_ctx;
    char content[256];
    int total_len = req->content_len;
    if (total_len >= sizeof(content)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Command too long");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, content, total_len);
    if (received <= 0) return ESP_FAIL;
    content[total_len] = '\0';
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *command_item = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(command_item)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    const char *command = command_item->valuestring;
    player_cmd_msg_t cmd_msg = {0};
    bool cmd_valid = false;
    if (strcmp(command, "play") == 0) {
        cJSON *track_item = cJSON_GetObjectItem(root, "track");
        if (cJSON_IsString(track_item)) {
            cmd_msg.cmd = PLAYER_CMD_PLAY;
            snprintf(cmd_msg.filepath, FILE_PATH_MAX, "%s/%s", ctx->base_path, track_item->valuestring);
            cmd_valid = true;
        }
    } else if (strcmp(command, "pause") == 0) {
        cmd_msg.cmd = PLAYER_CMD_PAUSE; cmd_valid = true;
    } else if (strcmp(command, "resume") == 0) {
        cmd_msg.cmd = PLAYER_CMD_RESUME; cmd_valid = true;
    } else if (strcmp(command, "stop") == 0) {
        cmd_msg.cmd = PLAYER_CMD_STOP; cmd_valid = true;
    } else if (strcmp(command, "seek") == 0) {
        cJSON *value_item = cJSON_GetObjectItem(root, "value");
        if (value_item && (cJSON_IsNumber(value_item) || cJSON_IsString(value_item))) {
            cmd_msg.cmd = PLAYER_CMD_SEEK;
            cmd_msg.seek_percent = cJSON_IsNumber(value_item) ? value_item->valueint : atoi(value_item->valuestring);
            cmd_valid = true;
        }
    }
    if (cmd_valid) {
        wave_player_send_cmd(&cmd_msg);
        httpd_resp_sendstr(req, "Command sent.");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_mode_post_handler(httpd_req_t *req) {
    char content[128];
    int total_len = req->content_len;
    if (total_len >= sizeof(content)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too long");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, content, total_len);
    if (received <= 0) return ESP_FAIL;
    content[total_len] = '\0';
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
    if (!cJSON_IsString(mode_item)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    const char *mode_str = mode_item->valuestring;
    audio_mode_t new_mode = (strcmp(mode_str, "mic") == 0) ? AUDIO_MODE_MIC : AUDIO_MODE_PLAYER;
    audio_manager_set_mode(new_mode);
    httpd_resp_sendstr(req, "Mode switched.");
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_color_post_handler(httpd_req_t *req) {
    char content[128];
    int total_len = req->content_len;
    if (total_len >= sizeof(content)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too long");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, content, total_len);
    if (received <= 0) return ESP_FAIL;
    content[total_len] = '\0';
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *mode_item = cJSON_GetObjectItem(root, "colorMode");
    if (!cJSON_IsNumber(mode_item)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    int color_mode = mode_item->valueint;
    led_control_set_color_mode(color_mode);
    httpd_resp_sendstr(req, "Color mode switched.");
    cJSON_Delete(root);
    return ESP_OK;
}

/* ------------------- 文件服务处理器 (核心修正) ------------------- */
/**
 * @brief 处理静态文件下载请求
 * **修正**: 移除嵌入式文件逻辑，只从VFS(SD卡)加载
 */
static esp_err_t file_download_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX_LOCAL];
    FILE *fd = NULL;
    struct stat file_stat;
    http_server_context_t *ctx = (http_server_context_t *)req->user_ctx;

    // 如果URI是根目录 "/", 则默认指向 "index.html"
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0) {
        uri = "/index.html";
    }

    // 从URI构建完整的文件路径
    const char *filename = get_path_from_uri(filepath, ctx->base_path, uri, sizeof(filepath));
    if (!filename) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    // 检查文件是否存在
    if (stat(filepath, &file_stat) == -1) {
        ESP_LOGE(TAG, "Failed to stat file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    // 打开文件
    fd = fopen(filepath, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    // 设置正确的MIME类型并发送文件内容
    set_content_type_from_file(req, filename);
    char *chunk = ctx->scratch;
    size_t chunksize;
    do {
        chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);
        if (chunksize > 0) {
            if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
                fclose(fd);
                httpd_resp_send_chunk(req, NULL, 0);
                ESP_LOGE(TAG, "File sending failed!");
                return ESP_FAIL;
            }
        }
    } while (chunksize != 0);

    fclose(fd);
    ESP_LOGI(TAG, "File sending complete: %s", filename);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ------------------- 服务器启动函数 (无变化) ------------------- */
esp_err_t start_file_and_api_server(const char *base_path) {
    http_server_context_t *server_ctx = calloc(1, sizeof(http_server_context_t));
    if (!server_ctx) return ESP_ERR_NO_MEM;
    strlcpy(server_ctx->base_path, base_path, sizeof(server_ctx->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file server!");
        free(server_ctx);
        return ESP_FAIL;
    }

    const httpd_uri_t playlist_api_uri = {"/api/playlist", HTTP_GET, api_playlist_get_handler, server_ctx};
    httpd_register_uri_handler(server, &playlist_api_uri);
    const httpd_uri_t status_api_uri = {"/api/status", HTTP_GET, api_status_get_handler, server_ctx};
    httpd_register_uri_handler(server, &status_api_uri);
    const httpd_uri_t control_api_uri = {"/api/control", HTTP_POST, api_control_post_handler, server_ctx};
    httpd_register_uri_handler(server, &control_api_uri);
    const httpd_uri_t mode_api_uri = {"/api/mode", HTTP_POST, api_mode_post_handler, server_ctx};
    httpd_register_uri_handler(server, &mode_api_uri);
    const httpd_uri_t color_api_uri = {"/api/color", HTTP_POST, api_color_post_handler, server_ctx};
    httpd_register_uri_handler(server, &color_api_uri);
    const httpd_uri_t file_download_uri = {"/*", HTTP_GET, file_download_handler, server_ctx};
    httpd_register_uri_handler(server, &file_download_uri);

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}
