find_package(Qt6 COMPONENTS Test Sql Network HttpServer Gui REQUIRED)
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
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/FileTransferManager.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/FileTransferManager.h
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/ThumbnailMaker.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/ThumbnailMaker.h
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
    Qt6::Gui
    XYChatCommon
)

# NetworkManager/KeyStorage 依赖 DPAPI 保护本地密钥
if (WIN32)
    target_link_libraries(TestNetworkManager PRIVATE crypt32)
endif()

add_test(NAME TestNetworkManager COMMAND TestNetworkManager)

xychat_copy_runtime_dlls(TestNetworkManager)

# ── TestFileProtocol (M8 文件清单/分片数学/文件加密原语) ──
add_executable(TestFileProtocol
    unit/TestFileProtocol.cpp
)

target_link_libraries(TestFileProtocol PRIVATE
    Qt6::Test
    XYChatCommon
)

add_test(NAME TestFileProtocol COMMAND TestFileProtocol)

xychat_copy_runtime_dlls(TestFileProtocol)

# ── TestObjectStorage (M8 本地对象存储实现) ──
add_executable(TestObjectStorage
    unit/TestObjectStorage.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage/LocalFileStorage.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage/LocalFileStorage.h
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage/IObjectStorage.h
)

target_include_directories(TestObjectStorage PRIVATE
    ${CMAKE_SOURCE_DIR}/Chat-Server
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage
)

target_link_libraries(TestObjectStorage PRIVATE
    Qt6::Test
    XYChatCommon
)

add_test(NAME TestObjectStorage COMMAND TestObjectStorage)

xychat_copy_runtime_dlls(TestObjectStorage)

# ── TestFileHttpService (M8.2 数据面 HTTP(S) 集成测试) ──
# 同时编译数据面、存储层与数据层：本用例走真实 HTTP 回环，验证票据授权、
# 分片长度/序号校验、Range 语义与“上传→组装→下载”的字节级往返
add_executable(TestFileHttpService
    unit/TestFileHttpService.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/http/FileHttpService.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/http/FileHttpService.h
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage/LocalFileStorage.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage/LocalFileStorage.h
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage/IObjectStorage.h
    ${CMAKE_SOURCE_DIR}/Chat-Server/database/DatabaseManager.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/database/DatabaseManager.h
)

target_include_directories(TestFileHttpService PRIVATE
    ${CMAKE_SOURCE_DIR}/Chat-Server
    ${CMAKE_SOURCE_DIR}/Chat-Server/http
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage
    ${CMAKE_SOURCE_DIR}/Chat-Server/database
    ${CMAKE_SOURCE_DIR}/Chat-Server/core
)

target_link_libraries(TestFileHttpService PRIVATE
    Qt6::Test
    Qt6::Network
    Qt6::Sql
    Qt6::HttpServer
    XYChatCommon
)

add_test(NAME TestFileHttpService COMMAND TestFileHttpService)

xychat_copy_runtime_dlls(TestFileHttpService)

# ── TestFileTransfer (M8.2 客户端传输引擎端到端) ──
# 起真实 HTTP 数据面 + 真实对象存储，控制面由测试内迷你实现顶替，
# 验证"明文→分片加密→上传→组装→下载→逐片解密→保存"字节级一致
# 且服务端全程只见密文
add_executable(TestFileTransfer
    unit/TestFileTransfer.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/FileTransferManager.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/FileTransferManager.h
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/ThumbnailMaker.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/ThumbnailMaker.h
    ${CMAKE_SOURCE_DIR}/Chat-Server/http/FileHttpService.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/http/FileHttpService.h
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage/LocalFileStorage.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage/LocalFileStorage.h
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage/IObjectStorage.h
    ${CMAKE_SOURCE_DIR}/Chat-Server/database/DatabaseManager.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Server/database/DatabaseManager.h
)

target_include_directories(TestFileTransfer PRIVATE
    ${CMAKE_SOURCE_DIR}/Chat-Client
    ${CMAKE_SOURCE_DIR}/Chat-Client/core
    ${CMAKE_SOURCE_DIR}/Chat-Server
    ${CMAKE_SOURCE_DIR}/Chat-Server/http
    ${CMAKE_SOURCE_DIR}/Chat-Server/storage
    ${CMAKE_SOURCE_DIR}/Chat-Server/database
    ${CMAKE_SOURCE_DIR}/Chat-Server/core
)

target_link_libraries(TestFileTransfer PRIVATE
    Qt6::Test
    Qt6::Network
    Qt6::Sql
    Qt6::HttpServer
    Qt6::Gui
    XYChatCommon
)

add_test(NAME TestFileTransfer COMMAND TestFileTransfer)

xychat_copy_runtime_dlls(TestFileTransfer)

# ── TestThumbnailMaker (M8.3 图片元数据与内联缩略图) ──
# 纯 QtGui 图像编解码，不依赖平台多媒体后端，可在无头环境与 CI 中运行
add_executable(TestThumbnailMaker
    unit/TestThumbnailMaker.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/ThumbnailMaker.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/ThumbnailMaker.h
)

target_include_directories(TestThumbnailMaker PRIVATE
    ${CMAKE_SOURCE_DIR}/Chat-Client
    ${CMAKE_SOURCE_DIR}/Chat-Client/core
)

target_link_libraries(TestThumbnailMaker PRIVATE
    Qt6::Test
    Qt6::Gui
    XYChatCommon
)

add_test(NAME TestThumbnailMaker COMMAND TestThumbnailMaker)

xychat_copy_runtime_dlls(TestThumbnailMaker)

# ── M7aGroupRepro（临时诊断工具：群聊端到端复现，不纳入 CTest） ──
add_executable(TestGroupRepro
    "e2e/TestGroupRepro.cpp"
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/NetworkManager.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/FileTransferManager.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/FileTransferManager.h
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/ThumbnailMaker.cpp
    ${CMAKE_SOURCE_DIR}/Chat-Client/core/ThumbnailMaker.h
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
    Qt6::Gui
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
