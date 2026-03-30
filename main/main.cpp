/**
 * ESP32 MNIST 推理 - 主程序
 *
 * 功能：
 * 1. 初始化 SPIFFS 文件系统
 * 2. 加载 ESPDL 模型
 * 3. 启动串口通信协议
 * 4. 处理上位机命令
 */

#include <iostream>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_spiffs.h"

// 自定义头文件
#include "mnist_infer.hpp"
#include "comm_protocol.hpp"

// 标签
static const char *TAG = "MAIN";

// 全局变量
MNISTInference *g_inferencer = nullptr;
CommProtocol *g_protocol = nullptr;

// 函数声明
static bool init_spiffs(void);
static void cleanup(void);

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32 MNIST 推理系统启动");
    ESP_LOGI(TAG, "========================================\n");

    // 初始化 SPIFFS
    if (!init_spiffs()) {
        ESP_LOGE(TAG, "SPIFFS 初始化失败，系统停止");
        return;
    }

    // 创建推理器
    g_inferencer = new MNISTInference();
    if (!g_inferencer->init("/storage/mnist_model.espdl")) {
        ESP_LOGE(TAG, "模型加载失败，系统停止");
        cleanup();
        return;
    }

    // 创建通信协议处理器
    g_protocol = new CommProtocol(g_inferencer);
    if (!g_protocol->init()) {
        ESP_LOGE(TAG, "通信协议初始化失败");
        cleanup();
        return;
    }

    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "系统初始化完成，等待上位机命令...");
    ESP_LOGI(TAG, "========================================\n");

    // 主循环
    while (1) {
        // 处理通信协议
        g_protocol->process();

        // 延时，避免占用过多 CPU
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief 初始化 SPIFFS 文件系统
 */
static bool init_spiffs(void)
{
    ESP_LOGI(TAG, "初始化 SPIFFS...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "无法挂载 SPIFFS 文件系统");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "未找到 SPIFFS 分区，请检查分区表配置");
        } else {
            ESP_LOGE(TAG, "SPIFFS 初始化失败，错误码: %d", ret);
        }
        return false;
    }

    // 检查模型文件是否存在
    FILE* f = fopen("/storage/mnist_model.espdl", "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "模型文件不存在: /storage/mnist_model.espdl");
        ESP_LOGW(TAG, "请使用 'idf.py -p PORT flash' 上传文件系统镜像");
    } else {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        ESP_LOGI(TAG, "模型文件大小: %ld bytes", size);
    }

    ESP_LOGI(TAG, "SPIFFS 初始化完成");
    return true;
}

/**
 * @brief 清理资源
 */
static void cleanup(void)
{
    ESP_LOGI(TAG, "清理资源...");

    if (g_protocol) {
        delete g_protocol;
        g_protocol = nullptr;
    }

    if (g_inferencer) {
        delete g_inferencer;
        g_inferencer = nullptr;
    }

    ESP_LOGI(TAG, "清理完成");
}
