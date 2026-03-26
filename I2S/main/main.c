#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"

// ESP32-S3 I2S引脚配置
// 推荐使用GPIO 4, 5, 6 (可根据开发板调整)
#define I2S_WS_PIN      4    // LRC (Word Select)
#define I2S_SCK_PIN     5    // BCLK (Bit Clock)
#define I2S_SD_PIN      6    // DIN (Data)
#define BOOT_BUTTON_PIN GPIO_NUM_0

// I2S配置参数
#define SAMPLE_RATE     48000
#define I2S_PORT        I2S_NUM_0

// 模式定义
typedef enum {
    MODE_SAWTOOTH_500Hz_RIGHT,      // 模式0: 右声道500Hz锯齿波
    MODE_BEAT_FREQUENCY,            // 模式1: 左1001Hz + 右999Hz 拍频
    MODE_1K_4K_SWITCH,              // 模式2: 左1kHz + 右4kHz 可切换
    MODE_MAX
} audio_mode_t;

static audio_mode_t current_mode = MODE_SAWTOOTH_500Hz_RIGHT;
static audio_mode_t sub_mode = 0;  // 用于模式2的子模式切换

// 生成正弦波样本
static int16_t generate_sine_wave(double frequency, double phase)
{
    return (int16_t)(32767 * 0.8 * sin(phase));
}

// 生成锯齿波样本
static int16_t generate_sawtooth_wave(double phase)
{
    // 锯齿波: 从-1到1线性递增
    double normalized = phase / (2 * M_PI);
    normalized = normalized - floor(normalized);
    return (int16_t)(32767 * 0.8 * (2 * normalized - 1));
}

// 填充I2S缓冲区 - 模式0: 右声道500Hz锯齿波
static void fill_buffer_sawtooth_500hz_right(int16_t *buffer, int samples_per_channel)
{
    static double phase_right = 0;
    double phase_increment_right = 2 * M_PI * 500.0 / SAMPLE_RATE;

    for (int i = 0; i < samples_per_channel; i++) {
        // 左声道静音，右声道锯齿波
        buffer[i * 2] = 0;  // L
        buffer[i * 2 + 1] = generate_sawtooth_wave(phase_right);  // R

        phase_right += phase_increment_right;
        if (phase_right >= 2 * M_PI) phase_right -= 2 * M_PI;
    }
}

// 填充I2S缓冲区 - 模式1: 拍频 (左1001Hz + 右999Hz)
static void fill_buffer_beat_frequency(int16_t *buffer, int samples_per_channel)
{
    static double phase_left = 0, phase_right = 0;
    double phase_increment_left = 2 * M_PI * 1001.0 / SAMPLE_RATE;
    double phase_increment_right = 2 * M_PI * 999.0 / SAMPLE_RATE;

    for (int i = 0; i < samples_per_channel; i++) {
        // L+R模式: 立体声两个通道同时输出
        buffer[i * 2] = generate_sine_wave(1001, phase_left);      // L
        buffer[i * 2 + 1] = generate_sine_wave(999, phase_right);  // R

        phase_left += phase_increment_left;
        phase_right += phase_increment_right;
        if (phase_left >= 2 * M_PI) phase_left -= 2 * M_PI;
        if (phase_right >= 2 * M_PI) phase_right -= 2 * M_PI;
    }
}

// 填充I2S缓冲区 - 模式2: 1kHz + 4kHz 可切换
static void fill_buffer_1k_4k_switch(int16_t *buffer, int samples_per_channel, audio_mode_t sub)
{
    static double phase_left = 0, phase_right = 0;
    double phase_increment_left = 2 * M_PI * 1000.0 / SAMPLE_RATE;
    double phase_increment_right = 2 * M_PI * 4000.0 / SAMPLE_RATE;

    for (int i = 0; i < samples_per_channel; i++) {
        switch (sub) {
            case 0: // 左声道
                buffer[i * 2] = generate_sine_wave(1000, phase_left);  // L
                buffer[i * 2 + 1] = 0;  // R 静音
                phase_right = 0;  // 重置相位
                break;
            case 1: // 右声道
                buffer[i * 2] = 0;  // L 静音
                buffer[i * 2 + 1] = generate_sine_wave(4000, phase_right);  // R
                phase_left = 0;  // 重置相位
                break;
            case 2: // L+R
            default:
                buffer[i * 2] = generate_sine_wave(1000, phase_left);   // L
                buffer[i * 2 + 1] = generate_sine_wave(4000, phase_right); // R
                break;
        }

        phase_left += phase_increment_left;
        phase_right += phase_increment_right;
        if (phase_left >= 2 * M_PI) phase_left -= 2 * M_PI;
        if (phase_right >= 2 * M_PI) phase_right -= 2 * M_PI;
    }
}

