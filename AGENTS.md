# AGENTS.md

面向 AI 编码代理与新加入开发者的工程入口。本文件固化构建/测试/验证命令、代码风格约定与里程碑完成定义，避免每次从 README 与脚本自行推导。历史与决策细节见 `docs/` 四文档与 `git log`。

## 项目概览

XYChat 是"类 Telegram"安全即时通信系统，Qt 6.8.3 / C++20 / CMake + Ninja + MSVC。模块化单体：`Chat-Client`（QML/Qt Quick + QWindowKit）、`Chat-Server`（每连接一线程 + SQLite）、`CommonModule`（协议/加密/安全共享层）。详见 `docs/ARCHITECTURE.md`。

## 构建与测试（Windows / PowerShell 7）

MSVC 与 Windows SDK 由 `Build.ps1` 经 vswhere + `Launch-VsDevShell.ps1` 动态定位，**不要**在 `CMakeUserPresets.json` 硬编码版本号。裸开 `cmake --build` 前必须先载入 VS 环境，否则报 `fatal error C1083: 无法打开包含文件 type_traits`。

```powershell
# 载入工具链环境后增量构建 Debug（输出目录 out/build/debug）
. ./Build.ps1 -EnvOnly
cmake --build out/build/debug

# 或一步：配置 + 构建（Qt-Debug 预设）
./Build.ps1 -Preset Qt-Debug -Build

# 全量单元测试（CTest）
ctest --test-dir out/build/debug --output-on-failure

# Release 构建
./Build.ps1 -Preset Qt-Release -Build
```

工具链事实来源：Qt `6.8.3 msvc2022_64`（`D:\Qt\6.8.3\msvc2022_64`），MSVC `14.51.36231`，Visual Studio 18 Enterprise。运行时需 Qt 与 QWindowKit/OpenSSL/zlib DLL 在 PATH 或可执行文件同级（CMake 已配置拷贝）。

## 测试诊断技巧（重要）

- Windows 下 Qt Test 的 stdout **全缓冲**：测试失败/崩溃时 `ctest --output-on-failure` 与 `LastTest.log` 常显示空输出，易把断言失败误判为崩溃或"沙箱偶发"。必须用以下任一方式取真实断言：
  - 设置 `$env:QT_FORCE_STDERR_LOGGING = "1"`（QTest/qDebug 转 stderr，不被丢弃）。
  - 直接运行用例可执行文件并重定向落盘：`TestXxx.exe -o result.txt,txt`。
- 曾因此把 `latestSenderKeyId` 的同秒 tie-break（约 50% 概率失败）误归因为"沙箱 DPAPI 偶发"。审查类任务应实跑测试并落盘输出，而非止步静态阅读。
- 单测均纳入 CTest：`TestPacketCodec`/`TestEncryptionManager`/`TestDatabaseManager`/`TestSecurity`/`TestLocalStore`/`TestGroupE2eeCrypto`。`tests/e2e/TestGroupRepro` 为手动双客户端工具，不纳入 CTest。

## 代码风格约定

- 字符串字面量用双引号（`"text"`）经隐式/显式 `QString` 转换，**禁用** `QStringLiteral`、`QString::fromUtf8` 包裹常量字面量（运行时拼接的 `QString("-%1 days").arg(...)` 等除外）。
- 注释**不加**分隔线/装饰性横线（如 `// ──── ... ────`、`//====`），不用悬挂式空 TODO 框。
- 遵循周边代码的命名与惯例；新增/修改密码学路径必须设步数与参数上限（见 `docs/SECURITY.md` DoS 教训）。
- 以时间戳作"最新"排序依据时用单调递增序号（如 SQLite `rowid`），不用秒级时间戳 + 随机值破口。
- 事件/推送 payload 必须携带解密所需全部寻址字段（群密文依赖 群/发送者/设备/keyId 四元组）。

## 里程碑完成定义（§9）

每个功能迭代必须同时交付，缺一不算完成：

1. 协议文档更新（`docs/PROTOCOL.md`）。
2. 数据库迁移脚本或 schema 变更说明。
3. 服务端处理逻辑。
4. 客户端调用与 UI。
5. 单元测试或集成测试。
6. 错误码和日志（结构化审计 + 安全事件）。
7. 安全影响说明（`docs/SECURITY.md`）。
8. 路线图状态同步（`docs/ROADMAP.md`：状态总表 + 能力摘要 + 欠账清单 + 变更记录）。

编码后应经代码审查（CodeReview 子代理）、修复审查问题、跑全量 `ctest` 后方可标记里程碑完成。**未经用户明确要求不得提交或推送。**

## 文档一致性

涉及协议/安全/架构事实的表述必须与代码一致；发现文档与代码不符时，**以代码为准**并当期修正文档。`docs/ROADMAP.md` §3 是集中管理的欠账清单，销账或新增时更新该表。
