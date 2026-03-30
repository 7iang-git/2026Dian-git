#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_check.h"

#define TAG "MP3_PLAYER"


#define BUTTON_PLAY_PAUSE   GPIO_NUM_1
#define BUTTON_NEXT         GPIO_NUM_2
#define BUTTON_PREV         GPIO_NUM_3
#define BUTTON_VOL_UP       GPIO_NUM_7
#define BUTTON_VOL_DOWN     GPIO_NUM_8

// I2S 定义 (GPIO 4, 5, 6)
#define I2S_WS_PIN          GPIO_NUM_4
#define I2S_BCK_PIN         GPIO_NUM_5
#define I2S_DATA_PIN        GPIO_NUM_6

// UART 定义
#define UART_NUM            UART_NUM_0
#define UART_BAUD_RATE      115200
#define UART_BUF_SIZE       1024

// 音频参数
#define SAMPLE_RATE         44100
#define BITS_PER_SAMPLE     I2S_DATA_BIT_WIDTH_16BIT
#define CHANNELS            I2S_SLOT_MODE_STEREO

// 缓冲区大小
#define AUDIO_BUFFER_SIZE   4096
#define MP3_BUF_SIZE        4096

// 播放状态
typedef enum {
    PLAY_STATE_STOPPED,
    PLAY_STATE_PLAYING,
    PLAY_STATE_PAUSED
} play_state_t;

// 歌曲信息
typedef struct {
    char name[64];
    char path[280];
    uint32_t duration_ms;
    uint32_t file_size;
} song_info_t;

// 播放器上下文
typedef struct {
    char device_name[32];
    play_state_t state;
    uint8_t current_song_index;
    uint8_t song_count;
    song_info_t songs[50];
    uint32_t current_position_ms;
    uint8_t volume;
    bool is_first_boot;
    uint32_t paused_position;
    uint32_t song_positions[50];  // 每首歌的播放位置
    FILE *current_file;
    bool audio_task_running;
    bool stop_audio_task;
} mp3_context_t;

static mp3_context_t g_ctx = {0};
static i2s_chan_handle_t g_i2s_tx_chan = NULL;
static nvs_handle_t g_nvs_handle = 0;
static SemaphoreHandle_t g_audio_mutex = NULL;

// 函数声明
static void init_hardware(void);
static void init_nvs(void);
static void init_spiffs(void);
static void init_i2s(void);
static void init_uart(void);
static void init_buttons(void);
static void load_device_config(void);
static void save_device_config(void);
static void scan_songs(void);
static void play_current_song(void);
static void pause_current_song(void);
static void resume_current_song(void);
static void stop_current_song(void);
static void next_song(void);
static void prev_song(void);
static void set_volume(uint8_t vol);
static void process_uart_command(const char *cmd);
static void uart_task(void *pvParameters);
static void button_task(void *pvParameters);
static void audio_task(void *pvParameters);
static void set_device_name(const char *name);

// 初始化 NVS
static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_open("mp3_storage", NVS_READWRITE, &g_nvs_handle));
    ESP_LOGI(TAG, "NVS initialized");
}

// 加载设备配置
static void load_device_config(void)
{
    size_t len = sizeof(g_ctx.device_name);
    esp_err_t ret = nvs_get_str(g_nvs_handle, "device_name", g_ctx.device_name, &len);
    if (ret != ESP_OK) {
        g_ctx.is_first_boot = true;
        strcpy(g_ctx.device_name, "ESP32_MP3");
        ESP_LOGI(TAG, "First boot detected");
    } else {
        g_ctx.is_first_boot = false;
        ESP_LOGI(TAG, "Device name loaded: %s", g_ctx.device_name);
    }

    ret = nvs_get_u8(g_nvs_handle, "volume", &g_ctx.volume);
    if (ret != ESP_OK) {
        g_ctx.volume = 70;
    }

    uint8_t saved = 0;
    ret = nvs_get_u8(g_nvs_handle, "has_saved_pos", &saved);
    if (saved) {
        nvs_get_u8(g_nvs_handle, "last_song", &g_ctx.current_song_index);
        nvs_get_u32(g_nvs_handle, "last_pos", &g_ctx.paused_position);
        ESP_LOGI(TAG, "Restored playback position: song %d, pos %lu ms",
                 g_ctx.current_song_index, (unsigned long)g_ctx.paused_position);
    }
}

// 保存设备配置
static void save_device_config(void)
{
    nvs_set_str(g_nvs_handle, "device_name", g_ctx.device_name);
    nvs_set_u8(g_nvs_handle, "volume", g_ctx.volume);
    ESP_LOGI(TAG, "Device config saved");
}

// 保存播放位置
static void save_playback_position(void)
{
    nvs_set_u8(g_nvs_handle, "has_saved_pos", 1);
    nvs_set_u8(g_nvs_handle, "last_song", g_ctx.current_song_index);
    nvs_set_u32(g_nvs_handle, "last_pos", g_ctx.current_position_ms);
    nvs_commit(g_nvs_handle);
    ESP_LOGI(TAG, "Playback position saved: song %d, pos %lu ms",
             g_ctx.current_song_index, (unsigned long)g_ctx.current_position_ms);
}

