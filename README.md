# XYChat Project

XYChat 是一个基于 Qt 6 / C++20 的即时通讯原型项目，当前包含桌面客户端 `Chat-Client` 与 TCP 服务端 `Chat-Server`。已具备账户体系、TLS 传输安全、一对一聊天端到端加密（M6）、群聊与群聊端到端加密（M7a/M7b，Sender Keys 方案，服务端仅存储密文）、本地加密缓存与增量同步等能力。本仓库现阶段的目标是提供稳定的工程基线，后续按 `docs/ROADMAP.md` 逐步演进协议、认证、消息与安全能力。

## 环境要求

| 组件 | 版本/要求 |
| --- | --- |
| CMake | 3.21 或更高版本 |
| C++ 编译器 | 支持 C++20；Windows 推荐 MSVC 2022，Linux/macOS 可使用 GCC/Clang |
| Qt | Qt 6.8.3 |
| OpenSSL | OpenSSL 3.x |
| 目标平台 | 当前以 Windows + MSVC 2022 为主要开发平台；CMake 工程保留跨平台构建能力 |

> 仓库的 `3rdparty/` 目录包含 Windows 开发用的 OpenSSL 相关文件。其他平台建议通过系统包管理器安装 OpenSSL，并通过 `CMAKE_PREFIX_PATH` 指向安装目录。

## 目录说明

```text
.
├── Chat-Client/          # Qt QML 客户端：登录/主窗口、NetworkManager、KeyStorage、LocalStore（M6.5 本地加密缓存）
├── Chat-Server/          # Qt Core/Network/Sql 服务端：TCP 监听、请求处理、SQLite、storage/（M8 对象存储）
├── CommonModule/         # 客户端与服务端共用模块：协议编解码（含 M8 FileProtocol）、加密（PBKDF2/E2EE/群 E2EE/M8 FileCrypto）、安全工具
├── docs/                 # 架构、协议、安全与路线图文档
├── tests/                # 自动化测试
├── 3rdparty/             # Windows 第三方依赖文件
└── CMakeLists.txt        # 顶层 CMake 工程
```

## 构建

### Windows（MSVC + Qt 6.8.3）

推荐使用仓库自带的 `Build.ps1`：它用 vswhere 动态定位最新的、带 C++ 工具集的 Visual Studio，
调用官方 `Launch-VsDevShell.ps1` 载入 x64 工具链后再执行 CMake 预设，因此无需在
`CMakeUserPresets.json` 中硬编码 MSVC / Windows SDK 版本号（VS 升级后脚本仍可用）。

```powershell
./Build.ps1                             # 仅配置 Qt-Debug
./Build.ps1 -Build                      # 配置并构建 Qt-Debug
./Build.ps1 -Preset Qt-Release -Build   # 配置并构建 Qt-Release
. ./Build.ps1 -EnvOnly                  # 只把 VS 开发环境载入当前 shell，不跑 cmake
```

预设与输出目录（`CMakePresets.json` / `CMakeUserPresets.json`）：

| 预设 | 生成器 | 构建类型 | 输出目录 |
| --- | --- | --- | --- |
| `Qt-Debug` | Ninja | Debug | `out/build/debug` |
| `Qt-Release` | Ninja | Release | `out/build/release` |

> `CMakeUserPresets.json` 内的 `QTDIR` 为本机 Qt 路径，该文件不入库（见 `.gitignore`），
> 换机时按本机实际路径调整即可。

也可绕过脚本手动配置（需自行保证 MSVC 环境与 Qt 路径正确）：

```powershell
cmake -S . -B out/build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="D:/Qt/6.8.3/msvc2022_64"
cmake --build out/build/release
```

