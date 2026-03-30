/**
 * @file comm_protocol.cpp
 * @brief 串口通信协议实现
 */

#include "comm_protocol.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include <cstring>

// 串口配置
#define UART_NUM UART_NUM_0
#define SERIAL_WRITE(data, len) uart_write_bytes(UART_NUM, (const char*)(data), (len))
#define SERIAL_WRITE_BYTE(data) uart_write_bytes(UART_NUM, (const char*)&(data), 1)
#define SERIAL_PRINT(msg) uart_write_bytes(UART_NUM, (msg), strlen(msg))
#define SERIAL_PRINTLN(msg) do { uart_write_bytes(UART_NUM, (msg), strlen(msg)); uart_write_bytes(UART_NUM, "\r\n", 2); } while(0)
#define SERIAL_AVAILABLE() ({ size_t len; uart_get_buffered_data_len(UART_NUM, &len); len; })
#define SERIAL_READ() ({ uint8_t data; uart_read_bytes(UART_NUM, &data, 1, 0); data; })

static const char *TAG = "COMM_PROT";

// ==================== 构造函数/析构函数 ====================

CommProtocol::CommProtocol(MNISTInference *inferencer)
    : inferencer_(inferencer)
    , initialized_(false)
    , batch_mode_(false)
    , buffer_index_(0)
    , receiving_data_(false)
{
    reset_buffer();
}

CommProtocol::~CommProtocol()
{
    // 清理
    initialized_ = false;
}

// ==================== 初始化 ====================

bool CommProtocol::init()
{
    if (initialized_) {
        return true;
    }

    ESP_LOGI(TAG, "初始化通信协议...");

    // 初始化缓冲区
    reset_buffer();

    // 重置统计
    reset_statistics();

#ifndef ARDUINO
    // 纯 ESP-IDF：配置 UART
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = 0  // 添加缺失的初始化器
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, 2048, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
#endif

    initialized_ = true;

    // 发送欢迎消息
    SERIAL_PRINTLN("");
    SERIAL_PRINTLN("==============================================");
    SERIAL_PRINTLN("ESP32 MNIST 推理系统");
    SERIAL_PRINTLN("==============================================");
    SERIAL_PRINTLN("");
    SERIAL_PRINTLN("命令:");
    SERIAL_PRINTLN("  P - 单次推理模式");
    SERIAL_PRINTLN("  B - 批量测试模式");
    SERIAL_PRINTLN("  S - 显示统计");
    SERIAL_PRINTLN("  R - 重置统计");
    SERIAL_PRINTLN("  H - 帮助");
    SERIAL_PRINTLN("");
    SERIAL_PRINTLN("等待命令...");
    SERIAL_PRINTLN("");

    ESP_LOGI(TAG, "通信协议初始化完成");
    return true;
}

// ==================== 主处理循环 ====================

void CommProtocol::process()
{
    if (!initialized_) {
        return;
    }

    // 检查是否有数据可读
    while (SERIAL_AVAILABLE() > 0) {
        int data = SERIAL_READ();
        if (data < 0) {
            continue;
        }

        uint8_t byte = (uint8_t)data;

        if (receiving_data_) {
            // 正在接收图像数据
            if (buffer_index_ < IMAGE_DATA_SIZE) {
                image_buffer_[buffer_index_++] = byte;

                if (is_buffer_full()) {
                    // 缓冲区已满，执行推理
                    do_single_inference();
                    reset_buffer();
                }
            }
        } else {
            // 处理命令字符
            if (byte >= 0x20 && byte <= 0x7E) {
                // 可打印 ASCII 字符，作为命令处理
                handle_command((char)byte);
            }
        }
    }
}

// ==================== 命令处理 ====================

void CommProtocol::handle_command(char cmd)
{
    switch (cmd) {
        case CMD_SINGLE_INFER:
        case 'p':  // 小写也接受
            // 进入单次推理模式，等待图像数据
            SERIAL_PRINTLN("[INFO] 单次推理模式，等待 784 字节图像数据...");
            reset_buffer();
            receiving_data_ = true;
            break;

        case CMD_BATCH_MODE:
        case 'b':
            // 切换批量模式
            batch_mode_ = !batch_mode_;
            if (batch_mode_) {
                SERIAL_PRINTLN("[INFO] 进入批量测试模式");
                reset_statistics();
            } else {
                SERIAL_PRINTLN("[INFO] 退出批量测试模式");
                send_statistics();
            }
            break;

        case CMD_SHOW_STATS:
        case 's':
            // 发送统计信息
            send_statistics();
            break;

        case CMD_RESET_STATS:
        case 'r':
            // 重置统计
            reset_statistics();
            SERIAL_PRINTLN("[INFO] 统计已重置");
            break;

        case CMD_HELP:
        case 'h':
        case '?':
            // 发送帮助信息
            send_help();
            break;

        case CMD_EXIT_BATCH:
        case 'e':
            // 退出批量模式
            if (batch_mode_) {
                batch_mode_ = false;
                SERIAL_PRINTLN("[INFO] 退出批量测试模式");
                send_statistics();
            }
            break;

        default:
            // 未知命令
            char error_buf[32];
            snprintf(error_buf, sizeof(error_buf), "[ERROR] 未知命令: %c", cmd);
            SERIAL_PRINTLN(error_buf);
            break;
    }
}

