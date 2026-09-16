# XYChat 长期实现路线图

本文档面向 Qt/C++ Client + Server 基础框架，目标是逐步演进为一个"类 Telegram"的安全即时通信系统。路线图按"先稳定基础，再做通信能力，再做安全与规模化"的顺序推进，便于长期迭代、验收和回滚。

> **文档形态（2026-09-15 精简）**：本文只保留**结论、取舍与约束**——能力矩阵、已完成里程碑摘要、集中管理的欠账清单、未来规划与变更记录。实施流水账（逐项勾选清单、审查过程的逐步修复记录、文档同步明细）一律不保留，历史细节经 `git log` 与 `docs/ARCHITECTURE.md` 追溯。同一事实只在一处展开：**现状**看 §1.2、**里程碑结论**看 §2 与 §4.1-§4.3、**欠账**看 §3，其余位置用指针引用。

## 0. 文档定位与维护约定

- **状态标注**：里程碑状态取 `已完成` / `未开始` / `进行中`；节内未实施项以"未实现"文字标注，不使用悬挂的空复选框。
- **更新方式**：里程碑完成时同步更新四处——① 状态总表；② 已完成能力摘要；③ 欠账清单（新增或销账）；④ 变更记录表。**禁止**再向文档头部追加流水账式更新引用块。
- **精简原则**：已完成内容只写"交付了什么、如何验证、已知限制是什么"，不写实现过程与审查过程；审查结论只保留"发现几项 P0/P1、是否已修、未修项已登记 §3"。
- **一致性要求**：涉及协议/安全/架构事实的表述必须与 `docs/PROTOCOL.md`、`docs/SECURITY.md`、`docs/ARCHITECTURE.md` 及代码一致；发现文档与代码不符时，以代码为准并在当期修正文档。
- **完成定义**：见第 9 节；里程碑勾选"已完成"前必须通过对应自动化测试与代码审查。

## 1. 项目现状总览（截至 2026-09-15）

### 1.1 里程碑状态总表

| 里程碑 | 名称 | 状态 | 完成日期 | 交付摘要 |
| --- | --- | --- | --- | --- |
| M0 | 工程基线与可维护性 | 已完成 | 2026-07-01 | README、`.gitignore`、`docs/` 四文档、GitHub Actions CI、Qt Test、顶层 CMake 统一（C++20、`XYCHAT_BUILD_TESTS`）。**注**：CI 工作流曾于 2026-08-03 随 `31622df` 被删、连续 5 周无机器门禁，2026-09-09 已重建（首次运行待验证，见 §3） |
| M1 | 网络协议层重构 | 已完成 | 2026-07-01 | 长度前缀帧协议（magic `XYCP`）、requestId 匹配、统一错误码、ping/pong 心跳与空闲超时 |
| M2 | 账户体系与认证安全 | 已完成 | 2026-07-29 | 注册、PBKDF2-HMAC-SHA256 密码存储、session token（只存摘要）、多设备管理、登录限流、版本化迁移 |
| M3 | 一对一文本聊天 MVP | 已完成 | 2026-07-29 | 用户搜索/联系人、会话与消息模型、收发/状态/离线同步接口、客户端聊天界面 |
| M4 | 客户端 QML UI 重构 | 已完成 | 2026-07-29 | QWindowKit 无边框窗口、Telegram 风格 QML 全套页面组件、NetworkManager QML 适配 |
| M4.5 | M4 遗留清理与聊天完善 | 已完成 | 2026-08-04 | 亮/暗主题切换、搜索直接发起对话、乐观发送、显式已读回执，及 8 项 E2E 验证期缺陷修复 |
| M5 | 传输层加密与会话安全 | 已完成 | 2026-08-03 | TLS 1.2+（QSslSocket）、重放保护字段、日志脱敏、安全内存 |
| M5.5 | 安全加固（审查修复） | 已完成 | 2026-08-03 | TLS fail-closed、timestamp/nonce 强制 + 全局 TTL 去重、会话/消息先授权再查询、`clientMessageId` 幂等、per-recipient 回执模型、账号级 `sync_events` 游标 |
| M6 | 端到端加密一对一聊天 | 已完成 | 2026-08-17 | 简化 Signal 方案：X25519 身份密钥 + 一次性预密钥 + 每消息临时密钥 ECDH + HKDF-SHA256 + AES-256-GCM envelope；服务端 fail-closed 只存密文；TOFU（08-20 追加修复离线发送丢失与重登解密） |
| M6.5 | 本地持久化缓存与 outbox | 已完成 | 2026-08-21 | `LocalStore` 按账号+设备隔离的加密本地库（会话/消息/持久化 outbox/解密缓存/同步游标），缓存先行展示 + 游标增量同步 |
| M7a | 明文群聊 | 已完成 | 2026-08-21 | 群管理五接口、群消息 fan-out + sync_events 兜底、系统消息与群变更通知、按人数回执聚合、客户端群聊 UI（08-22 热修复联调崩溃，提交 `bafd4f6`） |
| M7b | 群聊端到端加密（Sender Keys） | 已完成 | 2026-09-02 | 每发送方每群独立 chain key + Ed25519 签名，HKDF ratchet 派生消息密钥，AES-256-GCM；sender-key 经 M6 pairwise 分发；`fetch_group_keys`（71/72）；群 envelope fail-closed；DoS 上限；成员变更 healing；双客户端联调通过 |
| M8 | 媒体、文件与对象存储 | 进行中 | — | **三切片全部完成**：M8.1 协议与存储地基（09-10）、M8.2 数据面 HTTP(S) 与客户端（09-11）、M8.3a/b/c 多媒体元数据与播放器（09-11）。文件消息端到端可用（控制面 90-99 + 独立 HTTP(S) 数据面、客户端加密上传/下载/另存、图片缩略图与应用内大图查看、音视频元数据与应用内播放）。余项与欠账见 §3、§4.1 |
| M9 | 多端同步与离线一致性 | 已完成 | 2026-09-09 | 已读多端同步 + `sync_events` 保留清理（09-04）；特性栈：会话置顶/免打扰 + 消息编辑/删除（软删除留墓碑），协议 81-87 + V9 迁移 + 三类事件（09-05，提交 `8224464`）；09-09 修复群编辑解密链路（补 `senderId`/`originDeviceId`、跳序密钥缓存、推送覆盖本人其他设备、删除 fail-closed），验收标准自此成立 |
| M10 | UI 重构与体验完善（保守范围） | 已完成 | 2026-09-14 | Phase 0-2 纯 QML 重构（Theme token 分层、SVG 图标库 + `Icon`、基础组件库、全局反馈层、`MainPage` 拆 9 个 Dialog、消息气泡重构、未读分隔线 + 跳到底部 FAB、输入区多行、会话/消息右键菜单）；Phase 3 两项新协议（会话整表删除 `100-102` + "正在输入" `103-105`）；Phase 4 六项历史欠账清理。`ctest` 12/12、`qmllint` 零错误。原任务的搜索/通知/草稿/emoji/设置页/响应式/国际化经用户确认明确排除 |
| M11 | 体验完善与稳定性 | 进行中 | — | **M11A 基础体验与系统集成（已完成 2026-09-15）**：设置页（`AppSettings` 统一管理 + `SettingsDialog` 四节）、系统托盘（`TrayManager` + 最小化到托盘）、桌面通知（免打扰/预览开关/点击跳转）、草稿（会话切换保留输入）、本地消息搜索（`LocalStore.searchMessages` 解密后 LIKE + `LocalSearchDialog` + 跳转滚动）。`ctest` 12/12、`qmllint` 零错误。M11B（消息交互增强，§4.4）与 M11C（群聊与布局完善，§4.5）未开始；原 M11 稳定性/可观测性任务（指标监控/崩溃捕获/压测）并行推进，见 §4.6 |

### 1.2 能力矩阵

| 能力域 | 现状 |
| --- | --- |
| 账户与认证 | 注册/登录/登出/token 续期/`terminate_session`（仅本人其他会话）；PBKDF2 密码存储；登录失败限流（IP 5min/10 次、用户 5min/5 次）；多设备识别（`deviceId` 取自机器唯一 ID）；逐包验 token + `validateSession()` 回查 `sessions` 表（过期/终止/续期换代即时失效，`expiresAt` 解析异常 fail-closed）；会话自动续期 + 失效自动重登（过期前 1 天 `renewToken`、60 秒看门狗、失效回登录页）。**未实现**：双因素认证、注销/找回 |
| 一对一聊天 | E2EE（envelope 密文，服务端 fail-closed）、`clientMessageId` 幂等、乐观发送、per-recipient 回执（delivered/read）、消息状态实时推送、离线 outbox（加密持久化，跨重启重发） |
| 群聊 | 建群/邀请/退群（群主自动转让）/踢人（角色层级保护）/群信息；群 E2EE（Sender Keys，服务端只见密文）；成员变更 Sender-Key healing（新成员获密钥、被移除成员失后续解密能力，离线经 `sync_events` 补偿）；系统消息胶囊；小群直推 fan-out + sync_events 兜底；按接收用户人数聚合的送达/已读。**未实现**：大群拉取模式、改群名接口（数据层已就绪） |
| 本地存储 | `LocalStore`（SQLite，按账号+设备隔离）：消息/会话预览/outbox/解密缓存 AES-256-GCM 加密落库，存储密钥 DPAPI 保护；含 `sender_keys` 与 `sender_key_skipped`（跳序消息密钥缓存）；登出清用户可见数据、保留密钥材料 |
| 多端同步 | 账号级 `sync_events`（message/receipt/contact_added/group_changed/conversation_prefs/message_edited/message_deleted/read_cursor/conversation_deleted）+ 设备本地游标；登录后缓存先行 + 增量拉取（hasMore 自动续拉）；已读、会话偏好、编辑/删除、会话删除均多端一致（实时推送 + sync_events 兜底） |
| 传输安全 | TLS 1.2+ fail-closed（服务端无证书拒启、客户端无 CA 拒连，开发明文需显式开关）；重放保护（timestamp ±300s + nonce 全局 TTL 600s 去重）；日志脱敏（LogSanitizer）；结构化日志（StructuredLogger 单行 JSON）；连接级限流（`RateWindow`：`send_message` 30/10s、`search_users` 20/60s、edit/delete 20/60s、prefs 30/60s、`fetch_keys` 60s/20、typing 10/10s）；专用推送类型 88/89 与会话删除/typing 类型 100-105 |
| 客户端 UI | QML/Qt Quick + QWindowKit 无边框双窗口（登录/主窗口独立）；Telegram 风格主题（亮/暗切换持久化）；M10 视觉系统与组件库（Theme token 分层含 `avatarColor(id)` 确定性配色、SVG 图标库 + `Icon.qml`、基础组件库 `AppButton`/`AppTextField`/`AppDialog`/`Avatar`/`Toast`/`EmptyState`/`LoadingIndicator`/`NetworkStatusBar`、全局 Toast + 网络状态条、`MainPage` 拆 9 个 Dialog、消息气泡重构、未读分隔线 + FAB、多行输入区、会话/消息右键菜单）；**M11A 桌面集成**（`AppSettings` 统一设置管理、系统托盘 `TrayManager` 最小化/恢复/退出、桌面通知 `QSystemTrayIcon.showMessage` 带免打扰与预览开关、草稿会话切换保留、本地消息搜索 `searchMessages` 解密后 LIKE 匹配 + 跳转滚动） |
| 文件与媒体传输 | 控制面走 TCP 主通道（类型 90-99），数据面为独立 HTTP(S) 服务（`QHttpServer` + `QSslServer`，同套证书与 fail-closed，基地址由登录响应 `fileTransferBaseUrl` 下发而非客户端猜端口）；文件字节客户端加密（每文件独立 AES-256 密钥 + 分片独立 AEAD，nonce/AAD 绑定分片序号），文件名/MIME/明文大小/文件密钥只在 `FileManifest` 内随正文经既有 E2EE 分发；客户端 `FileTransferManager`（两遍加密上传、失败后先查已收分片、流式下载 + 整体摘要自校验、进度/取消/重试）；本地缓存密文原样落盘（sha256 命名 + 256 桶），明文只在"另存为"时写出；清单经唯一脱敏出口 `sanitizeForUi` 拦截，四条 UI 路径均不进 QML/JS；图片内联缩略图与应用内大图查看（逐片解密在内存，明文不落盘）；音视频元数据（时长/分辨率/封面）+ 应用内播放器（`DecryptingIODevice` 流式解密）；M10 传输体验（hashing/另存为改时间片增量泵送、传输横幅多任务、状态更新改 Map 索引）。**未实现**：视频动态画面渲染、文件消息持久化 outbox |