### Linux/macOS（Qt 已安装在自定义路径时）

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/gcc_64
cmake --build build -j
```

如需跳过测试目标，可在配置时传入 `-DXYCHAT_BUILD_TESTS=OFF`。

> 持续集成：`.github/workflows/cmake.yml` 在 windows-latest 上执行 configure / build / ctest
> 三步（Qt 由 `jurplel/install-qt-action` 安装，OpenSSL/QWindowKit/zlib 已随仓库提交在 `3rdparty/`）。

## 运行

先启动服务端，再启动客户端（Windows 预设输出路径）：

```powershell
./out/build/release/Chat-Server.exe
./out/build/release/Chat-Client.exe
```

Linux/macOS 按上面手动配置的 `-B build` 目录运行 `./build/Chat-Server` 与 `./build/Chat-Client`。

> 客户端自 M6.5 起会在系统 AppData 目录下维护按账号+设备隔离的本地加密缓存（消息/会话以 AES-256-GCM 加密落库，未发送消息跨重启保留）；登出时自动清除。详见 `docs/SECURITY.md` 的本地存储安全章节。

> 服务端自 M8.1 起维护对象存储根目录 `<GenericDataLocation>/XYChat-Server/data/files`（存放客户端加密后的文件密文分片与组装后的对象，可在 `start()` 前调 `Server::setStorageRoot` 改路径）。初始化失败时不阻断启动，但文件相关接口一律 fail-closed 返回 `FileStorageFailed`，启动日志会输出 `Object storage ready at ...` 或失败告警。

## 测试

配置并构建后运行全部单元测试（CTest 纳入 9 套）：

```powershell
# Windows 预设（Build.ps1 / Qt-Debug）
ctest --test-dir out/build/debug --output-on-failure
```

```bash
# Linux/macOS 或手动 -B build 配置时
ctest --test-dir build --output-on-failure
```

> 排查单个套件时建议直接跑测试可执行文件并用 `-o <file>,txt` 落盘：
> Qt Test 的输出经 ctest 转发后在部分终端下会丢失，容易把断言失败误判为“无输出/崩溃”。
> 例：`./out/build/debug/TestLocalStore.exe -o out/ls.txt,txt`

| 套件 | 覆盖范围 |
| --- | --- |
| `TestPacketCodec` | 帧协议编解码 |
| `TestEncryptionManager` | PBKDF2 / Token 生成 |
| `TestDatabaseManager` | 服务端数据层（含群组、V1-V10 迁移、会话偏好与消息编辑/删除、M8 文件元数据/票据/访问控制/回收） |
| `TestSecurity` | TLS 辅助 / 日志脱敏 / NonceCache 重放保护 / RateWindow 限流 / StructuredLogger |
| `TestLocalStore` | 客户端本地加密缓存、持久化 outbox、Sender Key 与跳序消息密钥缓存 |
| `TestGroupE2eeCrypto` | 群 Sender-Key 加密原语（M7b）、DoS 上限、乱序解密与跳序密钥缓存 |
| `TestNetworkManager` | 客户端链路层（编辑/删除响应多槽匹配、88/89 推送发起设备去重、私聊编辑队列化、断线清理） |
| `TestFileProtocol` | M8 清单编解码与 fail-closed、分片数学、边界与非法入参 |
| `TestObjectStorage` | M8 对象存储（分片读写/组装校验/断点续传/幂等删除/崩溃残留清理/路径安全） |

另有 `tests/e2e/TestGroupRepro`：双客户端群 E2EE 端到端复现工具，**不纳入 CTest**，需先启动 `Chat-Server` 后手动运行：

```powershell
./out/build/debug/TestGroupRepro.exe
```

## 开发约定

- C++ 标准统一为 C++20。
- 文件编码统一为 UTF-8，换行符统一为 CRLF。
- CMake 目标按客户端、服务端、公共模块、测试分层组织。
- 新协议或安全行为变更需要同步更新 `docs/PROTOCOL.md`、`docs/SECURITY.md` 与 `docs/ROADMAP.md`。
- 不要提交构建目录、SQLite 数据库、临时日志、IDE 用户文件或本地 CMake preset。

### C++ 代码风格（适用于所有源文件）

- **注释格式**：禁止在注释中使用长横线分隔线（如 `──────────────`）或长破折号（如 `——`）作为段落/区块分隔符；区分代码区块使用简洁的短注释标题（例如 `// 文本加解密`，而非 `// ── 文本加解密 ─────────`）。
- **字符串字面量**：禁止使用 `QStringLiteral(...)` 宏包装常量字符串，统一使用双引号字面量（`const char*` 或依赖隐式转换）；需要 `QString` 类型的场景（如三元表达式、字面量成员调用）直接使用 `QString("...")`。