// 设置设备名称
static void set_device_name(const char *name)
{
    strncpy(g_ctx.device_name, name, sizeof(g_ctx.device_name) - 1);
    g_ctx.device_name[sizeof(g_ctx.device_name) - 1] = '\0';
    g_ctx.is_first_boot = false;
    save_device_config();
    ESP_LOGI(TAG, "Device name set to: %s", g_ctx.device_name);
}

// 初始化 SPIFFS
static void init_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS...");

    // 首先检查分区表中的SPIFFS分区
    const esp_partition_t *spiffs_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");

    if (spiffs_partition == NULL) {
        ESP_LOGE(TAG, "SPIFFS partition 'storage' not found in partition table!");
        ESP_LOGE(TAG, "Please check your partitions.csv configuration");
        printf("\r\n[ERROR] SPIFFS partition not found, check partition table\r\n");
        return;
    }

    ESP_LOGI(TAG, "Found SPIFFS partition at offset 0x%lx, size %lu bytes",
             (unsigned long)spiffs_partition->address,
             (unsigned long)spiffs_partition->size);

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 10,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Partition not found - was spiffs.bin flashed correctly?");
            printf("\r\n[ERROR] SPIFFS partition not found, please flash spiffs.bin\r\n");
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info("storage", &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted successfully: total=%lu KB, used=%lu KB",
                 (unsigned long)(total / 1024), (unsigned long)(used / 1024));
        printf("SPIFFS mounted: Total %lu KB, Used %lu KB\r\n",
               (unsigned long)(total / 1024), (unsigned long)(used / 1024));
    } else {
        ESP_LOGW(TAG, "Failed to get SPIFFS info: %s", esp_err_to_name(ret));
    }
}

// 扫描歌曲文件
static void scan_songs(void)
{
    ESP_LOGI(TAG, "Scanning for songs in /spiffs...");
    printf("Scanning for audio files...\r\n");

    DIR *dir = opendir("/spiffs");
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open /spiffs directory");
        printf("[ERROR] Failed to open /spiffs directory\r\n");
        printf("Please make sure SPIFFS filesystem image is flashed\r\n");
        return;
    }

    g_ctx.song_count = 0;
    struct dirent *entry;
    int file_count = 0;

    // 列出所有文件
    while ((entry = readdir(dir)) != NULL) {
        file_count++;
        ESP_LOGI(TAG, "Found file: %s", entry->d_name);
        printf("  Found file: %s\r\n", entry->d_name);

        if (g_ctx.song_count < 50) {
            // 检查是否是音频文件
            char *ext = strrchr(entry->d_name, '.');
            if (ext != NULL) {
                if (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0) {
                    song_info_t *song = &g_ctx.songs[g_ctx.song_count];
                    strncpy(song->name, entry->d_name, sizeof(song->name) - 1);
                    song->name[sizeof(song->name) - 1] = '\0';
                    snprintf(song->path, sizeof(song->path), "/spiffs/%s", entry->d_name);
                    song->duration_ms = 0;
                    song->file_size = 0;
                    g_ctx.song_count++;
                    ESP_LOGI(TAG, "Added song: %s", song->name);
                    printf("  -> Added song: %s\r\n", song->name);
                }
            }
        }
    }
    closedir(dir);

    ESP_LOGI(TAG, "Total files in /spiffs: %d, Audio files: %d", file_count, g_ctx.song_count);
    printf("\r\nScan complete: %d total files, %d audio songs\r\n", file_count, g_ctx.song_count);

    if (g_ctx.song_count == 0) {
        printf("\r\n[WARN] No audio files found!\r\n");
        printf("Please upload MP3 or WAV files to /spiffs\r\n");
        printf("Supported formats: .mp3, .wav\r\n");
    }

    // 初始化所有歌曲位置为0
    for (int i = 0; i < 50; i++) {
        g_ctx.song_positions[i] = 0;
    }
}

// 初始化 I2S
static void init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &g_i2s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(BITS_PER_SAMPLE, CHANNELS),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DATA_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(g_i2s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(g_i2s_tx_chan));
    ESP_LOGI(TAG, "I2S initialized");
}

// 初始化 UART
static void init_uart(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_cfg));
    ESP_LOGI(TAG, "UART initialized");
}

// 初始化按键
static void init_buttons(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_PLAY_PAUSE) |
                       (1ULL << BUTTON_NEXT) |
                       (1ULL << BUTTON_PREV) |
                       (1ULL << BUTTON_VOL_UP) |
                       (1ULL << BUTTON_VOL_DOWN),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Buttons initialized");
}

// 应用音量
static void apply_volume(int16_t *samples, int count, uint8_t volume)
{
    if (volume >= 100) return;

    int32_t vol_factor = (volume * 65536) / 100;

    for (int i = 0; i < count; i++) {
        int32_t sample = samples[i];
        sample = (sample * vol_factor) >> 16;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        samples[i] = (int16_t)sample;
    }
}