## 2. 已完成能力摘要

各里程碑的目标、关键交付、验证证据与已知限制。实施细节经 `git log` 与 `docs/ARCHITECTURE.md` 追溯；遗留欠账统一见 §3。

### M0：工程基线（2026-07-01）

- 交付：README、`.gitignore`、`docs/` 四文档、GitHub Actions CI（configure/build/test）、Qt Test 框架与 `TestEncryptionManager`、顶层 CMake 统一（C++20、警告选项、`XYCHAT_BUILD_TESTS`）。
- **更正（2026-09-09 周度审查）**：上述"CI 全流程通过"自 2026-08-03 起已不成立——`.github/workflows/cmake.yml` 随 `31622df` 被删，连续 5 周无任何机器门禁，期间各里程碑的 "`ctest` 6/6" 仅靠本地手工运行得出（并因此遗漏一项约 50% 概率失败的单测，见 M9）。工作流已于 2026-09-09 重建（windows-latest + Qt 6.8.3，三步 + 失败上传 `LastTest.log`）；**重建后的首次运行即暴露配置缺陷**——安装器默认只装 Qt base，而项目依赖的 Multimedia/HttpServer 属独立 add-on 模块，configure 阶段报 `Failed to find required Qt component "Multimedia"`。已于 2026-09-16 补 `modules` 修复（见 §10），**待一次绿色运行确认**。

### M1：网络协议层（2026-07-01）

- 交付：`CommonModule/protocol` 长度前缀帧协议（magic `XYCP` + version + messageType + requestId + payloadLength，payload 上限 4 MiB）；客户端连接状态机（未连接/连接中/已连接/登录中/已认证/断线重连）；服务端连续包处理；ping/pong 心跳与 90 秒空闲超时。
- 验证：`TestPacketCodec`（连续 1000 小包、大包分片到达）。

### M2：账户体系（2026-07-29）

- 交付：注册（用户名主标识，邮箱/手机可选）；PBKDF2-HMAC-SHA256（100K 迭代 + 16B 随机盐 + 参数版本 `v1:`）；session token（服务端只存 SHA-256 摘要，7 天有效期）；`users`/`devices`/`sessions`/`login_audit` 表拆分；版本化迁移机制（`schema_version`）；登录限流；token 续期与 `terminate_session`。
- 验证：`TestDatabaseManager`（注册/session/审计/迁移）、`TestEncryptionManager`（PBKDF2/常数时间比较）。

### M3：一对一聊天 MVP（2026-07-29）

- 交付：用户搜索、双向联系人；`conversations`/`conversation_members`/`messages` 模型；`send_message`/`ack_message`/`sync_messages`；客户端会话列表、聊天窗口、消息气泡、五态消息状态。本地缓存项当时未实施，由 M6.5 承接。
- 验证：`TestDatabaseManager` 消息/会话用例；双客户端实时收发与离线补收人工验证。

### M4 + M4.5：QML UI（2026-07-29 / 2026-08-04）

- 交付：QWindowKit 无边框窗口（自定义标题栏/拖拽/Snap Layout）；`LoginPage`/`MainPage`/`ConversationList`/`ChatView`/`MessageInput`/`MessageBubble`/`TitleBar`；`Theme.qml` darkMode 双配色 + `ThemeSettings` 持久化；旧 Widgets UI 删除。M4.5 补齐搜索直接发起对话（虚拟会话 + 首条消息 ACK 后绑定）、乐观发送、显式已读回执、日期分隔线、未读角标本地更新、登出入口，并修复 8 项验证期缺陷。
- 验证：qmllint；M1-M3 功能在 QML 下回归；E2E 人工验证。

### M5 + M5.5：传输安全与加固（2026-08-03）

- 交付：TLS 1.2+（开发自签 CA 自动生成，SAN localhost/127.0.0.1）；fail-closed（无静默降级，开发明文需 `--allow-plaintext`/`XYCHAT_ALLOW_PLAINTEXT=1`）；timestamp/nonce 强制必填 + 全局 `NonceCache`（TTL 600s、上限 10 万条、跨连接）；会话/消息接口先授权再查询（`isConversationMember`/`canAccessMessage`，越权 3006）；`terminate_session`（仅本人会话）；handler 线程内发送代理；`clientMessageId` 幂等键 + 部分唯一索引 + 内存 outbox；`message_receipts` 回执表 + 成员读游标（只前进）；`sync_events` 账号级游标。
- 验证：`TestSecurity`（nonce 系列）、`TestDatabaseManager`（越权拒绝/幂等去重/回执聚合/读游标单调/sync_events 游标）。
- 已知限制：nonce 缓存为单服务器内存态；端到端 TLS 集成测试缺失（均见 §3）。

### M6：一对一 E2EE（2026-08-17，08-20 联调修复）

- 交付：每设备 X25519 身份密钥 + 批量一次性预密钥（`register_keys`/`fetch_keys`，服务端只存公钥）；每消息临时密钥 ECDH + HKDF-SHA256（salt `xychat-e2ee-v1`）+ AES-256-GCM envelope（逐设备条目 + 发送方自身拷贝 `prekeyId=0`）；服务端 fail-closed（非法/明文正文拒入库，3008；入库与预密钥消费同事务）；预密钥生命周期（认领即消费、10 分钟超时回退、身份变更废弃旧世代、`fetch_keys` 连接级限流）；TOFU 指纹 + 变更告警；私钥 `KeyStorage`（DPAPI，临时文件 + 替换原子写入）；解密缓存持久化。产品决策：历史消息不可恢复（仅限丢失密钥材料场景），UI 显示"无法解密此消息"。
- 验证：`TestEncryptionManager`、`TestDatabaseManager`；双客户端联调（08-20 修复：对方未注册密钥时 outbox 保留重试不丢弃；自身拷贝 + 持久化解密缓存解决登出重登解密）。
- 已知限制：TOFU 无带外验证；无密钥备份/设备间迁移；非 Windows 平台私钥明文回退（均见 §3）。

### M6.5：本地持久化缓存（2026-08-21）

- 交付：`LocalStore`（AppData/localstore，`<username>_<deviceId>.db`）：消息/会话/持久化 outbox/解密缓存（归口替代 M6 `.cache` 文件，遗留自动迁入）/sync_events 游标；全部正文 AES-256-GCM 加密落库（存储密钥随机生成、DPAPI 保护、加密失败拒写 fail-closed）；登录后缓存先行展示 + 游标增量同步；登出清用户可见数据、保留解密缓存与存储密钥；连接失效自愈（`ensureUsableDb()` 重开，失败则禁用缓存）。
- 验证：`TestLocalStore`（磁盘字节级密文校验、outbox 幂等、登出语义、群字段）。

### M7a：明文群聊（2026-08-21，08-22 热修复）

- 交付：协议消息类型 60-70 与错误码 3009-3012；V7（`conversations.name` + `conversation_members.role`）；服务端五处理器（建群成员上限 200；邀请单批 ≤100、已在群拒绝；退群群主自动转让最早入群成员；踢人按 owner/admin 层级；群信息仅成员）；`send_message` 按 `conversationId`/`toUserId` 分流 + 群 fan-out（在线直推 + 全员 sync_events 兜底）；成员变更系统消息与 `GroupChangedNotification`；回执按接收用户人数聚合（多设备去重）；客户端群组五接口 + 建群/群信息/邀请三对话框 + 系统消息胶囊。
- 验证：`TestDatabaseManager` 群组 8 用例；qmllint 零错误；08-22 热修复联调崩溃（QML delegate 悬空通知端点，提交 `bafd4f6`）后双客户端联调通过。
- 已知限制：大群拉取模式未实现；改群名接口未开放（`setGroupName` 数据层就绪）。

### M7b：群聊 E2EE（2026-09-02 提交 `c806d90`）

