/**
 * @file mnist_infer.hpp
 * @brief MNIST 推理类头文件
 */

#ifndef MNIST_INFER_HPP
#define MNIST_INFER_HPP

#include <cstdint>
#include <memory>
#include <vector>
#include <string>

// 常量定义
#define MNIST_IMAGE_SIZE    784     // 28x28
#define MNIST_NUM_CLASSES   10      // 0-9

// 向前声明 ESP-DL 类
namespace dl {
    class TensorBase;
}

namespace fbs {
    class FbsModel;
}

/**
 * @brief 推理结果结构体
 */
typedef struct {
    int predicted_digit;          // 预测的数字 (0-9)
    float confidence;             // 置信度 (0-1)
    float probabilities[10];      // 各类别的概率
    uint32_t inference_time_ms;   // 推理时间 (毫秒)
} InferenceResult;

/**
 * @brief MNIST 推理类
 *
 * 封装了 ESP-DL 模型的加载和推理功能
 */
class MNISTInference {
public:
    /**
     * @brief 构造函数
     */
    MNISTInference();

    /**
     * @brief 析构函数
     */
    ~MNISTInference();

    /**
     * @brief 初始化推理器
     *
     * @param model_path ESPDL 模型文件路径
     * @return true 初始化成功
     * @return false 初始化失败
     */
    bool init(const char *model_path);

    /**
     * @brief 执行推理
     *
     * @param image_data 图像数据，28x28 展平为 784 的数组，像素值 0-255
     * @param result 输出结果结构体
     * @return true 推理成功
     * @return false 推理失败
     */
    bool infer(const uint8_t *image_data, InferenceResult &result);

    /**
     * @brief 获取模型信息
     *
     * @return const char* 模型信息字符串
     */
    const char* get_model_info() const;

private:
    void *model_;                              // ESP-DL FbsModel 对象 (void* 避免暴露头文件)
    void *input_tensor_base_;                  // 输入张量基类指针
    void *output_tensor_base_;                 // 输出张量基类指针
    float *input_buffer_;                      // 输入缓冲区（浮点）
    float *output_buffer_;                     // 输出缓冲区
    std::vector<std::string> input_shape_;     // 输入形状
    std::vector<std::string> output_shape_;    // 输出形状
    bool initialized_;                         // 初始化标志

    /**
     * @brief 准备输入数据
     *
     * @param image_data 原始图像数据 (uint8)
     */
    void prepare_input(const uint8_t *image_data);

    /**
     * @brief 解析输出结果
     *
     * @param result 结果结构体
     */
    void parse_output(InferenceResult &result);

    /**
     * @brief 加载模型文件
     *
     * @param model_path 模型文件路径
     * @param data 输出数据指针
     * @param size 输出数据大小
     * @return true 加载成功
     * @return false 加载失败
     */
    bool load_model_file(const char *model_path, uint8_t **data, size_t *out_size);
};

#endif // MNIST_INFER_HPP