// I2S初始化 - 使用旧API (兼容ESP-IDF v5.4)
static void i2s_init(void)
{
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_SD_PIN,
        .data_in_num = -1,
    };

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_PORT);
}

// GPIO初始化 - BOOT按键
static void gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);
}

// 按键检测 - 带消抖
static uint8_t last_button_state = 1;
static uint32_t last_press_time = 0;

static void check_button(void)
{
    uint32_t current_time = xTaskGetTickCount();
    uint8_t button_state = gpio_get_level(BOOT_BUTTON_PIN);

    // 检测下降沿（按下）
    if (button_state == 0 && last_button_state == 1) {
        if (current_time - last_press_time > pdMS_TO_TICKS(50)) {  // 使用pdMS_TO_TICKS
            last_press_time = current_time;

            // 只有在模式2时才切换子模式
            if (current_mode == MODE_1K_4K_SWITCH) {
                sub_mode++;
                if (sub_mode > 2) sub_mode = 0;

                const char *mode_name[] = {"Left", "Right", "L+R"};
                printf("Mode: 1k+4k Switch, Sub-mode: %s\n", mode_name[sub_mode]);
            }
        }
    }

    last_button_state = button_state;
}

// 音频播放任务
static void audio_play_task(void *param)
{
    const int buffer_size = 512;
    int16_t *i2s_buffer = malloc(buffer_size * sizeof(int16_t) * 2);

    if (!i2s_buffer) {
        printf("Failed to allocate buffer\n");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        // 检查按键
        check_button();

        switch (current_mode) {
            case MODE_SAWTOOTH_500Hz_RIGHT:
                fill_buffer_sawtooth_500hz_right(i2s_buffer, buffer_size);
                break;
            case MODE_BEAT_FREQUENCY:
                fill_buffer_beat_frequency(i2s_buffer, buffer_size);
                break;
            case MODE_1K_4K_SWITCH:
                fill_buffer_1k_4k_switch(i2s_buffer, buffer_size, sub_mode);
                break;
            default:
                break;
        }

        // 写入I2S
        size_t bytes_written;
        i2s_write(I2S_PORT, i2s_buffer, buffer_size * sizeof(int16_t) * 2, &bytes_written, portMAX_DELAY);
    }
}

void app_main(void)
{
    printf("ESP32 I2S Audio Test\n");
    printf("===================\n");

    // 初始化
    i2s_init();
    gpio_init();

    printf("\n--- Audio Modes ---\n");
    printf("Mode 0: Right channel 500Hz Sawtooth\n");
    printf("Mode 1: L=1001Hz + R=999Hz (Beat frequency)\n");
    printf("Mode 2: L=1kHz + R=4kHz (Press BOOT to switch L/R/L+R)\n");
    printf("\nCurrent Mode: 0 (500Hz Sawtooth - Right channel)\n");

    // 创建音频任务
    xTaskCreate(audio_play_task, "audio_play", 4096, NULL, 5, NULL);

    // 主循环 - 通过串口切换模式（使用 fgets 避免刷屏）
    char input[32];
    while (1) {
        printf("\nSelect mode (0-2): ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) != NULL) {
            // 去除换行符
            size_t len = strlen(input);
            if (len > 0 && input[len - 1] == '\n') {
                input[len - 1] = '\0';
            }

            int mode = atoi(input);
            if (mode >= 0 && mode < MODE_MAX) {
                current_mode = (audio_mode_t)mode;
                sub_mode = 0;
                switch (mode) {
                    case 0:
                        printf("Mode 0: Right channel 500Hz Sawtooth\n");
                        break;
                    case 1:
                        printf("Mode 1: L=1001Hz + R=999Hz (Beat frequency)\n");
                        break;
                    case 2:
                        printf("Mode 2: L=1kHz + R=4kHz - Press BOOT button to switch\n");
                        break;
                }
            } else {
                printf("Invalid mode. Please enter 0, 1, or 2.\n");
            }
        } else {
            // fgets 失败（极少情况），稍作延时再试
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}