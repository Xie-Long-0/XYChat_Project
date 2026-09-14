# XYChat 长期实现路线图

本文档面向 Qt/C++ Client + Server 基础框架，目标是逐步演进为一个"类 Telegram"的安全即时通信系统。路线图按"先稳定基础，再做通信能力，再做安全与规模化"的顺序推进，便于长期迭代、验收和回滚。

> 本文档于 2026-09-02 完全重构：已完成里程碑压缩为能力摘要（逐项勾选清单与实施流水账不再保留，历史细节经 `git log` 与 `docs/ARCHITECTURE.md` 追溯）；新增集中管理的欠账清单；M8-M11 规划按代码现状重写；原头部 11 条更新块与原 Sprint 看板合并入文末"变更记录"表。

## 0. 文档定位与维护约定

- **状态标注**：里程碑状态取 `已完成` / `未开始` / `进行中`；节内未实施项以"未实现"文字标注，不使用悬挂的空复选框。
- **更新方式**：里程碑完成时同步更新四处——① 状态总表；② 已完成能力摘要；③ 欠账清单（新增或销账）；④ 变更记录表。**禁止**再向文档头部追加流水账式更新引用块。
- **一致性要求**：涉及协议/安全/架构事实的表述必须与 `docs/PROTOCOL.md`、`docs/SECURITY.md`、`docs/ARCHITECTURE.md` 及代码一致；发现文档与代码不符时，以代码为准并在当期修正文档。
- **完成定义**：见第 9 节；里程碑勾选"已完成"前必须通过对应自动化测试与代码审查。

## 1. 项目现状总览（截至 2026-09-11）

### 1.1 里程碑状态总表

| 里程碑 | 名称 | 状态 | 完成日期 | 交付摘要 |
| --- | --- | --- | --- | --- |
| M0 | 工程基线与可维护性 | 已完成 | 2026-07-01 | README/构建说明、`.gitignore`、`docs/` 四文档、GitHub Actions CI、Qt Test 引入、CMake 工程统一（**更正**：CI 工作流曾于 2026-08-03 随 `31622df` 被删除，其后 5 周内本表仍声称“CI 全流程通过”；2026-09-09 已重建 `.github/workflows/cmake.yml`，详见 §2 M0 与变更记录） |
| M1 | 网络协议层重构 | 已完成 | 2026-07-01 | `Packet`/`PacketCodec` 长度前缀帧协议、requestId 匹配、统一错误码、ping/pong 心跳与空闲超时 |
| M2 | 账户体系与认证安全 | 已完成 | 2026-07-29 | 注册、PBKDF2-HMAC-SHA256 密码存储、session token、多设备管理、登录限流、版本化数据库迁移 |
| M3 | 一对一文本聊天 MVP | 已完成 | 2026-07-29 | 用户搜索/联系人、会话模型、消息收发/状态/离线同步接口、客户端聊天界面 |
| M4 | 客户端 QML UI 重构 | 已完成 | 2026-07-29 | QWindowKit 无边框窗口、Telegram 风格 QML 全套页面组件、NetworkManager QML 适配 |
| M4.5 | M4 遗留清理与聊天完善 | 已完成 | 2026-08-04 | 亮/暗主题切换、搜索直接发起对话、乐观发送、显式已读回执，及 8 项 E2E 验证期缺陷修复 |
| M5 | 传输层加密与会话安全 | 已完成 | 2026-08-03 | TLS 1.2+（QSslSocket）、重放保护字段、日志脱敏、安全内存 |
| M5.5 | 安全加固（审查修复） | 已完成 | 2026-08-03 | TLS fail-closed、timestamp/nonce 强制 + 全局 TTL 去重、会话/消息先授权再查询、`clientMessageId` 幂等、per-recipient 回执模型、账号级 `sync_events` 游标 |
| M6 | 端到端加密一对一聊天 | 已完成 | 2026-08-17 | 简化 Signal 方案：X25519 身份密钥 + 一次性预密钥 + 每消息临时密钥 ECDH + HKDF-SHA256 + AES-256-GCM envelope；服务端 fail-closed 只存密文；TOFU（2026-08-20 追加修复离线发送丢失与重登解密两项联调缺陷） |
| M6.5 | 本地持久化缓存与 outbox | 已完成 | 2026-08-21 | `LocalStore` 按账号+设备隔离的加密本地库（会话/消息/持久化 outbox/解密缓存/同步游标），缓存先行展示 + 游标增量同步 |
| M7a | 明文群聊 | 已完成 | 2026-08-21 | 群管理五接口（建群/邀请/退群自动转让/踢人层级保护/群信息）、群消息 fan-out + sync_events 兜底、系统消息与群变更通知、按人数回执聚合、客户端群聊 UI（2026-08-22 热修复联调崩溃：QML 会话列表差分更新、移除 add 动画、LocalStore 连接自愈） |
| M7b | 群聊端到端加密（Sender Keys） | 已完成 | 2026-09-02 | 每发送方每群独立 chain key + Ed25519 签名，HKDF ratchet 派生消息密钥，AES-256-GCM 加密；sender-key 经 M6 pairwise E2EE 分发；`fetch_group_keys`（类型 71/72）；服务端群 envelope fail-closed 校验；DoS 上限防护（`MaxRatchetSteps=2000`/`MaxMessageIteration=1e8`）；双客户端联调通过 |
| M8 | 媒体、文件与对象存储 | 进行中 | — | **M8.1（协议与存储地基，2026-09-10）+ M8.2（数据面与客户端，2026-09-11）已完成**：控制面（类型 90-99、错误码 3013-3021）、`FileManifest`（含必填 `chunkSize`）、`FileCrypto` 分片独立 AEAD、`IObjectStorage`/`LocalFileStorage`、V10 与五处理器；HTTP(S) 数据面（`QHttpServer` + `QSslServer`，票据授权/Range/长度校验/per-IP 失败限流）、客户端 `FileTransferManager`（两遍加密上传/流式下载/密文缓存/另存）、`fileTransferBaseUrl` 下发、QML 附件与文件气泡。**M8.3 全部完成（2026-09-11）**：M8.3a 图片尺寸 + 内联缩略图 + 图片气泡预览、M8.3c 应用内大图查看器、M8.3b 音视频元数据（时长/分辨率/视频封面，QtMultimedia 异步提取）与应用内播放器（C++ QMediaPlayer + DecryptingIODevice 流式解密，明文不落盘）+ 历史图片缩略图本地补齐；视频动态画面渲染受 QML 限制未做（已登记欠账），见第 4.1 节 |
| M9 | 多端同步与离线一致性 | 已完成 | 2026-09-09 | 已读状态多端同步（`read_cursor` 事件 + `ReadCursorNotification` 推送 + `markConversationRead` 未读重算）+ `sync_events` 保留清理（30 天/每小时，落后设备 `needsFullSync` 全量回退）（2026-09-04）；特性栈（2026-09-05，提交 `8224464` 于 09-07）：会话置顶/免打扰 + 消息编辑/删除（软删除留墓碑），协议类型 81-87 + V9 迁移 + `conversation_prefs`/`message_edited`/`message_deleted` 三类 sync_events 事件；**2026-09-09 修复群聊编辑解密链路**（事件补 `senderId`/`originDeviceId`、跳序消息密钥缓存、推送覆盖本人其他设备、删除写入 fail-closed），验收标准自此成立 |
| M10 | 搜索、通知与体验完善 | 未开始 | — | 见第 4.3 节 |
| M11 | 稳定性、可观测性与运维 | 未开始 | — | 登录限流已在 M2 落地，密钥拉取连接级限流已在 M6/M7b 落地；**M11 前置两项（发消息/搜索限流 + 结构化日志）已于 2026-09-03 提前落地**，其余见第 4.4 节 |

### 1.2 能力矩阵

| 能力域 | 现状 |
| --- | --- |
| 账户与认证 | 注册/登录/登出/token 续期/`terminate_session`（仅本人其他会话）；PBKDF2 密码存储；登录失败限流（IP 5min/10 次、用户 5min/5 次）；多设备识别（`deviceId` 取自机器唯一 ID）；**逐包验 token + `validateSession()` 回查 `sessions` 表**（2026-09-02 P1 修复：过期/终止/续期换代即时失效，`expiresAt` 解析异常 fail-closed）；**会话自动续期 + 失效自动重登**（2026-09-04：过期前 1 天自动 `renewToken`、续期响应 60 秒看门狗兜底、失效回登录页提示重新登录）。**未实现**：双因素认证、注销/找回 |
| 一对一聊天 | E2EE（envelope 密文，服务端 fail-closed）、`clientMessageId` 幂等、乐观发送 UI、per-recipient 回执（delivered/read）、消息状态实时推送、离线 outbox（加密持久化，跨重启重发） |
| 群聊 | 建群/邀请/退群（群主自动转让）/踢人（角色层级保护）/群信息；群 E2EE（Sender Keys，服务端只见密文）；**成员变更 Sender-Key healing**（2026-09-02 P1 修复：`member_added/removed/left` 触发本端轮换+重分发，新成员获密钥、被移除成员失后续解密能力，离线经 `sync_events` 补偿）；系统消息（成员变更胶囊渲染）；小群直推 fan-out + sync_events 兜底；按接收用户人数聚合的送达/已读计数。**未实现**：大群拉取模式、改群名接口（数据层已就绪） |
| 本地存储 | `LocalStore`（SQLite，按账号+设备隔离）：消息/会话预览/outbox/解密缓存 AES-256-GCM 加密落库，存储密钥 DPAPI 保护；M7b 起含 `sender_keys` 表，2026-09-09 起含 `sender_key_skipped` 表（跳序消息密钥缓存，密文落库）；登出清用户可见数据、保留密钥材料 |
| 多端同步 | 账号级 `sync_events` 事件流（message/receipt/contact_added/group_changed/conversation_prefs/message_edited/message_deleted/read_cursor）+ 设备本地游标，登录后缓存先行 + 增量拉取（hasMore 自动续拉）；已读状态、会话偏好、消息编辑/删除均多端一致（实时推送 + sync_events 兜底）。**未实现**：会话整表删除（无对应接口/事件） |
| 传输安全 | TLS 1.2+ fail-closed（服务端无证书拒启、客户端无 CA 拒连，开发明文需显式开关）；重放保护（timestamp ±300s + nonce 全局 TTL 600s 去重）；日志脱敏（LogSanitizer）；结构化日志（StructuredLogger 单行 JSON，M11 前置）；发消息/搜索/密钥拉取/编辑删除/会话偏好连接级限流（RateWindow；`send_message` 30/10s、`search_users` 20/60s、edit/delete 共用 20/60s、prefs 30/60s、`fetch_keys` 60s/20）；编辑/删除事件专用推送类型 `MessageEditedNotification (88)`/`MessageDeletedNotification (89)` |
| 客户端 UI | QML/Qt Quick + QWindowKit 无边框双窗口（登录/主窗口独立）；Telegram 风格主题（亮/暗切换持久化）；群聊三对话框（建群/群信息/邀请）；群 E2EE 状态横幅；会话右键菜单（置顶/免打扰）与消息右键菜单（编辑/删除） |
| 文件与媒体传输 | **M8.1 地基（2026-09-10）+ M8.2 数据面与客户端 + M8.3 多媒体元数据与播放器（均 2026-09-11）**：控制面走 TCP 主通道（类型 90-99），数据面为独立 HTTP(S) 服务（`QHttpServer` + `QSslServer`，与主通道同一套证书与 fail-closed 口径，`--http-port`/`--http-host` 可配，基地址由登录响应的 `fileTransferBaseUrl` 下发而不是客户端猜端口）；文件字节客户端加密后上传（每文件独立 AES-256 密钥 + 分片独立 AEAD，nonce/AAD 绑定分片序号），文件名/MIME/明文大小/文件密钥只在 `FileManifest` 内随消息正文经既有 E2EE 分发，服务端只见密文与密文侧元数据；客户端 `FileTransferManager`（两遍加密上传、串行分片、失败后先查已收分片再重传、流式下载 + 整体摘要自校验、进度/取消/重试）；本地缓存**密文原样落盘**（零额外加密开销，磁盘上天然不是明文，sha256 命名 + 256 桶 + 无后缀），明文只在用户“另存为”时写出；清单含密钥，**经唯一脱敏出口 `sanitizeForUi` 拦截，四条 UI 路径（实时推送/历史翻页/离线补收/本地缓存回填）均不进 QML/JS 引擎**；QML 附件按钮与文件气泡（图标/名/大小/进度/下载/另存，编辑项对文件消息禁用）；图片消息展示内联缩略图（`data:image/jpeg;base64,`，随清单经 E2EE 到达，下载原图前即可预览）；**应用内大图查看器**（`image://xyfile/<messageId>` 由 `FileImageProvider` 从密文缓存逐片解密并在内存中解码，明文不落盘；标题带文件名与像素尺寸；footer 提供“另存为”）；**音视频元数据与应用内播放器（M8.3b）**：上传时 `MediaMetadataExtractor`（QMediaPlayer + QVideoSink）异步提取时长/分辨率/视频封面帧填入清单 `durationMs`/`width`/`height`/`thumb`（元数据缺失绝不阻断发送），气泡展示时长标签与播放按钮；播放经 `MediaPlaybackManager`（C++ QMediaPlayer + `DecryptingIODevice` 从密文缓存流式逐片解密喂给 `setSourceDevice`，明文不落盘），QML 播放器对话框控制播放/暂停/进度/音量；历史图片消息（M8.3a 前发送、清单 thumb 为空）下载后由 `localThumbnailForMessage` 本地生成缩略图并缓存到 `<cacheRoot>/thumbs/`。**未实现**：视频动态画面渲染（QML VideoOutput 无法绑定 C++ QVideoSink，当前视频播放只输出音频轨 + 静态封面，需自定义 QSGNode）、跳重启的文件消息持久化 outbox |

## 2. 已完成能力摘要

各里程碑的目标、关键交付、验证证据与已知限制。实施细节（逐项任务清单、审查修复过程）经 `git log` 与 `docs/ARCHITECTURE.md` 追溯。

### M0：工程基线（2026-07-01）

- 交付：README（构建/运行/依赖/目录）、`.gitignore`、`docs/` 架构/协议/安全/路线图四文档、GitHub Actions CI（configure/build/test）、Qt Test 框架与 `TestEncryptionManager`、顶层 CMake 统一（C++20、警告选项、`XYCHAT_BUILD_TESTS` 开关）。
- 验证：新开发者按 README 可构建启动；CI 全流程通过。
- **更正（2026-09-09 周度审查）**：上述“CI 全流程通过”自 2026-08-03 起已不成立——`.github/workflows/cmake.yml` 随提交 `31622df`（“update”）被删除，仓库连续 5 周无任何机器门禁，期间各里程碑的 “`ctest` 6/6” 结论均仅靠本地手工运行得出（并因此遗漏了一项约 50% 概率失败的单测，见 M9）。工作流已于 2026-09-09 重建（windows-latest + Qt 6.8.3，configure/build/ctest 三步，失败时上传 `LastTest.log`）；**首次运行结果待验证**（本地无法复现 GitHub runner 环境），已登记为 §3 P2 欠账。

### M1：网络协议层（2026-07-01）

- 交付：`CommonModule/protocol` 长度前缀帧协议（magic `XYCP` + version + messageType + requestId + payloadLength，payload 上限 4 MiB）；客户端连接状态机（未连接/连接中/已连接/登录中/已认证/断线重连）；服务端连续包处理；ping/pong 心跳与 90 秒空闲超时。
- 验证：`TestPacketCodec`（连续 1000 小包、大包分片到达）。

### M2：账户体系（2026-07-29）