- 交付：`GroupE2eeCrypto`（简化 Signal Sender Keys）——每发送方每群独立 `SenderKey`（32B chain key + Ed25519 签名密钥对，`keyId` = 签名公钥 SHA-256 hex 前 32 字符）；chain key 经 HKDF-SHA256 ratchet（salt `xychat-grp-chain`）派生消息密钥；群消息 AES-256-GCM + Ed25519 签名（覆盖 `iv || ciphertext`）；envelope（`contentType=e2ee_group`）含 `keyId`/`iteration`/`senderDeviceId`；sender-key 分发复用 M6 pairwise E2EE 逐设备加密 chain key；服务端 `fetch_group_keys`（71/72，共享 fetch_keys 限流窗口）；服务端对两类群正文 fail-closed（非法回 3008）；`LocalStore.sender_keys` 表加密保存、登出保留；分发消息只处理不展示不落库。
- 安全修复：ratchet DoS 上限（`MaxRatchetSteps=2000`、`MaxMessageIteration=1e8`）；成员变更 Sender-Key healing（2026-09-02 P1：`member_added/removed/left` 触发本端轮换 + 重分发，新成员获密钥、被移除成员失后续解密能力，离线经 `sync_events` 补偿）。
- 验证：`TestGroupE2eeCrypto` 20 用例；`tests/e2e/TestGroupRepro` 双客户端全链路（建群→分发→加密收发→登出重登→再发）退出码 0。M7a 验收标准一并经联调确认。
- 已知限制：大群（成员设备数约 >60）单条 `sender_key_distribution` 可能超 16384 字符上限致分发失败；轮换"先落盘后分发"存在群解密不可用窗口（均见 §3）。

### M9：多端同步与离线一致性（2026-09-04 / 09-05 / 09-09）

- 交付（核心一致性）：已读状态多端同步（`ack_message(read)` 向已读者自身 `sync_events` 追加 `read_cursor` 并经 `ReadCursorNotification (80)` 推送其全部在线设备；客户端 `markConversationRead` 按剩余未读重算角标、状态只前进）；`sync_events` 保留清理（V8 `sync_meta` 水位线，每小时按 30 天保留期清理；落后设备返回 `needsFullSync` + `fullSyncSeq`，客户端重置游标全量回退，历史经 `sync_messages` 补齐）。
- 交付（特性栈 09-05，提交 `8224464`）：会话置顶/免打扰（81-83，按成员×会话维度、多端共享）；消息编辑/删除（84-87，仅发送者可操作；编辑须保持原 contentType 并经服务端 fail-closed 密文校验拒绝明文注入；删除为软删除留墓碑且幂等）；V9（`conversation_members.pinned/muted`、`messages.edited_at/deleted`）；`conversation_prefs`/`message_edited`/`message_deleted` 三类事件离线补偿；客户端全链路 + QML 右键菜单与"已编辑/已删除"展示。
- 交付（2026-09-09 周度审查修复）：① 编辑/删除事件与推送补 `senderId`（群密文靠它定位 Sender Key；旧实现还硬置 `senderId = 0`，致群消息编辑后在所有接收端解密失败并清空已可读正文）与 `originDeviceId`（发起设备去重），并增按（群, 设备, keyId）反查发送者的兼容路径，使修复前已落库的无 `senderId` 事件仍可解；② 跳序消息密钥缓存（Signal skipped-message-keys 语义，`MaxSkippedMessageKeys=1000`，命中即一次性消费、"**先认证后消费**"以防伪造消息烧毁合法密钥、伪造输入不污染缓存与链状态），解决"编辑重加密使 `iteration` 与 `message_id` 解耦、离线按 id 升序补收时后续消息被回滚检查永久拒绝"；经 `sender_key_skipped` 表加密持久化，退群与 `sender_keys` 同一事务清理；③ 编辑/删除推送覆盖操作者本人其他设备；④ `deleteMessage` 写入失败改 fail-closed；⑤ `latestSenderKeyId` 改按 `rowid DESC` 选取（原按秒级 `updated_at` 排序 + 随机 `key_id` 作并列破口，同秒写入两把密钥时选中哪把完全随机）。
- 验证：`TestGroupE2eeCrypto` +6（乱序解密/缓存一次性消费/伪造不污染/伪造不烧毁/容量淘汰/编辑重加密回归，共 27 passed）；`TestLocalStore` +2（跳序密钥密文落库与维度隔离、按设备+keyId 反查发送者，共 24 passed、连跑 20 次 0 失败，修复前约 50% 概率失败）。
- **（2026-09-10 M8 前置清理）** 编辑/删除/偏好三端点接入连接级限流；编辑/删除响应匹配由单发槽位改为多槽 + 私聊编辑队列串行；新增专用推送类型 88/89；新增 `TestNetworkManager`；项目根新增 `AGENTS.md`；`ctest` 7/7。

## 3. 已知欠账与风险清单

集中管理所有已识别但未实施的修复/功能项；销账或新增时更新本表（优先级 P1 最高）。

> **近期销账（历史，明细见 §10）**：2026-09-10 M8 前置 P2/P3 清理（三端点限流、编辑/删除多槽匹配与队列串行、`TestNetworkManager`、`AGENTS.md`）；2026-09-11 M8.2（数据面未实施、票据校验无生产调用点）；2026-09-14 M10 Phase 4（上传 hashing 与 `saveToFile` 异步化、传输横幅多任务、QML 状态更新 O(n)→Map、文件消息重发引导、Theme 装饰性注释）。异步化采**单线程时间片增量泵送**（非工作线程）——无数据竞争、密文逐字节不变，规避了规划中标注的并发损坏风险。同轮 CodeReview 的已修项（含 M8.2 的 3 项 P0：清单密钥经非推送路径泄入 QML、任务容器悬垂引用、泵送中 erase 致迭代器失效）不在下表。
> **未销账**：P2「文件控制面五处理器无自动化测试」仍成立——M8.2 补的是**数据面**与**客户端引擎**的集成测试，`RequestHandler` 的五个 M8 处理器（鉴权与入参顺序、限流、幂等、finalize 分类）依旧只有人工复核。下表含 2026-09-11 M8.2 与 2026-09-14 M10 审查提出但**未修**的项。

