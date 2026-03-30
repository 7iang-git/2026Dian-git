/**
 * @file mnist_infer.cpp
 * @brief MNIST 推理类实现
 */

#include "mnist_infer.hpp"
#include "fbs_model.hpp"
#include "dl_tensor_base.hpp"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

static const char *TAG = "MNIST_INFER";

// ==================== 构造函数/析构函数 ====================

MNISTInference::MNISTInference()
    : model_(nullptr)
    , input_tensor_base_(nullptr)
    , output_tensor_base_(nullptr)
    , input_buffer_(nullptr)
    , output_buffer_(nullptr)
    , initialized_(false)
{
}

MNISTInference::~MNISTInference()
{
    // 清理资源
    if (input_tensor_base_) {
        delete static_cast<dl::TensorBase*>(input_tensor_base_);
        input_tensor_base_ = nullptr;
    }

    if (output_tensor_base_) {
        delete static_cast<dl::TensorBase*>(output_tensor_base_);
        output_tensor_base_ = nullptr;
    }

    if (input_buffer_) {
        free(input_buffer_);
        input_buffer_ = nullptr;
    }

    if (output_buffer_) {
        free(output_buffer_);
        output_buffer_ = nullptr;
    }

    if (model_) {
        delete static_cast<fbs::FbsModel*>(model_);
        model_ = nullptr;
    }

    initialized_ = false;
    ESP_LOGI(TAG, "资源已清理");
}

// ==================== 初始化 ====================

bool MNISTInference::init(const char *model_path)
{
    ESP_LOGI(TAG, "初始化推理器...");
    ESP_LOGI(TAG, "模型路径: %s", model_path);

    if (initialized_) {
        ESP_LOGW(TAG, "推理器已初始化，跳过");
        return true;
    }

    // 加载模型文件
    uint8_t *model_data = nullptr;
    size_t model_size = 0;
    if (!load_model_file(model_path, &model_data, &model_size)) {
        ESP_LOGE(TAG, "模型文件加载失败");
        return false;
    }

    // 创建 FbsModel
    ESP_LOGI(TAG, "创建 FbsModel...");
    fbs::FbsModel *fbs_model = new fbs::FbsModel(
        model_data,
        model_size,
        fbs::MODEL_LOCATION_IN_FLASH_PARTITION,
        false,   // encrypt
        false,   // rodata_move
        true,    // auto_free (模型数据由 FbsModel 管理)
        false    // param_copy
    );
    if (!fbs_model) {
        ESP_LOGE(TAG, "FbsModel 创建失败");
        if (model_data) {
            free(model_data);
        }
        return false;
    }
    model_ = fbs_model;
    ESP_LOGI(TAG, "FbsModel 创建成功");

    // 打印模型信息
    fbs_model->print();

    // 获取输入输出信息
    input_shape_ = fbs_model->get_graph_inputs();
    output_shape_ = fbs_model->get_graph_outputs();

    ESP_LOGI(TAG, "输入节点数: %d", (int)input_shape_.size());
    ESP_LOGI(TAG, "输出节点数: %d", (int)output_shape_.size());

    // 分配缓冲区
    input_buffer_ = (float*)heap_caps_malloc(MNIST_IMAGE_SIZE * sizeof(float), MALLOC_CAP_DEFAULT);
    output_buffer_ = (float*)heap_caps_malloc(MNIST_NUM_CLASSES * sizeof(float), MALLOC_CAP_DEFAULT);

    if (!input_buffer_ || !output_buffer_) {
        ESP_LOGE(TAG, "内存分配失败");
        return false;
    }

    // 初始化为0
    memset(input_buffer_, 0, sizeof(float) * MNIST_IMAGE_SIZE);
    memset(output_buffer_, 0, sizeof(float) * MNIST_NUM_CLASSES);

    // 创建张量
    // TensorBase 构造函数: TensorBase(std::vector<int> shape, const void* element, int exponent, dtype_t dtype, bool deep, uint32_t caps)
    // 注意：ESP-IDF 禁用 C++ 异常，TensorBase 的自定义 operator new 可能在内存不足时返回 nullptr
    input_tensor_base_ = new dl::TensorBase(
        std::vector<int>{1, MNIST_IMAGE_SIZE},  // shape
        input_buffer_,                           // element data
        0,                                       // exponent
        dl::DATA_TYPE_FLOAT,                     // dtype
        false,                                   // deep (不拷贝数据)
        MALLOC_CAP_DEFAULT                       // caps
    );
    if (!input_tensor_base_) {
        ESP_LOGE(TAG, "输入张量创建失败");
        return false;
    }

    output_tensor_base_ = new dl::TensorBase(
        std::vector<int>{1, MNIST_NUM_CLASSES},  // shape
        output_buffer_,                          // element data
        0,                                       // exponent
        dl::DATA_TYPE_FLOAT,                     // dtype
        false,                                   // deep (不拷贝数据)
        MALLOC_CAP_DEFAULT                       // caps
    );
    if (!output_tensor_base_) {
        ESP_LOGE(TAG, "输出张量创建失败");
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "推理器初始化完成！");
    return true;
}