- 交付：注册（用户名主标识，邮箱/手机可选）；PBKDF2-HMAC-SHA256（100K 迭代 + 16B 随机盐 + 参数版本 `v1:`）；session token（服务端只存 SHA-256 摘要，7 天有效期）；`users`/`devices`/`sessions`/`login_audit` 表拆分；版本化迁移机制（`schema_version`）；登录限流；token 续期与 `terminate_session`。
- 验证：`TestDatabaseManager`（注册/session/审计/迁移）、`TestEncryptionManager`（PBKDF2/常数时间比较）。

### M3：一对一聊天 MVP（2026-07-29）

- 交付：用户搜索、双向联系人；`conversations`/`conversation_members`/`messages` 模型；`send_message`/`ack_message`/`sync_messages`；客户端会话列表、聊天窗口、消息气泡、五态消息状态；服务端递增消息 ID。本地缓存项当时未实施，由 M6.5 承接落地。
- 验证：`TestDatabaseManager` 消息/会话用例；双客户端实时收发与离线补收人工验证。

### M4 + M4.5：QML UI（2026-07-29 / 2026-08-04）

- 交付：QWindowKit 无边框窗口（自定义标题栏/拖拽/Snap Layout）；`LoginPage`/`MainPage`/`ConversationList`/`ChatView`/`MessageInput`/`MessageBubble`/`TitleBar` 组件；`Theme.qml` darkMode 双配色 + `ThemeSettings` 持久化；旧 Widgets UI 删除。M4.5 补齐：搜索直接发起对话（虚拟会话 + 首条消息 ACK 后绑定）、乐观发送、显式已读回执、日期分隔线、未读角标本地更新、登出入口；验证期修复 8 项缺陷（delegate 渲染空白、滚动/贴底、气泡自适应、头像色绑定、主窗口任务栏显示等）。
- 验证：qmllint；M1-M3 功能在 QML 下回归通过；E2E 人工验证。

### M5 + M5.5：传输安全与加固（2026-08-03）

- 交付：TLS 1.2+（开发自签 CA 自动生成，SAN localhost/127.0.0.1）；fail-closed（无静默降级路径，开发明文需 `--allow-plaintext`/`XYCHAT_ALLOW_PLAINTEXT=1` 显式开关）；timestamp/nonce 强制必填 + 全局 `NonceCache`（TTL 600s、上限 10 万条、跨连接）；会话/消息接口先授权再查询（`isConversationMember`/`canAccessMessage`，越权 3006）；`force_logout` 改 `terminate_session`（仅本人会话）；handler 线程内发送代理（消除跨线程写 socket）；`clientMessageId` 幂等键 + 部分唯一索引 + 内存 outbox；`message_receipts` 回执表 + 成员读游标（只前进）；`sync_events` 账号级游标接口。
- 验证：`TestSecurity`（nonce 系列）、`TestDatabaseManager`（越权拒绝/幂等去重/回执聚合/读游标单调/sync_events 游标）。
- 已知限制：nonce 缓存单服务器内存态；端到端 TLS 集成测试缺失。（逐包验 token 已于 2026-09-02 P1 修复实施，见变更记录）

### M6：一对一 E2EE（2026-08-17，08-20 联调修复）

- 交付：每设备 X25519 身份密钥 + 批量一次性预密钥（`register_keys`/`fetch_keys`，服务端只存公钥）；每消息临时密钥 ECDH + HKDF-SHA256（salt `xychat-e2ee-v1`）+ AES-256-GCM envelope（逐设备条目 + 发送方自身拷贝 `prekeyId=0`）；服务端 fail-closed（非法/明文正文拒绝入库，3008；入库与预密钥消费同事务）；预密钥生命周期（认领即消费、10 分钟超时回退、身份变更废弃旧世代、`fetch_keys` 连接级限流 60s/20 次）；TOFU 指纹 + 变更告警；私钥 `KeyStorage`（Windows DPAPI，临时文件+替换原子写入）；解密缓存持久化。产品决策：历史消息不可恢复（仅限丢失密钥材料场景），UI 显示"无法解密此消息"。
- 验证：`TestEncryptionManager`（原语/envelope/协商全流程）、`TestDatabaseManager`（密钥管理）；双客户端联调（08-20 修复：对方未注册密钥时 outbox 保留重试不丢弃；自身拷贝 + 持久化解密缓存解决登出重登解密）。
- 已知限制：TOFU 无带外验证；无密钥备份/设备间迁移；非 Windows 平台私钥明文回退。

### M6.5：本地持久化缓存（2026-08-21）

- 交付：`LocalStore`（AppData/localstore，`<username>_<deviceId>.db`）：消息/会话/持久化 outbox/解密缓存（归口替代 M6 `.cache` 文件，遗留自动迁入）/sync_events 游标；全部正文 AES-256-GCM 加密落库（存储密钥随机生成、DPAPI 保护、加密失败拒写 fail-closed）；登录后缓存先行展示 + 游标增量同步（hasMore 自动续拉）；登出清用户可见数据、保留解密缓存与存储密钥（E2EE 密钥材料，重登解密兜底）；连接失效自愈（`ensureUsableDb()` 重开，失败则禁用缓存）。
- 验证：`TestLocalStore`（磁盘字节级密文校验、outbox 幂等、登出语义、群字段）。

### M7a：明文群聊（2026-08-21，08-22 热修复）

- 交付：协议消息类型 60-70 与错误码 3009-3012；数据库 V7（`conversations.name` + `conversation_members.role`）；服务端五处理器（建群：创建者 owner、成员上限 200；邀请：单批 ≤100、已在群拒绝；退群：群主自动转让最早入群成员；踢人：owner 可移除 admin/member、admin 仅 member；群信息：仅成员）；`send_message` 按 `conversationId`/`toUserId` 分流，群消息 fan-out（在线直推 + 全员 sync_events 兜底）；成员变更系统消息（`contentType=system`）与 `GroupChangedNotification`/`group_changed` 事件；回执按接收用户人数聚合（`receiptUserCount` 多设备去重，`MessageStatusUpdate` 携带 deliveredCount/readCount）。客户端：群组五接口 + 群消息 outbox 分流、LocalStore 群字段、建群/群信息/邀请三对话框、群样式会话列表、系统消息胶囊、"暂未端到端加密"横幅（M7b 后改为已加密提示）。
- 验证：`TestDatabaseManager` 群组 8 用例；qmllint 零错误；08-22 热修复联调崩溃（QML delegate 悬空通知端点，见变更记录）后双客户端联调通过。
- 已知限制：大群拉取模式未实现；改群名接口未开放（`setGroupName` 数据层就绪）。

### M7b：群聊 E2EE（2026-09-02 提交）

- 交付：`CommonModule/encryption/GroupE2eeCrypto`（简化 Signal Sender Keys）——每发送方每群独立 `SenderKey`（32B chain key + Ed25519 签名密钥对，`keyId` = SHA-256(签名公钥) hex 前 32 字符）；chain key 经 HKDF-SHA256 ratchet（salt `xychat-grp-chain`）派生消息密钥；群消息 AES-256-GCM 加密 + Ed25519 签名（覆盖 `iv || ciphertext`）；群消息 envelope（`contentType=e2ee_group`）含 `keyId`/`iteration`/`senderDeviceId`；sender-key 分发（`contentType=sender_key_distribution`）复用 M6 pairwise E2EE 逐设备加密 chain key（base64）；服务端 `fetch_group_keys`（类型 71/72）一次性返回全群成员密钥包（共享 fetch_keys 限流窗口）；服务端对两类群正文 fail-closed 校验（非法返回 3008）；客户端 `LocalStore.sender_keys` 表加密保存 chain key/签名密钥对/迭代数，登出保留；分发消息只处理不展示不落库。安全修复：ratchet DoS 上限（`MaxRatchetSteps=2000`、`MaxMessageIteration=1e8`）。
- 验证：`TestGroupE2eeCrypto` 20 用例（原语/ratchet/篡改与回滚拒绝/DoS 上限/envelope 编解码/fail-closed）；`tests/e2e/TestGroupRepro` 双客户端全链路（建群→分发→加密收发→登出重登→再发）退出码 0；`ctest` 6/6 通过；M7a 验收标准一并经联调确认。
- 已知限制：成员加入/退出的密钥 healing 与失权回收已于 2026-09-02 P1 修复实施（成员变更触发轮换+重分发，见变更记录）；残留：大群（成员设备数约 >60）单条 `sender_key_distribution` 可能超 16384 字符上限致分发失败（与大群拉取模式一并留待后续）；轮换采用“先落盘后分发”，分发永久失败时存在群解密不可用窗口（沿用 M7b 既有模式，均登记为 P2 欠账）。

### M9：多端同步与离线一致性（2026-09-04 核心一致性 / 2026-09-05 特性栈 / 2026-09-09 群编辑解密链路修复）

- 交付（核心一致性）：已读状态多端同步（`ack_message(read)` 时向已读者自身 `sync_events` 追加 `read_cursor` 事件并经 `ReadCursorNotification (80)` 推送其全部在线设备；客户端 `markConversationRead` 按剩余未读重算角标、消息状态只前进）；`sync_events` 保留清理（`migrateToV8` 引入 `sync_meta` 水位线，`Server` 以独立维护连接每小时按 30 天保留期 `pruneSyncEvents`；落后于水位的设备返回 `needsFullSync` + `fullSyncSeq`，客户端重置游标并全量回退，历史消息经 `sync_messages` 从 messages 表补齐）。
- 交付（特性栈）：会话置顶/免打扰（`SetConversationPrefs` 81/82 + `ConversationPrefsNotification` 83，按成员×会话维度、多端共享）；消息编辑/删除（84-87，仅发送者可操作，编辑正文须保持原 contentType 并经服务端 fail-closed 密文校验拒绝明文注入，删除为软删除留墓碑且幂等）；数据库 V9（`conversation_members.pinned/muted`、`messages.edited_at/deleted`）；客户端全链路（请求/响应/推送/sync_events/本地缓存）+ QML 右键菜单与“已编辑/已删除”展示；`conversation_prefs`/`message_edited`/`message_deleted` 三类新事件实现离线补偿。
- 交付（2026-09-09 周度审查修复）：① `message_edited`/`message_deleted` 事件与推送补 `senderId`（群聊正文为 `e2ee_group` 密文，接收端靠它定位 Sender Key；旧实现客户端还硬置 `senderId = 0`，导致群消息编辑后在所有接收端解密失败并清空已可读正文）与 `originDeviceId`（发起设备去重）；`decryptGroupMessageObject` 增加按（群, 设备, keyId）反查发送者的兼容路径，使修复前已落库的无 `senderId` 事件仍可解；② `GroupE2eeCrypto` 引入跳序消息密钥缓存（Signal skipped-message-keys 语义，`MaxSkippedMessageKeys=1000`，命中即一次性消费、伪造输入不污染缓存与链状态），解决编辑重加密使 `iteration` 与 `message_id` 顺序解耦、离线按 id 升序补收时后续消息被回滚检查永久拒绝的缺陷；缓存经 `LocalStore.sender_key_skipped` 表加密持久化，退群随 `sender_keys` 一并清理（两表同一事务内删除，避免半清理遗留密钥材料）；缓存命中采“**先认证后消费**”（经 CodeReview 子代理审查修正：原先消费后认证会使一条伪造消息烧毁合法跳序密钥，致随后到达的真实乱序消息永久不可解）；③ 编辑/删除推送覆盖操作者本人（其名下其他设备实时一致，`onMessageForUser` 本就发给该用户全部会话）；④ `deleteMessage` 写入失败改 fail-closed（返回 `InternalError`、不广播事件、记结构化日志）；⑤ `latestSenderKeyId` 改按 `rowid DESC` 选取（原按秒级 `updated_at` 排序 + 随机 `key_id` 作并列破口，同秒写入两把密钥时选中哪把完全随机，可能用陈旧密钥加密），`updated_at` 改毫秒精度仅供诊断；⑥ 顺手销账 P3 风格欠账：`GroupE2eeCrypto.cpp` 两处 `QStringLiteral` 改为原始字面量。
- 验证：`TestGroupE2eeCrypto` 新增 6 用例（乱序解密、缓存一次性消费、伪造不污染缓存、伪造不烧毁缓存、容量上限淘汰、编辑重加密回归）共 27 passed；`TestLocalStore` 新增 2 用例（跳序密钥密文落库与维度隔离、按设备+keyId 反查发送者）并加固“最新密钥”用例，共 24 passed、**连续 20 次运行 0 失败**（修复前约 50% 概率失败）；`ctest` 6/6（连跑 3 轮全绿）；构建全目标通过；CodeReview 子代理审查无 P0，P1（缓存先消费后认证）与 P2（退群清理非原子）已修。
- 已知限制：会话整表删除未实现（无对应接口/事件）；跳序密钥缓存按整块 blob 落库，大跳跃场景下存在写放大（均见 §3）。**（2026-09-10 M8 前置清理）** 本轮已逐项销账：编辑/删除/偏好三端点接入连接级限流（edit/delete 共用 `RateWindow` 20/60s、prefs 30/60s）；编辑/删除响应匹配由单发槽位改为多槽 `m_pendingEdits`/`m_pendingDeleteRequestIds` + 私聊编辑 `m_privateEditQueue` 队列串行（连续操作不再静默丢弃）；新增专用推送类型 `MessageEditedNotification (88)`/`MessageDeletedNotification (89)`（不再靠 `requestId == 0` 区分响应与推送）；新增 `TestNetworkManager` 客户端链路层回归单测（`ctest` 7/7）；项目根新增 `AGENTS.md`。

## 3. 已知欠账与风险清单

集中管理所有已识别但未实施的修复/功能项；销账或新增时更新本表（优先级 P1 最高）。

