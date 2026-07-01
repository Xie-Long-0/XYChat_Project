cmake_minimum_required(VERSION 3.21)

project(XYChat-Project VERSION 0.1.0 LANGUAGES CXX)

option(XYCHAT_BUILD_TESTS "Build XYChat unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${PROJECT_SOURCE_DIR}/3rdparty)

add_library(XYChatCommon STATIC
    CommonModule/encryption/EncryptionManager.cpp
    CommonModule/encryption/EncryptionManager.h
    CommonModule/protocol/Packet.h
    CommonModule/protocol/PacketCodec.cpp
    CommonModule/protocol/PacketCodec.h
)

target_include_directories(XYChatCommon PUBLIC
    ${PROJECT_SOURCE_DIR}/CommonModule
    ${PROJECT_SOURCE_DIR}/CommonModule/encryption
)

find_package(Qt6 COMPONENTS Core REQUIRED)
find_package(OpenSSL REQUIRED)

target_link_libraries(XYChatCommon PUBLIC
    Qt6::Core
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
