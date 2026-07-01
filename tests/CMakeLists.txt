find_package(Qt6 COMPONENTS Test REQUIRED)

set(CMAKE_AUTOMOC ON)

add_executable(TestEncryptionManager
    unit/TestEncryptionManager.cpp
)

target_link_libraries(TestEncryptionManager PRIVATE
    Qt6::Test
    XYChatCommon
)

add_test(NAME TestEncryptionManager COMMAND TestEncryptionManager)

add_executable(TestPacketCodec
    unit/TestPacketCodec.cpp
)

target_link_libraries(TestPacketCodec PRIVATE
    Qt6::Test
    XYChatCommon
)

add_test(NAME TestPacketCodec COMMAND TestPacketCodec)
