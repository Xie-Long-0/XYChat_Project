cmake_minimum_required(VERSION 3.21)

project(XYChat-Project)

set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${PROJECT_SOURCE_DIR}/3rdparty)

# 子项目
add_subdirectory(Chat-Client)
add_subdirectory(Chat-Server)