// 播放当前歌曲
static void play_current_song(void)
{
    if (g_ctx.song_count == 0) {
        ESP_LOGW(TAG, "No songs available");
        printf("ERROR: No audio files available\r\n");
        printf("Please upload MP3 files to /spiffs directory\r\n");
        return;
    }

    song_info_t *song = &g_ctx.songs[g_ctx.current_song_index];

    // 关闭之前的文件
    if (g_ctx.current_file != NULL) {
        fclose(g_ctx.current_file);
        g_ctx.current_file = NULL;
    }

    // 打开新的音频文件
    g_ctx.current_file = fopen(song->path, "rb");
    if (g_ctx.current_file == NULL) {
        ESP_LOGE(TAG, "Failed to open: %s", song->path);
        printf("ERROR: Cannot open file %s\r\n", song->name);
        return;
    }

    // 获取该歌曲应该恢复的位置（从数组中读取，而不是从paused_position）
    uint32_t resume_position = g_ctx.song_positions[g_ctx.current_song_index];

    // 如果是断点续播，跳转到保存的位置
    if (resume_position > 0) {
        // 简单估算：假设 44.1kHz, 16bit, stereo = 176400 bytes/sec
        // 对于WAV文件，还需要加上WAV头的44字节
        uint32_t bytes_per_sec = 176400;
        long offset = ((long)resume_position * bytes_per_sec) / 1000;
        // 限制最大偏移为10MB，防止异常值
        if (offset > 10 * 1024 * 1024) {
            offset = 0;
            ESP_LOGW(TAG, "Position too large, reset to beginning");
        }
        // 加上WAV头偏移（至少44字节）
        offset += 44;
        fseek(g_ctx.current_file, offset, SEEK_SET);
        ESP_LOGI(TAG, "Seek to %ld bytes (position: %lu ms)", offset, (unsigned long)resume_position);
    }

    g_ctx.current_position_ms = resume_position;
    g_ctx.state = PLAY_STATE_PLAYING;

    ESP_LOGI(TAG, "Playing: %s", song->name);

    printf("\r\n========== Now Playing ==========\r\n");
    printf("Song: %s\r\n", song->name);
    printf("Track: %d/%d\r\n", g_ctx.current_song_index + 1, g_ctx.song_count);
    if (resume_position > 0) {
        printf("Resume position: %lu sec\r\n", (unsigned long)(resume_position / 1000));
    }
    printf("Volume: %d%%\r\n", g_ctx.volume);
    printf("==================================\r\n\r\n");
}

// 暂停当前歌曲
static void pause_current_song(void)
{
    if (g_ctx.state == PLAY_STATE_PLAYING) {
        g_ctx.state = PLAY_STATE_PAUSED;
        g_ctx.paused_position = g_ctx.current_position_ms;
        save_playback_position();

        // 发送静音数据清空I2S缓冲区，防止喇叭卡在最后一个音
        if (g_i2s_tx_chan != NULL) {
            size_t written = 0;

            // 1. 先禁用I2S TX通道，停止新的数据传输
            i2s_channel_disable(g_i2s_tx_chan);

            // 2. 短暂延时，让DMA完成当前传输
            vTaskDelay(pdMS_TO_TICKS(30));

            // 3. 启用通道，发送大量零数据覆盖任何残留样本
            i2s_channel_enable(g_i2s_tx_chan);

            int16_t zero_buf[512] = {0};
            // 连续发送静音，确保DAC接收到零值
            for (int i = 0; i < 10; i++) {
                i2s_channel_write(g_i2s_tx_chan, zero_buf, sizeof(zero_buf), &written, pdMS_TO_TICKS(5));
            }

            // 4. 再次禁用通道，此时DAC应保持零输出
            i2s_channel_disable(g_i2s_tx_chan);

            // 5. 延时确保DAC稳定
            vTaskDelay(pdMS_TO_TICKS(50));

            ESP_LOGI(TAG, "I2S muted successfully");
        }

        ESP_LOGI(TAG, "Paused at %lu ms", (unsigned long)g_ctx.paused_position);
        printf("[PAUSED, position: %lu sec]\r\n", (unsigned long)(g_ctx.paused_position / 1000));
    }
}

// 恢复播放
static void resume_current_song(void)
{
    if (g_ctx.state == PLAY_STATE_PAUSED) {
        // 确保I2S通道已启用
        if (g_i2s_tx_chan != NULL) {
            i2s_channel_enable(g_i2s_tx_chan);
        }

        g_ctx.state = PLAY_STATE_PLAYING;
        ESP_LOGI(TAG, "Resumed from %lu ms", (unsigned long)g_ctx.paused_position);
        printf("[RESUME PLAYING]\r\n");
    }
}

// 停止播放
static void stop_current_song(void)
{
    if (g_ctx.state != PLAY_STATE_STOPPED) {
        g_ctx.state = PLAY_STATE_STOPPED;
        g_ctx.paused_position = 0;

        if (g_ctx.current_file != NULL) {
            fclose(g_ctx.current_file);
            g_ctx.current_file = NULL;
        }

        ESP_LOGI(TAG, "Stopped");
        printf("[STOPPED]\r\n");
    }
}

// 切歌锁，防止重复触发
static bool g_switching_song = false;

