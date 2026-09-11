cmake_minimum_required(VERSION 3.21)

project(Chat-Client VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 向编译器添加编译选项
if (MSVC)
    add_compile_options(
        /W3 /utf-8
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Release>:/O2>
    )
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# QML 模块：Qml + Quick；M6.5 本地持久化缓存需要 Sql（QSQLITE）
find_package(Qt6 COMPONENTS Quick QuickControls2 Network Sql REQUIRED)
# OpenSSL 由 XYChatOpenSSL 模块查找（已按配置绑定 Release/Debug 导入库）
include(XYChatOpenSSL)
xychat_find_openssl()

# QWindowKit
find_package(QWindowKit COMPONENTS Core Quick REQUIRED)

qt_standard_project_setup(REQUIRES 6.8)

# 资源文件
set(RESOURCES
    resources/resources.qrc
)

# C++ 源文件
set(SOURCES
    main.cpp
    core/NetworkManager.cpp
    core/KeyStorage.cpp
    core/LocalStore.cpp
    core/FileTransferManager.cpp
    core/ThumbnailMaker.cpp
)

# C++ 头文件
set(HEADERS
    core/NetworkManager.h
    core/ThemeSettings.h
    core/KeyStorage.h
    core/LocalStore.h
    core/FileTransferManager.h
    core/ThumbnailMaker.h
)

# 生成可执行文件
qt_add_executable(Chat-Client ${SOURCES} ${HEADERS} ${RESOURCES})

# 包含目录
target_include_directories(Chat-Client PRIVATE
    core/
)

# 链接 Qt、OpenSSL、QWindowKit 和公共库
target_link_libraries(Chat-Client PUBLIC
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Sql
    OpenSSL::SSL
    QWindowKit::Core
    QWindowKit::Quick
    XYChatCommon
)

# M6: Windows 下 KeyStorage 使用 DPAPI 保护本地密钥
if (WIN32)
    target_link_libraries(Chat-Client PRIVATE crypt32)
endif()

# 添加 Network/Quick 模块的头文件路径
target_include_directories(Chat-Client SYSTEM PUBLIC
    ${Qt6Network_INCLUDE_DIRS}
    ${Qt6Quick_INCLUDE_DIRS}
)

# 构建后将运行时 DLL 拷贝到输出目录
# $<TARGET_FILE:...> 会自动根据当前构建配置选择对应的 DLL（Debug 为 QWKCored/QWKQuickd，Release 为 QWKCore/QWKQuick）
add_custom_command(TARGET Chat-Client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:QWindowKit::Core>
        $<TARGET_FILE:QWindowKit::Quick>
        $<TARGET_FILE_DIR:Chat-Client>
    COMMENT "Copying QWindowKit runtime DLLs to output"
)

# OpenSSL/zlib 运行时 DLL：Debug 取 3rdparty/debug/bin，其余配置取 3rdparty/bin
xychat_copy_runtime_dlls(Chat-Client)