| 优先级 | 类别 | 条目 | 来源 | 影响/说明 |
| --- | --- | --- | --- | --- |
| P2 | 功能 | 大群单条 `sender_key_distribution` 超 16384 字符上限 | 2026-09-02 P1 修复审查 | 成员设备数约 >60 时首次分发/healing 分发消息超限被服务端拒（`InvalidRequest`）；需分片分发或提高上限（与 P3 大群拉取模式相关） |
| P2 | 安全 | 群 Sender-Key “先落盘后分发”，分发永久失败留解密窗口 | 2026-09-02 P1 修复审查 | 轮换后新 key 在分发 ACK 前即启用；瞬时失败已延迟重试，确定性失败（如超大群）下本端以新 keyId 加密而他人未收到 → 群消息不可解；建议改为 ACK 后启用/pending 提交 |
| P3 | 工程 | 群成员变更 healing 的 O(N²) 重分发与预密钥消耗 | 2026-09-02 P1 修复审查 | 一次成员变更触发全员各自轮换+重分发；已加同群去重与单发槽位节流，但大群跨成员风暴仍需聚合策略（如群主统一分发或延迟合并） |
| P2 | 工程 | 端到端 TLS 集成测试缺失 | M5.5 遗留 | `tests/e2e/TestGroupRepro` 为手动工具（不纳入 CTest，需手动启动服务端），无自动化 TLS 双端集成测试 |
| P2 | 安全 | nonce 去重为单服务器内存态 | M5.5 | 服务端重启清空；多服务器部署需持久化/共享存储 |
| P2 | 安全 | TOFU 无带外验证；无密钥备份/设备间迁移 | M6 | 首次通信无法抵抗服务端中间人；更换设备/清数据后历史消息不可恢复（产品已决策接受） |
| P2 | 安全 | 非 Windows 平台私钥/存储密钥明文回退 | M6/M6.5 | DPAPI 仅 Windows；Linux/macOS 部署需接平台密钥环（libsecret/Keychain） |
| P3 | 功能 | 大群拉取/游标模式；改群名接口 | M7a 遗留 | 当前仅小群直推；`setGroupName` 数据层就绪、接口层未开放 |
| P3 | 功能 | 桌面通知；简化图片消息 | M6.5 提前项（未实施） | 分别归属 M10/M8 完整实现；图片消息的协议与存储地基已由 M8.1 提供（清单预留 `width`/`height`/`thumb`），M8.3a/b 的缩略图管线已完成，桌面通知仍待立项 |
| P3 | 工程 | 跳序消息密钥缓存按整块 blob 落库，大跳跃场景有写放大 | 2026-09-09 CodeReview | `decryptGroupMessageObject` 每次解密都会 `loadSkippedMessageKeys`（整块解密），且缓存非空时每条消息重写整块（重新序列化+加密+写库），近似 O(N × cacheSize)；上限 1000 条时单块可达数十 KB。典型编辑场景（小跳跃）影响微小，新设备/长期离线的大跳跃补收才明显。建议改为每跳序密钥一行（PK 含 iteration）+ 增量写入，或提升为批次内存态缓存 |
| P3 | 安全 | 跳序密钥序列化时以 base64 `QString` 形态短暂驻堆，无法可靠清零 | 2026-09-09 CodeReview | `SecureMemory::wipe` 对 COW/只读的 `QString` 缓冲无效，与现有 chain key/正文落库路径（`encryptText`/`decryptText` 均返回 `QString`）为同一固有限制；如需更严格的密钥卫生，序列化应走 `QByteArray` 并用后 wipe |
| P2 | 工程 | 编辑/删除在途请求（`m_pendingEdits`/`m_pendingDeleteRequestIds`）无超时清扫 | 2026-09-10 CodeReview | 若服务端漏答且连接未断（无 disconnect 触发清理），已发出的编辑/删除条目会残留至下次登出/断线；`requestId` 单调不回绕不会误配，仅无信号的内存泄漏（受 edit/delete 20/60s 限流天然封顶）。建议加 30s 超时 `erase` + 失败上报（镜像 token 续期看门狗） |
| P3 | 工程 | 私聊编辑额外占用一次共享 `fetch_keys` 预算 | 2026-09-10 CodeReview | 每条私聊编辑 = 1 次 `fetch_keys`（与发送/fetch_group_keys 共设 20/60s 窗口），编辑自身窗口亦 20/60s；混合场景可能先撞 `fetch_keys` 上限使编辑以 `RateLimited` 失败，实际有效编辑率低于标称。与 P2 三端点限流相关，待真实用量评估后调参 |
| P3 | 工程 | `TestNetworkManager` 未覆盖 pump 与编辑解密回退路径 | 2026-09-10 CodeReview | 现有用例锁定 requestId 多槽匹配/消费、去重、断线清理；但 `pumpPrivateEditFetch`+`handleFetchKeysResponse` 编辑分支（需模拟 fetch 响应、会写 socket）与 88 推送“解密失败不写空”幂等回退不变量因难构造无网络环境而未断言；属测设完善，不阻塞 |
| P2 | 工程 | 对象存储为单机本地文件系统，无副本/无冗余 | 2026-09-10 M8.1 | `LocalFileStorage` 磁盘损坏即文件丢失；限流与并发配额为单实例/单库口径，多实例部署需换共享对象存储（`IObjectStorage` 已抽象，可接 S3/MinIO）并把配额改为全局口径 |
| P2 | 工程 | 文件控制面五处理器无自动化测试 | 2026-09-10 M8.1 | `RequestHandler` 的 M8 处理器（鉴权与入参校验顺序、限流、幂等、finalize 结果分类、枚举预言机合并）只有人工复核；数据层（14 用例）、存储层（19 用例）、**数据面（11 用例）与客户端引擎（12 用例，M8.2 补）**已覆盖。建议复用 `TestNetworkManager` 的 friend 注入范式补 handler 级测试 |
| P2 | 功能 | 下载票据 TTL（300s）与大文件下载不匹配，且中途不续期 | 2026-09-11 CodeReview | 2 GiB / 4 MiB = 512 次串行 Range GET，慢链路下总时长易超 300s；票据过期后回 401，而 4xx 被判为非瞬时故障 → 直接失败并删临时文件，**已下载的全部进度丢弃且无续传**。建议：提高 TTL 或改为按“最后一次使用”滑动续期，并在客户端收到 401 且 `downloadIndex > 0` 时走“重新申请票据 + 从断点续传”分支 |
| P2 | 安全 | `QHttpServer` 在进入 handler 前已缓冲整个请求体 | 2026-09-11 CodeReview | `QHttpServerRequest::body()` 返回已缓冲的 `QByteArray`，因此代码里的“超大分片早退 413”发生在内存已被消耗之后；`QAbstractHttpServer` 无内建请求体上限，**未认证**客户端可用巨大 `Content-Length` 的 PUT 造成内存放大（而 per-IP 限流只计失败，对首次请求无效）。建议：前置一层自行解析请求头做 `Content-Length` 预检，或强制要求数据面部署在带 body 上限的反向代理之后（当前 `--http-host` 默认仅回环是正确的）；并补已授权请求的并发数与字节速率上限 |
| P3 | 工程 | `LocalStore.messages` 表无 `file_id` 列 | 2026-09-11 CodeReview | 从本地缓存回填的消息不带 `fileId`，现由 `attachFileInfo` 从清单内取回（清单经 E2EE 保护且自带 `fileId`，功能等价），但少了服务端权威字段的交叉校验，且每次回填都要解一次清单 JSON。建议下一轮客户端迁移（V11）补列 |
| P3 | 工程 | `clearCache` 会删掉在途下载的临时文件 | 2026-09-11 CodeReview | `entryList(QDir::Files)` 会匹配 `<sha>.<uuid>.tmp` 并删除，随后还可能 rmdir 桶目录，使在途任务下次写入失败；返回的删除条数也把 `.tmp` 计入。影响有限（任务会明确失败而不是静默写坏缓存），建议跳过 `*.tmp` 或先取消在途下载 |
| P3 | 工程 | `finalizeDownload` 的 exists + rename 存在 TOCTOU | 2026-09-11 CodeReview | 两个任务并发下载同一文件时，Windows 的 `rename` 不覆盖已存在文件 → 报“Cannot move ... into the cache”，而缓存其实已完好。建议 rename 失败后重新 `isCached` 判定，命中即视为成功 |
| P3 | 安全 | `markFileTicketUsed` 仍无生产调用点；过期票据行以 `used=0` 残留 | 2026-09-11 CodeReview | 下载票据刻意允许 TTL（300s）内重复使用以支持 Range 分段，因此不消费是设计意图；但 `FileProtocol.h` 附近的注释把它描述成一次性票据，应修正；过期行由 `pruneExpiredFileTickets` 每小时清理，若需更严可改为每段单独签发或绑定数据面会话 |
| P3 | 功能 | 视频播放无动态画面（只输出音频轨 + 静态封面） | 2026-09-11 M8.3b | `MediaPlaybackManager` 用 C++ QMediaPlayer + `DecryptingIODevice` 流式解密播放（明文不落盘），但 QML VideoOutput 无法绑定 C++ QVideoSink（无公开 videoSink 属性），且 QML Video 元素的 source 只接受 URL 不支持自定义 QIODevice。当前视频播放只闻其声不见其画（静态封面取自清单 thumb）；补齐需自定义 QSGNode/QQuickPaintedItem 渲染 QVideoFrame，或在 C++ 侧 qobject_cast QML VideoOutput 调 setVideoSink（脆弱） |
| P3 | 功能 | 无按用户的存储用量配额 | 2026-09-10 M8.1 | 现有约束为单文件 ≤2 GiB + 并发上传 ≤8 + 48 小时超期回收，但已就绪文件可无限累积（仅受消息删除联动回收影响）；需按用户/按会话的字节配额与用量统计接口 |
| P3 | 安全 | 下载票据在 TTL 内可重复使用 | 2026-09-10 M8.1 | 为支持 `Range` 分段与断点续下而刻意允许（TTL 300 秒），泄露后可在窗口内重放下载该文件；一次性消费（`markFileTicketUsed`）与分段下载互斥，属取舍。大文件场景下 TTL 不足的风险另见上方 P2 |
| P3 | 工程 | `putChunk` 不入条带锁 | 2026-09-10 CodeReview | 依赖 `finalize` 的逐片长度 + 整体 SHA-256 关卡兜底：并发写同一片只会导致组装判失败（要求重传），不会把损坏对象推上下载路径；代价是极端并发下多一次重传 |
| P3 | 工程 | 回收查询每轮 `limit=100`，积压大时需多轮收敛 | 2026-09-10 M8.1 | `getStaleUploads`/`getTerminalFiles`/`getUnreferencedReadyFiles` 均为每轮上限 100 行、每小时一轮；大量遗留时收敛慢且无积压告警指标 |
| P3 | 工程 | 文件回收任务在主线程做同步磁盘 I/O | 2026-09-10 CodeReview | `pruneFileUploads` 由 Server（主）线程的定时器驱动，该线程同时承担 `incomingConnection` 与 `onMessageForUser` 路由；`remove()`（内部 `removeRecursively`）为阻塞调用，三轮合计每轮最多约 300 次删除，大文件/多分片目录时可能短时阻塞连接接受与消息转发。量级有界（每小时、limit=100）且定时器不重入，属响应性隐患而非正确性缺陷；积压增大后可移至独立维护线程或工作池 |
| P3 | 工程 | `runHashSlice` 从 hashing 转 creating 时 emit 后仍持有 `Task&` 引用（理论悬空） | 2026-09-14 M10 CodeReview | 完成分支先 `clearLocalStateForToken(token)` 再 `emit taskProgress`/`uploadCreateRequested`，其间持续经 `Task &t = tit.value()` 访问 task；若某 `taskProgress` 的 QML 槽同步触发 `cancelTask → finishTask → m_tasks.erase`，`t`/`tit` 将悬空。当前 QML 未在 `taskProgress` 回调中调 `cancelTask`，且此为原同步版 `beginHashing` 既有模式（非 M10 新引入），故实际不触发。建议 emit 前把 phase/cipherSize/chunkSize/chunkCount/sha256Hex 全拷入局部、emit 序列不再解引用 `t` |
| P3 | 安全 | `processDeleteConversationRequest` 对非成员暴露“会话是否存在”的错误码差异（存在性 oracle） | 2026-09-14 M10 CodeReview | 权限校验顺序为 形态→限流→`getConversation`(存在性)→`isConversationMember`→群主校验；非成员探测时“存在但非成员”回 `PermissionDenied`、“不存在”回 `ConversationNotFound`，二者可区分。会话 ID 自增需先验知识、成员间本已互知，信息增益极低。建议（可选）对“非成员”与“不存在”统一错误码消除区分度 |

## 4. 未来里程碑规划

### 4.1 M8：媒体、文件与对象存储（三切片全部完成）

- **目标**：支持图片、语音、视频和文件消息。**依赖**：M3/M7a 消息通道、M6/M7b 加密基础（均已完成）。
- **切片划分依据**：原整块里程碑含三个可独立验收、依赖方向单一的切片，拆开后可逐片入库与回滚，避免长期悬置分支。

#### M8.1 协议与存储地基（已完成，2026-09-10，提交 `e8b6df1`）

- 交付：`FileProtocol`（类型 90-99、错误码 3013-3021、`FileManifest` 编解码与 fail-closed、分片数学 `chunkCountFor`/`isChunkingValid`/`expectedChunkBytes`、体积/分片/票据/配额常量）；`send_message` 新增可选 `fileId`（校验存在/本人/ready）并在响应/推送/事件/历史四条路径回传，带 `fileId` 的消息不可编辑正文。`FileCrypto`：每文件独立 AES-256 密钥 + 12 字节 nonce 前缀（第 i 片 nonce = `iv` 后 4 字节 XOR 大端 `i`、AAD = 大端 `i`）、流式 SHA-256、票据摘要。**刻意不复用消息 ratchet**——链式密钥一旦推进，早先分片就永久不可解，而文件需要可重复下载。`IObjectStorage` + `LocalFileStorage`（临时文件 + 原子改名、blobKey 前两位 256 桶、同键 `finalize`/`remove` 条带锁、组装逐片核长 + 整体核 SHA-256、结果六分类）。V10 迁移（`files`/`file_tickets`/`messages.file_id` + 四个索引）、元数据 CRUD（创建含原子并发配额、终态不可逆、票据只存摘要、`canUserAccessFile`、三个回收查询）。控制面五处理器 + 两个限流窗口 + 幂等 + finalize 分类回错误码。`Server` 注入存储与 `pruneFileUploads` 三轮回收（超期上传先删盘后标 cancelled；终态行先删盘后删行；已就绪无引用行**只迁终态不碰磁盘**，销毁推下一轮——引用判定与迁移合并为单条语句消除 TOCTOU）。
- 验证：`TestFileProtocol`（30）、`TestObjectStorage`（19）、`TestDatabaseManager` 新增 14 个 M8 用例（共 63 passed）；`ctest` 9/9（共 227 用例）。
- 审查：两轮 CodeReview 无 P0；已修 2 项 P1（元数据枚举预言机、存储层并发）、1 项 P2（配额 TOCTOU）、1 项 P3（终态行无回收路径），以及第二轮 P1（"终态行不可能再被引用"在跨线程下不成立 → 发送侧条件下推为 `INSERT` 守卫子查询 + 回收侧删盘前再判引用，双侧防护）。
- 已知限制：单机本地文件系统存储；无自动化文件传输集成测试（见 §3）。