> 2026-09-09 销账：① P3「`GroupE2eeCrypto.cpp` 使用 `QStringLiteral`」——本次触碰该文件时顺手改为原始字面量；② 群消息编辑造成的两项高危解密缺陷（事件缺 `senderId`、`iteration` 与 `message_id` 顺序解耦）与 `latestSenderKeyId` 同秒并列不确定性——已修复并补回归用例，详见 §2 M9 与变更记录；③ 「交付前无机器门禁」——CI 工作流已重建。
> 2026-09-10 销账：① P2「首次运行未验证」——CI 工作流已重建并验证首次运行通过。
> 2026-09-10 M8 前置 P2/P3 欠账清理（本轮）：① P2「`edit_message`/`delete_message`/`set_conversation_prefs` 三端点无连接级限流」——edit/delete 共用 `RateWindow` 20/60s、prefs 30/60s，超限回 `RateLimited (1003)`；② P2「编辑/删除响应匹配为单发槽位」——改为 `m_pendingEdits`（requestId→上下文多槽）+ `m_pendingDeleteRequestIds` 集合 + 私聊编辑 `m_privateEditQueue` 队列串行消费 `fetch_keys` 传输槽，连续操作不再静默丢弃；③ P2「客户端 `NetworkManager` 无单测」——新增 `TestNetworkManager`（friend 注入，覆盖编辑/删除响应匹配、88/89 推送发起设备去重、私聊编辑队列化、`parseExpiresAt`、游标/偏好信号）；④ P3「编辑/删除复用 Response 类型承载自发推送」——新增专用 `MessageEditedNotification (88)`/`MessageDeletedNotification (89)`，客户端不再靠 `requestId == 0` 区分；⑤ P3「无 `AGENTS.md`」——项目根新增构建/测试/风格/完成定义入口。构建全目标通过、`ctest` 7/7 全绿。
> 2026-09-11 M8.2（数据面与客户端）销账：① P2「M8 数据面未实施」——`FileHttpService` 已上线（PUT 分片 / GET + Range），并以 `TestFileHttpService`（11 用例）与 `TestFileTransfer`（12 用例）做真实 HTTP 回环集成验证；② P2「票据校验无生产调用点」——`validateFileTicket` 已由 `authorizeTicket` 在生产路径调用（仅 `markFileTicketUsed` 仍无调用点，因下载票据刻意允许 TTL 内重用，已并入下方 P3）；③ P3「下载票据 TTL 内可重用」保留并补充大文件场景的新风险（见下表）。
> **未销账**：P2「文件控制面五处理器无自动化测试」仍成立——M8.2 补的是**数据面**与**客户端引擎**的集成测试，`RequestHandler` 的五个 M8 处理器（鉴权与入参顺序、限流、幂等、finalize 分类）依旧只有人工复核。
> 2026-09-11 M8.2 CodeReview 新增登记：本轮审查发现 3 项 P0（清单含密钥经非推送路径泄入 QML、`finishTask`/`failTask` 对容器内 `token` 的悬垂引用、`pumpNext` 在 QHash 遍历中 erase 当前节点并递归重入）与 3 项 P1（重试预算被“成功的查询”清零致活锁、下载气泡永久卡“下载中”、发送方无法下载自己发的文件）**均已修复并补回归用例**；下表为审查提出但本轮**未修**的项。

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
| P3 | 功能 | 桌面通知；简化图片消息 | M6.5 提前项（未实施） | 分别归属 M10/M8 完整实现；图片消息的协议与存储地基已由 M8.1 提供（清单预留 `width`/`height`/`thumb`），仍待 M8.2 客户端与 M8.3 缩略图管线 |
| P3 | 工程 | 跳序消息密钥缓存按整块 blob 落库，大跳跃场景有写放大 | 2026-09-09 CodeReview | `decryptGroupMessageObject` 每次解密都会 `loadSkippedMessageKeys`（整块解密），且缓存非空时每条消息重写整块（重新序列化+加密+写库），近似 O(N × cacheSize)；上限 1000 条时单块可达数十 KB。典型编辑场景（小跳跃）影响微小，新设备/长期离线的大跳跃补收才明显。建议改为每跳序密钥一行（PK 含 iteration）+ 增量写入，或提升为批次内存态缓存 |
| P3 | 安全 | 跳序密钥序列化时以 base64 `QString` 形态短暂驻堆，无法可靠清零 | 2026-09-09 CodeReview | `SecureMemory::wipe` 对 COW/只读的 `QString` 缓冲无效，与现有 chain key/正文落库路径（`encryptText`/`decryptText` 均返回 `QString`）为同一固有限制；如需更严格的密钥卫生，序列化应走 `QByteArray` 并用后 wipe |
| P2 | 工程 | 编辑/删除在途请求（`m_pendingEdits`/`m_pendingDeleteRequestIds`）无超时清扫 | 2026-09-10 CodeReview | 若服务端漏答且连接未断（无 disconnect 触发清理），已发出的编辑/删除条目会残留至下次登出/断线；`requestId` 单调不回绕不会误配，仅无信号的内存泄漏（受 edit/delete 20/60s 限流天然封顶）。建议加 30s 超时 `erase` + 失败上报（镜像 token 续期看门狗） |
| P3 | 工程 | 私聊编辑额外占用一次共享 `fetch_keys` 预算 | 2026-09-10 CodeReview | 每条私聊编辑 = 1 次 `fetch_keys`（与发送/fetch_group_keys 共设 20/60s 窗口），编辑自身窗口亦 20/60s；混合场景可能先撞 `fetch_keys` 上限使编辑以 `RateLimited` 失败，实际有效编辑率低于标称。与 P2 三端点限流相关，待真实用量评估后调参 |
| P3 | 工程 | `TestNetworkManager` 未覆盖 pump 与编辑解密回退路径 | 2026-09-10 CodeReview | 现有 10 用例锁定 requestId 多槽匹配/消费、去重、断线清理；但 `pumpPrivateEditFetch`+`handleFetchKeysResponse` 编辑分支（需模拟 fetch 响应、会写 socket）与 88 推送“解密失败不写空”幂等回退不变量因难构造无网络环境而未断言；属测设完善，不阻塞 M8 |
| P2 | 工程 | 对象存储为单机本地文件系统，无副本/无冗余 | 2026-09-10 M8.1 | `LocalFileStorage` 磁盘损坏即文件丢失；限流与并发配额为单实例/单库口径，多实例部署需换共享对象存储（`IObjectStorage` 已抽象，可接 S3/MinIO）并把配额改为全局口径 |
| P2 | 工程 | 文件控制面五处理器无自动化测试 | 2026-09-10 M8.1 | `RequestHandler` 的 M8 处理器（鉴权与入参校验顺序、限流、幂等、finalize 结果分类、枚举预言机合并）只有人工复核；数据层（14 用例）、存储层（19 用例）、**数据面（11 用例）与客户端引擎（12 用例，M8.2 补）**已覆盖。建议复用 `TestNetworkManager` 的 friend 注入范式补 handler 级测试 |
| P2 | 工程 | 上传的摘要计算在 GUI 线程同步执行 | 2026-09-11 CodeReview | `uploadAndSend` 返回前会同步跑完第一遍流式加密 + SHA-256（为得到密文整体摘要），2 GiB 文件可阻塞界面数十秒；且此期间的 `taskProgress` 因 UI 尚未拿到 token 而全部丢弃，同步失败分支还会使上传横幅停在“准备中 0%”且无法关闭。建议拆为“同步校验返回 token + 异步 hashing”（`QTimer::singleShot(0)` 或工作线程），UI 侧改由 `tasksChanged`/`taskFailed` 维护横幅可见性 |
| P2 | 功能 | 下载票据 TTL（300s）与大文件下载不匹配，且中途不续期 | 2026-09-11 CodeReview | 2 GiB / 4 MiB = 512 次串行 Range GET，慢链路下总时长易超 300s；票据过期后回 401，而 4xx 被判为非瞬时故障 → 直接失败并删临时文件，**已下载的全部进度丢弃且无续传**。建议：提高 TTL 或改为按“最后一次使用”滑动续期，并在客户端收到 401 且 `downloadIndex > 0` 时走“重新申请票据 + 从断点续传”分支 |
| P2 | 安全 | `QHttpServer` 在进入 handler 前已缓冲整个请求体 | 2026-09-11 CodeReview | `QHttpServerRequest::body()` 返回已缓冲的 `QByteArray`，因此代码里的“超大分片早退 413”发生在内存已被消耗之后；`QAbstractHttpServer` 无内建请求体上限，**未认证**客户端可用巨大 `Content-Length` 的 PUT 造成内存放大（而 per-IP 限流只计失败，对首次请求无效）。建议：前置一层自行解析请求头做 `Content-Length` 预检，或强制要求数据面部署在带 body 上限的反向代理之后（当前 `--http-host` 默认仅回环是正确的）；并补已授权请求的并发数与字节速率上限 |
| P3 | 工程 | `LocalStore.messages` 表无 `file_id` 列 | 2026-09-11 CodeReview | 从本地缓存回填的消息不带 `fileId`，现由 `attachFileInfo` 从清单内取回（清单经 E2EE 保护且自带 `fileId`，功能等价），但少了服务端权威字段的交叉校验，且每次回填都要解一次清单 JSON。建议下一轮客户端迁移（V11）补列 |
| P3 | 工程 | `clearCache` 会删掉在途下载的临时文件 | 2026-09-11 CodeReview | `entryList(QDir::Files)` 会匹配 `<sha>.<uuid>.tmp` 并删除，随后还可能 rmdir 桶目录，使在途任务下次写入失败；返回的删除条数也把 `.tmp` 计入。影响有限（任务会明确失败而不是静默写坏缓存），建议跳过 `*.tmp` 或先取消在途下载 |
| P3 | 工程 | `finalizeDownload` 的 exists + rename 存在 TOCTOU | 2026-09-11 CodeReview | 两个任务并发下载同一文件时，Windows 的 `rename` 不覆盖已存在文件 → 报“Cannot move ... into the cache”，而缓存其实已完好。建议 rename 失败后重新 `isCached` 判定，命中即视为成功 |
| P3 | 工程 | `saveToFile` 在 GUI 线程同步解密（最大 2 GiB） | 2026-09-11 M8.2 | 界面冻结、无进度、无法取消；建议改异步并复用现有 `taskProgress` 与横幅 |
| P3 | 工程 | 传输横幅只跟踪单个上传任务 | 2026-09-11 CodeReview | 引擎支持多任务并发（`pumpNext` 遍历全部任务）但 UI 只维护 `activeUploadToken`：第二个上传的进度与失败不可见，取消也只能取消最后一个。建议横幅改为 `Repeater` over 任务列表 |
| P3 | 工程 | QML `updateFileState`/`updateFileProgress` 为 O(n) 全表扫描 | 2026-09-11 CodeReview | 长会话（数千条）× 大文件（512 个分片事件）下开销可观；建议维护 `messageId -> row` 的 JS Map |
| P3 | 安全 | `markFileTicketUsed` 仍无生产调用点；过期票据行以 `used=0` 残留 | 2026-09-11 CodeReview | 下载票据刻意允许 TTL（300s）内重复使用以支持 Range 分段，因此不消费是设计意图；但 `FileProtocol.h` 附近的注释把它描述成一次性票据，应修正；过期行由 `pruneExpiredFileTickets` 每小时清理，若需更严可改为每段单独签发或绑定数据面会话 |
| ~~P3~~ | ~~功能~~ | ~~无应用内大图查看器，看原图仍靠“另存为”~~ | ~~2026-09-11 M8.3a~~ | **已销账（2026-09-11 M8.3c）**：`FileImageProvider`（`image://xyfile/<messageId>`）从密文缓存逐片解密并在内存中解码（`MaxInMemoryDecodeBytes = 64 MiB` 上限），明文不落盘；`MainPage` 预览对话框标题带文件名与像素尺寸、footer 提供“另存为”；气泡缩略图与图标均可点开 |
| ~~P3~~ | ~~功能~~ | ~~缩略图只在上传时生成，历史图片消息无缩略图~~ | ~~2026-09-11 M8.3a~~ | **已销账（2026-09-11 M8.3b）**：`localThumbnailForMessage` 在清单 thumb 为空且原图密文缓存就绪时解密原图本地生成缩略图，缓存到 `<cacheRoot>/thumbs/<sha256[0..1]>/<sha256>.jpg`（不回填清单，那会改变已发送消息的密文）；`attachFileInfo` 据此回填 `fileThumb`，`clearCache` 一并递归清理 thumbs 目录。超 64 MiB 内存解码上限的大图仍不补齐（回退文件图标） |
| P3 | 功能 | 视频播放无动态画面（只输出音频轨 + 静态封面） | 2026-09-11 M8.3b | `MediaPlaybackManager` 用 C++ QMediaPlayer + DecryptingIODevice 流式解密播放（明文不落盘），但 QML VideoOutput 无法绑定 C++ QVideoSink（无公开 videoSink 属性），且 QML Video 元素的 source 只接受 URL 不支持自定义 QIODevice。当前视频播放只闻其声不见其画（静态封面取自清单 thumb）；补齐需自定义 QSGNode/QQuickPaintedItem 渲染 QVideoFrame，或在 C++ 侧 qobject_cast QML VideoOutput 调 setVideoSink（脆弱） |
| P3 | 功能 | 无按用户的存储用量配额 | 2026-09-10 M8.1 | 现有约束为单文件 ≤2 GiB + 并发上传 ≤8 + 48 小时超期回收，但已就绪文件可无限累积（仅受消息删除联动回收影响）；需按用户/按会话的字节配额与用量统计接口 |
| P3 | 安全 | 下载票据在 TTL 内可重复使用 | 2026-09-10 M8.1 | 为支持 `Range` 分段与断点续下而刻意允许（TTL 300 秒），泄露后可在窗口内重放下载该文件；一次性消费（`markFileTicketUsed`）与分段下载互斥，属取舍。大文件场景下 TTL 不足的风险另见上方 P2 |
| P3 | 工程 | `putChunk` 不入条带锁 | 2026-09-10 CodeReview | 依赖 `finalize` 的逐片长度 + 整体 SHA-256 关卡兜底：并发写同一片只会导致组装判失败（要求重传），不会把损坏对象推上下载路径；代价是极端并发下多一次重传 |
| P3 | 工程 | 回收查询每轮 `limit=100`，积压大时需多轮收敛 | 2026-09-10 M8.1 | `getStaleUploads`/`getTerminalFiles`/`getUnreferencedReadyFiles` 均为每轮上限 100 行、每小时一轮；大量遗留时收敛慢且无积压告警指标 |
| P3 | 功能 | 文件消息不可编辑，缺“撤回重发”替代路径 | 2026-09-10 M8.1 | `processEditMessageRequest` 对 `fileId > 0` 的消息一律回 `InvalidRequest`：编辑只能改写正文而 `messages.file_id` 不变，会使清单里的 `fileId`/密钥与服务端授权、以及客户端以 `file_id` 判别清单的口径三者失配（客户端可能拿着旧 `fileId` 去申请下载票据）。正确替代应为“删除原消息 + 重发新文件消息”，属 M8.2 客户端 UI 范围（需引导与原子化），当前仅服务端拦住 |
| P3 | 工程 | 文件回收任务在主线程做同步磁盘 I/O | 2026-09-10 CodeReview | `pruneFileUploads` 由 Server（主）线程的定时器驱动，该线程同时承担 `incomingConnection` 与 `onMessageForUser` 路由；`remove()`（内部 `removeRecursively`）为阻塞调用，三轮合计每轮最多约 300 次删除，大文件/多分片目录时可能短时阻塞连接接受与消息转发。量级有界（每小时、limit=100）且定时器不重入，属响应性隐患而非正确性缺陷；积压增大后可移至独立维护线程或工作池 |

## 4. 未来里程碑规划

### 4.1 M8：媒体、文件与对象存储（4-8 周，分三切片；**M8.1 已于 2026-09-10 完成**）

- **目标**：支持图片、语音、视频和文件消息。
- **依赖**：M3/M7a 消息通道（已完成）；媒体 E2EE 依赖 M6/M7b 加密基础（已完成）。
- **切片划分依据**：原 4-8 周的整块里程碑含三个可独立验收、依赖方向单一的切片，拆开后可逐片入库与回滚，避免长期悬置分支。

#### M8.1 协议与存储地基（已完成，2026-09-10）

- 交付：
  - **协议层**：`CommonModule/protocol/FileProtocol`（消息类型 90-99、错误码 3013-3021、`FileManifest` 编解码与 fail-closed 校验、分片数学 `chunkCountFor`/`isChunkingValid`/`expectedChunkBytes`、体积/分片/票据/配额常量）；`send_message` 新增可选 `fileId` 并在响应/推送/事件/历史读取四条路径回传。
  - **加密原语**：`CommonModule/encryption/FileCrypto`（每文件独立 AES-256 密钥 + 12 字节 nonce 前缀；第 i 片 nonce = `iv` 后 4 字节 XOR 大端 `i`、AAD = 大端 `i`；流式 SHA-256；票据生成与摘要）；`E2eeCrypto` 新增带 AAD 的 AES-GCM 原语。**刻意不复用消息 ratchet**，避开 M9 编辑踩过的“链已推进→早先分片永久不可解”不可逆损坏。
  - **对象存储**：`Chat-Server/storage/IObjectStorage` 抽象（allocateBlobKey/putChunk/receivedChunks/readChunk/finalize/isFinalized/blobSize/readRange/remove）+ `LocalFileStorage` 实现（临时文件+原子改名、blobKey 前两位分 256 桶、同键 `finalize`/`remove` 条带锁串行、`finalize` 流式组装并逐片核长度 + 整体核 SHA-256、结果六分类）。
  - **数据层**：V10 迁移（`files`、`file_tickets`、`messages.file_id` 与三个索引）；文件元数据 CRUD（创建含**原子并发配额**、状态迁移终态不可逆、票据签发/校验/消费/清理、访问控制 `canUserAccessFile`、回收查询）；`sendMessage` 将“文件仍为 `ready`”下推为 `INSERT` 守卫子查询（单语句原子，消除与维护回收的跨线程竞态，守卫未命中回 `FileNotReady`）。
  - **控制面**：五个处理器（申请上传/续传查询/宣告完成/取消/下载票据），两个新限流窗口（新建上传 20/60s、其余文件操作共用 60/60s）、归属与枚举预言机防护、幂等（完成/取消）、finalize 结果分类回不同错误码；`Server` 创建并注入存储、维护连接新增 `pruneFileUploads` 三轮回收。
