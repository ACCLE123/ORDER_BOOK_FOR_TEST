cmake_minimum_required(VERSION 3.10)
project(OrderBookSync)

set(CMAKE_CXX_STANDARD 11)

# 1. 设置 Homebrew 路径
list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/openssl@3")
list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/nlohmann-json")

# 2. 寻找依赖
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

# 3. 配置 IXWebSocket：强制开启 TLS 和 OpenSSL 支持
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPENSSL ON CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

# 添加本地的 IXWebSocket 源码
add_subdirectory(IXWebSocket)

# 4. 创建可执行文件
add_executable(okx_sync main.cpp)

# 5. 链接所有库
target_link_libraries(okx_sync PRIVATE 
    ixwebsocket 
    nlohmann_json::nlohmann_json
    OpenSSL::SSL 
    OpenSSL::Crypto
)

# 6. 包含路径
target_include_directories(okx_sync PRIVATE "/opt/homebrew/include")
target_link_directories(okx_sync PRIVATE "/opt/homebrew/lib")
