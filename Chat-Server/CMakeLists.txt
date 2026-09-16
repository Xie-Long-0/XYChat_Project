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

# M8.2: HttpServer 承载文件传输数据面（QHttpServer 自 Qt 6.4 起属 qtbase，
# 随基础 Qt 安装提供，无需额外模块）
find_package(Qt6 COMPONENTS Core Network Sql HttpServer REQUIRED)
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
    http/FileHttpService.cpp
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
    http/FileHttpService.h
)

# 生成可执行文件
qt_add_executable(Chat-Server ${SOURCES} ${HEADERS})

# 包含目录
target_include_directories(Chat-Server PRIVATE
    core/
    database/
    storage/
    http/
)

# 链接 Qt 和 OpenSSL 库
target_link_libraries(Chat-Server PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::HttpServer
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

# M5: 构建后确保 certs 目录存在 —— 服务端首次启动时会在该目录自动生成开发证书
# （TlsHelper::generateDevCertificates），客户端也优先从 exe 同级的 certs/ 读取 CA
add_custom_command(TARGET Chat-Server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:Chat-Server>/certs
    COMMENT "Creating certs directory for Chat-Server"
)