// ==================== 图像数据和推理 ====================

void CommProtocol::do_single_inference()
{
    if (!inferencer_) {
        SERIAL_PRINTLN("[ERROR] 推理器未初始化");
        return;
    }

    // 执行推理
    InferenceResult result;
    bool success = inferencer_->infer(image_buffer_, result);

    if (success) {
        // 如果在批量模式，更新统计
        if (batch_mode_) {
            update_statistics(result, 0);  // 标签未知，假设正确性无法判断
        }

        // 发送结果
        send_result(result);
    } else {
        SERIAL_PRINTLN("[ERROR] 推理失败");
    }
}

void CommProtocol::send_result(const InferenceResult &result)
{
    // 格式: DIGIT: X, CONF: Y.YY
    char buffer[128];
    snprintf(buffer, sizeof(buffer),
             "DIGIT: %d, CONF: %.4f, TIME: %lu ms",
             result.predicted_digit,
             result.confidence,
             result.inference_time_ms);

    SERIAL_PRINTLN(buffer);

    // 也发送所有类别的概率
    SERIAL_PRINT("PROBS:");
    char prob_buf[16];
    for (int i = 0; i < MNIST_NUM_CLASSES; i++) {
        snprintf(prob_buf, sizeof(prob_buf), " %.4f", result.probabilities[i]);
        SERIAL_PRINT(prob_buf);
    }
    SERIAL_PRINTLN("");
}

// ==================== 统计信息 ====================

void CommProtocol::update_statistics(const InferenceResult &result, int true_label)
{
    stats_.total_samples++;
    stats_.total_inference_time_ms += result.inference_time_ms;
    stats_.sum_confidence += result.confidence;

    if (result.confidence < stats_.min_confidence) {
        stats_.min_confidence = result.confidence;
    }
    if (result.confidence > stats_.max_confidence) {
        stats_.max_confidence = result.confidence;
    }

    // 如果知道真实标签，统计正确率
    if (true_label >= 0 && true_label < MNIST_NUM_CLASSES) {
        if (result.predicted_digit == true_label) {
            stats_.correct_predictions++;
        }
    }
}

void CommProtocol::send_statistics()
{
    char buffer[256];

    SERIAL_PRINTLN("\n========== 统计信息 ==========");

    snprintf(buffer, sizeof(buffer), "总样本数: %d", stats_.total_samples);
    SERIAL_PRINTLN(buffer);

    if (stats_.total_samples > 0) {
        float accuracy = stats_.correct_predictions / (float)stats_.total_samples * 100.0f;
        float avg_confidence = stats_.sum_confidence / stats_.total_samples;
        float avg_inference_time = stats_.total_inference_time_ms / (float)stats_.total_samples;

        snprintf(buffer, sizeof(buffer), "正确预测: %d", stats_.correct_predictions);
        SERIAL_PRINTLN(buffer);

        snprintf(buffer, sizeof(buffer), "准确率: %.2f%%", accuracy);
        SERIAL_PRINTLN(buffer);

        snprintf(buffer, sizeof(buffer), "平均置信度: %.4f", avg_confidence);
        SERIAL_PRINTLN(buffer);

        snprintf(buffer, sizeof(buffer), "平均推理时间: %.2f ms", avg_inference_time);
        SERIAL_PRINTLN(buffer);

        snprintf(buffer, sizeof(buffer), "最小置信度: %.4f", stats_.min_confidence);
        SERIAL_PRINTLN(buffer);

        snprintf(buffer, sizeof(buffer), "最大置信度: %.4f", stats_.max_confidence);
        SERIAL_PRINTLN(buffer);
    }

    SERIAL_PRINTLN("==============================\n");
}

void CommProtocol::send_help()
{
    SERIAL_PRINTLN("\n========== 命令帮助 ==========");
    SERIAL_PRINTLN("P - 单次推理模式");
    SERIAL_PRINTLN("    发送 'P' 后，上位机发送 784 字节图像数据");
    SERIAL_PRINTLN("    ESP32 返回预测结果");
    SERIAL_PRINTLN("");
    SERIAL_PRINTLN("B - 批量测试模式");
    SERIAL_PRINTLN("    进入交互式批量测试模式");
    SERIAL_PRINTLN("    支持多轮推理和统计");
    SERIAL_PRINTLN("");
    SERIAL_PRINTLN("S - 显示统计");
    SERIAL_PRINTLN("    显示批量测试的统计信息");
    SERIAL_PRINTLN("");
    SERIAL_PRINTLN("R - 重置统计");
    SERIAL_PRINTLN("    清除所有统计数据");
    SERIAL_PRINTLN("");
    SERIAL_PRINTLN("H - 帮助");
    SERIAL_PRINTLN("    显示此帮助信息");
    SERIAL_PRINTLN("==============================\n");
}
