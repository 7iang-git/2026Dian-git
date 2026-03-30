# 主组件 CMakeLists.txt
# 指向真正的代码目录 esp32_mnist_idf/main/

# 获取项目根目录
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# 定义源文件
set(srcs
    "main.cpp"
    "mnist_infer.cpp"
    "comm_protocol.cpp"
)

# 定义头文件路径
set(include_dirs
    "."
    "${PROJECT_ROOT}/managed_components/espressif__esp-dl/fbs_loader/include"
    "${PROJECT_ROOT}/managed_components/espressif__esp-dl/dl/tensor/include"
    "${PROJECT_ROOT}/managed_components/espressif__esp-dl/vision/image"
)

# 注册组件
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES
        esp-dl
        esp_timer
        driver
        spiffs
        vfs
        esp_mm
        mbedtls
)
