cmake_minimum_required(VERSION 3.18)

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

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Widgets Network REQUIRED)
find_package(OpenSSL REQUIRED)

# 源文件列表
set(SOURCES
    src/main.cpp
    src/views/LoginWindow.cpp
    src/views/MainWindow.cpp
    src/core/NetworkManager.cpp
    src/core/EncryptionManager.cpp
)

# 头文件列表
set(HEADERS
    src/views/LoginWindow.h
    src/views/MainWindow.h
    src/core/NetworkManager.h
    src/core/EncryptionManager.h
)

# UI 文件列表
set(UIS
    src/ui/LoginWindow.ui
    src/ui/MainWindow.ui
)

# 添加 ui 目录到 ui 搜索路径
set(CMAKE_AUTOUIC_SEARCH_PATHS src/ui)

# 生成可执行文件
add_executable(Chat-Client ${SOURCES} ${HEADERS} ${UIS})

# 包含目录
target_include_directories(Chat-Client PRIVATE
    src
    src/views
    src/core
    src/ui
)

# 链接 Qt 和 OpenSSL 库
target_link_libraries(Chat-Client PUBLIC
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    OpenSSL::Crypto
    OpenSSL::SSL
)

# 添加Widgets, Network 模块的头文件路径
target_include_directories(Chat-Client SYSTEM PUBLIC
    ${Qt${QT_VERSION_MAJOR}Widgets_INCLUDE_DIRS}
    ${Qt${QT_VERSION_MAJOR}Network_INCLUDE_DIRS}
    ${OpenSSL_INCLUDE_DIR}
)