#### M8.2 数据面与客户端（已完成，2026-09-11）

- 交付：`FileHttpService`——`QHttpServer` 经 `QSslServer` 承载（**先 `listen()` 后 `bind()`**，反序会被 Qt 拒绝），与主通道共用证书与 fail-closed 口径；`PUT /file/<fileId>/chunk/<index>` 与 `GET /file/<fileId>`（单区间 Range，206/416/413/400 语义完整，range-unit 大小写不敏感）；票据只走 `X-XYChat-Ticket` 头（不进 URL query，避免落入代理/访问日志），四种票据失败统一 401 且响应体一致（防枚举预言机）；分片长度按 `expectedChunkBytes` 精确匹配（防自选边界绕过校验）；下载恒 `application/octet-stream` + `Cache-Control: no-store`；单次 GET 上限 4 MiB（客户端本就按分片解密）；per-IP 限流只计授权失败与畸形请求（成功搬运不计，否则大文件上传会被挡）。`Server::setFileHttpEndpoint` + 登录响应下发 `fileTransferBaseUrl`（未启动则不下发，客户端据此禁用文件能力而非猜端口）；`--http-port`/`--http-host` 可配；上传完成/取消/失败后 `revokeFileTickets` 吊销票据。
- 客户端引擎 `FileTransferManager`：两遍加密上传（第一遍只算密文整体 SHA-256、**不落临时文件**，代价是多一遍 AES，换来磁盘占用不翻倍且无崩溃残留）；串行分片调度（token 快照 + `m_pumping` 重入护栏）；失败后先查 `receivedChunks` 再决定跳过/重传；重试预算双层（分片 3 次 + 恢复 5 轮，后者防止"成功的查询"清零预算而形成活锁）；下载 → 临时密文 → 整体 SHA-256 自校验 → 原子改名进缓存；与控制面经"请求信号 + seq 回调"解耦（不持 socket，可脱离网络单测）。
- 本地缓存（经调研后定调）：下载的密文**原样落盘**（`<cacheRoot>/<sha256[0..1]>/<sha256>`，无后缀）——零额外加密开销且磁盘上天然不是明文。调研依据：Signal Desktop / Telegram Desktop / Signal-Session Android 的附件缓存**无一家采用"明文 + 混淆文件名"**，随机命名只防索引与目录浏览，不承担保密职责（文件头魔数使类型探测成本极低）；若明文缓存，则 `LocalStore` 对正文的加密在含附件的会话上完全被绕过。明文只在用户"另存为"时写出。
- 协议修正：`FileManifest` 新增**必填** `chunkSize`——接收方必须只凭清单（E2EE 可信）就能确定分片边界，若改用服务端声明的口径，不可信的服务端就能让解密错乱；客户端把服务端返回的 `sizeBytes`/`chunkSize`/`chunkCount`/`sha256` 四项与清单逐一比对。
- `NetworkManager` 集成：接线引擎（5 请求/5 响应 + `requestId`→`seq` 映射）；**文件消息不进持久化 outbox**（outbox 表无 `file_id` 列，落库后重发会退化成"正文是清单"的普通消息 = 把密钥当文本发出）；`attachFileInfo` 登记清单 + 补脱敏展示字段；`sanitizeForUi` 作为唯一脱敏出口覆盖四条 UI 路径；会话预览改 `[File] <名>`；登出与断线均 `reset()`（清零清单密钥）。
- QML UI：`MessageInput` 附件按钮 + `FileDialog`（`QUrl` 原样交给 C++，由 `fileTransfer.toLocalPath()` 转本地路径）；`MessageBubble` 文件面板（图标/名/大小/进度/下载/另存，编辑项对文件消息禁用）；`ChatView` 新字段与信号 + `updateFileState`/`updateFileProgress`；`MainPage` 传输横幅、另存对话框、`Connections` 接线；引擎注册为 context property `fileTransfer`。
- 验证：`TestFileHttpService`（11）、`TestFileTransfer`（12，含 2.5 MiB 跨 3 片文件端到端字节级往返 + "服务端只见密文"断言）、`TestNetworkManager` +1（P0 回归 `fileManifestNeverReachesUiLayer`）、`TestFileProtocol` +1；`ctest` **11/11**；`qmllint` 零错误。集成测试当场抓到编译期无法发现的真实缺陷：`QAbstractHttpServer::bind()` 要求 server 已在监听，原顺序反了（生产同样会启动失败）。
- 审查：CodeReview 发现 **3 项 P0 均已修**（① 清单密钥只在实时推送路径脱敏，`sync_messages`/本地缓存回填/`sync_events` 三条路径会把含密钥的 JSON 渲染进气泡 → 收口为唯一出口 + "形态像清单就置空"兜底；② `finishTask`/`failTask` 的 `token` 参数常是容器内元素的引用，`erase` 后悬垂而 `emit` 还要读它（每次成功/失败/取消都走到）；③ `pumpNext` 在 `QHash` 遍历中 erase 当前节点致迭代器失效 + 递归泵送破坏串行承诺）；3 项 P1 已修（重试预算被成功查询清零致活锁、下载气泡永久卡"下载中"、发送方无法下载自己发的文件）；P2/P3 各修两项。未修项已登记 §3。
- 已知限制：hashing 与 `saveToFile` 解密曾在 GUI 线程同步（已由 M10 P4.4/P4.3 销账）；下载票据 TTL 对不匹配大文件且无断点续传；数据面无请求体上限预检；**双客户端实机联调尚未做**（属人工验证）。

#### M8.3 多媒体元数据（M8.3a/b/c 全部已完成 2026-09-11）

##### M8.3a 图片元数据与缩略图（已完成）

- 交付：`ThumbnailMaker`（**纯 QtGui**，不依赖平台多媒体后端，故可在无头环境与 CI 稳定验证）：按最长边 160px 缩放（`setScaledSize` 先缩放再解码，避免把大图完整读进内存），逐步降质量 70/55/40/25/15、到底仍超限再折半降尺寸（下限 32px）；**压不进 `MaxThumbnailBytes`（4096）就不内联**（UI 回退文件图标，绝不放宽上限——否则清单会撑破群消息正文长度而使整条文件消息被拒收）；`setAutoTransform(true)` 校正 EXIF 方向并据此修正上报宽高（旋转 90/270 度交换宽高）。`FileTransferManager` 上传前提取并写入清单 `width`/`height`/`thumb`；`attachFileInfo` 新增脱敏字段 `fileWidth`/`fileHeight`/`fileThumb`；`MessageBubble` 内联缩略图（仅在 `status === Image.Ready` 时显示，解码失败不留空白）+ 像素尺寸行。
- 安全口径：缩略图是 JPEG **明文**字节（修正了 M8.1 将其注释为"密文"的错误表述）：清单整体随正文经既有 E2EE 加密，再单加一层只增复杂度而无收益；也因它**不含任何密钥**，可经脱敏出口交给 QML，使接收方**在下载原图之前**就能预览（零流量零等待），服务端仍全程不可见。
- 验证：`TestThumbnailMaker`（8 用例，JPEG 插件缺失时相关断言 QSKIP）；`TestFileTransfer` +1；`ctest` 12/12；`qmllint` 零错误。
- 已知限制：缩略图只在上传时生成，M8.3a 之前的历史图片消息无缩略图（M8.3b 已补本地生成）。

##### M8.3c 应用内大图查看器（已完成，2026-09-11）

- 交付：`FileImageProvider`（`QQuickImageProvider`，以 `image://xyfile/<messageId>` 注册）：从密文缓存逐片解密并在内存中解码，**明文不落盘**（看原图不再必须"另存为"）；`decryptedFileBytes(messageId)`（渲染线程调用，`manifestFor` 加锁拷贝清单、IO 与 GCM 认证在锁外）+ `MaxInMemoryDecodeBytes = 64 MiB` 上限（超限返回空，避免把整个明文读进内存）；`MessageBubble` 缩略图与图标加 `MouseArea`（仅 `isImageFile && fileState === "available"` 可点）发 `previewRequested`；`MainPage` 预览对话框（标题带文件名与像素尺寸、加载态、错误态、footer 提供"另存为"）。
- 同时修复 P0 运行时错误：`Qt.urlToLocalFile()` 在 QML 全局 `Qt` 对象上**不存在**（那是 C++ `QUrl` 的方法），抛 `TypeError` 致附件无法发送、另存为失败；修为把 `QUrl` 原样交给 C++（`fileTransfer.toLocalPath()`），并同步修正 `AGENTS.md` 中误导性的规则。
- 验证：`TestFileTransfer` +2（`toLocalPathAcceptsUrlsAndPlainPaths` 七种形态、`decryptedFileBytesRestoresPlainForRegisteredMessage`）；`ctest` 12/12；`qmllint` 零错误。
- 已知限制：超过 64 MiB 的图片无法在应用内预览；无缩放/平移控件。

##### M8.3b 音视频元数据与播放器（已完成，2026-09-11）