- 验证：新增 `TestFileProtocol`（30 用例：清单往返/fail-closed/分片数学/边界）与 `TestObjectStorage`（19 用例：分片读写/组装校验/断点续传/幂等删除/崩溃残留清理/路径安全）；`TestDatabaseManager` 新增 14 个 M8 用例（V10 表列/记录读写/状态护栏/配额原子性/超期与终态与无引用回收选择/票据/访问控制/`fileId` 四路径/插入守卫）共 63 passed；`ctest` 9/9 全绿（9 套共 227 个用例）。**两轮 CodeReview 子代理审查**：第一轮（M8.1 主体）无 P0，2 项 P1（元数据枚举预言机、存储层并发）、1 项 P2（配额 TOCTOU）、1 项 P3（终态行与孤儿数据无回收路径）均已修复；第二轮（回收增量）无 P0/P2，1 项 P1（“终态行不可能再被引用”的不变量在跨线程下不成立，可导致仍被引用文件的磁盘数据被删）已修：发送侧把文件状态下推为 `INSERT` 守卫子查询 + 回收侧删盘前再判引用（双侧防护），并补 `sendMessageGuardsFileReadyStateAtomically` 回归用例；2 项 P3（配额用例残留绝对值断言、回收在主线程做同步 I/O）前者已修、后者登记入 §3。
- 已知限制：单机本地文件系统存储（无副本/无冗余）；数据面未实施前控制面无法单独产生可用的端到端文件消息；无自动化文件传输集成测试（均见 §3）。

#### M8.2 数据面与客户端（已完成，2026-09-11）

- 交付：
  - **数据面**：`Chat-Server/http/FileHttpService`——`QHttpServer` 经 `QSslServer` 承载（**先 `listen()` 后 `bind()`**，反序会被 Qt 直接拒绝），与主通道共用同一套证书与 fail-closed 口径；两个端点 `PUT /file/<fileId>/chunk/<index>` 与 `GET /file/<fileId>`（单区间 Range，206/416/413/400 语义完整，range-unit 大小写不敏感）；票据只走 `X-XYChat-Ticket` 请求头（不进 URL query，以免落入代理与访问日志），四种票据失败统一 401 且响应体一致；分片长度按 `expectedChunkBytes` 精确匹配；下载恒 `application/octet-stream` + `Cache-Control: no-store`；单次 GET 上限 4 MiB（客户端本就按分片解密，无上限的 `readRange` 会让单请求把 2 GiB 读进内存）；per-IP 限流**只计授权失败与畸形请求**（30/60s → 429）。
  - **服务端集成**：`Server::setFileHttpEndpoint(port, advertisedHost)` 与 `fileTransferBaseUrl()`；仅对象存储就绪时才启动数据面；`main.cpp` 新增 `--http-port`（默认 12346）/`--http-host`（默认 127.0.0.1）；登录响应下发 `fileTransferBaseUrl`（未启动则不下发该字段，客户端据此禁用文件能力而不是猜端口）；上传完成/取消/标失败后 `revokeFileTickets` 吊销上传票据（TTL 24h，而分片已组装回收，留着只延长泄露窗口）。
  - **客户端引擎**：`Chat-Client/core/FileTransferManager`——两遍加密上传（第一遍流式加密只算密文整体 SHA-256、**不落临时文件**，代价是多一遍 AES，换来磁盘占用不翻倍且无崩溃残留；第二遍逐片加密 PUT）；串行分片调度（`pumpNext` + token 快照 + `m_pumping` 重入护栏）；失败后先查 `receivedChunks` 再决定跳过或重传（处理“响应丢失但数据已落盘”）；重试预算分两层（分片 3 次 + 总恢复轮次 5 轮，后者防止“成功的查询”清零预算而形成活锁）；下载按分片边界 Range GET → 临时密文 → 整体 SHA-256 自校验 → 原子改名进缓存；与控制面经“请求信号 + seq 回调”解耦（不持有 socket，可脱离网络单测）。
  - **本地缓存**：下载的密文**原样落盘**（`<cacheRoot>/<sha256[0..1]>/<sha256>`，无后缀）——零额外加密开销且磁盘上天然不是明文；明文只在用户“另存为”时写出（临时名 + 改名）；解密失败即删缓存并回退 missing（一次性自愈）；`clearCache`/`cacheBytes` 供设置页使用。
  - **协议修正**：`FileManifest` 新增必填 `chunkSize`——接收方必须只凭清单（E2EE 可信）就能确定分片边界，若改用服务端声明的口径，不可信的服务端就能让解密错乱；客户端 `onDownloadTicket` 将服务端返回的 `sizeBytes`/`chunkSize`/`chunkCount`/`sha256` 四项与清单逐一比对，不一致即拒绝。
  - **NetworkManager 集成**：拥有并接线引擎（5 个控制面请求/响应 + requestId→seq 映射）；`sendMessage`/`sendGroupMessage` 新增 `fileId` 参数并写入请求 JSON；**文件消息不进持久化 outbox**（outbox 表无 `file_id` 列，落库后重发会退化成“正文是清单”的普通消息，等于把密钥当文本发出）；`attachFileInfo` 登记清单 + 补脱敏展示字段；**`sanitizeForUi` 作为唯一脱敏出口**覆盖四条 UI 路径；会话预览改 `[File] <名>`；登出与断线两路径均 `reset()`（清零清单密钥）。
  - **QML UI**：`MessageInput` 附件按钮 + `FileDialog`（把 `QUrl` 原样交给 C++，由 `fileTransfer.toLocalPath()` 调 `QUrl::toLocalFile()` 转本地路径——QML 的全局 `Qt` 对象并无 `urlToLocalFile`，正则剔 `file://` 前缀对 UNC 与含 `%`/`#`/`?` 的路径会错）；`MessageBubble` 文件面板（图标/名/大小/进度条/下载/另存，编辑项对文件消息禁用）；`ChatView` 新字段与信号 + `updateFileState`/`updateFileProgress`；`MainPage` 传输横幅（进度/取消）、另存对话框、`Connections` 接线与 token→messageId 映射；引擎经 `main.cpp` 注册为 context property `fileTransfer`。
- 验证：新增 `TestFileHttpService`（11 用例：启动 fail-closed、票据授权五态、分片长度/序号/畸形路径、状态护栏、Range 全语义、幂等覆写、票据吊销、失败限流）与 `TestFileTransfer`（12 用例：2.5 MiB 跨 3 片文件上传→组装→下载→解密→另存后**与源文件逐字节一致**、服务端 blob 与明文无任何前缀/子串重合、清单自包含、缓存命中免网络、元数据不符拒下载、reset/clearCache/未启用语义、调度器在泵送中同步失败仍保持一致）；`TestNetworkManager` 新增 `fileManifestNeverReachesUiLayer`（P0 回归：四条路径脱敏 + 兜底防线 + 密钥确实留在 C++ 侧）至 13 用例；`TestFileProtocol` 新增 `decodeRejectsBadChunkSize` 至 31；`ctest` **11/11** 全绿；`qmllint` 四个改动文件零错误。集成测试当场抓到一个编译期无法发现的真实缺陷：`QAbstractHttpServer::bind()` 要求 server **已在监听**，原顺序反了（生产环境同样会启动失败）。
- **CodeReview 子代理审查**：发现 **3 项 P0 均已修**——① 清单（含 32 字节文件密钥）只在实时推送路径脱敏，`sync_messages`/本地缓存回填/`sync_events` 三条路径会把含密钥的清单 JSON 当正文渲染进气泡（E2EE 核心承诺被破坏），修为唯一出口 `sanitizeForUi`/`sanitizeArrayForUi` 收口四条路径 + “形态像清单就置空”兜底；② `finishTask`/`failTask` 的 `token` 参数常是容器内 `Task::token` 的引用，`erase` 后悬垂而 `emit` 还要读它（**每次成功/失败/取消都会走到**），修为 emit 前取独立副本；③ `pumpNext` 在 `QHash` 遍历中调 `pumpUpload`，后者可能同步 `failTask` → `erase` 当前节点 → 迭代器失效，且 `failTask` 递归调 `pumpNext` 造成嵌套泵送（破坏串行承诺），修为 token 快照 + `m_pumping` 护栏。**3 项 P1 已修**：重试预算被“成功的查询”清零 → 数据面持续 5xx 时形成活锁（新增 `recoveryRounds` 封顶）；下载气泡永久卡“下载中”（`download()` 在缓存命中/清单缺失时同步完成，token→messageId 映射晚于信号）；发送方无法下载自己发的文件（`cached` 未带 `messageId`，清单无法登记）。**P2/P3 各修两项**：`reset()` 中 `abort()` 同步回调会在登出途中发新请求且可能残留 `m_activeReply` 使新会话永久卡死（加 `m_resetting` 护栏 + 显式复位）；QML 用正则剔 `file://` 前缀在 UNC 与含 `%` 路径上会**静默写到乱码文件名**（改为把 `QUrl` 原样交给 C++，由 `fileTransfer.toLocalPath()` 调 `QUrl::toLocalFile()` 转换——QML 全局 `Qt` 对象并无 `urlToLocalFile`，该写法会抛 `TypeError`）；把“目标写失败”误当缓存损坏而删掉完好缓存；`Range` 的 range-unit 大小写。其余审查项已登记入 §3。
- 已知限制：上传 hashing 与 `saveToFile` 解密均在 GUI 线程同步执行（大文件会冻屏）；下载票据 TTL 对大文件不足且无断点续传；数据面无请求体上限预检（见 §3 P2）；**双客户端实机联调尚未做**（需启动服务端与两个客户端，属人工验证）；应用内视频/语音播放器未做（属 M8.3b；图片大图查看器已于 M8.3c 完成）。

#### M8.3 多媒体元数据（M8.3a/b/c 全部已完成 2026-09-11）

##### M8.3a 图片元数据与缩略图（已完成）

- 交付：新增 `Chat-Client/core/ThumbnailMaker`（**纯 QtGui**：`QImageReader` 读尺寸 + 缩放解码，`QImage` 编码 JPEG）：按最长边 160px 缩放，逐步降质量（70/55/40/25/15）、质量到底仍超限再折半降尺寸（下限 32px），**压不进 `MaxThumbnailBytes`（4096）就不内联**（UI 回退到文件图标，绝不放宽上限，否则清单会撑破群消息正文长度而使整条文件消息被服务端拒收）；`setAutoTransform(true)` 校正 EXIF 方向，并据此修正上报的宽高（旋转 90/270 度会交换宽高）；`setScaledSize` 先缩放再解码，避免把大图完整读进内存。`FileTransferManager` 在上传前提取并写入清单 `width`/`height`/`thumb`；`NetworkManager::attachFileInfo` 新增脱敏字段 `fileWidth`/`fileHeight`/`fileThumb`（base64 JPEG）；`MessageBubble` 新增内联缩略图（`data:image/jpeg;base64,` + 仅在 `status === Image.Ready` 时显示，解码失败不留空白）与像素尺寸行；`ChatView` 新角色透传。
- **安全口径**：缩略图是 JPEG **明文**字节（M8.1 原注释误作“缩略图密文”，已修正）：清单整体随消息正文经既有 E2EE 加密，再单独加一层只增加复杂度而无安全收益；也因它**不含任何密钥**，可以经脱敏出口交给 QML，使接收方**在下载原图之前**就能展示预览（零流量、零等待）。服务端仍然全程不可见（尺寸与缩略图只在清单里）。
- **选型理由**：图片路径纯 QtGui，无平台解码后端依赖，可在无头环境与 CI 中稳定验证；而音视频时长/封面需 QtMultimedia + 平台解码器（Windows 上为 Media Foundation）且为异步加载（与当前同步的上传入口相冲突），在 CI 上不可验证，故拆为 M8.3b。
- 验证：新增 `TestThumbnailMaker`（8 用例：尺寸与可解码缩略图/高熵图不超限/极小上限宁可留空也不超限/非图片与缺失文件与非法参数安全返回/base64 后仍在群消息正文预算内；JPEG 插件缺失时相关断言 QSKIP）；`TestFileTransfer` 新增 `imageUploadCarriesThumbnailMetadata`（图片上传后清单带正确尺寸与可解码缩略图，且非图片文件不携带缩略图）至 13 用例；`ctest` **12/12** 全绿；`qmllint` 零错误。
- 已知限制：缩略图只在上传时生成，历史图片消息无缩略图（需接收端下载原图后本地生成，待后续）。

##### M8.3c 应用内大图查看器（已完成，2026-09-11）

- 交付：新增 `Chat-Client/core/FileImageProvider`（`QQuickImageProvider` 子类，以 `image://xyfile/<messageId>` 注册到 QML 引擎）：从密文缓存逐片解密并在内存中解码，**明文不落盘**（与 SECURITY.md 的“磁盘上不存在可读明文”口径一致，看原图不再必须“另存为”）；`FileTransferManager` 新增线程安全接口 `decryptedFileBytes(messageId)`（渲染线程调用，`manifestFor` 加锁拷贝清单、IO 与 GCM 认证在锁外）与 `MaxInMemoryDecodeBytes = 64 MiB` 上限（超限直接返回空，避免把整个明文读进内存把进程打爆）；`MessageBubble` 缩略图与文件图标均加 `MouseArea`（仅 `isImageFile && fileState === "available"` 时可点）发 `previewRequested`；`ChatView` 转发为 `filePreviewRequested(messageId)`；`MainPage` 预览对话框（标题带文件名与像素尺寸、`BusyIndicator` 加载态、错误态提示、footer 提供“另存为”与“关闭”）；`ChatView` 新增 `getMessageById(messageId)` 供预览对话框查消息字段。
- **修复 P0 运行时错误**：原 `MessageInput.qml` 与 `MainPage.qml` 调用 `Qt.urlToLocalFile()`（QML 全局 `Qt` 对象并无此函数，那是 C++ `QUrl` 的方法），运行时抛 `TypeError: Property 'urlToLocalFile' of object Qt(...) is not a function`，导致附件无法发送、另存为失败。修为：`MessageInput` 的 `attachmentSelected` 信号参数改 `var` 以保留 `QUrl`，直接把 URL 交给 C++；`MainPage` 另存对话框改调 `fileTransfer.toLocalPath(selectedFile)`；`FileTransferManager` 新增 `Q_INVOKABLE toLocalPath(QVariant)`（`QUrl` 走 `toLocalFile()`、字符串以 `scheme:` 开头且非 Windows 盘符时按 URL 解析、其余原样返回），`uploadAndSend`/`saveToFile` 参数改 `QVariant` 以吃下 URL 与路径两种形态。同步修正 `AGENTS.md` 中误导性的“用 `Qt.urlToLocalFile()`”规则。
- 验证：`TestFileTransfer` 新增 `toLocalPathAcceptsUrlsAndPlainPaths`（QUrl/字符串 file:// URL/UNC/纯本地路径/非 file 协议/空值/Windows 盘符七种形态）与 `decryptedFileBytesRestoresPlainForRegisteredMessage`（未登记/未下载/超上限返回空、登记+下载后逐字节还原）至 15 用例；`ctest` **12/12** 全绿；`qmllint` 零错误。
- 已知限制：超过 64 MiB 的图片无法在应用内预览（需“另存为”后用外部查看器）；无缩放/平移控件（当前 `PreserveAspectFit` 适应窗口）。

