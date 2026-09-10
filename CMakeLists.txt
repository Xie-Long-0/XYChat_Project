cmake_minimum_required(VERSION 3.21)

project(XYChat-Project VERSION 0.1.1 LANGUAGES CXX)

option(XYCHAT_BUILD_TESTS "Build XYChat unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${PROJECT_SOURCE_DIR}/3rdparty)

# 3rdparty 内置 OpenSSL：按构建配置在 Release / Debug 版本之间切换
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(XYChatOpenSSL)
xychat_find_openssl()

add_library(XYChatCommon STATIC
    CommonModule/encryption/EncryptionManager.cpp
    CommonModule/encryption/EncryptionManager.h
    CommonModule/encryption/E2eeCrypto.cpp
    CommonModule/encryption/E2eeCrypto.h
    CommonModule/encryption/GroupE2eeCrypto.cpp
    CommonModule/encryption/GroupE2eeCrypto.h
    CommonModule/encryption/FileCrypto.cpp
    CommonModule/encryption/FileCrypto.h
    CommonModule/protocol/Packet.h
    CommonModule/protocol/PacketCodec.cpp
    CommonModule/protocol/PacketCodec.h
    CommonModule/protocol/FileProtocol.cpp
    CommonModule/protocol/FileProtocol.h
    CommonModule/security/TlsHelper.cpp
    CommonModule/security/TlsHelper.h
    CommonModule/security/SecureMemory.cpp
    CommonModule/security/SecureMemory.h
    CommonModule/security/LogSanitizer.cpp
    CommonModule/security/LogSanitizer.h
    CommonModule/security/StructuredLogger.cpp
    CommonModule/security/StructuredLogger.h
)

target_include_directories(XYChatCommon PUBLIC
    ${PROJECT_SOURCE_DIR}/CommonModule
    ${PROJECT_SOURCE_DIR}/CommonModule/encryption
    ${PROJECT_SOURCE_DIR}/CommonModule/protocol
    ${PROJECT_SOURCE_DIR}/CommonModule/security
)

find_package(Qt6 COMPONENTS Core Network REQUIRED)
# OpenSSL 由 XYChatOpenSSL 模块查找（已按配置绑定 Release/Debug 导入库）

qt_standard_project_setup(REQUIRES 6.8)

target_link_libraries(XYChatCommon PUBLIC
    Qt6::Core
    Qt6::Network
    OpenSSL::SSL
    OpenSSL::Crypto
)

if (MSVC)
    target_compile_options(XYChatCommon PRIVATE /W3 /utf-8)
else()
    target_compile_options(XYChatCommon PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_subdirectory(Chat-Client)
add_subdirectory(Chat-Server)

if (XYCHAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