- 交付：`MediaMetadataExtractor`（`QMediaPlayer` + `QVideoSink` 异步加载）：提取音频时长、视频时长/分辨率，视频 seek 到 min(1000, duration/2) 毫秒抓封面帧（`ThumbnailMaker::encodeThumbnail` 压缩）；5 秒超时护栏；已拿到时长/分辨率但抓帧超时仍上报前者。`uploadAndSend` 重构为"元数据提取 → hashing → creating"三阶段（音视频走 `extracting` 异步阶段，同时只提取一个其余排队；**元数据缺失绝不阻断发送**）；`cancelTask`/`finishTask`/`failTask`/`reset` 均清理提取队列。清单 `durationMs` 填充并透传 `fileDurationMs`；气泡新增音视频判定、播放按钮与时长标签、按类型分流的点击行为。
- 播放器（明文不落盘）：`DecryptingIODevice`（`QIODevice`，从密文缓存流式逐片解密，实现 `readData`/`size`/`seek`/`bytesAvailable`，支持拖动进度条）+ `MediaPlaybackManager`（C++ `QMediaPlayer` + `QAudioOutput` + `QVideoSink`，`setSourceDevice` 播放）；QML 播放器对话框（播放/暂停/进度/音量）。
- 历史图片缩略图补齐（销 M8.3a 欠账）：`localThumbnailForMessage` 在清单 thumb 为空且原图就绪时解密生成缩略图缓存到 `<cacheRoot>/thumbs/`，`attachFileInfo` 回填 `fileThumb`，`clearCache` 一并清理。
- 验证：`TestFileTransfer` +3（`DecryptingIODevice` 顺序读取逐字节还原、seek 跨分片边界、未下载/未登记 open 失败）至 18 用例；`ctest` 12/12；`qmllint` 零错误。集成测试抓到真实缺陷：`ensureChunkFor` 重新解密分片后把 `m_chunkBufferOffset` 置 0 而非片内偏移，致 seek 到片中间时错位返回片头数据。
- 已知限制：音视频元数据提取与播放依赖平台解码后端（Windows Media Foundation），CI/无头不可验证（提取失败留空、播放报错，均不崩溃）；**视频播放无动态画面**（见 §3 P3）。

### 4.2 M10：UI 重构与体验完善（保守范围，已完成 2026-09-14）

- **目标**：把 MVP 从"能用"提升到"好用"。**依赖**：M6.5 本地缓存（本地消息搜索的数据底座）。
- **已交付（保守主体 + 精选新能力）**：
  - Phase 0 视觉系统与组件库地基：Theme token 扩展（elevation/状态色/`avatarColor(id)`/字号 scale/动画曲线）+ 清理装饰性横线注释；SVG 图标库 + `Icon.qml`（MultiEffect 着色，替换全部 Canvas 手绘与 emoji）；基础组件库；全局反馈层（顶层 Toast + 网络状态条，失败信号全部经 Toast）。
  - Phase 1 可用性关键路径：加载指示器；`MainPage` 拆分为 9 个独立 Dialog（`resources/dialogs/`）；搜索体验（自动聚焦/键盘导航/`EmptyState`）。
  - Phase 2 消息与会话体验：`Avatar` 接入；消息气泡重构（SVG 矢量状态勾、failed 点击重发、hover 编辑/删除/复制、文本可选中）；未读分隔线 + 跳到底部 FAB；输入区多行 `TextArea`（Enter 发送/Shift+Enter 换行/字符计数器）；会话右键菜单。
  - Phase 3 精选新协议：会话整表删除（类型 `100-102`，硬删除 + 通知全体前成员，群聊仅群主可删，**无需 V11 迁移**）；"正在输入"指示（类型 `103-105`，服务端 fan-out + 限流 10/10s，客户端节流 4s + 5s 自动隐藏）。
  - Phase 4 历史欠账清理：见 §3 2026-09-14 销账。
- **验收**：`ctest` 12/12（`TestFileTransfer` 扩至 20 用例，`TestNetworkManager`/`TestDatabaseManager` 各新增会话删除与 typing 用例）；`qmllint` 零错误。
- **明确排除（经用户确认，保守范围；如需另立项）**：客户端本地消息搜索、系统托盘、桌面通知、草稿、emoji 选择器、设置页、响应式/三栏布局、国际化（.ts）、群信息右侧抽屉、消息回复/引用/转发、最后在线时间、多选模式。**后续处置**：除国际化外的 11 项已立 M11 体验完善计划（§4.3-§4.5），国际化仍待另立项。
- **偏离规划记录**：① 协议号由规划的 `82-87` 顺延至 `100-105`（`82-89` 已被 M9 占用、`90-99` 属 M8 文件控制面）；② 会话删除无需 V11 迁移（复用既有表 + 事务内显式外键安全删除，不依赖 `PRAGMA foreign_keys`）；③ 群聊删除由规划的"仅会话成员可删"收紧为"仅群主可删"，通知范围由"本人所有在线设备"扩展为"全体前成员"；④ 异步化采单线程时间片泵送而非工作线程（规避数据竞争与密文损坏风险）。

### 4.3 M11A：基础体验与系统集成（已完成 2026-09-15）

- **目标**：补齐桌面应用的基础能力（设置页/系统托盘/桌面通知/草稿/本地消息搜索），使产品从"能用"到"像桌面应用"。**全部为纯客户端改动：无线上协议变更、无数据库迁移**。
- **切片划分依据**：M11 体验完善计划按"依赖方向"拆为三片——M11A 纯客户端且无迁移（可独立验收回滚）、M11B 含唯一一次协议扩展与 V11 迁移、M11C 需服务端回填在线状态；A 完成后再做 B/C 才能避免"迁移与 UI 重构混在一个提交里"。
- **交付**：
  - **设置页**：`AppSettings`（`QSettings`）统一管理主题/通知/预览/托盘偏好，替代原 `ThemeSettings`；`SettingsDialog` 外观（亮暗切换）/通知（桌面通知、消息预览开关）/存储（缓存用量 + 清除缓存）/关于（版本）四节；入口在侧边栏用户栏（登出按钮旁）。
  - **系统托盘**：`TrayManager` 持有 `QSystemTrayIcon`（`main.cpp` 改用 `QApplication` 并 `setQuitOnLastWindowClosed(false)`）；关闭窗口最小化到托盘、双击/单击恢复、右键菜单"打开/退出"；**托盘不可用时关闭窗口直接退出**（否则窗口隐藏后无恢复入口）。
  - **桌面通知**：`NetworkManager` 在 `NewMessageNotification` 之后按"通知开关开、非免打扰会话、非本人发送、非当前活动会话"四项过滤后 emit，`TrayManager::showNotification` 弹系统通知；点击通知切到对应会话并恢复窗口。私聊标题=发送者，群聊标题=群名且正文带发送者前缀；消息预览开关关闭时正文降级为 `[新消息]`（通知文案取自已脱敏的 UI 字段，不含清单密钥）。
  - **草稿**：`MainPage.drafts`（会话 ID → 文本，内存级），切换会话保存/恢复输入框文本，发送成功清除。
  - **本地消息搜索**：`LocalStore.searchMessages` 逐条解密后 LIKE 匹配（不新增明文索引表）、结果附带会话名；`LocalSearchDialog` 搜索与结果列表；点击结果切会话并滚动到命中消息（`ChatView.scrollToMessage` + `pendingScrollMessageId` 在异步加载完成后执行）。**入口为会话列表工具栏的独立按钮**——用户搜索（`SearchDialog`）入口保持原样，二者互不占用。
- **验证**：`ctest` 12/12（`TestLocalStore` +2：搜索命中/范围隔离/limit、免打扰与显示名）；`qmllint` 零错误。
- **审查**：CodeReview 3 项 P1（私聊通知冗余前缀、托盘不可用致窗口永久隐藏、搜索跳转在消息异步加载前失效）+ 1 项 P2（托盘 `QMenu` 泄漏）均已修；复审另发现 1 项 P1 功能回退——消息搜索曾顶替搜索按钮导致"用户搜索发起对话"入口丢失，已修为独立入口。
- **已知限制**：草稿不跨重启（内存级；持久化需客户端新建 `drafts` 表，属可选增强）；本地搜索逐条解密匹配，千级消息量可接受、万级需 FTS5 或异步搜索 + 进度指示；托盘图标与通知样式存在平台差异，仅 Windows 实机验证。

### 4.4 M11B：消息交互增强（未开始）

- **目标**：补齐消息维度的高级交互（多选/回复引用/转发/emoji）。
- **Phase 与执行顺序**：B1 多选模式 → B3 消息转发（转发复用多选的选择集，**依赖 B1**）；B2 消息回复/引用、B4 emoji 选择器各自独立可并行。
- **协议影响**：`send_message` 新增可选 `replyToMessageId`，响应/推送/历史/事件回传引用字段——**这是 M11 唯一的协议扩展与数据库迁移**（服务端 V11：`messages.reply_to_message_id`；客户端 `LocalStore` 同步加列）。除 B2 外其余 Phase 无协议变更。
- **关键约束**：群聊被引用消息的正文对服务端是密文，引用摘要**不得**由服务端填充——只回传 `replyToMessageId`/`replyToSender`，摘要由客户端本地解密被引用消息后渲染（服务端不接触明文）；引用字段与既有 E2EE、跳序密钥缓存的交互需回归（编辑会改正文，引用仍指向原消息 ID）。
- **交付与验收**：`ctest` 12/12 全绿（`TestDatabaseManager` V11 迁移 + 引用字段存取、`TestNetworkManager` 回复/转发用例）；`qmllint` 零错误；多选 → 转发/删除、引用显示与跳转、emoji 插入手动验证；文档同步 PROTOCOL（`replyToMessageId`）/ARCHITECTURE/SECURITY/ROADMAP。

### 4.5 M11C：群聊与布局完善（未开始）

- **目标**：群聊体验优化（信息抽屉、最后在线时间）与宽屏布局适配。
- **Phase 与执行顺序**：C1 群信息抽屉（`Drawer` 替代弹出对话框）→ C3 响应式布局（宽屏三栏由 C1 的抽屉组件常驻右侧实现，**依赖 C1**）；C2 最后在线时间独立，需服务端回填，可与 C1/C3 并行。
- **协议影响**：`get_conversations` 回填 `lastActiveAt`/`online`（复用既有 `sessions.last_active_at`，无迁移）。
- **风险**：无边框窗口（QWindowKit）的 Snap Layout 与三栏布局、会话列表滑出的交互需实机验证不冲突。
- **交付与验收**：`ctest` 12/12 全绿（`TestDatabaseManager` 最后在线时间用例）；`qmllint` 零错误；抽屉滑出/收起、在线状态文案、宽窄窗口布局切换手动验证；文档同步 PROTOCOL（`lastActiveAt`/`online`）/ARCHITECTURE/ROADMAP。

### 4.6 M11 稳定性、可观测性与运维（持续，与 M11A/B/C 并行）

