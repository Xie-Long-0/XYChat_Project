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
- QML 错误只在运行时暴露，改完 QML 应按「静态 → 无头 → 起进程」三步验证：① 静态扫描 `qmllint.exe -I D:/Qt/6.8.3/msvc2022_64/qml <全部 .qml>`（能报 `Label is not a type`、`Cannot assign to non-existent property` 这类硬错误；`QWindowKit` 模块解析失败属正常误报——该模块由 CMake target 注册、无 qmldir）；② 无头加载验证用 `QT_QPA_PLATFORM=offscreen qml.exe <probe.qml>`，probe 需放在 `Chat-Client/resources/` 下才能解析相对 `import`，**不要**写进 `resources.qrc`，验完即删；③ 最终以 `QT_QPA_PLATFORM=offscreen timeout 12 ./Chat-Client.exe` 输出中不出现任何 `.qml` 行与 `TypeError`/`ReferenceError` 为准（该 exe 为 console 子系统，stderr 可直接落盘）。④ 只在使用时才触发的运行期告警（如信号处理器隐式参数注入），"加载但不触发"的 probe 抓不到，需用 Qt Quick Test 打真实按键：`QT_QPA_PLATFORM=offscreen QT_QUICK_CONTROLS_STYLE=Basic qmltestrunner.exe -input <tst_*.qml>`（`tst_*.qml` 同样放 `Chat-Client/resources/` 以解析 `import "components"`，验完即删）；注意 `keyClicks` 不存在，`keySequence()` 只接受 `"Ctrl+A"` 这类键位串（传裸文本会 assert 在 `qasciikey.cpp`），逐字符输入用 `keyClick(Qt.Key_A + (c - 97))`。断言须先用 `git checkout HEAD -- <文件>` 回退到改动前跑一遍复现，确认用例真能抓到缺陷。
- 单测均纳入 CTest（当前 12 套）：`TestPacketCodec`/`TestEncryptionManager`/`TestDatabaseManager`/`TestSecurity`/`TestLocalStore`/`TestGroupE2eeCrypto`/`TestNetworkManager`/`TestFileProtocol`/`TestObjectStorage`/`TestFileHttpService`/`TestFileTransfer`/`TestThumbnailMaker`。`TestFileHttpService` 与 `TestFileTransfer` 为 M8.2 集成测试（起真实 HTTP 回环 + 真实对象存储 + 内存 SQLite，端口用 0 交由 OS 分配以避免冲突）；`TestFileTransfer` 另含 M8.3b/c 的 `DecryptingIODevice`（播放器解密设备：顺序读取/seek 跨分片/未下载 open 失败）与 `toLocalPath`/`decryptedFileBytes` 用例；`TestThumbnailMaker` 只依赖 QtGui 图像编解码，无需平台多媒体后端（JPEG 编码器缺失时相关断言会 QSKIP）。Chat-Client 自 M8.3b 起链接 `Qt6::Multimedia`（音视频元数据提取与播放依赖平台解码后端，CI/无头环境提取失败留空、播放报错，均不崩溃）。`tests/e2e/TestGroupRepro` 为手动双客户端工具，不纳入 CTest。

## 代码风格约定

