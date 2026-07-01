cmake_minimum_required(VERSION 3.21)

project(Chat-Server VERSION 1.0 LANGUAGES CXX)

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

find_package(Qt6 COMPONENTS Core Network Sql REQUIRED)
find_package(OpenSSL REQUIRED)

# 源文件列表
set(SOURCES
    main.cpp
    core/Server.cpp
    core/RequestHandler.cpp
    database/DatabaseManager.cpp
    ../CommonModule/encryption/EncryptionManager.cpp
)

# 头文件列表
set(HEADERS
    core/Server.h
    core/RequestHandler.h
    database/DatabaseManager.h
    ../CommonModule/encryption/EncryptionManager.h
)

# 生成可执行文件
add_executable(Chat-Server ${SOURCES} ${HEADERS})

# 包含目录
target_include_directories(Chat-Server PRIVATE
    core/
    database/
    ../CommonModule/encryption/
)

# 链接 Qt 和 OpenSSL 库
target_link_libraries(Chat-Server PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    OpenSSL::SSL
    OpenSSL::Crypto
)

# 添加Widgets, Network 模块的头文件路径
target_include_directories(Chat-Server SYSTEM PUBLIC
    ${Qt6Widgets_INCLUDE_DIRS}
    ${Qt6Network_INCLUDE_DIRS}
    ${Qt6Sql_INCLUDE_DIRS}
    ${OpenSSL_INCLUDE_DIR}
)
