#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"

// 根据你的开发板版本选择 GPIO
#define RGB_LED_GPIO   48   // 或 38 (v1.1)
#define LED_NUM        1

static const char *TAG = "example";

void app_main(void)
{
    // 1. 配置 LED strip 基本参数
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = LED_NUM,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,   // 注意颜色顺序
        .flags = {
            .invert_out = false,      // 不用反转
        },
    };

    // 2. 配置 RMT 外设参数
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10MHz 分辨率
        .flags = {
            .with_dma = false,              // 少量 LED 不需要 DMA
        },
    };

    // 3. 创建 LED strip 设备
    led_strip_handle_t strip;
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        return;
    }

    // 4. 点亮红色（全亮）
    led_strip_set_pixel(strip, 0, 255, 0, 0);   // R, G, B
    led_strip_refresh(strip);                    // 更新显示

    // 保持 2 秒
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 5. 熄灭
    led_strip_clear(strip);
    led_strip_refresh(strip);

    // 6. 循环演示（可选）
    while (1) {
        // 红色
        led_strip_set_pixel(strip, 0, 255, 0, 0);
        led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(1000));

        // 绿色
        led_strip_set_pixel(strip, 0, 0, 255, 0);
        led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(1000));

        // 蓝色
        led_strip_set_pixel(strip, 0, 0, 0, 255);
        led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}