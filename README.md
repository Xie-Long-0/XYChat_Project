# XYChat Project

XYChat 是一个基于 Qt 6 / C++20 的即时通讯原型项目，当前包含桌面客户端 `Chat-Client` 与 TCP 服务端 `Chat-Server`。本仓库现阶段的目标是提供稳定的工程基线，后续按 `docs/ROADMAP.md` 逐步演进协议、认证、消息与安全能力。

## 环境要求

| 组件 | 版本/要求 |
| --- | --- |
| CMake | 3.21 或更高版本 |
| C++ 编译器 | 支持 C++20；Windows 推荐 MSVC 2022，Linux/macOS 可使用 GCC/Clang |
| Qt | Qt 6.8.3（至少需要 Widgets、Network、Sql、Test 模块） |
| OpenSSL | OpenSSL 3.x |
| 目标平台 | 当前以 Windows + MSVC 2022 为主要开发平台；CMake 工程保留跨平台构建能力 |

> 仓库的 `3rdparty/` 目录包含 Windows 开发用的 OpenSSL 相关文件。其他平台建议通过系统包管理器安装 OpenSSL，并通过 `CMAKE_PREFIX_PATH` 指向 Qt 安装目录。

## 目录说明

```text
.
├── Chat-Client/          # Qt Widgets 客户端：登录窗口、主窗口、网络管理
├── Chat-Server/          # Qt Core/Network/Sql 服务端：TCP 监听、请求处理、SQLite 用户表
├── CommonModule/         # 客户端与服务端共用模块，目前包含密码摘要工具
├── docs/                 # 架构、协议、安全与路线图文档
├── tests/                # 自动化测试
├── 3rdparty/             # Windows 第三方依赖文件
└── CMakeLists.txt        # 顶层 CMake 工程
```

## 构建

### Windows（MSVC 2022 + Qt 6.8.3）

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build --config Release
```

### Linux/macOS（Qt 已安装在自定义路径时）

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/gcc_64
cmake --build build -j
```

如需跳过测试目标，可在配置时传入 `-DXYCHAT_BUILD_TESTS=OFF`。

## 运行

先启动服务端：

```bash
./build/Chat-Server/Chat-Server
```

再启动客户端：

```bash
./build/Chat-Client/Chat-Client
```

当前服务端会尝试使用相对路径 `resources/db/chatapp.db` 创建 SQLite 数据库，并初始化演示账号：

- 用户名：`admin`
- 密码：`passwd`

## 测试

配置并构建后运行：

```bash
ctest --test-dir build --output-on-failure
```

当前至少包含公共密码摘要模块的 Qt Test 单元测试，用作 M0 阶段的基础测试安全网。

## 开发约定

- C++ 标准统一为 C++20。
- 文件编码统一为 UTF-8，换行符统一为 CRLF。
- CMake 目标按客户端、服务端、公共模块、测试分层组织。
- 新协议或安全行为变更需要同步更新 `docs/PROTOCOL.md`、`docs/SECURITY.md` 与 `docs/ROADMAP.md`。
- 不要提交构建目录、SQLite 数据库、临时日志、IDE 用户文件或本地 CMake preset。
