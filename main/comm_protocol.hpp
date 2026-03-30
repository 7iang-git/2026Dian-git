/**
 * @file comm_protocol.hpp
 * @brief 串口通信协议头文件
 *
 * 定义与上位机通信的协议接口，包括：
 * - 命令解析
 * - 数据接收
 * - 结果发送
 * - 批量测试模式
 */

#ifndef COMM_PROTOCOL_HPP
#define COMM_PROTOCOL_HPP

#include <cstdint>
#include <cstring>
#include "mnist_infer.hpp"

// 命令字符定义
#define CMD_SINGLE_INFER    'P'     // 单次推理
#define CMD_BATCH_MODE      'B'     // 批量测试模式
#define CMD_SHOW_STATS      'S'     // 显示统计
#define CMD_RESET_STATS     'R'     // 重置统计
#define CMD_HELP            'H'     // 帮助
#define CMD_EXIT_BATCH      'E'     // 退出批量模式

// 协议常量
#define IMAGE_DATA_SIZE     784     // 28x28 = 784 字节
#define MAX_CMD_LEN         16      // 最大命令长度
#define SERIAL_BAUDRATE     115200  // 串口波特率

// 批量测试统计
struct BatchStatistics {
    int total_samples;                    // 总样本数
    int correct_predictions;              // 正确预测数
    uint32_t total_inference_time_ms;     // 总推理时间
    float min_confidence;                 // 最小置信度
    float max_confidence;                 // 最大置信度
    float sum_confidence;                 // 置信度总和

    BatchStatistics() {
        reset();
    }

    void reset() {
        total_samples = 0;
        correct_predictions = 0;
        total_inference_time_ms = 0;
        min_confidence = 1.0f;
        max_confidence = 0.0f;
        sum_confidence = 0.0f;
    }

    float get_accuracy() const {
        if (total_samples == 0) return 0.0f;
        return (float)correct_predictions / total_samples * 100.0f;
    }

    float get_avg_confidence() const {
        if (total_samples == 0) return 0.0f;
        return sum_confidence / total_samples;
    }

    float get_avg_inference_time() const {
        if (total_samples == 0) return 0.0f;
        return (float)total_inference_time_ms / total_samples;
    }
};

/**
 * @brief 通信协议类
 *
 * 处理与上位机的串口通信，包括：
 * - 命令解析和执行
 * - 图像数据接收
 * - 推理结果发送
 * - 批量测试模式管理
 */
class CommProtocol {
public:
    /**
     * @brief 构造函数
     *
     * @param inferencer MNIST 推理器指针
     */
    CommProtocol(MNISTInference *inferencer);

    /**
     * @brief 析构函数
     */
    ~CommProtocol();

    /**
     * @brief 初始化通信协议
     *
     * @return true 初始化成功
     * @return false 初始化失败
     */
    bool init();

    /**
     * @brief 处理串口通信
     *
     * 在主循环中调用，处理接收到的命令和数据
     */
    void process();

    /**
     * @brief 获取批量测试统计
     *
     * @return const BatchStatistics& 统计信息
     */
    const BatchStatistics& get_statistics() const { return stats_; }

    /**
     * @brief 重置统计信息
     */
    void reset_statistics() { stats_.reset(); }

private:
    MNISTInference *inferencer_;          // 推理器指针
    BatchStatistics stats_;                // 批量测试统计
    bool initialized_;                     // 初始化标志
    bool batch_mode_;                      // 批量模式标志

    uint8_t image_buffer_[IMAGE_DATA_SIZE];  // 图像数据缓冲区
    int buffer_index_;                       // 缓冲区索引
    bool receiving_data_;                  // 正在接收数据标志

    /**
     * @brief 处理单个字符命令
     *
     * @param cmd 命令字符
     */
    void handle_command(char cmd);

    /**
     * @brief 处理图像数据
     *
     * @param data 数据指针
     * @param len 数据长度
     */
    void handle_image_data(const uint8_t *data, size_t len);

    /**
     * @brief 执行单次推理
     */
    void do_single_inference();

    /**
     * @brief 发送推理结果
     *
     * @param result 推理结果
     */
    void send_result(const InferenceResult &result);

    /**
     * @brief 发送统计信息
     */
    void send_statistics();

    /**
     * @brief 发送帮助信息
     */
    void send_help();

    /**
     * @brief 检查缓冲区是否已满
     *
     * @return true 缓冲区已满
     * @return false 缓冲区未满
     */
    bool is_buffer_full() const {
        return buffer_index_ >= IMAGE_DATA_SIZE;
    }

    /**
     * @brief 重置缓冲区
     */
    void reset_buffer() {
        buffer_index_ = 0;
        receiving_data_ = false;
        std::memset(image_buffer_, 0, IMAGE_DATA_SIZE);
    }

    /**
     * @brief 更新批量测试统计
     *
     * @param result 推理结果
     * @param true_label 真实标签（-1表示未知）
     */
    void update_statistics(const InferenceResult &result, int true_label);
};

#endif // COMM_PROTOCOL_HPP
