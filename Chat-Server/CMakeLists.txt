cmake_minimum_required(VERSION 3.18)

project(Chat-Server VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
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
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Core Network Sql REQUIRED)
find_package(OpenSSL REQUIRED)

# 源文件列表
set(SOURCES
    src/main.cpp
    src/server/Server.cpp
    src/server/RequestHandler.cpp
    src/database/DatabaseManager.cpp
    src/encryption/EncryptionManager.cpp
)

# 头文件列表
set(HEADERS
    src/server/Server.h
    src/server/RequestHandler.h
    src/database/DatabaseManager.h
    src/encryption/EncryptionManager.h
)

# 生成可执行文件
add_executable(Chat-Server ${SOURCES} ${HEADERS})

# 包含目录
target_include_directories(Chat-Server PRIVATE
    src
    src/server
    src/database
    src/encryption
)

# 链接 Qt 和 OpenSSL 库
target_link_libraries(Chat-Server PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Sql
    OpenSSL::SSL
    OpenSSL::Crypto
)

# 添加Widgets, Network 模块的头文件路径
target_include_directories(Chat-Server SYSTEM PUBLIC
    ${Qt${QT_VERSION_MAJOR}Widgets_INCLUDE_DIRS}
    ${Qt${QT_VERSION_MAJOR}Network_INCLUDE_DIRS}
    ${Qt${QT_VERSION_MAJOR}Sql_INCLUDE_DIRS}
    ${OpenSSL_INCLUDE_DIR}
)