// ==================== 推理 ====================

bool MNISTInference::infer(const uint8_t *image_data, InferenceResult &result)
{
    if (!initialized_) {
        ESP_LOGE(TAG, "推理器未初始化");
        return false;
    }

    if (image_data == nullptr) {
        ESP_LOGE(TAG, "输入图像为空");
        return false;
    }

    // 准备输入数据（归一化到 [0, 1]）
    prepare_input(image_data);

    // 记录开始时间
    uint64_t start_time = esp_timer_get_time();

    // TODO: 使用 ESP-DL 执行推理
    // 注意：新版 ESP-DL API 需要使用 fbs::FbsModel 的推理方法
    // 这里暂时使用简化的推理逻辑

    // 简化的推理：使用简单的加权求和模拟神经网络
    // 注意：这只是一个占位符，实际使用时需要完整的推理实现
    float temp_output[MNIST_NUM_CLASSES] = {0};

    // 简化的全连接层计算
    for (int i = 0; i < MNIST_NUM_CLASSES; i++) {
        float sum = 0.0f;
        for (int j = 0; j < MNIST_IMAGE_SIZE; j++) {
            // 使用输入值乘以模拟的权重
            sum += input_buffer_[j] * (0.01f + (float)(i * j % 10) / 100.0f);
        }
        temp_output[i] = sum;
    }

    // Softmax 激活
    float max_val = temp_output[0];
    for (int i = 1; i < MNIST_NUM_CLASSES; i++) {
        if (temp_output[i] > max_val) max_val = temp_output[i];
    }

    float sum_exp = 0.0f;
    for (int i = 0; i < MNIST_NUM_CLASSES; i++) {
        output_buffer_[i] = expf(temp_output[i] - max_val);
        sum_exp += output_buffer_[i];
    }

    for (int i = 0; i < MNIST_NUM_CLASSES; i++) {
        output_buffer_[i] /= sum_exp;
    }

    // 记录结束时间
    uint64_t end_time = esp_timer_get_time();

    // 解析输出
    parse_output(result);

    // 计算推理时间（微秒转毫秒）
    result.inference_time_ms = (end_time - start_time) / 1000;

    return true;
}

// ==================== 辅助函数 ====================

void MNISTInference::prepare_input(const uint8_t *image_data)
{
    // 将 uint8 [0, 255] 转换为 float [0, 1]
    for (int i = 0; i < MNIST_IMAGE_SIZE; i++) {
        input_buffer_[i] = image_data[i] / 255.0f;
    }
}

void MNISTInference::parse_output(InferenceResult &result)
{
    // 找到概率最大的类别
    int predicted_class = 0;
    float max_prob = output_buffer_[0];

    for (int i = 1; i < MNIST_NUM_CLASSES; i++) {
        if (output_buffer_[i] > max_prob) {
            max_prob = output_buffer_[i];
            predicted_class = i;
        }
    }

    // 填充结果结构体
    result.predicted_digit = predicted_class;
    result.confidence = max_prob;

    // 复制所有类别的概率
    for (int i = 0; i < MNIST_NUM_CLASSES; i++) {
        result.probabilities[i] = output_buffer_[i];
    }
}

bool MNISTInference::load_model_file(const char *model_path, uint8_t **data, size_t *out_size)
{
    ESP_LOGI(TAG, "加载模型文件: %s", model_path);

    FILE *f = fopen(model_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "无法打开模型文件");
        return false;
    }

    // 获取文件大小
    fseek(f, 0, SEEK_END);
    *out_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (*out_size == 0) {
        ESP_LOGE(TAG, "模型文件为空");
        fclose(f);
        return false;
    }

    // 分配内存
    *data = (uint8_t*)heap_caps_malloc(*out_size, MALLOC_CAP_DEFAULT);
    if (!*data) {
        ESP_LOGE(TAG, "无法分配内存加载模型");
        fclose(f);
        return false;
    }

    // 读取文件
    if (fread(*data, 1, *out_size, f) != *out_size) {
        ESP_LOGE(TAG, "读取模型文件失败");
        free(*data);
        *data = nullptr;
        fclose(f);
        return false;
    }

    fclose(f);
    ESP_LOGI(TAG, "模型文件加载成功，大小: %d 字节", (int)*out_size);
    return true;
}

const char* MNISTInference::get_model_info() const
{
    if (!initialized_ || model_ == nullptr) {
        return "模型未加载";
    }

    fbs::FbsModel *fbs_model = static_cast<fbs::FbsModel*>(model_);
    return fbs_model->get_model_name().c_str();
}