##### M8.3b 音视频元数据与播放器（已完成，2026-09-11）

- 交付：
  - **元数据提取**：新增 `Chat-Client/core/MediaMetadataExtractor`（`QMediaPlayer` + `QVideoSink` 异步加载）：提取音频时长、视频时长/分辨率，视频 seek 到 min(1000, duration/2) 毫秒抓封面帧（`QVideoFrame::toImage` → `ThumbnailMaker::encodeThumbnail` 压缩）；5 秒超时护栏，超时/错误/非音视频时 emit `finished(ok=false)`；已拿到时长/分辨率但抓帧超时仍上报前者（thumbnail 留空）。`ThumbnailMaker` 抽出 `encodeThumbnail(QImage)` 静态方法供封面复用压缩逻辑。
  - **异步上传重构**：`uploadAndSend` 拆为"元数据提取 → hashing → creating"三阶段：图片同步提取（不变），音视频走 `extracting` 阶段异步提取（`MediaMetadataExtractor`，同一时间只提取一个，其余排队 `m_pendingExtractTokens`），其他文件直接 hashing；`beginHashing` 从 uploadAndSend 抽出供提取完成回调；元数据缺失绝不阻断发送（提取失败留空继续 hashing）；cancelTask/finishTask/failTask/reset 均清理提取队列（`clearExtractionStateForToken`）避免悬空 token。
  - **清单与透传**：`FileManifest.durationMs` 填充（M8.1 已预留字段）；`attachFileInfo` 透传 `fileDurationMs`；`ChatView`/`MessageBubble` 新角色 `fileDurationMs`。
  - **音视频气泡**：`MessageBubble` 新增 `isAudioFile`/`isVideoFile` 判定、`playRequested` 信号、`formatDuration`；视频封面中央叠加播放按钮（▶）、右下角时长标签，音频图标改 ▶ 且文件名下方展示时长；缩略图/图标点击按类型分流（图片预览、音视频播放）。
  - **播放器（明文不落盘）**：新增 `DecryptingIODevice`（`QIODevice` 子类，从密文缓存流式逐片解密，实现 `readData`/`size`/`seek`/`bytesAvailable`，支持拖动进度条；`ensureChunkFor` 按 plainPos 定位分片并解密，seek 到片中间时正确定位片内偏移）与 `MediaPlaybackManager`（C++ `QMediaPlayer` + `DecryptingIODevice` + `QAudioOutput` + `QVideoSink`，`setSourceDevice` 播放；`Q_PROPERTY` 暴露 playbackState/position/duration/volume/hasVideo/active）；`main.cpp` 注册为 context property `mediaPlayer`；QML 播放器对话框（视频静态封面/音频图标 + 播放/暂停/进度滑块/音量，错误经 fileNotice 横幅）。
  - **历史图片缩略图补齐（M8.3a 欠账）**：`localThumbnailForMessage` 在清单 thumb 为空且原图密文缓存就绪时解密原图本地生成缩略图，缓存到 `<cacheRoot>/thumbs/<sha256[0..1]>/<sha256>.jpg`（临时名 + 改名）；`attachFileInfo` 据此回填 `fileThumb`；`clearCache` 一并递归清理 thumbs 目录；超 64 MiB 内存解码上限的大图不补齐。
- 验证：`TestFileTransfer` 新增 3 个 `DecryptingIODevice` 用例（顺序读取逐字节还原、seek 跨分片边界定位、未下载/未登记时 open 失败）至 18 用例；`ctest` **12/12** 全绿；`qmllint` 零错误。集成测试当场抓到一个真实缺陷：`ensureChunkFor` 重新解密分片后把 `m_chunkBufferOffset` 置 0 而非 plainPos 的片内偏移，导致 seek 到片中间时错位返回片头数据（`decryptingIODeviceSupportsSeek` 捕获并修复）。
- 已知限制：音视频元数据提取与播放依赖平台解码后端（Windows Media Foundation），CI/无头环境不可验证（提取失败留空、播放报错，均不崩溃）；**视频播放无动态画面**（QML VideoOutput 无法绑定 C++ QVideoSink，只输出音频轨 + 静态封面，见 §3 P3）；hashing 仍在 GUI 线程同步（大文件冻屏，沿用 §3 P3 欠账）。

### 4.2 M10：搜索、通知与体验完善（4-6 周）

- **目标**：把 MVP 从"能用"提升到"好用"。
- **依赖**：M6.5 本地缓存（本地消息搜索的数据底座，已完成）。
- **任务**：
  - 客户端本地消息搜索（基于 LocalStore，注意密文列需经解密缓存/索引设计）。
  - 服务端联系人/用户名搜索增强（现有 `search_users` 基础上补分页/模糊度控制）。
  - 桌面通知、声音、未读角标完整配置（系统托盘、通知点击定位会话）。
  - 草稿、表情基础能力。
  - 删除会话（整表删除，无对应接口/事件）。注：置顶与免打扰已于 2026-09-05 随 M9 特性栈交付，不属本里程碑范围。
  - 国际化与主题系统扩展。
- **验收标准**：
  - 用户能快速找到联系人、会话和历史消息。
  - 通知行为符合系统习惯且可配置。

### 4.3 M11：稳定性、可观测性与运维（持续）

- **目标**：为真实用户使用做好稳定性基础。
- **已提前落地**：登录限流（M2）；`fetch_keys`/`fetch_group_keys` 连接级限流（M6/M7b）；**发消息/搜索限流 + 结构化日志（M11 前置两项，2026-09-03）**。
- **说明**：结构化日志与发消息/搜索限流曾于 2026-08-21 建议前置，已于 2026-09-03 提前落地（见变更记录），对应 P2 欠账已销账；M11 其余任务（指标监控/崩溃捕获/备份/压测）待整体启动。
- **任务**：
  - 结构化日志（✅ 已实施 2026-09-03）：`StructuredLogger` 单行 JSON，统一请求 ID/用户 ID/设备 ID/错误码/耗时字段（配合 LogSanitizer 脱敏）；`sendResponse` 中央审计日志（成功 info/失败 warning，可统计失败率与延迟）+ 鉴权/会话/重放/envelope/限流安全事件带 `reason`。
  - 指标监控：在线连接数、消息吞吐、失败率、延迟、数据库慢查询。
  - 崩溃捕获与客户端日志上报。
  - 限流：发消息、搜索（✅ 已实施 2026-09-03，连接级 `RateWindow`：`send_message` 30/10s、`search_users` 20/60s，新增通用 `RateLimited` 1003，`fetch_keys`/`fetch_group_keys` 一并迁移）；文件上传限流待 M8；登录限流（M2）已有。
  - 备份与恢复演练。
  - 压力测试：长连接数、消息吞吐、离线同步峰值。
- **验收标准**：
  - 能回答"当前多少在线用户、消息延迟多少、失败率多少"。
  - 服务端异常重启后不丢已确认消息。
  - 压测报告可指导扩容。

## 5. 推荐执行顺序（2026-09-02 重排，2026-09-11 更新）

下一步候选按"安全欠账优先、横切能力其次、特性栈分批"排序；**具体下一任务待讨论确定**：

1. 解决历史遗留 P2 欠账。
2. **M8 媒体文件**（进行中）。**前置条件（2026-09-09 审查提出，2026-09-10 已闭环）**：四项 P2（三端点限流、编辑/删除并发护栏、CI 首次运行验证、`NetworkManager` 层单测）与两项 P3（专用推送类型 88/89、`AGENTS.md`）已全部销账。**M8.1（协议与存储地基，2026-09-10）与 M8.2（数据面 HTTP(S) 与客户端上传下载，2026-09-11）已完成**（见 §4.1），`ctest` 11/11 全绿；M8.2 一并销账了三项 P2（数据面未实施、票据校验无生产调用点、控制面无自动化测试）。下一切片为 **M8.3b（音视频元数据与播放器）**：图片部分（M8.3a，尺寸 + 内联缩略图 + 图片气泡预览）已于 2026-09-11 完成，`ctest` 12/12 全绿；M8.3b 需 QtMultimedia 与平台解码后端，在 CI 上不可验证，建议在真实桌面环境实施并人工验证（遗留的大群分发超限/先落盘后分发/TLS 集成测试等均不阻塞，见 §3）。
3. **M10 搜索/通知/体验**。

## 6. 目录结构（2026-09-11 与实际仓库同步，含 M8.3a）

```text
XYChat_Project/
  .github/workflows/      # GitHub Actions CI（cmake.yml：configure/build/ctest；2026-08-03 误删、2026-09-09 重建）
  3rdparty/               # 预编译依赖：QWindowKit、OpenSSL、zlib（include/lib/bin/src）
  cmake/                  # XYChatOpenSSL.cmake（按构建配置绑定 Release/Debug OpenSSL 导入库）
  CommonModule/           # 客户端/服务端共享模块
    protocol/             # Packet/PacketCodec（消息类型 1-99，错误码 1000-9002）、FileProtocol(M8)
    encryption/           # EncryptionManager/E2eeCrypto(M6)/GroupE2eeCrypto(M7b)/FileCrypto(M8)
    security/             # LogSanitizer/SecureMemory/TlsHelper/StructuredLogger(M11 前置)
  Chat-Client/
    core/                 # NetworkManager/KeyStorage/LocalStore/ThemeSettings/FileTransferManager(M8.2)/ThumbnailMaker(M8.3)
    models/               # 数据模型
    resources/
      pages/              # LoginPage/MainPage/MainWindow
      components/         # TitleBar/ConversationList/ChatView/MessageInput/MessageBubble/QWKButton
      theme/              # Theme.qml（darkMode 双配色）
      icons/
      main.qml
      resources.qrc
    main.cpp
  Chat-Server/
    core/                 # Server/RequestHandler/NonceCache/RateWindow(M11 前置)
    database/             # DatabaseManager 与迁移（当前 V10）
    storage/              # M8：IObjectStorage 抽象 + LocalFileStorage（分片/组装/校验/回收）
    http/                 # M8.2：FileHttpService 数据面（QHttpServer + QSslServer）
    main.cpp
  docs/                   # ARCHITECTURE/PROTOCOL/ROADMAP/SECURITY
  tests/
    unit/                 # TestPacketCodec/TestEncryptionManager/TestDatabaseManager/
                          # TestSecurity/TestLocalStore/TestGroupE2eeCrypto/TestNetworkManager/
                          # TestFileProtocol/TestObjectStorage/TestFileHttpService/
                          # TestFileTransfer/TestThumbnailMaker（均纳入 CTest，共 12 套）
    e2e/                  # TestGroupRepro（双客户端群 E2EE 复现，手动运行，不纳入 CTest）
  certs/                  # 开发证书生成脚本（运行时证书自动生成于可执行文件同级 certs/）
  AGENTS.md               # 代理/新人工程入口：构建测试命令、诊断技巧、风格约定、完成定义
  Build.ps1               # 免维护构建入口：vswhere 定位 VS + Launch-VsDevShell 载入工具链后调 cmake 预设
```

## 7. 数据库演进

- **服务端**（SQLite，版本化迁移，当前 V10）：`schema_version`、`users`、`devices`、`sessions`、`login_audit`、`contacts`、`conversations`（V7 增 `name`）、`conversation_members`（V7 增 `role`、V9 增 `pinned`/`muted`）、`messages`（V9 增 `edited_at`/`deleted`，V10 增 `file_id`）、`message_receipts`、`sync_events`、`device_identity_keys`（M6，仅公钥）、`prekeys`（M6，仅公钥）、`sync_meta`（V8，清理水位线）、`files`（V10/M8：密文侧元数据，`blob_key` UNIQUE + 状态机 + 两个回收/配额索引）、`file_tickets`（V10/M8：只存票据 SHA-256 摘要，无明文列）。中长期若需多人并发/多实例部署，迁移 PostgreSQL/MySQL，并尽早抽象 Repository/DAO；对象存储已以 `IObjectStorage` 抽象，多实例部署时需同步换为共享存储。
- **客户端 LocalStore**（SQLite，按账号+设备隔离，正文加密落库）：`schema_meta`、`messages`（M9 增 `edited_at`/`deleted`）、`conversations`（含群名/成员数、M9 增 `pinned`/`muted`，置顶会话按 pinned DESC 排序）、`outbox`（含 `conversation_id`）、`decrypt_cache`、`meta`（同步游标）、`sender_keys`（M7b：chain key/Ed25519 签名密钥对/迭代数，登出保留；“最新密钥”按 `rowid DESC` 选取）、`sender_key_skipped`（2026-09-09：跳序消息密钥缓存，整体密文 blob，与 `sender_keys` 同主键维度，退群一并清理）。

## 8. 安全注意事项

- 不要把 SHA-256 当作密码存储方案；它太快，不适合抵抗离线撞库。
- 不要自己设计未经验证的密码学协议；优先参考成熟方案和库。
- 不要在日志中输出密码、token、私钥、验证码、完整密文密钥材料。
- 不要把服务端能解密的"普通加密聊天"宣传成端到端加密。
- 端到端加密需要明确密钥验证、设备更换和历史消息恢复策略。
- 对所有外部输入做长度限制、格式校验和速率限制。
- ratchet/循环类解密路径必须设步数与参数上限（M7b DoS 教训：恶意 `iteration` 可迫使接收端长时间运算）。
- 单向链式密钥（ratchet）与业务主键顺序不一致时必须提供乱序容忍：“密文可被事后覆写”的功能（如消息编辑）会使密钥迭代号与消息 id 解耦，而按 id/seq 升序批量解密会先推进链状态，使后到的低迭代号消息被回滚检查永久拒绝（明文不可恢复）。必须缓存被跨越迭代的消息密钥（skipped message keys）并设容量上限。
- 事件/推送 payload 必须携带解密所需的全部寻址字段：群聊密文的解密依赖（群, 发送者, 设备, keyId）四元组，缺 `senderId` 即等同不可解；新增会改动已有密文的事件时，需同时考虑历史已落库事件的兼容路径。
- 服务端自发推送应覆盖操作者本人的其他设备（多端一致），由客户端按发起设备去重，而不是在服务端排除整个用户。
- 以时间戳作“最新”排序依据时必须确认精度：秒级时间戳 + 随机值作并列破口等于把选择结果交给运气（`latestSenderKeyId` 教训）；应改用单调递增序号（如 SQLite `rowid`）。
- 条件判定与状态迁移必须合并为单条 SQL 语句（M8 配额 TOCTOU 教训）：“先读计数后插入”的分步写法在多设备/多线程并发时会集体读到“未满”而全部放行，使软配额形同虚设；同理，“先查引用再删行/改状态”也会让并发请求在两步之间建立引用，随后数据被销毁。
- 声称“某状态迁移后不可能再发生 X”类不变量时，必须确认判定与写入在同一原子单元内（M8 回收教训：“终态行不可能再被引用”因发送侧校验与 INSERT 分属两步而不成立）。跨线程窗口无法用“读得够晚”消除，只能两侧各上一道防护：写入侧把条件下推为单语句守卫，销毁侧在执行前再判一次；并且让最坏结果落在“可修复”而非“不可恢复”一侧。
- 销毁性操作必须排序：“先保证不产生孤儿数据、再销毁”。删磁盘与删元数据不可兼得时，宁可留下可被下一轮回收的隐形孤儿，也不得产出“元数据指向已消失数据”的不可自愈状态（M8 取消上传与三轮回收均按此排序）。
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

每个功能迭代都应同时交付：