- 文件编码统一为UTF-8，换行符为CRLF。
- 字符串字面量用双引号（`"text"`）经隐式/显式 `QString` 转换，**禁用** `QStringLiteral`、`QString::fromUtf8` 包裹常量字面量（运行时拼接的 `QString("-%1 days").arg(...)` 等除外）。
- 注释**不加**分隔线/装饰性横线（如 `// ──── ... ────`、`//====`），不用悬挂式空 TODO 框。
- 遵循周边代码的命名与惯例；新增/修改密码学路径必须设步数与参数上限（见 `docs/SECURITY.md` DoS 教训）。
- 以时间戳作"最新"排序依据时用单调递增序号（如 SQLite `rowid`），不用秒级时间戳 + 随机值破口。
- 事件/推送 payload 必须携带解密所需全部寻址字段（群密文依赖 群/发送者/设备/keyId 四元组）。
- 条件判定与状态迁移合并为**单条 SQL 语句**（如 `INSERT...SELECT` 带配额子查询、`UPDATE ... WHERE NOT EXISTS`）：先查后改的分步版本存在 TOCTOU，并发请求会集体读到“未满足”而全部放行。
- 销毁性操作（删磁盘/删行）遵循“先保证不产生孤儿数据、再销毁”；无法两者兼得时，宁可留下可被下一轮回收的隐形孤儿，也不得产出“元数据指向已消失的数据”这类不可自愈状态。
- 错误分类必须区分“数据故障”（重传同批输入只会得到同样结果，标失败并回收）与“存储/瞬时故障”（保留现场让调用方重试）；把后者归为前者会造成客户端无限重传。
- 携密钥的结构（如文件清单）必须有**唯一脱敏出口**，且每一条通向 UI/JS 的路径都经它：只在一条路径（如实时推送）上脱敏等于没做——离线补收、历史翻页、本地缓存回填同样会把密钥渲染上屏，而字符串一旦进入 JS 堆就无法可靠清零。出口处再加一道“形态像就置空”兜底，使新增出口不会重蹈覆辙。
- 把引用传给会销毁容器的函数时先取副本：`erase` 之后再 `emit` 一个指向已销毁节点的 `QString&` 是 use-after-free。同理，遍历容器时若循环体可能删除元素，必须先取键快照；会递归推进自己的调度器必须有重入护栏。
- 重试预算不得被“恢复动作的成功”清零：否则当数据面持续故障而控制面正常时（每次查询都成功）会形成活锁；恢复轮次需单独封顶。
- 异步回调里不得捕获容器元素的引用（用键重新查表）；`abort()` 会同步触发 `finished`，清理在途请求前先断开回调并立护栏，否则会在登出/重置途中发新请求。
- QML 的全局 `Qt` 对象**没有** `urlToLocalFile`（那是 C++ `QUrl` 的方法），运行时调用会抛 `TypeError: Property 'urlToLocalFile' of object Qt(...) is not a function`。把 `file://` URL 转本地路径只有两条可靠出路：① 直接把 `QUrl`（signal 参数用 `var`）传给 C++，由 C++ 侧 `QUrl::toLocalFile()` 转换（本仓统一走 `fileTransfer.toLocalPath(urlOrPath)`）；② 在 C++ 侧完成整段路径处理，QML 不参与。**禁止**在 QML 里用正则剔 `file://` 前缀——对 UNC（`file://server/share/x`）与含 `%`/`#`/`?` 的路径会给出错误结果，保存时甚至会静默写到带 percent 转义的乱码文件名里。引用 context property 前用 `typeof x !== "undefined"` 防御。
- Qt 6 QML 必须用 Qt 6 的 API，写成 Qt 5 的写法会在运行时以“编译期”错误暴露：① 控件（`Label`/`Button`/`Dialog`/`TextField` 等）来自 `QtQuick.Controls`，只 `import QtQuick` 会报 `Label is not a type`；② 图标着色用 `QtQuick.Effects` 的 `MultiEffect`（`colorizationColor: <颜色>` + `colorization: 1.0`），Qt 5 的 `ColorOverlay.colorizationStrength` 在 Qt 6 不存在，且 `colorization` 是 0~1 的 real 而**不是**颜色；③ 内边距命名为 `padding`/`topPadding`/`bottomPadding`/`leftPadding`/`rightPadding`，**没有** `paddingBottom`；④ 信号处理器**禁止**依赖隐式参数注入——需要事件对象必须写带形参的箭头函数：`Keys.onReturnPressed: (event) => { ... }`，写成 `Keys.onReturnPressed: { ... event ... }` 会报 `Parameter "event" is not declared. Injection of parameters into signal handlers is deprecated`（同为 `qt.qml.context` 类别，运行期输出，`qmllint` 归类为 `[unqualified]`）。
- 元数据提取（缩略图/尺寸/时长）失败一律留空字段，**绝不阻断主流程**（文件仍应能正常上传与发送）；内联到消息正文的元数据受正文长度上限硬约束，压不进上限就不内联，绝不放宽上限。

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