// 下一首
static void next_song(void)
{
    if (g_ctx.song_count == 0 || g_switching_song) return;

    g_switching_song = true;

    // 先停止播放，让音频任务退出当前播放
    play_state_t old_state = g_ctx.state;
    g_ctx.state = PLAY_STATE_STOPPED;

    // 等待音频任务完全停止，避免杂音
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // 清空I2S缓冲区，发送静音数据防止爆音
    if (g_i2s_tx_chan != NULL) {
        i2s_channel_disable(g_i2s_tx_chan);

        int16_t zero_buf[256] = {0};
        size_t written = 0;
        // 发送2次静音数据清空缓冲区
        for (int i = 0; i < 2; i++) {
            i2s_channel_write(g_i2s_tx_chan, zero_buf, sizeof(zero_buf), &written, pdMS_TO_TICKS(10));
        }
    }

    // 保存当前歌曲的播放位置（如果正在播放或暂停）
    if (old_state == PLAY_STATE_PLAYING || old_state == PLAY_STATE_PAUSED) {
        g_ctx.song_positions[g_ctx.current_song_index] = g_ctx.current_position_ms;
        ESP_LOGI(TAG, "Saved position for song %d: %lu ms", g_ctx.current_song_index, (unsigned long)g_ctx.current_position_ms);
        save_playback_position();
    }

    // 切换到下一首
    g_ctx.current_song_index++;
    if (g_ctx.current_song_index >= g_ctx.song_count) {
        g_ctx.current_song_index = 0;
    }

    // 恢复目标歌曲的播放位置
    g_ctx.paused_position = g_ctx.song_positions[g_ctx.current_song_index];
    ESP_LOGI(TAG, "Restored position for song %d: %lu ms", g_ctx.current_song_index, (unsigned long)g_ctx.paused_position);

    // 重新启用I2S通道
    if (g_i2s_tx_chan != NULL) {
        i2s_channel_enable(g_i2s_tx_chan);
    }

    play_current_song();

    vTaskDelay(100 / portTICK_PERIOD_MS);  // 冷却时间
    g_switching_song = false;
}

// 上一首
static void prev_song(void)
{
    if (g_ctx.song_count == 0 || g_switching_song) return;

    g_switching_song = true;

    // 先停止播放，让音频任务退出当前播放
    play_state_t old_state = g_ctx.state;
    g_ctx.state = PLAY_STATE_STOPPED;

    // 等待音频任务完全停止，避免杂音
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // 清空I2S缓冲区，发送静音数据防止爆音
    if (g_i2s_tx_chan != NULL) {
        i2s_channel_disable(g_i2s_tx_chan);

        int16_t zero_buf[256] = {0};
        size_t written = 0;
        // 发送2次静音数据清空缓冲区
        for (int i = 0; i < 2; i++) {
            i2s_channel_write(g_i2s_tx_chan, zero_buf, sizeof(zero_buf), &written, pdMS_TO_TICKS(10));
        }
    }

    // 保存当前歌曲的播放位置（如果正在播放或暂停）
    if (old_state == PLAY_STATE_PLAYING || old_state == PLAY_STATE_PAUSED) {
        g_ctx.song_positions[g_ctx.current_song_index] = g_ctx.current_position_ms;
        ESP_LOGI(TAG, "Saved position for song %d: %lu ms", g_ctx.current_song_index, (unsigned long)g_ctx.current_position_ms);
        save_playback_position();
    }

    // 切换到上一首
    if (g_ctx.current_song_index == 0) {
        g_ctx.current_song_index = g_ctx.song_count - 1;
    } else {
        g_ctx.current_song_index--;
    }

    // 恢复目标歌曲的播放位置
    g_ctx.paused_position = g_ctx.song_positions[g_ctx.current_song_index];
    ESP_LOGI(TAG, "Restored position for song %d: %lu ms", g_ctx.current_song_index, (unsigned long)g_ctx.paused_position);

    // 重新启用I2S通道
    if (g_i2s_tx_chan != NULL) {
        i2s_channel_enable(g_i2s_tx_chan);
    }

    play_current_song();

    vTaskDelay(100 / portTICK_PERIOD_MS);  // 冷却时间
    g_switching_song = false;
}

// 设置音量
static void set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    g_ctx.volume = vol;
    ESP_LOGI(TAG, "Volume set to %d%%", vol);
    printf("[VOLUME: %d%%]\r\n", vol);
}

// 显示播放列表
static void show_playlist(void)
{
    printf("\r\n========== PLAYLIST ==========\r\n");
    printf("Total %d songs:\r\n\r\n", g_ctx.song_count);

    for (int i = 0; i < g_ctx.song_count; i++) {
        if (i == g_ctx.current_song_index) {
            printf("  > %2d. %s [CURRENT]\r\n", i + 1, g_ctx.songs[i].name);
        } else {
            printf("    %2d. %s\r\n", i + 1, g_ctx.songs[i].name);
        }
    }
    printf("\r\nCurrent: Track %d\r\n", g_ctx.current_song_index + 1);
    printf("================================\r\n\r\n");
}

// 显示帮助信息
static void show_help(void)
{
    printf("\r\n========== COMMAND HELP ==========\r\n");
    printf("  play          - Play/Pause\r\n");
    printf("  pause         - Pause\r\n");
    printf("  stop          - Stop\r\n");
    printf("  next          - Next song\r\n");
    printf("  prev          - Previous song\r\n");
    printf("  list          - Show playlist\r\n");
    printf("  vol [0-100]   - Set volume\r\n");
    printf("  name [name]   - Set device name\r\n");
    printf("  status        - Show status\r\n");
    printf("  help          - Show help\r\n");
    printf("===================================\r\n\r\n");
}

