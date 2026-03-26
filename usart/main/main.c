#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#define UART_NUM UART_NUM_0
#define BUF_SIZE 1024

// 每秒发送 Hello World\r\n
void tx_task(void *param)
{
    while (1) {
        uart_write_bytes(UART_NUM, "Hello World\r\n", strlen("Hello World\r\n"));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// 接收用户输入，检测回车后输出三行字符串
void rx_task(void *param)
{
    uint8_t data[BUF_SIZE];

    while (1) {
        int len = uart_read_bytes(UART_NUM, data, BUF_SIZE, pdMS_TO_TICKS(10));
        if (len > 0) {
            // 检查是否包含回车符 (\r 或 \n)
            for (int i = 0; i < len; i++) {
                if (data[i] == '\r' || data[i] == '\n') {
                    // 立即输出三行字符串
                    uart_write_bytes(UART_NUM, "GEL37KXHDU9G\r\n", strlen("GEL37KXHDU9G\r\n"));
                    uart_write_bytes(UART_NUM, "FXLKNKWHVURC\r\n", strlen("FXLKNKWHVURC\r\n"));
                    uart_write_bytes(UART_NUM, "CE4K7KEYCUPQ\r\n", strlen("CE4K7KEYCUPQ\r\n"));
                    break;
                }
            }
        }
    }
}

void app_main(void)
{
    // 配置UART参数
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    // 安装UART驱动
    uart_param_config(UART_NUM, &uart_config);
    uart_driver_install(UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0);

    // 创建发送任务 (1Hz)
    xTaskCreate(tx_task, "tx_task", 4096, NULL, 10, NULL);

    // 创建接收任务
    xTaskCreate(rx_task, "rx_task", 4096, NULL, 10, NULL);
}