- **目标**：为真实用户使用做好稳定性基础。
- **已提前落地**：登录限流（M2）；`fetch_keys`/`fetch_group_keys` 连接级限流（M6/M7b）；**发消息/搜索限流 + 结构化日志（M11 前置两项，2026-09-03）**——对应 P2 欠账已销账。
- **任务**：
  - 结构化日志（✅ 已实施 2026-09-03）：`StructuredLogger` 单行 JSON，统一请求 ID/用户 ID/设备 ID/错误码/耗时字段（配合 LogSanitizer 脱敏）；`sendResponse` 中央审计日志（成功 info/失败 warning，可统计失败率与延迟）+ 鉴权/会话/重放/envelope/限流安全事件带 `reason`。
  - 指标监控：在线连接数、消息吞吐、失败率、延迟、数据库慢查询。
  - 崩溃捕获与客户端日志上报。
  - 限流：发消息、搜索（✅ 已实施 2026-09-03，连接级 `RateWindow` + 通用 `RateLimited` 1003，`fetch_keys`/`fetch_group_keys` 一并迁移）；文件相关限流已在 M8 落地；登录限流（M2）已有。
  - 备份与恢复演练。
  - 压力测试：长连接数、消息吞吐、离线同步峰值。
- **验收标准**：能回答"当前多少在线用户、消息延迟多少、失败率多少"；服务端异常重启后不丢已确认消息；压测报告可指导扩容。

## 5. 推荐执行顺序（2026-09-16 更新）

下一步候选按"安全欠账优先、横切能力其次、特性栈分批"排序；**具体下一任务待讨论确定**：

1. **解决历史遗留 P2 欠账**（§3 中 P2 优先）：大群分发超限、群密钥"先落盘后分发"窗口、TLS 端到端集成测试、下载票据 TTL 与断点续传、数据面请求体上限预检、文件控制面 handler 级测试。
2. **M11B 消息交互增强（§4.4）**，片内顺序：B1 多选模式 → B3 消息转发（依赖 B1 的选择集）→ B2 消息回复/引用（含 V11 迁移与服务端改动，**单独提交以便回滚**）→ B4 emoji 选择器。B2 是 M11 唯一一次协议扩展，宜排在纯 QML 的 B1/B3/B4 之后单独入库。
3. **M11C 群聊与布局完善（§4.5）**，片内顺序：C1 群信息抽屉 → C3 响应式布局（依赖 C1 的抽屉组件）；C2 最后在线时间（需服务端回填）可并行。
4. **M11 稳定性剩余项（§4.6）**：指标监控、崩溃捕获与客户端日志上报、备份恢复演练、压测（持续，可与 M11B/M11C 并行）。
5. **M8 遗留收尾**：视频动态画面渲染（需自定义 QSGNode 渲染 `QVideoFrame`）、文件消息持久化 outbox、大群拉取/游标模式与改群名接口。
6. **M11 未涵盖的体验项**：国际化（.ts）——如需另立项。

## 6. 目录结构

> 该部分见 `ARCHITECTURE.md`。

## 7. 数据库演进

- **服务端**（SQLite，版本化迁移，当前 V10）：`schema_version`、`users`、`devices`、`sessions`、`login_audit`、`contacts`、`conversations`（V7 增 `name`）、`conversation_members`（V7 增 `role`、V9 增 `pinned`/`muted`）、`messages`（V9 增 `edited_at`/`deleted`，V10 增 `file_id`）、`message_receipts`、`sync_events`、`device_identity_keys`（M6，仅公钥）、`prekeys`（M6，仅公钥）、`sync_meta`（V8，清理水位线）、`files`（V10/M8：密文侧元数据，`blob_key` UNIQUE + 状态机 + 两个回收/配额索引）、`file_tickets`（V10/M8：只存票据 SHA-256 摘要，无明文列）。中长期若需多人并发/多实例部署，迁移 PostgreSQL/MySQL，并尽早抽象 Repository/DAO；对象存储已以 `IObjectStorage` 抽象，多实例部署时需同步换为共享存储。**M10 会话整表删除未引入迁移**（仍 V10）：复用既有表，删除在事务内按外键安全顺序显式执行（`message_receipts → messages → conversation_members → conversations`），不依赖 `PRAGMA foreign_keys`（该 pragma 仅在 `openDatabase()` 设置）。
- **客户端 LocalStore**（SQLite，按账号+设备隔离，正文加密落库）：`schema_meta`、`messages`（M9 增 `edited_at`/`deleted`）、`conversations`（含群名/成员数、M9 增 `pinned`/`muted`，置顶按 pinned DESC 排序）、`outbox`（含 `conversation_id`）、`decrypt_cache`、`meta`（同步游标）、`sender_keys`（M7b：chain key/Ed25519 签名密钥对/迭代数，登出保留；"最新密钥"按 `rowid DESC` 选取）、`sender_key_skipped`（2026-09-09：跳序消息密钥缓存，整体密文 blob，与 `sender_keys` 同主键维度，退群一并清理）。

## 8. 安全注意事项

- 不要把 SHA-256 当作密码存储方案；它太快，不适合抵抗离线撞库。
- 不要自己设计未经验证的密码学协议；优先参考成熟方案和库。
- 不要在日志中输出密码、token、私钥、验证码、完整密文密钥材料。
- 不要把服务端能解密的"普通加密聊天"宣传成端到端加密。
- 端到端加密需要明确密钥验证、设备更换和历史消息恢复策略。
- 对所有外部输入做长度限制、格式校验和速率限制。
- ratchet/循环类解密路径必须设步数与参数上限（M7b DoS 教训：恶意 `iteration` 可迫使接收端长时间运算）。
- 单向链式密钥（ratchet）与业务主键顺序不一致时必须提供乱序容忍：“密文可被事后覆写”的功能（如消息编辑）会使密钥迭代号与消息 id 解耦，而按 id/seq 升序批量解密会先推进链状态，使后到的低迭代号消息被回滚检查永久拒绝（明文不可恢复）。必须缓存被跨越迭代的消息密钥（skipped message keys）并设容量上限。
- 服务端自发推送应覆盖操作者本人的其他设备（多端一致），由客户端按发起设备去重，而不是在服务端排除整个用户。
- 以时间戳作“最新”排序依据时必须确认精度：秒级时间戳 + 随机值作并列破口等于把选择结果交给运气（`latestSenderKeyId` 教训）；应改用单调递增序号（如 SQLite `rowid`）。
- 条件判定与状态迁移必须合并为单条 SQL 语句（M8 配额 TOCTOU 教训）：“先读计数后插入”的分步写法在多设备/多线程并发时会集体读到“未满”而全部放行，使软配额形同虚设；同理，“先查引用再删行/改状态”也会让并发请求在两步之间建立引用，随后数据被销毁。
- 声称“某状态迁移后不可能再发生 X”类不变量时，必须确认判定与写入在同一原子单元内（M8 回收教训）。跨线程窗口无法用“读得够晚”消除，只能两侧各上一道防护：写入侧把条件下推为单语句守卫，销毁侧在执行前再判一次；并且让最坏结果落在“可修复”而非“不可恢复”一侧。
- 销毁性操作必须排序：“先保证不产生孤儿数据、再销毁”。删磁盘与删元数据不可兼得时，宁可留下可被下一轮回收的隐形孤儿，也不得产出“元数据指向已消失数据”的不可自愈状态。
- 错误分类必须区分“数据故障”与“存储/瞬时故障”：前者重传同批输入只会得到同样结果（应标失败并回收），后者应保留现场让调用方重试；把写满/改名失败误报成校验和错误会造成客户端无限重传（M8 `finalize` 六分类教训）。
- 顺序整数主键（如 `fileId`）对外暴露时，“不存在”与“无权”必须合并为同一错误码，否则任何已登录用户可遍历判定他人资源的存在性与状态（元数据枚举预言机）；同理，票据校验的多种失败原因也不得向调用方区分。真实原因只进服务端日志。
- 客户端加密后再上传的大对象应使用**分片独立 AEAD 且把分片序号绑入 nonce/AAD**，不要复用消息 ratchet：链式密钥一旦推进，早先分片就永久不可解，而文件需要可重复下载与可转发（M8 刻意与 M9 编辑教训对齐的取舍）。
- 存储路径拼接绝不使用用户可控字符串：存储键由服务端分配且每次访问前重校形态，从根源排除路径穿越；写入统一走“临时文件 + 原子改名”，避免崩溃/写满留下被当作完整数据的半截文件。
- 携密钥的结构（如文件清单）必须有**唯一脱敏出口**，且每一条通向 UI/JS 的路径都经它：只在“实时推送”一条路径上脱敏等于没做（离线补收、历史翻页、本地缓存回填同样会把密钥渲染上屏），而字符串一旦进入 JS 堆就无法可靠清零，还会被截图/日志/调试器捕获。出口处再加一道“形态像就置空”兜底，使将来新增的消息出口不会重蹈覆辙（M8.2 P0 教训）。
- 客户端本地缓存优先“缓存传输密文原文”而不是“解密后再加密”：E2EE 场景下密文本身就是最好的静态保护，零额外开销且与“磁盘无明文”口径天然一致；密钥只在清单里，而清单已由本地存储密钥加密落库。
- 把引用传给会销毁容器的函数时必须先取副本：`erase` 之后再 `emit` 一个指向已销毁节点的 `QString&` 是 use-after-free（M8.2 P0：每次任务成功/失败/取消都会走到）。
- 遍历容器时若循环体可能删除元素，必须先取键快照；会递归推进自己的调度器必须有重入护栏，否则嵌套泵送会破坏“串行”承诺并产生并发请求（M8.2 P0）。
- 重试预算不得被“恢复动作的成功”清零：数据面持续故障而控制面正常时，每次查询都成功，若用它重置计数就会形成“上传-失败-查询-重上传”的活锁（永不放弃、UI 无失败态、持续向服务端灌大请求体）；恢复轮次需单独封顶。

## 9. 每个迭代的完成定义

> 该部分已移至 `AGENTS.md`，见“完成定义”章节。

## 10. 变更记录

里程碑级事件与关键修复的**一行摘要**；详细过程（逐项任务、审查逐条修复、文档同步明细）经 `git log` 追溯，能力结论见 §1.2/§2/§4。

