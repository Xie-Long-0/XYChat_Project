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
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Qt6 COMPONENTS Core Network Sql REQUIRED)
# OpenSSL 由 XYChatOpenSSL 模块查找（已按配置绑定 Release/Debug 导入库）
include(XYChatOpenSSL)
xychat_find_openssl()

qt_standard_project_setup(REQUIRES 6.8)

# 源文件列表
set(SOURCES
    main.cpp
    core/Server.cpp
    core/RequestHandler.cpp
    core/NonceCache.cpp
    database/DatabaseManager.cpp
    storage/LocalFileStorage.cpp
)

# 头文件列表
set(HEADERS
    core/Server.h
    core/RequestHandler.h
    core/NonceCache.h
    core/RateWindow.h
    database/DatabaseManager.h
    storage/IObjectStorage.h
    storage/LocalFileStorage.h
)

# 生成可执行文件
qt_add_executable(Chat-Server ${SOURCES} ${HEADERS})

# 包含目录
target_include_directories(Chat-Server PRIVATE
    core/
    database/
    storage/
)

# 链接 Qt 和 OpenSSL 库
target_link_libraries(Chat-Server PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    OpenSSL::SSL
    XYChatCommon
)

# 添加 Network/Sql 模块的头文件路径
target_include_directories(Chat-Server SYSTEM PUBLIC
    ${Qt6Network_INCLUDE_DIRS}
    ${Qt6Sql_INCLUDE_DIRS}
)

# 构建后将 OpenSSL 运行时 DLL 拷贝到输出目录
# Debug 取 3rdparty/debug/bin，其余配置取 3rdparty/bin
xychat_copy_runtime_dlls(Chat-Server)

# M5: 构建后拷贝证书生成脚本到输出目录
add_custom_command(TARGET Chat-Server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:Chat-Server>/certs
    COMMENT "Creating certs directory for Chat-Server"
)
