find_package(Qt6 COMPONENTS Test Sql Network REQUIRED)
# OpenSSL 由 XYChatOpenSSL 模块查找（已按配置绑定 Release/Debug 导入库）
include(XYChatOpenSSL)
xychat_find_openssl()

set(CMAKE_AUTOMOC ON)

# ── TestEncryptionManager ──
add_executable(TestEncryptionManager
    unit/TestEncryptionManager.cpp
)

target_link_libraries(TestEncryptionManager PRIVATE
    Qt6::Test
    XYChatCommon
)

add_test(NAME TestEncryptionManager COMMAND TestEncryptionManager)

xychat_copy_runtime_dlls(TestEncryptionManager)

# ── TestPacketCodec ──
add_executable(TestPacketCodec
    unit/TestPacketCodec.cpp
)

target_link_libraries(TestPacketCodec PRIVATE
    Qt6::Test
    XYChatCommon
)

add_test(NAME TestPacketCodec COMMAND TestPacketCodec)

# ── TestDatabaseManager ──
add_executable(TestDatabaseManager
    unit/TestDatabaseManager.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/database/DatabaseManager.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/database/DatabaseManager.h
)

target_include_directories(TestDatabaseManager PRIVATE
    ${CMAKE_SOURCE_DIR}/Chat-Server
    ${CMAKE_SOURCE_DIR}/Chat-Server/database
)

target_link_libraries(TestDatabaseManager PRIVATE
    Qt6::Test
    Qt6::Sql
    XYChatCommon
)

add_test(NAME TestDatabaseManager COMMAND TestDatabaseManager)

xychat_copy_runtime_dlls(TestDatabaseManager)

# ── TestSecurity (M5) ──
add_executable(TestSecurity
    unit/TestSecurity.cpp
    # M5.5: NonceCache 重放保护组件（服务端 core）
    ${CMAKE_SOURCE_DIR}/Chat-Server/core/NonceCache.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/core/NonceCache.h
)

target_include_directories(TestSecurity PRIVATE
    ${CMAKE_SOURCE_DIR}/Chat-Server/core
)

target_link_libraries(TestSecurity PRIVATE
    Qt6::Test
    Qt6::Network
    XYChatCommon
)

add_test(NAME TestSecurity COMMAND TestSecurity)

xychat_copy_runtime_dlls(TestSecurity)

# ── TestLocalStore (M6.5 客户端本地加密持久化缓存) ──
add_executable(TestLocalStore
    unit/TestLocalStore.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/LocalStore.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/LocalStore.h
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/KeyStorage.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/KeyStorage.h
)

target_include_directories(TestLocalStore PRIVATE
    ${CMAKE_SOURCE_DIR}/Chat-Client/core
)

target_link_libraries(TestLocalStore PRIVATE
    Qt6::Test
    Qt6::Sql
    XYChatCommon
)

# KeyStorage 使用 DPAPI 保护本地密钥与存储密钥
if (WIN32)
    target_link_libraries(TestLocalStore PRIVATE crypt32)
endif()

add_test(NAME TestLocalStore COMMAND TestLocalStore)

xychat_copy_runtime_dlls(TestLocalStore)

# ── TestGroupE2eeCrypto (M7b 群聊 Sender-Key E2EE 原语) ──
add_executable(TestGroupE2eeCrypto
    unit/TestGroupE2eeCrypto.cpp
)

target_link_libraries(TestGroupE2eeCrypto PRIVATE
    Qt6::Test
    XYChatCommon
)

add_test(NAME TestGroupE2eeCrypto COMMAND TestGroupE2eeCrypto)

xychat_copy_runtime_dlls(TestGroupE2eeCrypto)

# ── TestNetworkManager (M9 欠账修复：客户端链路层编辑/删除回归单测) ──
add_executable(TestNetworkManager
    unit/TestNetworkManager.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/NetworkManager.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/NetworkManager.h
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/LocalStore.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/LocalStore.h
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/KeyStorage.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/KeyStorage.h
)

target_include_directories(TestNetworkManager PRIVATE
    ${CMAKE_SOURCE_DIR}/Chat-Client/core
)

target_link_libraries(TestNetworkManager PRIVATE
    Qt6::Test
    Qt6::Sql
    Qt6::Network
    XYChatCommon
)

# NetworkManager/KeyStorage 依赖 DPAPI 保护本地密钥
if (WIN32)
    target_link_libraries(TestNetworkManager PRIVATE crypt32)
endif()

add_test(NAME TestNetworkManager COMMAND TestNetworkManager)

xychat_copy_runtime_dlls(TestNetworkManager)

# ── M7aGroupRepro（临时诊断工具：群聊端到端复现，不纳入 CTest） ──
add_executable(TestGroupRepro
    "e2e/TestGroupRepro.cpp"
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/NetworkManager.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/NetworkManager.h
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/LocalStore.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/LocalStore.h
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/KeyStorage.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/KeyStorage.h
)

target_include_directories(TestGroupRepro PRIVATE
    ${CMAKE_SOURCE_DIR}/Chat-Client/core
)

target_link_libraries(TestGroupRepro PRIVATE
    Qt6::Network
    Qt6::Sql
    XYChatCommon
)

if (WIN32)
    target_link_libraries(TestGroupRepro PRIVATE crypt32)
endif()

xychat_copy_runtime_dlls(TestGroupRepro)

add_custom_command(TARGET TestGroupRepro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        $<TARGET_FILE_DIR:Chat-Server>/certs
        $<TARGET_FILE_DIR:TestGroupRepro>/certs
)