// 显示状态
static void show_status(void)
{
    const char *state_str[] = {"STOPPED", "PLAYING", "PAUSED"};
    printf("\r\n========== DEVICE STATUS ==========\r\n");
    printf("Device name: %s\r\n", g_ctx.device_name);
    printf("Play state: %s\r\n", state_str[g_ctx.state]);
    printf("Current song: %s\r\n", g_ctx.song_count > 0 ? g_ctx.songs[g_ctx.current_song_index].name : "None");
    printf("Song count: %d\r\n", g_ctx.song_count);
    printf("Position: %lu ms\r\n", (unsigned long)g_ctx.current_position_ms);
    printf("Volume: %d%%\r\n", g_ctx.volume);
    printf("====================================\r\n\r\n");
}

// 处理 UART 命令
static void process_uart_command(const char *cmd)
{
    char cmd_buf[256];

    // printf("[DEBUG] process_uart_command called\r\n");

    strncpy(cmd_buf, cmd, sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    // printf("[DEBUG] Raw cmd: [%s]\r\n", cmd);

    // 去除换行符
    char *p = strchr(cmd_buf, '\r');
    if (p) *p = '\0';
    p = strchr(cmd_buf, '\n');
    if (p) *p = '\0';

    // 去除前导空格
    char *cmd_start = cmd_buf;
    while (*cmd_start == ' ' || *cmd_start == '\t') cmd_start++;

    // printf("[DEBUG] Processed cmd: [%s]\r\n", cmd_start);

    if (strlen(cmd_start) == 0) {
        // printf("[DEBUG] Empty command\r\n");
        return;
    }

    ESP_LOGI(TAG, "Command: %s", cmd_start);
    printf("\r\n执行命令: %s\r\n", cmd_start);

    if (strcasecmp(cmd_start, "play") == 0) {
        if (g_ctx.state == PLAY_STATE_PLAYING) {
            pause_current_song();
        } else if (g_ctx.state == PLAY_STATE_PAUSED) {
            resume_current_song();
        } else {
            play_current_song();
        }
    }
    else if (strcasecmp(cmd_start, "pause") == 0) {
        pause_current_song();
    }
    else if (strcasecmp(cmd_start, "stop") == 0) {
        stop_current_song();
    }
    else if (strcasecmp(cmd_start, "next") == 0) {
        next_song();
    }
    else if (strcasecmp(cmd_start, "prev") == 0) {
        prev_song();
    }
    else if (strcasecmp(cmd_start, "list") == 0) {
        show_playlist();
    }
    else if (strcasecmp(cmd_start, "help") == 0) {
        show_help();
    }
    else if (strcasecmp(cmd_start, "status") == 0) {
        show_status();
    }
    else if (strncasecmp(cmd_start, "vol ", 4) == 0) {
        int vol = atoi(cmd_start + 4);
        set_volume((uint8_t)vol);
    }
    else if (strncasecmp(cmd_start, "name ", 5) == 0) {
        set_device_name(cmd_start + 5);
        printf("Device name set to: %s\r\n", g_ctx.device_name);
    }
    else {
        printf("未知命令: %s\r\n", cmd_start);
        printf("输入 'help' 查看帮助\r\n");
    }
}

// UART 任务
static void uart_task(void *pvParameters)
{
    uint8_t data[256];
    char cmd_buf[256];
    int cmd_pos = 0;

    ESP_LOGI(TAG, "UART task started, waiting for commands...");
    printf("\r\n[系统就绪，请输入命令]\r\n");

    while (1) {
        int len = uart_read_bytes(UART_NUM, data, sizeof(data) - 1, 10 / portTICK_PERIOD_MS);
        if (len > 0) {
            data[len] = '\0';
            ESP_LOGI(TAG, "Received %d bytes: [%s]", len, data);

            for (int i = 0; i < len; i++) {
                char c = data[i];
                // 回显字符（方便调试）
                if (c >= 32 && c < 127) {
                    printf("%c", c);
                } else if (c == '\r' || c == '\n') {
                    printf("\r\n");
                }

                if (c == '\r' || c == '\n') {
                    if (cmd_pos > 0) {
                        cmd_buf[cmd_pos] = '\0';
                        ESP_LOGI(TAG, "Processing command: [%s]", cmd_buf);
                        process_uart_command(cmd_buf);
                        cmd_pos = 0;
                    }
                } else if (cmd_pos < sizeof(cmd_buf) - 1) {
                    cmd_buf[cmd_pos++] = c;
                }
            }
        }
    }
}

// 按键任务
static void button_task(void *pvParameters)
{
    uint32_t last_states[5] = {1, 1, 1, 1, 1};  // 每个按键上次状态（上拉，默认高电平）
    uint32_t debounce_cnt[5] = {0};             // 消抖计数器
    uint32_t last_press_ms[5] = {0};            // 上次触发时间
    const uint32_t buttons[] = {BUTTON_PLAY_PAUSE, BUTTON_NEXT, BUTTON_PREV, BUTTON_VOL_UP, BUTTON_VOL_DOWN};
    const uint32_t DEBOUNCE_THRESHOLD = 3;        // 消抖阈值（连续3次一致）
    const uint32_t COOLDOWN_MS = 250;           // 按键冷却时间

    while (1) {
        vTaskDelay(10 / portTICK_PERIOD_MS);

        for (int i = 0; i < 5; i++) {
            uint32_t current = gpio_get_level(buttons[i]);

            // 消抖处理
            if (current == last_states[i]) {
                // 状态一致，增加计数
                if (debounce_cnt[i] < DEBOUNCE_THRESHOLD) {
                    debounce_cnt[i]++;
                }
            } else {
                // 状态变化，重置计数
                debounce_cnt[i] = 0;
                last_states[i] = current;
            }

            // 只有当消抖完成后，且是按下动作（低电平）时才触发
            if (debounce_cnt[i] == DEBOUNCE_THRESHOLD && current == 0) {
                uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // 检查冷却时间
                if ((now - last_press_ms[i]) >= COOLDOWN_MS) {
                    last_press_ms[i] = now;
                    debounce_cnt[i] = 0;  // 触发后重置，防止重复

                    switch (i) {
                        case 0:
                            ESP_LOGI(TAG, "Button: Play/Pause");
                            if (g_ctx.state == PLAY_STATE_PLAYING) {
                                pause_current_song();
                            } else if (g_ctx.state == PLAY_STATE_PAUSED) {
                                resume_current_song();
                            } else {
                                play_current_song();
                            }
                            break;
                        case 1:
                            ESP_LOGI(TAG, "Button: Next");
                            next_song();
                            break;
                        case 2:
                            ESP_LOGI(TAG, "Button: Previous");
                            prev_song();
                            break;
                        case 3:
                            ESP_LOGI(TAG, "Button: Volume Up");
                            if (g_ctx.volume + 10 <= 100) {
                                set_volume(g_ctx.volume + 10);
                            } else {
                                set_volume(100);
                            }
                            break;
                        case 4:
                            ESP_LOGI(TAG, "Button: Volume Down");
                            if (g_ctx.volume >= 10) {
                                set_volume(g_ctx.volume - 10);
                            } else {
                                set_volume(0);
                            }
                            break;
                    }
                }
            }
        }
    }
}

// WAV 文件头
#pragma pack(push, 1)
typedef struct {
    char riff[4];
    uint32_t size;
    char wave[4];
} wav_riff_t;

typedef struct {
    char id[4];
    uint32_t size;
} wav_chunk_t;

typedef struct {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_fmt_t;
#pragma pack(pop)

// 打印文件前64字节用于调试
static void dump_file_header(FILE *file)
{
    uint8_t buf[64];
    long pos = ftell(file);
    size_t n = fread(buf, 1, 64, file);
    fseek(file, pos, SEEK_SET);

    ESP_LOGI(TAG, "File header dump (%d bytes read):", (int)n);
    ESP_LOGI(TAG, "  ASCII: %.4s %.4s %.4s %.4s %.4s %.4s %.4s %.4s",
             (char*)&buf[0], (char*)&buf[4], (char*)&buf[8], (char*)&buf[12],
             (char*)&buf[16], (char*)&buf[20], (char*)&buf[24], (char*)&buf[28]);

    for (int i = 0; i < 4 && (i*16) < (int)n; i++) {
        char hex_str[50] = {0};
        char ascii_str[20] = {0};
        for (int j = 0; j < 16 && (i*16+j) < (int)n; j++) {
            sprintf(hex_str + j*3, "%02x ", buf[i*16+j]);
            ascii_str[j] = (buf[i*16+j] >= 32 && buf[i*16+j] < 127) ? buf[i*16+j] : '.';
        }
        ESP_LOGI(TAG, "  %04x: %s| %s", i*16, hex_str, ascii_str);
    }
}

// 播放 WAV 文件 - 支持各种非标准格式
static void play_wav_file(FILE *file)
{
    wav_riff_t riff;
    wav_fmt_t fmt;
    int fmt_parsed = 0;
    int data_found = 0;
    long data_start_offset = 0;

    // 重要：重置文件指针到文件开头
    if (fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to beginning of file");
        return;
    }

    ESP_LOGI(TAG, "=== Starting WAV playback ===");

    // 打印文件头用于调试
    dump_file_header(file);

    // 读取 RIFF 头
    if (fread(&riff, 1, sizeof(riff), file) != sizeof(riff)) {
        ESP_LOGE(TAG, "Failed to read RIFF header");
        return;
    }

    ESP_LOGI(TAG, "RIFF header: '%.4s' '%.4s' Size: %lu",
             riff.riff, riff.wave, (unsigned long)riff.size);

    // 检查 RIFF 标记 - 兼容更多格式
    int is_riff = (memcmp(riff.riff, "RIFF", 4) == 0 ||
                   memcmp(riff.riff, "RIFX", 4) == 0 ||
                   memcmp(riff.riff, "riff", 4) == 0);

    int is_wave = (memcmp(riff.wave, "WAVE", 4) == 0 ||
                   memcmp(riff.wave, "wave", 4) == 0);

    if (!is_riff) {
        ESP_LOGW(TAG, "Not a standard RIFF file, found: '%.4s', trying to continue anyway", riff.riff);
        // 尝试继续，可能能播放
    }

    if (!is_wave) {
        ESP_LOGW(TAG, "Not a standard WAVE file, found: '%.4s', trying to continue anyway", riff.wave);
        // 尝试继续
    }

    // 解析 chunks - 支持非标准格式
    while (!data_found && !feof(file)) {
        wav_chunk_t chunk;
        long chunk_start = ftell(file);

        if (fread(&chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
            ESP_LOGW(TAG, "Failed to read chunk header at offset %ld", chunk_start);
            break;
        }

        ESP_LOGI(TAG, "Chunk: %.4s, size: %lu, at offset: %ld",
                 chunk.id, (unsigned long)chunk.size, chunk_start);

        if (memcmp(chunk.id, "fmt ", 4) == 0) {
            if (chunk.size >= 16) {
                // 读取基本的 fmt 数据
                uint8_t fmt_buf[40];
                size_t to_read = (chunk.size < sizeof(fmt_buf)) ? chunk.size : sizeof(fmt_buf);
                if (fread(fmt_buf, 1, to_read, file) == to_read) {
                    // 解析 fmt
                    fmt.format = fmt_buf[0] | (fmt_buf[1] << 8);
                    fmt.channels = fmt_buf[2] | (fmt_buf[3] << 8);
                    fmt.sample_rate = fmt_buf[4] | (fmt_buf[5] << 8) | (fmt_buf[6] << 16) | (fmt_buf[7] << 24);
                    fmt.byte_rate = fmt_buf[8] | (fmt_buf[9] << 8) | (fmt_buf[10] << 16) | (fmt_buf[11] << 24);
                    fmt.block_align = fmt_buf[12] | (fmt_buf[13] << 8);
                    fmt.bits_per_sample = fmt_buf[14] | (fmt_buf[15] << 8);

                    fmt_parsed = 1;
                    ESP_LOGI(TAG, "WAV Format: %lu Hz, %u ch, %u bits, format=%u",
                             (unsigned long)fmt.sample_rate, (unsigned)fmt.channels, (unsigned)fmt.bits_per_sample, (unsigned)fmt.format);
                }
                // 跳过剩余的 fmt 数据
                if (chunk.size > 16) {
                    fseek(file, chunk.size - 16, SEEK_CUR);
                }
            } else {
                ESP_LOGW(TAG, "fmt chunk too small: %lu", (unsigned long)chunk.size);
                fseek(file, chunk.size, SEEK_CUR);
            }
        } else if (memcmp(chunk.id, "data", 4) == 0) {
            // 找到数据块
            if (!fmt_parsed) {
                ESP_LOGW(TAG, "Found data chunk but fmt not parsed yet");
                // 尝试继续，使用默认参数
                fmt.sample_rate = 44100;
                fmt.channels = 2;
                fmt.bits_per_sample = 16;
                fmt_parsed = 1;
            }
            data_found = 1;
            data_start_offset = ftell(file);  // 记录数据起始位置
            ESP_LOGI(TAG, "Found data chunk, size: %lu, data starts at offset %ld, starting playback",
                     (unsigned long)chunk.size, data_start_offset);
            break;
        } else {
            // 跳过其他 chunk (LIST, fact, cue 等)
            ESP_LOGI(TAG, "Skipping chunk: %.4s, size: %lu", chunk.id, (unsigned long)chunk.size);
            // 考虑对齐
            long skip_size = chunk.size;
            if (chunk.size & 1) skip_size++;
            fseek(file, skip_size, SEEK_CUR);
        }

        // 防止无限循环
        long current_pos = ftell(file);
        if (current_pos <= chunk_start) {
            ESP_LOGE(TAG, "File position not advancing, breaking");
            break;
        }
    }

    // 播放音频数据
    static int16_t audio_buf[512];
    size_t bytes_written;
    uint32_t sample_rate = fmt.sample_rate ? fmt.sample_rate : 44100;
    uint16_t channels = fmt.channels ? fmt.channels : 2;
    uint16_t bits = fmt.bits_per_sample ? fmt.bits_per_sample : 16;

    ESP_LOGI(TAG, "Starting playback: %lu Hz, %u ch, %u bits, data_found=%d, data_offset=%ld",
             (unsigned long)sample_rate, channels, bits, data_found, data_start_offset);

    int total_samples = 0;
    bool paused_by_user = false;

    // 如果有恢复位置，需要跳过前面部分
    // 注意：play_current_song中的fseek被play_wav_file开头的fseek覆盖了，需要在这里重新跳转
    if (g_ctx.current_position_ms > 0 && data_start_offset > 0) {
        // 计算需要跳过的样本数
        uint32_t bytes_per_sample = (bits / 8) * channels;
        uint32_t samples_per_ms = sample_rate / 1000;
        uint32_t samples_to_skip = g_ctx.current_position_ms * samples_per_ms;
        long bytes_to_skip = samples_to_skip * bytes_per_sample;

        // 确保至少跳到数据区域开始
        long target_offset = data_start_offset + bytes_to_skip;

        fseek(file, target_offset, SEEK_SET);

        ESP_LOGI(TAG, "Resuming from position: %lu ms, skipped %lu samples (%ld bytes), file offset: %ld",
                 (unsigned long)g_ctx.current_position_ms, (unsigned long)samples_to_skip, bytes_to_skip, target_offset);

        // 验证文件位置
        long actual_pos = ftell(file);
        if (actual_pos != target_offset) {
            ESP_LOGW(TAG, "Seek mismatch: expected %ld, got %ld", target_offset, actual_pos);
        }
    }

    while (!g_ctx.stop_audio_task) {
        // 快速检查状态变化，让切歌响应更快
        if (g_ctx.state == PLAY_STATE_STOPPED) {
            ESP_LOGI(TAG, "Playback stopped by state change");
            break;
        }

        // 检查播放状态
        if (g_ctx.state == PLAY_STATE_PAUSED) {
            if (!paused_by_user) {
                paused_by_user = true;
                ESP_LOGI(TAG, "Playback paused at %lu ms", (unsigned long)g_ctx.current_position_ms);
            }
            vTaskDelay(20 / portTICK_PERIOD_MS);  // 等待时定期让出CPU
            continue;
        }

        if (g_ctx.state != PLAY_STATE_PLAYING) {
            // 停止播放
            break;
        }

        // 从暂停恢复
        if (paused_by_user) {
            paused_by_user = false;
            ESP_LOGI(TAG, "Playback resumed");
        }

        // 读取并播放音频数据
        size_t bytes_read = fread(audio_buf, 1, sizeof(audio_buf), file);
        if (bytes_read == 0) {
            // 文件结束，播放下一首
            ESP_LOGI(TAG, "End of file reached, total samples: %d", total_samples);
            // 歌曲播放完成，清零该歌曲的保存位置
            g_ctx.song_positions[g_ctx.current_song_index] = 0;
            next_song();
            break;
        }

        total_samples += bytes_read / (bits / 8);

        // 应用音量 (only for 16-bit samples)
        if (bits == 16) {
            int sample_count = bytes_read / sizeof(int16_t);
            apply_volume(audio_buf, sample_count, g_ctx.volume);
        }

        // 输出到 I2S
        esp_err_t ret = i2s_channel_write(g_i2s_tx_chan, audio_buf, bytes_read, &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write error: %s", esp_err_to_name(ret));
        } else if (total_samples < 1000) {
            // 只在开始打印几次，避免刷屏
            ESP_LOGI(TAG, "I2S wrote %d bytes, ret=%d", (int)bytes_written, ret);
        }

        // 更新播放位置
        if (channels > 0 && sample_rate > 0) {
            g_ctx.current_position_ms += (bytes_read * 1000) / (sample_rate * channels * (bits/8));
        }
    }

    ESP_LOGI(TAG, "Playback stopped");
}

// 音频任务
static void audio_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Audio task started");

    while (!g_ctx.stop_audio_task) {
        if (g_ctx.state == PLAY_STATE_PLAYING && g_ctx.current_file != NULL) {
            song_info_t *song = &g_ctx.songs[g_ctx.current_song_index];

            // 根据文件类型选择播放方式
            if (strstr(song->name, ".wav") || strstr(song->name, ".WAV")) {
                play_wav_file(g_ctx.current_file);
            } else {
                // 对于 MP3 文件，暂时作为原始 PCM 处理（需要添加 MP3 解码器）
                // 这里简化处理，实际应该使用 MP3 解码库
                ESP_LOGW(TAG, "MP3 decoding not implemented in this version, use WAV files");
                printf("Note: This version only supports WAV files\r\n");
                vTaskDelay(3000 / portTICK_PERIOD_MS);
                next_song();
            }
        } else {
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }
    }

    g_ctx.audio_task_running = false;
    ESP_LOGI(TAG, "Audio task stopped");
    vTaskDelete(NULL);
}

// 初始化硬件
static void init_hardware(void)
{
    ESP_LOGI(TAG, "Initializing hardware...");

    g_audio_mutex = xSemaphoreCreateMutex();

    if (g_ctx.volume == 0) {
        g_ctx.volume = 70;
    }

    ESP_LOGI(TAG, "Hardware initialized");
}

// 首次启动提示
static void first_boot_prompt(void)
{
    printf("\r\n");
    printf("============================================\r\n");
    printf("       Welcome to ESP32 MP3 Player          \r\n");
    printf("============================================\r\n");
    printf("\r\n");
    printf("*** Please set a name for me ***\r\n");
    printf("Use command: name <your_device_name>\r\n");
    printf("Example: name MyMP3\r\n\r\n");
}

// 正常启动显示
static void normal_boot_display(void)
{
    printf("\r\n");
    printf("============================================\r\n");
    printf("       Welcome to ESP32 MP3 Player          \r\n");
    printf("============================================\r\n");
    printf("\r\n");
    printf("Device name: %s\r\n", g_ctx.device_name);
    printf("Song count: %d\r\n", g_ctx.song_count);
    printf("\r\nType 'help' for command list\r\n\r\n");
}

// 主函数
void app_main(void)
{
    esp_log_level_set("MP3_PLAYER", ESP_LOG_INFO);

    ESP_LOGI(TAG, "ESP32 MP3 Player starting...");

    // 初始化各个模块
    init_nvs();
    load_device_config();
    init_spiffs();
    init_hardware();
    init_i2s();
    init_uart();
    init_buttons();

    // 扫描歌曲
    scan_songs();

    // 创建任务
    xTaskCreate(uart_task, "uart_task", 4096, NULL, 5, NULL);
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);
    xTaskCreate(audio_task, "audio_task", 16384, NULL, 10, NULL);

    // 显示启动信息
    if (g_ctx.is_first_boot) {
        first_boot_prompt();
    } else {
        normal_boot_display();
    }

    ESP_LOGI(TAG, "MP3 Player initialized successfully");
}