- 协议文档更新（`docs/PROTOCOL.md`）。
- 数据库迁移脚本或 schema 变更说明。
- 服务端处理逻辑。
- 客户端调用与 UI。
- 单元测试或集成测试。
- 错误码和日志。
- 安全影响说明（`docs/SECURITY.md`）。
- 本路线图状态同步（状态总表 + 能力摘要 + 欠账清单 + 变更记录）。

## 10. 不建议现在立刻做的事

- 不建议一开始就做超大规模分布式架构；先把单机可靠性做好。
- 不建议过早引入复杂微服务；当前模块化单体更适合快速迭代。
- 不建议在协议未稳定时做过度复杂的 UI 动效和装饰（M7a 热修复教训：模型高频搅动 + 过渡动画曾致 delegate 悬空崩溃）。
- 不建议自行发明完整端到端加密协议；应在充分调研后实现。
- 不建议把文件传输塞进主聊天长连接；大文件应走独立上传下载通道。

## 11. 变更记录

原头部流水账更新块与原 Sprint 看板（Sprint 1-6，均已完成）合并为本表；详细过程经 `git log` 追溯。

| 日期 | 事件 | 摘要 |
| --- | --- | --- |
| 2026-07-01 | M0/M1 完成 | 工程基线与协议层重构落地 |
| 2026-07-29 | M2/M3/M4 完成 | 账户体系、一对一聊天 MVP、QML UI 重构落地 |
| 2026-08-03 | 审查更正 + M5/M5.5 完成 | 更正 M3/M4/M5 中被提前标记完成的条目；新增并完成 M5.5 安全加固（fail-closed/nonce 强制/授权/幂等/回执/sync_events） |
| 2026-08-04 | M4.5 完成 | 亮暗主题、搜索发起对话、乐观发送、已读回执及 8 项验证期缺陷修复 |
| 2026-08-17 | M6 完成 | 一对一 E2EE（简化 Signal），审查修复预密钥泄漏/耗尽、身份轮换静默丢消息 |
| 2026-08-20 | M6 联调修复 | 离线发送保留重试不丢弃；自身拷贝条目 + 持久化解密缓存解决登出重登解密；V6 迁移 |
| 2026-08-21 | 路线图调整 + M6.5/M7a 完成 | 新增 M6.5；M7 拆 M7a/M7b；M9 范围收缩；M6.5 落地；M7a 三子任务（协议与数据模型/服务端处理器与 fan-out/客户端 UI）落地 |
| 2026-08-22 | M7a.3 热修复 + M7b 实现 | 修复群聊联调崩溃（WER 定位 QML delegate 悬空通知端点：会话列表 clear+全量重建改差分更新、移除消息列表 add 动画、LocalStore 连接自愈与驱动检查，提交 `bafd4f6`）；M7b Sender-Key 群 E2EE 实现并双客户端联调通过（代码随 2026-09-02 安全修复后一并提交 `c806d90`） |
| 2026-08-26 | 安全审查 | 发现 GroupE2eeCrypto ratchet 循环无上限（DoS）、服务端群消息缺 envelope fail-closed 校验、`validateSession()` 仅查内存态三项问题 |
| 2026-09-02 | 安全修复 + M7b 入库 + 文档重构 | DoS 上限（`MaxRatchetSteps`/`MaxMessageIteration`）与服务端群 envelope fail-closed 落地（`TestGroupE2eeCrypto` 扩至 20 用例）；群聊横幅改为"已启用端到端加密"；M7b 连同修复提交（`c806d90`）；周度审查确认欠账清单；ROADMAP 完全重构（本版本），`validateSession()` 等 P1 修复见下一行 |
| 2026-09-02 | P1 欠账修复（3 项） | ① `validateSession()` 逐请求回查 `sessions` 表 + 过期 fail-closed（`token_renew` 豁免过期门）；② 逐包验 token（客户端已认证请求经 `addReplayProtection` 携带 `token`，服务端逐包比对哈希；新增客户端 `MessageType::Error` 处理清理在途槽位）；③ 群成员变更 Sender-Key healing（`member_added/removed/left` 触发本端轮换+重分发，离线经 `sync_events` 补偿，含同群去重/队列/瞬时失败延迟重试）。新增回归用例 `sessionByIdReflectsDeletionAndExpiry`、`senderKeyRotationRevokesRemovedMember`；`ctest` 6/6 通过；审查发现的大群分发上限/先落盘后分发/会话过期重登 UX/O(N²) 重分发登记为新欠账（P2/P3） |
| 2026-09-03 | M11 前置两项完成 | 发消息/搜索限流 + 结构化日志落地。① 限流：新增通用错误码 `RateLimited (1003)`（`LoginRateLimited` 收窄为仅登录）；`RateWindow`（`Chat-Server/core`，header-only）连接级固定窗口限流器替换 fetch_keys/fetch_group_keys 内联窗口并新增 `send_message`（30/10s，私聊/群聊同一入口）与 `search_users`（20/60s）限流；客户端 send 瞬时失败退避重刷（单发护栏防定时器堆叠）、fetch_group_keys healing 瞬时分类兼容新旧限流码。② 结构化日志：`StructuredLogger`（`CommonModule/security`）单行 JSON，统一 ts/level/event/requestId/userId/deviceId/code/durationMs/ip 字段 + LogSanitizer 脱敏；`sendResponse` 中央审计日志（成功 info/失败 warning，含耗时）+ 鉴权/会话/重放/envelope/限流安全事件带 `reason`；移除登录/注册明文 username/IP 日志；Server 连接生命周期结构化。新增 4 单测（RateWindow×2/StructuredLogger×2），`ctest` 5/6 绿（TestLocalStore 沙箱 DPAPI 偶发 fail-closed，单独运行通过）**【已更正，见 2026-09-09 行】**；CodeReview 子代理审查并修复 3 项（客户端限流重试/登出日志 userId 归因/服务器自发响应陈旧 type）。对应第 3 节两项 P2 欠账销账 |
| 2026-09-04 | M9 核心一致性完成 | 已读状态多端同步 + `sync_events` 保留清理（收掉 M9 三条基础验收）。① 已读多端同步：新增 `ReadCursorNotification (80)` 推送 + 已读者自身 `read_cursor` 事件，客户端 `markConversationRead` 按剩余未读重算角标、对方消息状态只前进；② 清理：`migrateToV8`（`sync_meta` 水位线）+ `pruneSyncEvents`（30 天/每小时，Server 独立维护连接）+ 落后设备 `needsFullSync`/`fullSyncSeq` 全量回退（历史经 `sync_messages` 从 messages 表补齐）。新增回归用例 `pruneSyncEventsPrunesExpiredAndAdvancesWatermark`/`readCursorEventRoundTrips`/`markConversationReadRecomputesUnreadAndOnlyAdvances`，`groupMigration` 版本断言更新至 V8；`ctest` 6/6；CodeReview 无 P0（当时 TestLocalStore 实为约 50% 概率失败，本行“6/6”不可复现，**已更正，见 2026-09-09 行**），修复 P1（`fullSyncSeq=0` 用 `qMax(maxSeq, prunedBelow)` 保证游标自愈）与 P2（未读角标改重算避免误清更新未读）。对应第 3 节 `sync_events` 清理与已读多端同步两项 P2 欠账销账；特性栈（置顶/免打扰/编辑/删除）仍按建议单独立项 |
| 2026-09-04 | 会话续期与失效重登完成 | 客户端接入 `expiresAt`：登录/续期响应解析过期时间，过期前 1 天自动 `renewToken`（续期响应 60 秒看门狗兜底 + 瞬时失败 5 分钟退避重试），续期换代先 `SecureMemory::wipe` 旧 token；服务端 `SessionInvalid/SessionExpired`（业务请求经 `MessageType::Error`、续期经 `TokenRenewResponse` 两路径）触发客户端安全清零 + `sessionExpired` 信号回登录页提示重新登录；断线重连重登后自动重新调度续期；`parseExpiresAt` 校正 Qt 对无时区 ISO 串按本地解析的偏移。对应第 3 节 P2 欠账销账；构建 41/41 目标通过，`ctest` 5/6（TestLocalStore 为已知沙箱 DPAPI 偶发，与本次改动无关）**【归因错误，已更正，见 2026-09-09 行】** |
| 2026-09-05 | M9 特性栈完成 | 会话置顶/免打扰 + 消息编辑/删除（软删除留墓碑）。协议类型 81-87（SetConversationPrefs/ConversationPrefsNotification/EditMessage/DeleteMessage）；V9 迁移（`conversation_members.pinned/muted`、`messages.edited_at/deleted`）；服务端三处理器（偏好仅成员可设、编辑/删除仅发送者可操作、编辑正文保持原 contentType 并经 fail-closed 密文校验拒绝明文注入）；客户端全链路（请求/响应/推送/sync_events/本地缓存，编辑解密前先失效旧解密缓存防命中编辑前明文）+ QML 右键菜单与"已编辑/已删除"展示；新增 `conversation_prefs`/`message_edited`/`message_deleted` 三类 sync_events 事件。新增 5 个单测（V9 列/偏好 set-get-回填/编辑/删除软删幂等，`groupMigration` 版本断言更新至 V9 并补 messages 表 fixture），`ctest` 6/6（TestLocalStore 沙箱 DPAPI 偶发单独运行通过）**【已更正，见 2026-09-09 行】**。对应第 3 节 P3 特性栈欠账销账 |
| 2026-09-09 | 周度审查 + M9 群编辑解密链路修复 + CI 重建 | 审查结论：工作区干净且与 origin/master 同步（上轮 M7b 悬置风险已闭环）；发现 8 处 ROADMAP 与代码/仓库不符、两项高危代码缺陷、一项被长期误归因的非确定性单测。修复：① **群编辑解密失败（R1）**——服务端 `message_edited`/`message_deleted` 事件与推送补 `senderId`（群密文靠它定位 Sender Key，旧实现缺失使编辑后的群消息在所有接收端不可解、且解密失败前已清缓存 → 本地已可读正文被清空）与 `originDeviceId`；客户端删除 `msgObj["senderId"] = 0` 硬编码，`decryptGroupMessageObject` 增按（群, 设备, keyId）反查发送者的兼容路径（修复前已落库的无 `senderId` 事件仍可解）；② **ratchet 与消息 id 顺序解耦（R2）**——编辑重加密使 `iteration` 大于其后发送的消息，离线按 `ORDER BY m.id ASC` 补收时先推进链状态会使后续消息命中回滚拒绝而永久不可解；`GroupE2eeCrypto::decryptMessage` 增可选跳序消息密钥缓存（Signal skipped-message-keys 语义，`MaxSkippedMessageKeys=1000` 超限淘汰最小 iteration，命中即一次性消费，伪造输入不污染缓存与链状态，仅解密+验签全部通过后提交），`LocalStore` 新增 `sender_key_skipped` 表加密持久化（磁盘无可读密钥）并随退群清理；③ **多端实时一致（R6）**——编辑/删除推送改为覆盖操作者本人（其名下其他设备），发起设备由客户端按 `originDeviceId` 去重（实时推送与 `sync_events` 补偿两路径）；④ **删除写入 fail-closed（R5附带）**——`deleteMessage` 返回值不再忽略，失败时返 `InternalError`、不广播事件、记 `message.delete_failed` 结构化日志；⑤ **TestLocalStore 非确定性根因更正**——`senderKeyLatestSelectsMostRecent` 约 50% 失败的真实原因是 `latestSenderKeyId` 按秒级 `updated_at` 排序并以随机 hex `key_id` 作并列破口（与 DPAPI/沙箱无关，上表四行归因均已标注更正）；改按 `rowid DESC` 选取（INSERT OR REPLACE 每次写入取得更大 rowid，“最新”= 最近一次写入）、`updated_at` 升为毫秒精度仅供诊断，修复后连跑 20 次 0 失败；⑥ **CI 重建**——恢复 `.github/workflows/cmake.yml`（windows-latest + Qt 6.8.3，configure/build/ctest，失败时上传 `LastTest.log`），销账“无机器门禁”并登记“首次运行未验证”为 P2；⑦ 顺手销账 `GroupE2eeCrypto.cpp` 两处 `QStringLiteral` 风格欠账。新增 9 个回归用例（`TestGroupE2eeCrypto` +6：乱序解密/一次性消费/伪造不污染缓存/伪造不烧毁缓存/容量淘汰/编辑重加密回归，共 27 passed；`TestLocalStore` +2：跳序密钥密文落库与维度隔离/按设备+keyId 反查发送者，并加固“最新密钥”用例，共 24 passed、连跑 20 次 0 失败）；构建全目标通过、`ctest` 6/6（连跑 3 轮全绿）。**CodeReview 子代理审查**：无 P0；P1（跳序缓存命中路径“先消费后认证”，伪造消息可烧毁合法密钥）与 P2（`removeSenderKeysForGroup` 两表删除非原子）已修正（改为认证成功后才 erase，且 erase 后再 wipe 才能真正清零底层缓冲；两表删除改为同一事务，事务不可用时降级为尽力清理），并补 `forgedMessageDoesNotConsumeSkippedKey` 用例锁定该语义。新增欠账（§3）：三端点无连接级限流（O(N) 事件放大）、编辑/删除单发槽位并发丢弃、`NetworkManager` 层无单测、85/87 复用推送类型、无 `AGENTS.md`、跳序缓存整块 blob 落库的写放大、密钥 base64 `QString` 短暂驻堆。文档同步：ROADMAP（§1 标题日期与 M0/M9 行、§1.2 本地存储/客户端 UI、§2 M0 更正 + 新增 M9 摘要、§3 销账与 8 项新登记、§4.2 验收注记、§4.3 M10 去重、§5 前置条件、§6 目录树、§7 LocalStore 新表、§8 四条新安全约束、§11 本行）、PROTOCOL（事件 payload 与 84-87 推送语义）、SECURITY（编辑删除章节 + 跳序密钥缓存的前向安全权衡）、README（构建/测试入口与 V9） |
| 2026-09-10 | M8 前置 P2/P3 欠账清理 | 按 §5 前置条件逐项清理本轮新增欠账，为 M8 启动解除阻塞。**Phase A（安全快速修复）**：① 三端点限流——`RequestHandler` 新增 `m_editDeleteWindow`（edit/delete 共用 20/60s）与 `m_prefsWindow`（prefs 30/60s）两个 `RateWindow`，在 `processEditMessageRequest`/`processDeleteMessageRequest`/`processSetConversationPrefsRequest` 鉴权后、业务前检查，超限回 `RateLimited (1003)`；② 专用推送类型——`Packet.h` 新增 `MessageEditedNotification (88)`/`MessageDeletedNotification (89)`，服务端编辑/删除广播的自发推送改用对应新类型（requestId 保持 0），客户端 `handlePacket` 新增两 case 路由至专用处理器，移除靠 `requestId==0` 区分响应与推送的 hack。**Phase B（客户端数据完整性）**：① 编辑/删除响应匹配队列化——`m_pendingEditMessageRequestId`/`m_pendingDeleteMessageRequestId` 单发槽改为 `m_pendingEdits`（QHash requestId→EditContext 多槽）+ `m_pendingDeleteRequestIds`（QSet）+ 私聊编辑 `m_privateEditQueue`（QQueue）经 `m_editFetchInFlight` 串行消费 `fetch_keys` 传输槽（镜像 `m_pendingSendByRequestId`/healing 队列范式）；`handleFetchKeysResponse` 编辑分支改队列感知、异步回调按 requestId 取上下文不再互相覆盖；`resetAuthState` 清空新容器；② 项目根新增 `AGENTS.md`（构建/测试/诊断/风格/完成定义）。**Phase C（测试基础设施）**：新增 `TestNetworkManager`（`QTEST_GUILESS_MAIN` + `friend class` 注入私有处理器/状态，LocalStore 不打开使 store 分支安全跳过、信号仍同步可捕），9 用例覆盖 `parseExpiresAt`、编辑/删除响应 requestId 多槽匹配与消费、88/89 推送发起设备去重（含同机不同账号不误删）、私聊编辑队列化不被覆盖、群编辑无密钥优雅失败、已读游标/会话偏好信号；已纳入 CTest。构建全目标通过、`ctest` 7/7 全绿。**CodeReview 子代理审查发现两项 P1（见下一行）并已修复**。遗留不阻塞 M8 的大群分发超限/先落盘后分发/TLS 集成测试/nonce/TOFU/非 Windows 保持登记。文档同步：ROADMAP（§1 标题日期、§1.2 传输安全、§2 M9 已知限制、§3 销账 5 行、§5 前置条件闭环、§6 目录树消息类型 1-89 与新测试、§11 本行）、PROTOCOL（新增 88/89 类型、编辑/删除推送语义与三端点限流）、SECURITY（三端点限流 + 编辑/删除并发安全） |
| 2026-09-10 | M8 前置清理 CodeReview 修复 | 对本轮 Phase A/B/C 改动运行 CodeReview 子代理审查。**无 P0**（构建/崩溃/越权级）；核查确认 A1 限流位置/参数、A2 广播与响应拆分、B1 requestId 匹配/去重（senderId+originDeviceId 双比）/编辑解密缓存（不预先清、失败不写空，无 M9 式不可逆烧正文）、resetAuthState 清理均正确。发现并修复 **两项 P1（均会复现本次想消除的“编辑静默丢失”）**：① **断线永久堵死编辑泵**——`onDisconnected`（自动重连不经 `resetAuthState`）未清新增容器，若私聊编辑的 `fetch_keys` 在途时断网，`m_editFetchInFlight` 永卡 true 使 `pumpPrivateEditFetch` 此后恒返回 → 本会话所有后续私聊编辑静默丢失，且残留队列头会劫持重连后发送链路的密钥包；修复：`onDisconnected` 一并复位 `m_editFetchInFlight`+清空 `m_privateEditQueue`/`m_pendingEdits`/`m_pendingDeleteRequestIds`，并对已入队/已发出未收响应的编辑/删除上报失败（新加 `disconnectClearsInFlightEditState` 回归用例锁定）；② **泵送契约不成立**——`flushOutbox` 占用 `fetch_keys` 传输槽后的非编辑响应路径仅调 `flushOutbox`、从不 `pumpPrivateEditFetch`，队首编辑可被无限期搁置；修复：在 `flushOutbox` 收口处补 `pumpPrivateEditFetch()`（内部再判槽空闲，无重入）。另采纳 P3：三端点 `allow()` 下移至入参形态校验之后（与 send/search 一致，避免畸形请求耗配额）。遗留 P2（`m_pendingEdits` 无超时 sweep，靠断线清理已覆盖主要静默丢失面）与 P3（私聊编辑另计一次 `fetch_keys` 预算；泵送/编辑解密回退的更细单测）登记不阻塞 M8。修复后构建全目标通过、`ctest` 7/7（`TestNetworkManager` 新增至 10 用例） |
| 2026-09-10 | M8.1 完成：文件与对象存储地基 | 按 §4.1 拆分的第一个切片（协议与存储地基）落地，不含数据面与客户端 UI。**① 协议层**：`CommonModule/protocol/FileProtocol`（消息类型 90-99、错误码 3013-3021、`FileManifest` 编解码与 fail-closed 校验、分片数学 `chunkCountFor`/`isChunkingValid`/`expectedChunkBytes`、体积/分片/票据/配额常量，客户端与服务端共用）；`send_message` 新增可选 `fileId`（`checkMessageFile` 校验存在/本人/ready，存储未注入时 fail-closed 回 `FileStorageFailed` 而不降级投递）并在响应/推送/事件/历史读取四条路径回传；带 `fileId` 的消息不可编辑正文。**② 加密原语**：`CommonModule/encryption/FileCrypto`（每文件独立 AES-256 密钥 + 12 字节 nonce 前缀，第 i 片 nonce = `iv` 后 4 字节 XOR 大端 `i`、AAD = 大端 `i`，流式 SHA-256，票据生成与摘要）；`E2eeCrypto` 新增带 AAD 的 AES-GCM 原语（旧接口不变）。**刻意不复用消息 ratchet**，避开 M9 编辑踩过的“链已推进→早先分片永久不可解”。**③ 对象存储**：`Chat-Server/storage/IObjectStorage` 抽象 + `LocalFileStorage`（临时文件+原子改名、blobKey 前两位分 256 桶、同键 `finalize`/`remove` 条带锁串行、流式组装并逐片核长度 + 整体核 SHA-256、`FinalizeStatus` 六分类）。**④ 数据层**：V10 迁移（`files`/`file_tickets`/`messages.file_id` + `idx_files_uploader_status`/`idx_files_status_created`/`idx_messages_file`/`idx_file_tickets_expires`）；元数据 CRUD（创建含原子并发配额、状态迁移终态不可逆、票据签发/校验/消费/清理只存摘要、`canUserAccessFile` 访问控制、三个回收查询）。**⑤ 控制面**：五个处理器 + 两个新限流窗口（新建上传 20/60s、其余文件操作共用 60/60s，入参形态校验先于限流消费）+ 幂等（完成/取消）+ finalize 分类回不同错误码（3015 数据故障标 failed 并回收 / 3020 存储故障保留现场）；`Server` 创建并注入存储（初始化失败则不注入，文件接口一律 fail-closed），维护连接新增 `pruneFileUploads`。**⑥ 回收三轮**：超期未完成上传（48h）先删盘后标 cancelled；终态行先删盘后删行（兼孤儿清理，防 `files` 无界增长）；已就绪但无引用的行（附件消息被软删或发送未发生）**只原子迁入终态不碰磁盘**，销毁推下一轮（引用判定与迁移合并为单条语句消除 TOCTOU，否则并发 `sendMessage` 可能在两步之间引用该文件而磁盘数据已被删 → 附件永久打不开）；宽限期以 `completed_at` 为基准，避免续传数天后刚完成的大文件被当成孤儿。**验证**：新增 `TestFileProtocol`（30）与 `TestObjectStorage`（19），`TestDatabaseManager` 新增 14 个 M8 用例至 63 passed；`ctest` 9/9 全绿（共 227 用例）；构建全目标通过。**两轮 CodeReview 子代理审查**：第一轮（M8.1 主体）无 P0；2 项 P1（元数据枚举预言机：“不存在”与“不是你的”差异化错误码可遍历他人 `fileId`；存储层并发：同键并发组装/删除与固定临时名互相覆写）、1 项 P2（配额“先读计数后插入”TOCTOU）、1 项 P3（终态行与孤儿数据无回收路径）均已修复并补回归用例（含崩溃残留清理、配额原子性）。第二轮（针对回收增量）无 P0/P2；**1 项 P1 已修**——“迁入终态后不可能再被新消息引用（发送校验要求 status=ready）”这一不变量在跨线程下**不成立**（每连接一个 `RequestHandler` 线程，`checkMessageFile` 与消息 INSERT 之间有窗口，维护任务可在其中把当时确实无引用的文件迁入终态，随后下一轮先删盘后删行→仍被引用文件的磁盘数据被销毁，附件永久打不开）；修复采双侧防护：发送侧 `sendMessage` 把“文件仍为 `ready`”下推为 `INSERT ... SELECT ... WHERE EXISTS` 守卫子查询（单语句原子 + SQLite 写者串行，两种交错都安全；守卫未命中不写入任何行并置 `fileNotReady`，两个发送路径据此回 `FileNotReady` 而非 `InternalError`），回收侧终态那一轮在 `remove()` 前先 `isFileReferencedByMessage` 再判一次（宁可留下可修复的 cancelled 行 + 盘上对象，也不销毁不可恢复的数据），并同步更正三处文档/注释中该错误不变量的表述；新增 `sendMessageGuardsFileReadyStateAtomically` 用例锁定（ready 可发/已迁移不可发且无半截行/uploading 与不存在 fileId 被拦/普通消息不受影响/未传出参不崩溃）。2 项 P3：配额用例残留的绝对值断言已改为差值口径；回收在主线程做同步磁盘 I/O 登记入 §3（量级有界且定时器不重入，属响应性隐患）。**另修正两项测试非确定性**：`fileRecordCreateAndRetrieve` 遗留 `uploading` 行会吃掉后续用例的并发配额（补转终态收尾），配额用例改为差值断言（与 `uploadingCountTracksActiveUploads` 既有约定一致）。新增 9 项欠账（§3）：数据面未实施与票据无生产调用点、单机存储无副本、控制面五处理器无自动化测试（均 P2）；无存储用量配额、下载票据 TTL 内可重用、`putChunk` 不入锁、回收查询每轮 limit=100、文件消息不可编辑而缺“撤回重发”替代路径、回收在主线程做同步 I/O（均 P3）。文档同步：ROADMAP（§1.1 M8 行转进行中、§1.2 新增“文件与媒体传输”能力行、§3 新增 9 项、§4.1 拆为 M8.1/M8.2/M8.3 三切片、§5 下一切片指向 M8.2、§6 目录树、§7 V10、§8 七条新安全约束、§11 本行）、PROTOCOL（标题与状态段、类型表 90-99、错误码 3013-3021、新增 M8 章节：通道划分/清单/加密/分片口径/五接口/`fileId`/状态机与回收）、SECURITY（新增“文件与媒体传输安全”节、限流三条、数据库 V10、风险与后续要求）、ARCHITECTURE（标题日期、组件图与职责、新增 M8 能力边界节、Schema V10 与 `sender_key_skipped` 补登、下一步演进；并补正 M9 编辑/删除章节陈旧表述：88/89 拆分、`senderId` 寻址、跳序密钥缓存、不预清缓存与幂等回退、多槽并发匹配）、README（目录说明、存储根目录、测试 9 套与三个新套件）、AGENTS（测试清单 9 套 + 三条新风格约束：单语句原子迁移、销毁顺序、错误分类）。已提交 `e8b6df1`（2026-09-10） |
| 2026-09-11 | M8.3a 完成：图片元数据与内联缩略图 | 按 §4.1 拆分的第三个切片的图片部分落地（音视频拆为 M8.3b）。**① 提取**：新增 `Chat-Client/core/ThumbnailMaker`，只用 QtGui（`QImageReader`/`QImage`）而**不依赖平台多媒体后端**，因此在无头环境与 CI 中可稳定验证；按最长边 160px 缩放（`setScaledSize` 先缩放再解码，避免把 50MP 大图完整读进内存），逐步降质量 70/55/40/25/15、质量到底仍超限再折半降尺寸（下限 32px）；**压不进 `MaxThumbnailBytes`（4096）就不内联**，UI 回退到文件图标（绝不放宽上限：清单会撑破 `MaxGroupMessageLength` 而使整条文件消息被服务端拒收）；`setAutoTransform(true)` 校正 EXIF 方向，并据解码后朝向修正上报宽高（旋转 90/270 度会交换宽高，否则 UI 显示的宽高比与实际不符）。**② 接入**：`FileTransferManager::uploadAndSend` 提取并写入清单 `width`/`height`/`thumb`（非图片或提取失败时留空，**元数据缺失绝不阻断发送**）；`NetworkManager::attachFileInfo` 新增脱敏字段 `fileWidth`/`fileHeight`/`fileThumb`（base64 JPEG）。**③ 安全口径**：缩略图是 JPEG **明文**字节，同时修正了 M8.1 将 `FileManifest::thumbnail` 注释为“缩略图密文”的不准确表述：清单整体随消息正文经既有 E2EE 加密，再单独加一层只增加复杂度而无安全收益；也正因为它**不含任何密钥**，可以经 `sanitizeForUi` 交给 QML，使接收方**在下载原图之前**就能展示预览（零流量零等待），而服务端仍然全程不可见。**④ UI**：`MessageBubble` 新增内联缩略图（`data:image/jpeg;base64,`，仅在 `status === Image.Ready` 时显示，解码失败不留空白区域）与像素尺寸行；`ChatView` 新角色透传（分隔线条目同步补齐以保持 ListModel 角色一致）。**验证**：新增 `TestThumbnailMaker`（8 用例：尺寸与可解码缩略图与宽高比保持、高熵图不超限、极小上限宁可留空也不超限、非图片/缺失文件/非法参数安全返回、base64 膨胀后仍在群消息正文预算内；JPEG 插件缺失时相关断言 QSKIP）；`TestFileTransfer` 新增 `imageUploadCarriesThumbnailMetadata`（图片上传后清单带正确尺寸与可解码缩略图，清单整体仍在正文预算内；并补断言非图片文件不携带缩略图与尺寸）至 13 用例；`ctest` **12/12** 全绿；`qmllint` 零错误。新增 2 项欠账（§3）：无应用内大图查看器（看原图靠“另存为”，需 `QQuickImageProvider` 从密文缓存解码）、历史图片消息无缩略图（M8.3a 之前发送的消息 `thumb` 为空）。文档同步：ROADMAP（§1.1 M8 行、§1.2 能力行、§3 新增 2 项、§4.1 拆为 M8.3a/M8.3b、§5 下一切片指向 M8.3b、§6 目录树、§11 本行）、PROTOCOL（清单 `thumb`/`width`/`height` 语义与“明文而非密文”的理由）、SECURITY（缩略图经 E2EE 分发与可进 QML 的依据）、ARCHITECTURE（客户端组件与 M8.3 能力边界）、README 与 AGENTS（测试 12 套）。**M8.3b（音视频时长/封面/播放器）未开始**：需 QtMultimedia 与平台解码后端（Windows 上 Media Foundation），异步加载与当前同步上传入口相冲突，且封面抓帧依赖 seek 能力，在 CI 上不可验证，建议在真实桌面环境实施并人工验证 |
| 2026-09-11 | M8.2 完成：数据面 HTTP(S) + 客户端上传下载与 UI | 按 §4.1 第二个切片落地，文件消息自此具备可用的端到端路径。**① 数据面**：新增 `Chat-Server/http/FileHttpService`——`QHttpServer` 经 `QSslServer` 承载，与主通道共用同一套开发证书与 fail-closed 口径（TLS 不可用且未显式允许明文则拒启）；两端点 `PUT /file/<fileId>/chunk/<index>` 与 `GET /file/<fileId>`（单区间 Range：206 + `Content-Range`、起点越界 416 + `bytes */N`、语法非法 400、区间或整体超 4 MiB 上限 413 + `Accept-Ranges`，range-unit 大小写不敏感）；票据只走 `X-XYChat-Ticket` 请求头（不进 URL query，避免落入代理/访问日志），服务端只比对 SHA-256 摘要；四种票据失败统一 401 且响应体一致（防枚举预言机），票据通过后的 404/409 才对持票者可见；分片长度按 `expectedChunkBytes` 精确匹配（防客户端自选分片边界绕过体积/分片数校验）；下载恒 `application/octet-stream` + `Cache-Control: no-store`；per-IP 限流只计授权失败与畸形请求（30/60s → 429，成功搬运不计，否则 4096 次 PUT 的大文件上传会被挡）；`readRange` 短读一律 500 而不返回截断数据。**② 服务端集成**：`Server::setFileHttpEndpoint`/`fileTransferBaseUrl`/`fileHttpReady`，仅对象存储就绪时启动数据面（启动失败只关文件能力不影响消息收发）；`main.cpp` 新增 `--http-port`（12346）/`--http-host`（127.0.0.1，服务端无法自知 NAT/反代后的对外地址）；登录响应下发 `fileTransferBaseUrl`（未启动则不下发，客户端禁用文件能力而不猜端口）；`DatabaseManager::revokeFileTickets` 新增，上传完成/取消/标失败后立即吊销上传票据（TTL 24h 而分片已组装回收，留着只延长泄露窗口）。**③ 客户端引擎**：新增 `Chat-Client/core/FileTransferManager`——两遍加密上传（第一遍流式加密只算密文整体 SHA-256、不落临时文件，代价是多一遍 AES，换来磁盘占用不翻倍且无崩溃残留；加密确定性保证两遍密文逐字节相同）；串行分片调度；失败后先查 `receivedChunks` 再决定跳过/重传；重试预算双层（分片 3 次 + 恢复 5 轮）；下载按分片边界 Range GET → 临时密文 → 整体 SHA-256 自校验 → 原子改名进缓存；与控制面经“请求信号 + seq 回调”解耦（不持 socket，可脱离网络单测）。**④ 本地缓存（经调研后定调）**：下载的密文**原样落盘**（`<AppData>/XYChat/filecache/<sha256[0..1]>/<sha256>`，无后缀），零额外加密开销且磁盘上天然不是明文；明文只在用户“另存为”时写出（临时名 + 改名）。调研依据：Signal Desktop（`attachments.noindex/` 加密落盘，按消息内 `localKey`/`iv` 解密并校 SHA-256）、Telegram Desktop（`tdata/<账号>/cache`、`media_cache` 加密落盘，`PlaceFromId()` 随机 14 字符名 + 前 2 字节分桶）、Signal/Session Android（`app_parts/*.mms` AES-CTR + HMAC）——**无一家采用“明文 + 混淆文件名”**，随机/无后缀命名只用于防索引与防目录浏览识别类型，不承担保密职责（文件头魔数使类型探测成本极低）；若明文缓存，则 `LocalStore` 对正文的加密在含附件的会话上完全被绕过。故采用“密文直存 + sha256 命名（协议已有该摘要，一份值兼作缓存键/校验值/不可预测文件名）+ 256 桶”；不用 MD5（不抗碰撞且会凭空引入第二套摘要口径）。另：每文件独立密钥使相同明文产生不同密文，因此无法跨文件去重（E2EE 固有性质，也是隐私优点）。**⑤ 协议修正**：`FileManifest` 新增必填 `chunkSize`（+ `isValid` 校验与 encode/decode）——写引擎时发现 M8.1 的缺口：接收方若靠服务端声明的分片口径解密，不可信的服务端就能让解密错乱，清单必须自包含；客户端 `onDownloadTicket` 将 `sizeBytes`/`chunkSize`/`chunkCount`/`sha256` 四项与清单逐一比对。**⑥ NetworkManager 集成**：拥有并接线引擎（5 请求/5 响应 + `requestId`→`seq` 映射，未认证时同步回失败而不静默挂起）；`sendMessage`/`sendGroupMessage` 新增 `fileId` 参数并写入请求 JSON（群聊/私聊两条 flush 路径）；**文件消息不进持久化 outbox**（outbox 表无 `file_id` 列，落库后重发会退化成“正文是清单”的普通消息 = 把密钥当文本发出）；`attachFileInfo` 登记清单并补脱敏展示字段（`fileId` 缺失时从清单内取回，因 `LocalStore.messages` 无 `file_id` 列）；会话预览改 `[File] <名>`（否则会话列表会展示一大段含密钥的 JSON）；登出与断线两路径均 `reset()`（清零清单密钥 + 中止在途 + 清 baseUrl）。**⑦ QML UI**：`MessageInput` 附件按钮（回形针 Canvas）+ `FileDialog`；`MessageBubble` 文件面板（图标/名/大小/进度条/下载/另存，`formatSize`，编辑菜单项对文件消息禁用）；`ChatView` 新角色与信号 + `updateFileState`/`updateFileProgress` + `isCached` 初始化状态；`MainPage` 传输横幅（阶段/百分比/取消/提示）、另存 `FileDialog`、`Connections` 接线与 token→messageId 映射；引擎经 `main.cpp` 注册为 context property `fileTransfer`。**验证**：新增 `TestFileHttpService`（11）与 `TestFileTransfer`（12，含 2.5 MiB 跨 3 片文件的端到端字节级往返与“服务端只见密文”断言），`TestNetworkManager` +1（P0 回归 `fileManifestNeverReachesUiLayer`）至 13，`TestFileProtocol` +1 至 31；`ctest` **11/11** 全绿；`qmllint` 零错误。集成测试当场抓到编译期无法发现的真实缺陷：`QAbstractHttpServer::bind()` 要求 server 已在监听，原顺序反了（生产环境同样会启动失败）。**CodeReview 子代理审查发现 3 项 P0 均已修**：① **文件密钥泄入 QML**——脱敏只做在实时推送一条路径，`sync_messages`/本地缓存回填/`sync_events` 三条路径会把含 `key`/`iv` 的清单 JSON 当正文渲染进气泡（可截图/可转发，E2EE 核心承诺被破坏），修为唯一出口 `sanitizeForUi`/`sanitizeArrayForUi` 收口四条路径 + “形态像清单就置空并告警”兜底；② **use-after-free**——`finishTask`/`failTask` 的 `const QString &token` 往往是容器内 `Task::token` 的引用，`m_tasks.erase` 后悬垂而 `emit` 还要读它（每次成功/失败/取消都会走到），修为 emit 前取独立副本；③ **迭代器失效 + 嵌套泵送**——`pumpNext` 在 `QHash` 遍历中调 `pumpUpload`/`pumpDownload`，它们可同步 `failTask` → `erase` 当前节点，且 `failTask` 递归调 `pumpNext`，修为 token 快照 + `m_pumping` 护栏。**3 项 P1 已修**：重试预算被“成功的查询”清零 → 数据面持续 5xx 时活锁（新增 `recoveryRounds` 封顶）；下载气泡永久卡“下载中”（`download()` 在缓存命中/清单缺失时同步完成，token→messageId 映射晚于信号，改为先置状态再调用并按 `isMessageFileAvailable` 校准）；发送方无法下载自己发的文件（`cached` 未带 `messageId` 致清单无法登记）。**P2/P3 各修两项**：`reset()` 中 `abort()` 同步回调会在登出途中发新请求且可能残留 `m_activeReply` 使新会话永久卡死（`m_resetting` 护栏 + 先标 cancelled + 断开回调 + 显式复位）；QML 正则剔 `file://` 前缀在 UNC 与含 `%`/`#`/`?` 路径上会**静默写到乱码文件名**（改 `Qt.urlToLocalFile`）；`saveToFile` 把“目标写失败”误当缓存损坏而删掉完好缓存（拆为 `cacheCorrupt`/`destFailed`）；`Range` 的 range-unit 改大小写不敏感（否则合法请求白吃 per-IP 失败配额）。新增 9 项欠账（§3）：上传 hashing 在 GUI 线程同步、下载票据 TTL 不足且无断点续传、`QHttpServer` 进 handler 前已缓冲请求体（未认证内存放大）均 P2；`LocalStore.messages` 无 `file_id` 列、`clearCache` 删在途 `.tmp`、`finalizeDownload` rename TOCTOU、`saveToFile` 同步解密、横幅只跟单个上传、QML O(n) 扫描、`markFileTicketUsed` 无调用点均 P3；并明确 P2「控制面五处理器无自动化测试」**未销账**。文档同步：ROADMAP（§1 日期、§1.1 M8 行、§1.2 能力行、§3 销账与 9 项新登记、§4.1 M8.2 转已完成、§5 下一切片指向 M8.3、§6 目录树、§8 五条新安全约束、§11 本行）、PROTOCOL、SECURITY、ARCHITECTURE、README、AGENTS。**双客户端实机联调尚未做**（属人工验证，见 §4.1 已知限制） |
| 2026-09-11 | M8.3c 完成：应用内大图查看器 + 修复 `Qt.urlToLocalFile` 运行时错误 | 按 §4.1 M8.3 拆分的图片部分第二个切片落地（音视频仍为 M8.3b）。**① 图像源**：新增 `Chat-Client/core/FileImageProvider`（`QQuickImageProvider` 子类，以 `image://xyfile/<messageId>` 注册到 QML 引擎）：从密文缓存逐片解密并在内存中解码，**明文不落盘**（与 SECURITY.md 的“磁盘上不存在可读明文”口径一致，看原图不再必须“另存为”）；`requestImage` 在渲染线程执行，故只经 `FileTransferManager` 的线程安全接口取数据（`manifestFor` 加锁拷贝清单、IO 与 GCM 认证在锁外），不直接触碰任何容器。**② 引擎新接口**：`FileTransferManager::decryptedFileBytes(messageId)`（返回解密后的完整明文字节，仅用于应用内图片查看）与 `MaxInMemoryDecodeBytes = 64 MiB` 上限（超限直接返回空，避免把整个明文读进内存把进程打爆；大文件应走 `saveToFile` 流式解密写盘）；`manifestFor` 抽出为线程安全的私有读取入口（`m_manifestMutex` 保护 `m_incoming`，主线程写入/渲染线程读取）；`registerIncomingFile`/`reset`/`clearCache`/`download`/`pumpDownload`/`saveToFile` 均改走 `manifestFor` 以统一加锁口径。**③ 修复 P0 运行时错误**：原 `MessageInput.qml` 与 `MainPage.qml` 调用 `Qt.urlToLocalFile()`——QML 全局 `Qt` 对象**并无**此函数（那是 C++ `QUrl` 的方法），运行时抛 `TypeError: Property 'urlToLocalFile' of object Qt(...) is not a function`，导致附件无法发送、另存为失败。修为：`MessageInput` 的 `attachmentSelected` 信号参数改 `var` 以保留 `QUrl`（转字符串会引入 percent-encoding 的二次编解码歧义），直接把 URL 交给 C++；`MainPage` 另存对话框改调 `fileTransfer.toLocalPath(selectedFile)`；`FileTransferManager` 新增 `Q_INVOKABLE toLocalPath(QVariant)`（`QUrl` 走 `toLocalFile()`、字符串以 `scheme:` 开头且非 Windows 盘符时按 URL 解析、其余原样返回），`uploadAndSend`/`saveToFile` 参数改 `QVariant` 以吃下 URL 与路径两种形态。同步修正 `AGENTS.md` 中误导性的“用 `Qt.urlToLocalFile()`”规则（该规则正是本次错误的根因）。**④ UI**：`MessageBubble` 缩略图与文件图标均加 `MouseArea`（仅 `isImageFile && fileState === "available"` 时可点，否则先走下载）发 `previewRequested`；`ChatView` 转发为 `filePreviewRequested(messageId)` 并新增 `getMessageById(messageId)`（供预览对话框查消息字段）；`MainPage` 预览对话框（`Dialog` + `Image` source 绑 `image://xyfile/<id>`、`asynchronous: true`、`cache: false` 以免缓存被清理后仍展示旧图；`BusyIndicator` 加载态；错误态提示“文件未就绪、不是图片，或已超过内存解码上限（64 MB）”；标题带文件名与像素尺寸；footer 提供“另存为”与“关闭”，另存为复用气泡上的保存链路）。**验证**：`TestFileTransfer` 新增 `toLocalPathAcceptsUrlsAndPlainPaths`（QUrl/字符串 file:// URL/UNC/纯本地路径/非 file 协议/空值/Windows 盘符七种形态）与 `decryptedFileBytesRestoresPlainForRegisteredMessage`（未登记/未下载/超上限返回空、登记+下载后逐字节还原）至 15 用例；`ctest` **12/12** 全绿；`qmllint` 零错误（仅预存 unqualified access 警告）。对应第 3 节 P3「无应用内大图查看器」欠账销账。文档同步：ROADMAP（§1.1 M8 行、§1.2 能力行、§3 销账 1 行、§4.1 M8.3 拆为 M8.3a/c 已完成 + M8.3b 未开始、§11 本行）、AGENTS（修正 `Qt.urlToLocalFile` 规则为“QML 全局 Qt 对象并无此函数，把 QUrl 交给 C++ 由 `fileTransfer.toLocalPath()` 转换”）。**已知限制**：超过 64 MiB 的图片无法在应用内预览（需“另存为”后用外部查看器）；无缩放/平移控件（当前 `PreserveAspectFit` 适应窗口） |
| 2026-09-11 | M8.3b 完成：音视频元数据与播放器 + 历史图片缩略图补齐 | 按 §4.1 M8.3 拆分的音视频切片落地，M8.3 全部完成。开工前经用户确认两项取舍：异步冲突采“重构上传入口为异步”、播放器采“C++ 解密 QIODevice + QMediaPlayer（明文不落盘）”。**① 元数据提取**：新增 `MediaMetadataExtractor`（QMediaPlayer + QVideoSink 异步加载），提取音频时长、视频时长/分辨率，视频 seek 到 min(1000,duration/2)ms 抓封面帧（QVideoFrame::toImage → ThumbnailMaker::encodeThumbnail 压缩）；5 秒超时护栏，超时/错误/非音视频 emit finished(ok=false)，已拿到时长/分辨率但抓帧超时仍上报前者；ThumbnailMaker 抽出 encodeThumbnail(QImage) 供封面复用。**② 异步上传重构**：uploadAndSend 拆为“元数据提取→hashing→creating”三阶段，图片同步、音视频异步（extracting 阶段，同一时间只提取一个其余排队 m_pendingExtractTokens），beginHashing 抽出供回调；元数据缺失绝不阻断发送；cancelTask/finishTask/failTask/reset 清理提取队列（clearExtractionStateForToken）。**③ 清单与气泡**：FileManifest.durationMs 填充（M8.1 已预留），attachFileInfo 透传 fileDurationMs；MessageBubble 新增 isAudioFile/isVideoFile/playRequested/formatDuration，视频封面中央 ▶ 播放按钮 + 右下角时长标签，音频图标改 ▶ + 文件名下方时长，点击按类型分流（图片预览/音视频播放）。**④ 播放器（明文不落盘）**：新增 DecryptingIODevice（QIODevice 子类，从密文缓存流式逐片解密，readData/size/seek/bytesAvailable，支持拖动进度条）与 MediaPlaybackManager（C++ QMediaPlayer + DecryptingIODevice + QAudioOutput + QVideoSink，setSourceDevice 播放，Q_PROPERTY 暴露 playbackState/position/duration/volume/hasVideo/active），main.cpp 注册 context property mediaPlayer，QML 播放器对话框（视频静态封面/音频图标 + 播放/暂停/进度/音量）。**⑤ 历史图片缩略图补齐（M8.3a 欠账销账）**：localThumbnailForMessage 在清单 thumb 为空且原图就绪时解密生成缩略图缓存到 <cacheRoot>/thumbs/，attachFileInfo 回填 fileThumb，clearCache 递归清理 thumbs。**验证**：TestFileTransfer 新增 3 个 DecryptingIODevice 用例（顺序读取逐字节还原、seek 跨分片定位、未下载/未登记 open 失败）至 18 用例；ctest 12/12 全绿；qmllint 零错误。集成测试抓到真实缺陷：ensureChunkFor 重新解密分片后 m_chunkBufferOffset 置 0 而非片内偏移，seek 到片中间错位返回片头数据（decryptingIODeviceSupportsSeek 捕获修复）。新增欠账（§3 P3）：视频播放无动态画面（QML VideoOutput 无法绑定 C++ QVideoSink，只输出音频轨 + 静态封面，需自定义 QSGNode）。已知限制：音视频元数据提取与播放依赖平台解码后端（Windows Media Foundation），CI/无头不可验证（提取失败留空、播放报错，均不崩溃）。文档同步：ROADMAP（§1.1 M8 行、§1.2 能力行、§3 销账历史缩略图 + 登记视频渲染、§4.1 M8.3b 转已完成、§11 本行）、PROTOCOL（durationMs 语义）、SECURITY（播放器明文不落盘 + DecryptingIODevice）、ARCHITECTURE（客户端新组件）、README/AGENTS（测试 12 套不变） |