| 日期 | 事件 | 摘要 |
| --- | --- | --- |
| 2026-07-01 | M0/M1 完成 | 工程基线与协议层重构落地 |
| 2026-07-29 | M2/M3/M4 完成 | 账户体系、一对一聊天 MVP、QML UI 重构落地 |
| 2026-08-03 | 审查更正 + M5/M5.5 完成 | 更正 M3/M4/M5 中被提前标记完成的条目；新增并完成 M5.5 安全加固 |
| 2026-08-04 | M4.5 完成 | 亮暗主题、搜索发起对话、乐观发送、已读回执及 8 项验证期缺陷修复 |
| 2026-08-17 | M6 完成 | 一对一 E2EE（简化 Signal），审查修复预密钥泄漏/耗尽、身份轮换静默丢消息 |
| 2026-08-20 | M6 联调修复 | 离线发送保留重试不丢弃；自身拷贝条目 + 持久化解密缓存解决登出重登解密；V6 迁移 |
| 2026-08-21 | 路线图调整 + M6.5/M7a 完成 | 新增 M6.5；M7 拆 M7a/M7b；M9 范围收缩；M6.5 与 M7a 三子任务落地 |
| 2026-08-22 | M7a.3 热修复 + M7b 实现 | 修复群聊联调崩溃（QML delegate 悬空通知端点，提交 `bafd4f6`）；M7b Sender-Key 群 E2EE 实现并双客户端联调通过 |
| 2026-08-26 | 安全审查 | 发现 ratchet 循环无上限（DoS）、服务端群消息缺 envelope fail-closed、`validateSession()` 仅查内存态三项问题 |
| 2026-09-02 | 安全修复 + M7b 入库 + 文档重构 | DoS 上限与服务端群 envelope fail-closed 落地；M7b 连同修复提交（`c806d90`）；ROADMAP 完全重构（欠账清单集中管理） |
| 2026-09-02 | P1 欠账修复（3 项） | `validateSession()` 逐请求回查 + 过期 fail-closed；逐包验 token；群成员变更 Sender-Key healing。新增大群分发上限/先落盘后分发等欠账 |
| 2026-09-03 | M11 前置两项完成 | 发消息/搜索限流（通用 `RateLimited` 1003 + `RateWindow`）+ 结构化日志（`StructuredLogger` 单行 JSON + `sendResponse` 中央审计）。销账两项 P2 |
| 2026-09-04 | M9 核心一致性完成 | 已读多端同步（`ReadCursorNotification (80)` + `read_cursor` 事件）+ `sync_events` 保留清理（V8 `sync_meta` 水位线、30 天、`needsFullSync` 全量回退）。销账两项 P2 |
| 2026-09-04 | 会话续期与失效重登完成 | 过期前 1 天自动 `renewToken`（60 秒看门狗 + 失败退避）；`SessionInvalid/SessionExpired` 触发安全清零并回登录页；`parseExpiresAt` 校正时区解析 |
| 2026-09-05 | M9 特性栈完成 | 会话置顶/免打扰 + 消息编辑/删除（软删除留墓碑）；协议 81-87 + V9 迁移 + 三类 sync_events 事件；QML 右键菜单 |
| 2026-09-09 | 周度审查 + M9 链路修复 + CI 重建 | 修复群编辑解密失败（事件补 `senderId`/`originDeviceId` + 兼容反查）、ratchet 与消息 id 顺序解耦（跳序密钥缓存 + `sender_key_skipped` 表）、编辑/删除推送覆盖本人其他设备、删除写入 fail-closed；更正 `TestLocalStore` 非确定性根因（`latestSenderKeyId` 改 `rowid DESC`，修复后连跑 20 次 0 失败）；重建 CI 工作流并登记"首次运行未验证"为 P2。新增 9 个回归用例；CodeReview 无 P0，P1/P2 已修 |
| 2026-09-10 | M8 前置 P2/P3 欠账清理 | 三端点接入连接级限流；编辑/删除响应改多槽匹配 + 私聊编辑队列串行；新增专用推送类型 88/89；新增 `TestNetworkManager`；项目根新增 `AGENTS.md`；`ctest` 7/7。CodeReview 两项 P1（断线堵死编辑泵、泵送契约不成立）已修 |
| 2026-09-10 | M8.1 完成：文件与对象存储地基 | `FileProtocol`（90-99/3013-3021/清单/分片数学）+ `FileCrypto`（分片独立 AEAD，刻意不复用 ratchet）+ `IObjectStorage`/`LocalFileStorage` + V10（`files`/`file_tickets`/`messages.file_id`）+ 控制面五处理器 + 三轮回收；`ctest` 9/9（227 用例）。两轮 CodeReview 的 P1/P2/P3（枚举预言机、存储层并发、配额 TOCTOU、"终态不可再引用"跨线程不成立）均已修。提交 `e8b6df1` |
| 2026-09-11 | M8.2 完成：数据面 HTTP(S) + 客户端上传下载与 UI | `FileHttpService`（PUT 分片 / GET + Range，票据走请求头、失败统一 401、单次 GET 上限 4 MiB、per-IP 只计失败）+ `FileTransferManager`（两遍加密上传、双层重试预算、下载自校验）+ 密文原样落盘 + `FileManifest` 补必填 `chunkSize` + QML 文件气泡与附件入口；`ctest` 11/11。CodeReview 3 项 P0（清单密钥经三条路径泄入 QML、任务容器悬垂引用、泵送中 erase 致迭代器失效）与 3 项 P1 均已修；集成测试抓到 `bind()` 需先 `listen()` 的真实缺陷 |
| 2026-09-11 | M8.3a 完成：图片元数据与内联缩略图 | `ThumbnailMaker`（纯 QtGui，最长边 160px，压不进上限就不内联，EXIF 校正）+ 清单 `width`/`height`/`thumb` + 气泡内联缩略图；`ctest` 12/12 |
| 2026-09-11 | M8.3c 完成：应用内大图查看器 | `FileImageProvider`（`image://xyfile/<id>`，逐片解密在内存、明文不落盘）+ 64 MiB 内存解码上限 + 预览对话框；同时修复 `Qt.urlToLocalFile()` 导致的附件/另存为 P0 运行时错误 |
| 2026-09-11 | M8.3b 完成：音视频元数据与播放器 | `MediaMetadataExtractor`（异步提取时长/分辨率/封面）+ 上传改三阶段 + `DecryptingIODevice`/`MediaPlaybackManager`（流式解密播放，明文不落盘）+ 历史图片缩略图本地补齐。M8.3 全部完成；集成测试抓到 seek 片内偏移错位缺陷。登记"视频无动态画面"欠账 |
| 2026-09-14 | M10 完成：UI 重构与体验完善（保守范围） | Phase 0-2 纯 QML 重构（Theme token、SVG 图标库 + `Icon`、基础组件库、全局反馈层、`MainPage` 拆 9 个 Dialog、消息气泡重构、未读分隔线 + FAB、多行输入区）；Phase 3 会话整表删除（`100-102`，仅群主可删、通知全体前成员）与"正在输入"（`103-105`）；Phase 4 六项历史欠账清理（异步化采单线程时间片泵送）。`ctest` 12/12、`qmllint` 零错误。CodeReview 无 P0，1 项 P1 + 2 项 P2 已修，2 项 P3 入表 |
| 2026-09-15 | UI 缺陷修复（两轮） | ① 暗色主题下图标全黑（`MultiEffect` 缺 `brightness: 1.0`，乘法着色使黑色源恒为黑）、文件气泡尺寸异常（漏算文件面板宽度）、空会话列表永久"加载中"；② 发送文件后气泡需手动刷新（服务端 fan-out 排除发送者且文件消息无法乐观插入 → 改发送确认后回显）、消息气泡悬停控件遮挡正文（改气泡外侧，窄窗口退回内部）、弹出菜单未与主题统一（Basic 样式取系统调色板 → 新增 `AppMenu`/`AppMenuItem`）。另收口"正文→会话预览文本"为唯一实现 `Protocol::filePreviewText()` 并补回归单测。`ctest` 12/12、`qmllint` 零错误 |
| 2026-09-15 | M11A 完成：基础体验与系统集成 | 设置页（`AppSettings` 替代 `ThemeSettings`，统一管理 darkMode/通知/预览/托盘偏好 + `SettingsDialog` 外观/通知/存储/关于四节）；系统托盘（`TrayManager` + `QSystemTrayIcon`，`QApplication` + `setQuitOnLastWindowClosed(false)`，关闭最小化到托盘、双击恢复、右键菜单退出）；桌面通知（`NetworkManager.handleNewMessageNotification` 中按设置/免打扰/非本人过滤后 emit → `TrayManager.showMessage`，点击跳转会话，当前活动会话抑制）；草稿（`MainPage.drafts` JS 对象，会话切换保存/恢复输入框文本，发送后清除）；本地消息搜索（`LocalStore.searchMessages` 解密后内存 LIKE 匹配 + `conversationName` 附带，`LocalSearchDialog` 搜索/结果列表/跳转，`ChatView.scrollToMessage` 定位滚动，`pendingScrollMessageId` 机制解决异步加载时序）。`ctest` 12/12（`TestLocalStore` +2 用例）、`qmllint` 零错误。CodeReview 3 项 P1（私聊通知冗余前缀、托盘不可用窗口永久隐藏、搜索跳转时序失败）+ 1 项 P2（QMenu 泄漏）均已修 |
| 2026-09-16 | M11A 复审修复 + 规划补全 | 复审发现 1 项 P1 功能回退——消息搜索顶替了侧边栏搜索按钮，致"用户搜索→发起一对一会话"（M4.5 能力）失去 UI 入口：已修为会话列表工具栏独立"消息搜索"按钮（新增 `search-messages` 图标），原搜索按钮恢复打开 `SearchDialog`；并补"目标会话已不在列表"的提示。ROADMAP 补全 M11B/M11C 规划章节（§4.4/§4.5，原 M11 稳定性任务顺延为 §4.6）与 §5 片内执行顺序 |
| 2026-09-16 | 修复 CI 配置缺陷：Qt add-on 模块缺失 | 重建后的 CI 首次运行在 configure 阶段失败：`jurplel/install-qt-action` 默认只装 Qt base（qtbase + qtdeclarative/qtquickcontrols2），而项目依赖的 `Qt6Multimedia`（客户端元数据提取与播放器、测试）与 `Qt6HttpServer`（服务端数据面、测试）属独立 add-on 模块；且 `Qt6HttpServer` 的 CMake 包 `find_dependency` 了 `Qt6WebSockets`。修为在安装步骤显式声明 `modules: qtmultimedia qthttpserver qtwebsockets` 并固定 `arch: win64_msvc2022_64`，不再依赖安装器的模块依赖自动解析。OpenSSL/QWindowKit/zlib 仍由已提交的 `3rdparty/` 提供，CI 无需另行安装 |
