cmake_minimum_required(VERSION 3.21)

project(Chat-Client VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 向编译器添加编译选项
if (MSVC)
    add_compile_options(/W3 /utf-8)
else()
    add_compile_options(-Wall -utf-8)
endif()

find_package(Qt6 COMPONENTS Widgets Network REQUIRED)
find_package(OpenSSL REQUIRED)

# 源文件列表
set(SOURCES
    main.cpp
    views/LoginWindow.cpp
    views/MainWindow.cpp
    core/NetworkManager.cpp
    ../CommonModule/encryption/EncryptionManager.cpp
)

# 头文件列表
set(HEADERS
    views/LoginWindow.h
    views/MainWindow.h
    core/NetworkManager.h
    ../CommonModule/encryption/EncryptionManager.h
)

# UI 文件列表
set(UIS
    ui/LoginWindow.ui
    ui/MainWindow.ui
)

# 添加 ui 目录到 ui 搜索路径
set(CMAKE_AUTOUIC_SEARCH_PATHS
    ui/
)

# 生成可执行文件
add_executable(Chat-Client ${SOURCES} ${HEADERS} ${UIS})

# 包含目录
target_include_directories(Chat-Client PRIVATE
    views/
    core/
    ui/
    ../CommonModule/encryption/
)

# 链接 Qt 和 OpenSSL 库
target_link_libraries(Chat-Client PUBLIC
    Qt6::Widgets
    Qt6::Network
    OpenSSL::Crypto
    OpenSSL::SSL
)

# 添加Widgets, Network 模块的头文件路径
target_include_directories(Chat-Client SYSTEM PUBLIC
    ${Qt6Widgets_INCLUDE_DIRS}
    ${Qt6Network_INCLUDE_DIRS}
    ${OpenSSL_INCLUDE_DIR}
)
