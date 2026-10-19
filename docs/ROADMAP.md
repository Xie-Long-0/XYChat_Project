# XYChat 长期实现路线图

本文档面向 Qt/C++ Client + Server 基础框架，目标是逐步演进为一个"类 Telegram"的安全即时通信系统。路线图按"先稳定基础，再做通信能力，再做安全与规模化"的顺序推进，便于长期迭代、验收和回滚。

> **文档形态（2026-09-15 精简）**：本文只保留**结论、取舍与约束**——能力矩阵、已完成里程碑摘要、集中管理的欠账清单、未来规划与变更记录。实施流水账（逐项勾选清单、审查过程的逐步修复记录、文档同步明细）一律不保留，历史细节经 `git log` 与 `docs/ARCHITECTURE.md` 追溯。同一事实只在一处展开：**现状**看 §1.2、**里程碑结论**看 §2 与 §4.1-§4.8、**欠账**看 §3，其余位置用指针引用。

## 0. 文档定位与维护约定

- **状态标注**：里程碑状态取 `已完成` / `未开始` / `进行中`；节内未实施项以"未实现"文字标注，不使用悬挂的空复选框。
- **更新方式**：里程碑完成时同步更新四处——① 状态总表；② 已完成能力摘要；③ 欠账清单（新增或销账）；④ 变更记录表。**禁止**再向文档头部追加流水账式更新引用块。
- **精简原则**：已完成内容只写"交付了什么、如何验证、已知限制是什么"，不写实现过程与审查过程；审查结论只保留"发现几项 P0/P1、是否已修、未修项已登记 §3"。
- **一致性要求**：涉及协议/安全/架构事实的表述必须与 `docs/PROTOCOL.md`、`docs/SECURITY.md`、`docs/ARCHITECTURE.md` 及代码一致；发现文档与代码不符时，以代码为准并在当期修正文档。
- **完成定义**：见第 9 节；里程碑勾选"已完成"前必须通过对应自动化测试与代码审查。

## 1. 项目现状总览（截至 2026-09-30）

### 1.1 里程碑状态总表

| 里程碑 | 名称 | 状态 | 完成日期 | 交付摘要 |
| --- | --- | --- | --- | --- |
| M0 | 工程基线与可维护性 | 已完成 | 2026-07-01 | README、`.gitignore`、`docs/` 四文档、GitHub Actions CI、Qt Test、顶层 CMake 统一（C++20、`XYCHAT_BUILD_TESTS`）。**注**：CI 工作流曾于 2026-08-03 随 `31622df` 被删、连续 5 周无机器门禁，2026-09-09 已重建（首次运行先后暴露两处配置缺陷，均已于 2026-09-16 修复，待绿色运行确认） |
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
| M11 | 体验完善与稳定性 | 进行中 | — | **M11A 基础体验与系统集成（已完成 2026-09-15）**：设置页（`AppSettings` 统一管理 + `SettingsDialog` 四节）、系统托盘（`TrayManager` + 最小化到托盘）、桌面通知（免打扰/预览开关/点击跳转）、草稿（会话切换保留输入）、本地消息搜索（`LocalStore.searchMessages` 解密后 LIKE + `LocalSearchDialog` + 跳转滚动）。`ctest` 12/12、`qmllint` 零错误。**M11B 消息交互增强（已完成 2026-09-21，含双客户端实机验收）**：多选模式、消息转发（文本）、消息回复/引用（`send_message` 可选 `replyToMessageId` + V11 迁移，M11 唯一协议扩展）、emoji 选择器；`ctest` 14/14、QML 行为用例 19 项、`qmllint` 零错误（09-23 入库前复查另修 B4 定位函数的死分支并补 outbox 恢复用例）。M11C（群聊与布局完善，§4.5）：**C1 群信息抽屉已完成 2026-09-30**（`GroupInfoPanel`+`GroupInfoDrawer` 替代弹出式 `GroupInfoDialog`，非模态右缘滑出，切会话跟随/退群收拢，QML 行为用例 10 项 + 负向对照 5 项）；**C2 最后在线时间与 C3 响应式布局已完成 2026-10-15**（私聊头部显示"在线 / 最后在线 …"，`get_conversations` 回填 `lastActiveAt`/`online`、无迁移；宽屏三栏由 `GroupInfoPanel` 常驻右栏，窄屏沿用抽屉，跨断点双向转移；顺带修复抽屉遮挡自定义标题栏），M11C 至此完成；原 M11 稳定性/可观测性任务（指标监控/崩溃捕获/压测）并行推进，见 §4.6 |
| M12 | 聊天流畅性与多会话体验 | 已完成 | 2026-09-16 | 修复两项用户报告的运行时问题：发送消息后视图滚出边界、切换会话时整页加载阻塞界面。① 滚动越界修复（贴底定位以末行 delegate 真实末端 `layoutExtent` 为准并据此夹取 `contentY`，贴底请求经 `Qt.callLater` 合并延迟，规避 ListView 布局重入）；② 消息加载异步化（C++ 单线程时间片泵 8ms/片、`sync_messages` 多槽路由、落库整页单事务；本地搜索命中的文件清单补过脱敏口径）；③ QML 增量应用消息（`setMessages` 按 messageId 合并：已存在行原地更新、新消息按 id 序插入，未读分隔线与附件下载进度不再被刷新页重置，状态只前进不回退）；④ 按会话堆叠 ChatView（每会话一个实例 + 活跃指针，切换只改可见性，消息/滚动/草稿/附件进度各视图自持；LRU 上限 8 实例并在驱逐时转存草稿；MainWindow 按 conversationId 路由消息页与推送、传输事件按 messageId 路由；切回已打开会话只补收 `lastMessageId` 之后的增量，已读回执改挂激活点）。测试：Qt Quick Test 10 + 11 项（含审查修复项 3 项负向对照）；`ctest` 12/12（`TestNetworkManager` +1，含跨片整页单次 emit 用例）。双客户端实机验收通过（2026-09-20：发送后稳定贴底不越界、快速切换会话无卡顿、各会话状态互不污染、群 E2EE 活链路密钥分发与解密正常）。详见 §4.7 |
| M13 | 退群语义与本机历史治理 | 已完成 | 2026-09-23 | 修复用户报告的缺陷“退群后刷新会话列表，已退出的群又出现且无法删除（可看历史、收不到也发不出消息）”，并把退群的本地历史去向交还用户：主动退群时二选一【仅退出（保留历史，会话留列表但只读）/ 退出并删除本机记录（清空本地缓存并从列表移除）】，被管理员移出默认保留历史。根因是客户端把本地缓存当只增不减的展示源（服务端不再返回的会话既不被标记也不被移除，任何一次缓存先行都会把它显示回来），而现在右键“删除会话”对非成员必被服务端拒绝。落地：`LocalStore` 新增 `left_group` 列与 `conversation_clear_marks` 清空水位表 + `reconcileConversations` 服务端权威对账（缺席的群标记为已退出、被移出与重加入自动复位）；`NetworkManager` 退群在途请求改多槽并携带用户选择，`syncMessages` 起拉点抬到清空水位（已删历史不复活），新增仅删本机的 `deleteLocalConversation`；QML 只读态（输入区换提示条、群信息入口与消息级操作收起、会话列表条目与右键菜单收敛）。**无线上协议变更、无服务端改动**；`ctest` 14/14、QML 行为用例 5 项（负向对照 4 项）、`qmllint` 零错误。详见 §4.8 |

### 1.2 能力矩阵

| 功能 | 详情 |
| --- | --- |
| 账户与认证 | 注册/登录/登出/token 续期/`terminate_session`（仅本人其他会话）；PBKDF2 密码存储；登录失败限流（IP 5min/10 次、用户 5min/5 次）；多设备识别（`deviceId` 取自机器唯一 ID）；逐包验 token + `validateSession()` 回查 `sessions` 表（过期/终止/续期换代即时失效，`expiresAt` 解析异常 fail-closed）；会话自动续期 + 失效自动重登（过期前 1 天 `renewToken`、60 秒看门狗、失效回登录页）。**未实现**：双因素认证、注销/找回 |
| 一对一聊天 | E2EE（envelope 密文，服务端 fail-closed）、`clientMessageId` 幂等、乐观发送、per-recipient 回执（delivered/read）、消息状态实时推送、离线 outbox（加密持久化，跨重启重发）、回复/引用（可选 `replyToMessageId`，服务端只回 ID 与被引用者名、不产出摘要，正文由接收端本地解密渲染） |
| 群聊 | 建群/邀请/退群（群主自动转让）/踢人（角色层级保护）/群信息；群 E2EE（Sender Keys，服务端只见密文）；成员变更 Sender-Key healing（新成员获密钥、被移除成员失后续解密能力，离线经 `sync_events` 补偿）；系统消息胶囊；小群直推 fan-out + sync_events 兜底；按接收用户人数聚合的送达/已读。**分发可靠性**：单条分发受 16384 上限约束，故按长度预算**分片**（同 keyId 独立可分发的多条，接收端无需重组）且**全部分片 ACK 后新密钥才启用**，分发在途/失败退避期内该群不发消息（fail-closed）。**M13 退群语义**：主动退群二选一（仅退出保留历史→会话留列表且只读 / 退出并删除本机记录→清空本地缓存并移除），被移出默认保留历史；会话列表按服务端权威对账（服务端不再返回的群会话标记为“已退出但保留历史”并由本地列表继续展示，缺此对账会表现为“退群后刷新又出现”）；本地清空记水位防重加入复活。**未实现**：大群拉取模式、改群名接口（数据层已就绪）|
| 本地存储 | `LocalStore`（SQLite，按账号+设备隔离）：消息/会话预览/outbox/解密缓存 AES-256-GCM 加密落库，存储密钥 DPAPI 保护；含 `sender_keys` 与 `sender_key_skipped`（跳序消息密钥缓存）；**M13 新增会话表 `left_group` 列（已退出但保留历史）与 `conversation_clear_marks` 表（本地清空水位，不随会话行删除）**；登出清用户可见数据、保留密钥材料 |
| 多端同步 | 账号级 `sync_events`（message/receipt/contact_added/group_changed/conversation_prefs/message_edited/message_deleted/read_cursor/conversation_deleted）+ 设备本地游标；登录后缓存先行 + 增量拉取（hasMore 自动续拉）；已读、会话偏好、编辑/删除、会话删除均多端一致（实时推送 + sync_events 兜底） |
| 传输安全 | TLS 1.2+ fail-closed（服务端无证书拒启、客户端无 CA 拒连，开发明文需显式开关）；重放保护（timestamp ±300s + nonce 全局 TTL 600s 去重）；日志脱敏（LogSanitizer）；结构化日志（StructuredLogger 单行 JSON）；连接级限流（`RateWindow`：`send_message` 30/10s、`search_users` 20/60s、edit/delete 20/60s、prefs 30/60s、`fetch_keys` 60s/20、typing 10/10s）；专用推送类型 88/89 与会话删除/typing 类型 100-105 |
| 客户端 UI | QML/Qt Quick + QWindowKit 无边框双窗口（登录/主窗口独立）；Telegram 风格主题（亮/暗切换持久化）；M10 视觉系统与组件库（Theme token 分层含 `avatarColor(id)` 确定性配色、SVG 图标库 + `Icon.qml`、基础组件库 `AppButton`/`AppTextField`/`AppDialog`/`Avatar`/`Toast`/`EmptyState`/`LoadingIndicator`/`NetworkStatusBar`、全局 Toast + 网络状态条、`MainPage` 拆 9 个 Dialog、消息气泡重构、未读分隔线 + FAB、多行输入区、会话/消息右键菜单）；**M11A 桌面集成**（`AppSettings` 统一设置管理、系统托盘 `TrayManager` 最小化/恢复/退出、桌面通知 `QSystemTrayIcon.showMessage` 带免打扰与预览开关、草稿会话切换保留、本地消息搜索 `searchMessages` 解密后 LIKE 匹配 + 跳转滚动）；**M11B 消息交互**（多选模式 + 批量删除、文本转发（按目标会话重新加密投递、不带引用）、回复/引用渲染与点击跳转 + 高亮、emoji 选择器 + 最近使用持久化）；**M11C C1 群信息抽屉**（群信息由 `GroupInfoDrawer` 自右缘滑出、非模态无遮罩，内容面板 `GroupInfoPanel` 供 C3 常驻右栏复用；抽屉打开时切会话跟随换载或收起，退群/被移出/会话删除/登出四路径收拢）；**M11C C3/C2**（宽屏三栏：群信息以 `GroupInfoPanel` 常驻右栏、窄屏沿用抽屉，跨断点双向转移开合状态；私聊头部"在线 / 最后在线 …"，服务端随 `get_conversations` 回填 `lastActiveAt`/`online`；顺带让抽屉顶边让开自定义标题栏） |
| 文件与媒体传输 | 控制面走 TCP 主通道（类型 90-99），数据面为独立 HTTP(S) 服务（`QHttpServer` + `QSslServer`，同套证书与 fail-closed，基地址由登录响应 `fileTransferBaseUrl` 下发而非客户端猜端口）；文件字节客户端加密（每文件独立 AES-256 密钥 + 分片独立 AEAD，nonce/AAD 绑定分片序号），文件名/MIME/明文大小/文件密钥只在 `FileManifest` 内随正文经既有 E2EE 分发；客户端 `FileTransferManager`（两遍加密上传、失败后先查已收分片、流式下载 + 整体摘要自校验、进度/取消/重试）；本地缓存密文原样落盘（sha256 命名 + 256 桶），明文只在"另存为"时写出；清单经唯一脱敏出口 `sanitizeForUi` 拦截，四条 UI 路径均不进 QML/JS；图片内联缩略图与应用内大图查看（逐片解密在内存，明文不落盘）；音视频元数据（时长/分辨率/封面）+ 应用内播放器（`DecryptingIODevice` 流式解密）；M10 传输体验（hashing/另存为改时间片增量泵送、传输横幅多任务、状态更新改 Map 索引）。**未实现**：视频动态画面渲染、文件消息持久化 outbox |

## 2. 已完成能力摘要

各里程碑的目标、关键交付、验证证据与已知限制。实施细节经 `git log` 与 `docs/ARCHITECTURE.md` 追溯；遗留欠账统一见 §3。

### M0：工程基线（2026-07-01）

- 交付：README、`.gitignore`、`docs/` 四文档、GitHub Actions CI（configure/build/test）、Qt Test 框架与 `TestEncryptionManager`、顶层 CMake 统一（C++20、警告选项、`XYCHAT_BUILD_TESTS`）。
- **更正（2026-09-09 周度审查）**：上述"CI 全流程通过"自 2026-08-03 起已不成立——`.github/workflows/cmake.yml` 随 `31622df` 被删，连续 5 周无任何机器门禁，期间各里程碑的 "`ctest` 6/6" 仅靠本地手工运行得出（并因此遗漏一项约 50% 概率失败的单测，见 M9）。工作流已于 2026-09-09 重建（windows-latest + Qt 6.8.3，三步 + 失败上传 `LastTest.log`）；**重建后的首次运行即暴露配置缺陷**——安装器默认只装 Qt base，而项目依赖的 Multimedia/HttpServer 属独立 add-on 模块，configure 阶段报 `Failed to find required Qt component "Multimedia"`。2026-09-16 连修两处：① 补 `modules` 声明，解决 configure 阶段的 add-on 模块缺失；② 修 build 阶段 `TestGroupRepro` 的 `certs` 目录自我拷贝竞态（详见 §10）。**2026-09-17 已确认绿色通过**，门禁自 2026-09-09 重建以来的悬空状态就此销账；**最近一次已完成的绿灯为 2026-10-12（`e5558ed`）**。

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
- 已知限制：nonce 缓存为单服务器内存态（见 §3）；端到端 TLS 集成测试已于 2026-09-20 补齐（`TestTlsIntegration`，见 §10）。

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
- 已知限制：分发覆盖不到的成员（未注册 E2EE / 预密钥耗尽）收不到 chain key 且无 UI 反馈；分片后一次轮换的分发消息数由 1 变为 N（均见 §3）。原先的"大群单条分发超 16384 上限"与"先落盘后分发窗口"两项已于 2026-09-17 销账（见 §10）。

### M9：多端同步与离线一致性（2026-09-04 / 09-05 / 09-09）

- 交付（核心一致性）：已读状态多端同步（`ack_message(read)` 向已读者自身 `sync_events` 追加 `read_cursor` 并经 `ReadCursorNotification (80)` 推送其全部在线设备；客户端 `markConversationRead` 按剩余未读重算角标、状态只前进）；`sync_events` 保留清理（V8 `sync_meta` 水位线，每小时按 30 天保留期清理；落后设备返回 `needsFullSync` + `fullSyncSeq`，客户端重置游标全量回退，历史经 `sync_messages` 补齐）。
- 交付（特性栈 09-05，提交 `8224464`）：会话置顶/免打扰（81-83，按成员×会话维度、多端共享）；消息编辑/删除（84-87，仅发送者可操作；编辑须保持原 contentType 并经服务端 fail-closed 密文校验拒绝明文注入；删除为软删除留墓碑且幂等）；V9（`conversation_members.pinned/muted`、`messages.edited_at/deleted`）；`conversation_prefs`/`message_edited`/`message_deleted` 三类事件离线补偿；客户端全链路 + QML 右键菜单与"已编辑/已删除"展示。
- 交付（2026-09-09 周度审查修复）：① 编辑/删除事件与推送补 `senderId`（群密文靠它定位 Sender Key；旧实现还硬置 `senderId = 0`，致群消息编辑后在所有接收端解密失败并清空已可读正文）与 `originDeviceId`（发起设备去重），并增按（群, 设备, keyId）反查发送者的兼容路径，使修复前已落库的无 `senderId` 事件仍可解；② 跳序消息密钥缓存（Signal skipped-message-keys 语义，`MaxSkippedMessageKeys=1000`，命中即一次性消费、"**先认证后消费**"以防伪造消息烧毁合法密钥、伪造输入不污染缓存与链状态），解决"编辑重加密使 `iteration` 与 `message_id` 解耦、离线按 id 升序补收时后续消息被回滚检查永久拒绝"；经 `sender_key_skipped` 表加密持久化，退群与 `sender_keys` 同一事务清理；③ 编辑/删除推送覆盖操作者本人其他设备；④ `deleteMessage` 写入失败改 fail-closed；⑤ `latestSenderKeyId` 改按 `rowid DESC` 选取（原按秒级 `updated_at` 排序 + 随机 `key_id` 作并列破口，同秒写入两把密钥时选中哪把完全随机）。
- 验证：`TestGroupE2eeCrypto` +6（乱序解密/缓存一次性消费/伪造不污染/伪造不烧毁/容量淘汰/编辑重加密回归，共 27 passed）；`TestLocalStore` +2（跳序密钥密文落库与维度隔离、按设备+keyId 反查发送者，共 24 passed、连跑 20 次 0 失败，修复前约 50% 概率失败）。
- **（2026-09-10 M8 前置清理）** 编辑/删除/偏好三端点接入连接级限流；编辑/删除响应匹配由单发槽位改为多槽 + 私聊编辑队列串行；新增专用推送类型 88/89；新增 `TestNetworkManager`；项目根新增 `AGENTS.md`；`ctest` 7/7。

## 3. 已知欠账与风险清单

集中管理所有已识别但未实施的修复/功能项；销账或新增时更新本表（优先级 P1 最高）。

> **近期销账（历史，明细见 §10）**：2026-09-10 M8 前置 P2/P3 清理（三端点限流、编辑/删除多槽匹配与队列串行、`TestNetworkManager`、`AGENTS.md`）；2026-09-11 M8.2（数据面未实施、票据校验无生产调用点）；2026-09-14 M10 Phase 4（上传 hashing 与 `saveToFile` 异步化、传输横幅多任务、QML 状态更新 O(n)→Map、文件消息重发引导、Theme 装饰性注释）；**2026-09-17 第一批（下载票据生命周期修复：销 P2「TTL 与大文件下载不匹配且中途不续期」，两条相关 P3 口径修正；在途请求超时清扫：销 P2「编辑/删除在途请求无超时清扫」与 P3「`sync_messages` 在途请求与页队列无超时/上限」；数据面绑定面修复：P2「请求体已缓冲」的**错误前提被更正**并修掉默认监听全部网卡，剩余部分拆为一条 P3，见下）**。异步化采**单线程时间片增量泵送**（非工作线程）——无数据竞争、密文逐字节不变，规避了规划中标注的并发损坏风险。**2026-09-17 第二批（群 Sender-Key 分发可靠性：销 P2「大群单条 `sender_key_distribution` 超 16384 上限」与 P2「先落盘后分发」，并把 P3「`TestNetworkManager::largePageEmittedOnceInOrderAcrossSlices` 的 5 秒预算不足」一并销掉——三项改动同轮，故 `ctest` 首次恢复 12/12 全绿）**。同轮 CodeReview 的已修项（含 M8.2 的 3 项 P0：清单密钥经非推送路径泄入 QML、任务容器悬垂引用、泵送中 erase 致迭代器失效）不在下表。
> **2026-09-20 销账（明细见 §10）**：P2「端到端 TLS 集成测试缺失」与 P2「文件控制面五处理器无自动化测试」两项同轮补齐——新增 `TestTlsIntegration`（真实证书 + 真实 `Server` + 真实 `QSslSocket`，覆盖 fail-closed 与认证全链路）与 `TestFileControlPlane`（进程内真实回环 + 真实 `RequestHandler`，覆盖五个文件处理器的入参校验顺序、限流窗口、幂等、finalize 分类与枚举预言机合并），`ctest` 12/12 → **14/14**。下表含 2026-09-11 M8.2 与 2026-09-14 M10 审查提出但**未修**的项。
>
> **2026-10-15 销账**：P3「桌面通知；简化图片消息」——条目所述两项均已落地：桌面通知由 M11A 交付（`TrayManager` 持有 `QSystemTrayIcon`，`showMessage` + 免打扰/预览开关 + 点击跳转），图片消息由 M8.3a/b 交付（内联缩略图与应用内大图查看）；原表述“桌面通知仍待立项”已失实，故整条销账。
>
> **新增（2026-09-17 第二批排查发现，非本轮功能引入）**：P3「分发覆盖不到的成员无用户可见反馈」——排查分片改造时确认：对“无密钥包”的成员（未注册 E2EE / 预密钥耗尽）原实现即只记 `skippedUsers` 日志后照常分发，这些成员收不到 chain key 却无任何 UI 提示。已登记见下表。

| 优先级 | 类别 | 条目 | 来源 | 影响/说明 |
| --- | --- | --- | --- | --- |
| P3 | 工程 | 群成员变更 healing 的 O(N²) 重分发与预密钥消耗 | 2026-09-02 P1 修复审查 | 一次成员变更触发全员各自轮换+重分发；已加同群去重与单发槽位节流，但大群跨成员风暴仍需聚合策略（如群主统一分发或延迟合并）。**2026-09-17 补充**：分发改分片后，一次轮换的分发消息数由 1 变为 N（设备条目数 ÷ 每片容量），fan-out 与 `fetch_group_keys` 消耗相应放大 |
| P2 | 安全 | nonce 去重为单服务器内存态 | M5.5 | 服务端重启清空；多服务器部署需持久化/共享存储 |
| P2 | 安全 | TOFU 无带外验证；无密钥备份/设备间迁移 | M6 | 首次通信无法抵抗服务端中间人；更换设备/清数据后历史消息不可恢复（产品已决策接受） |
| P2 | 安全 | 非 Windows 平台私钥/存储密钥明文回退 | M6/M6.5 | DPAPI 仅 Windows；Linux/macOS 部署需接平台密钥环（libsecret/Keychain） |
| P3 | 功能 | 大群拉取/游标模式；改群名接口 | M7a 遗留 | 当前仅小群直推；`setGroupName` 数据层就绪、接口层未开放 |
| P3 | 工程 | 跳序消息密钥缓存按整块 blob 落库，大跳跃场景有写放大 | 2026-09-09 CodeReview | `decryptGroupMessageObject` 每次解密都会 `loadSkippedMessageKeys`（整块解密），且缓存非空时每条消息重写整块（重新序列化+加密+写库），近似 O(N × cacheSize)；上限 1000 条时单块可达数十 KB。典型编辑场景（小跳跃）影响微小，新设备/长期离线的大跳跃补收才明显。建议改为每跳序密钥一行（PK 含 iteration）+ 增量写入，或提升为批次内存态缓存 |
| P3 | 安全 | 跳序密钥序列化时以 base64 `QString` 形态短暂驻堆，无法可靠清零 | 2026-09-09 CodeReview | `SecureMemory::wipe` 对 COW/只读的 `QString` 缓冲无效，与现有 chain key/正文落库路径（`encryptText`/`decryptText` 均返回 `QString`）为同一固有限制；如需更严格的密钥卫生，序列化应走 `QByteArray` 并用后 wipe |
| P3 | 工程 | 私聊编辑额外占用一次共享 `fetch_keys` 预算 | 2026-09-10 CodeReview | 每条私聊编辑 = 1 次 `fetch_keys`（与发送/fetch_group_keys 共设 20/60s 窗口），编辑自身窗口亦 20/60s；混合场景可能先撞 `fetch_keys` 上限使编辑以 `RateLimited` 失败，实际有效编辑率低于标称。与 P2 三端点限流相关，待真实用量评估后调参 |
| P3 | 工程 | `TestNetworkManager` 未覆盖 pump 与编辑解密回退路径 | 2026-09-10 CodeReview | 现有用例锁定 requestId 多槽匹配/消费、去重、断线清理；但 `pumpPrivateEditFetch`+`handleFetchKeysResponse` 编辑分支（需模拟 fetch 响应、会写 socket）与 88 推送“解密失败不写空”幂等回退不变量因难构造无网络环境而未断言；属测设完善，不阻塞 |
| P2 | 工程 | 对象存储为单机本地文件系统，无副本/无冗余 | 2026-09-10 M8.1 | `LocalFileStorage` 磁盘损坏即文件丢失；限流与并发配额为单实例/单库口径，多实例部署需换共享对象存储（`IObjectStorage` 已抽象，可接 S3/MinIO）并把配额改为全局口径 |
| P2 | 安全 | `QHttpServer` 在进入 handler 前已缓冲整个请求体；数据面此前默认监听全部网卡 | 2026-09-11 CodeReview / **2026-09-17 实测更正并部分修复** | **原判断有误**：“`--http-host` 默认仅回环是正确的”不成立——`--http-host` 只设置下发给客户端的 `fileTransferBaseUrl`（`setAdvertisedHost`），而 `start()` 无条件 `listen(QHostAddress::Any, port)`，数据面**实际默认监听全部网卡**。于是“未认证请求体在票据校验前被读进内存”（`QHttpServerRequest::body()` 返回已缓冲的 `QByteArray`；`QAbstractHttpServer` 无内建请求体上限；per-IP 限流只计失败，对首次请求无效）这一放大面是**默认对局域网敞开**的。**已修**：绑定地址与通告地址分离，默认仅回环；绑非回环须显式 `--http-allow-public` 并打警告；非法地址 fail-closed 拒绝启动（绝不静默退回 `Any`）。**仍成立**：进程内无法给请求体设上限——Qt 6.8.3 的 `QAbstractHttpServer`/`QHttpServer` **没有任何限流或 body 上限 API**（核验过公开头与私有头），而自建请求头预检等于手写 HTTP 解析器（与真实解析器不一致即引入请求走私），`setReadBufferSize` 也无效（解析器会持续抽干 socket 缓冲、把数据累积进自己的 body 缓冲）。**因此对外部署的前置条件仍是带请求体上限的反向代理**，现已由 CLI 显式确认 + 启动警告强制可见 |
| P3 | 工程 | 数据面未补“已授权请求”的并发数与字节速率上限 | 2026-09-17 本轮拆分 | 原 P2 建议中“并补已授权请求的并发数与字节速率上限”这一半**未实施**：数据面的资源消耗已由控制面配额封顶（每用户并发上传 8、单文件 2 GiB、单次 GET 4 MiB、单分片 ≤4 MiB），而按字节速率限流会在没有真实用量数据的情况下误伤合法的大文件传输（合法上传一个大文件需多达 4096 次 PUT）。待有真实用量或压测数据后再定阈值，属**有意留白**而非遗漏 |
| P3 | 工程 | `LocalStore.messages` 表无 `file_id` 列 | 2026-09-11 CodeReview | 从本地缓存回填的消息不带 `fileId`，现由 `attachFileInfo` 从清单内取回（清单经 E2EE 保护且自带 `fileId`，功能等价），但少了服务端权威字段的交叉校验，且每次回填都要解一次清单 JSON。建议下一轮客户端迁移（V11）补列 |
| P3 | 工程 | `clearCache` 会删掉在途下载的临时文件 | 2026-09-11 CodeReview | `entryList(QDir::Files)` 会匹配 `<sha>.<uuid>.tmp` 并删除，随后还可能 rmdir 桶目录，使在途任务下次写入失败；返回的删除条数也把 `.tmp` 计入。影响有限（任务会明确失败而不是静默写坏缓存），建议跳过 `*.tmp` 或先取消在途下载 |
| P3 | 工程 | `finalizeDownload` 的 exists + rename 存在 TOCTOU | 2026-09-11 CodeReview | 两个任务并发下载同一文件时，Windows 的 `rename` 不覆盖已存在文件 → 报“Cannot move ... into the cache”，而缓存其实已完好。建议 rename 失败后重新 `isCached` 判定，命中即视为成功 |
| P3 | 安全 | `markFileTicketUsed` 仍无生产调用点；过期票据行以 `used=0` 残留 | 2026-09-11 CodeReview | **注释失实部分已于 2026-09-17 修正**：`FileProtocol.h` 原文写“一次性、短时效”，与“刻意允许窗口内重复使用”的实际语义相反，现已改为准确表述（空闲容忍窗口 + 绝对寿命上限），并同步 `SECURITY.md`/`PROTOCOL.md`。**仍然成立的是设计取舍本身**：下载票据不消费（一次性消费与 `Range` 分段互斥），过期行由 `pruneExpiredFileTickets` 每小时清理，若需更严可改为每段单独签发或绑定数据面会话 |
| P3 | 功能 | 视频播放无动态画面（只输出音频轨 + 静态封面） | 2026-09-11 M8.3b | `MediaPlaybackManager` 用 C++ QMediaPlayer + `DecryptingIODevice` 流式解密播放（明文不落盘），但 QML VideoOutput 无法绑定 C++ QVideoSink（无公开 videoSink 属性），且 QML Video 元素的 source 只接受 URL 不支持自定义 QIODevice。当前视频播放只闻其声不见其画（静态封面取自清单 thumb）；补齐需自定义 QSGNode/QQuickPaintedItem 渲染 QVideoFrame，或在 C++ 侧 qobject_cast QML VideoOutput 调 setVideoSink（脆弱） |
| P3 | 功能 | 无按用户的存储用量配额 | 2026-09-10 M8.1 | 现有约束为单文件 ≤2 GiB + 并发上传 ≤8 + 48 小时超期回收，但已就绪文件可无限累积（仅受消息删除联动回收影响）；需按用户/按会话的字节配额与用量统计接口 |
| P3 | 安全 | 下载票据在窗口内可重复使用 | 2026-09-10 M8.1 | 为支持 `Range` 分段与断点续下而刻意允许（一次性消费与分段下载互斥），泄露后可在窗口内重放下载该文件。**2026-09-17 修复后的口径**：窗口由“固定 300 秒”改为“最后一次使用起算的空闲容忍窗口”，但以 `DownloadTicketMaxLifetimeSeconds`（自签发起算 24 小时）为绝对上限封顶——上限不可省，否则持续使用的泄露票据可无限续命。真实暴露面由“300 秒”变为“最多 24 小时”，属**风险后移而非消除**，故本条保留。原先“大文件下 TTL 不足”的风险已随同轮修复销账 |
| P3 | 工程 | `putChunk` 不入条带锁 | 2026-09-10 CodeReview | 依赖 `finalize` 的逐片长度 + 整体 SHA-256 关卡兜底：并发写同一片只会导致组装判失败（要求重传），不会把损坏对象推上下载路径；代价是极端并发下多一次重传 |
| P3 | 工程 | 回收查询每轮 `limit=100`，积压大时需多轮收敛 | 2026-09-10 M8.1 | `getStaleUploads`/`getTerminalFiles`/`getUnreferencedReadyFiles` 均为每轮上限 100 行、每小时一轮；大量遗留时收敛慢且无积压告警指标 |
| P3 | 工程 | 文件回收任务在主线程做同步磁盘 I/O | 2026-09-10 CodeReview | `pruneFileUploads` 由 Server（主）线程的定时器驱动，该线程同时承担 `incomingConnection` 与 `onMessageForUser` 路由；`remove()`（内部 `removeRecursively`）为阻塞调用，三轮合计每轮最多约 300 次删除，大文件/多分片目录时可能短时阻塞连接接受与消息转发。量级有界（每小时、limit=100）且定时器不重入，属响应性隐患而非正确性缺陷；积压增大后可移至独立维护线程或工作池 |
| P3 | 工程 | `runHashSlice` 从 hashing 转 creating 时 emit 后仍持有 `Task&` 引用（理论悬空） | 2026-09-14 M10 CodeReview | 完成分支先 `clearLocalStateForToken(token)` 再 `emit taskProgress`/`uploadCreateRequested`，其间持续经 `Task &t = tit.value()` 访问 task；若某 `taskProgress` 的 QML 槽同步触发 `cancelTask → finishTask → m_tasks.erase`，`t`/`tit` 将悬空。当前 QML 未在 `taskProgress` 回调中调 `cancelTask`，且此为原同步版 `beginHashing` 既有模式（非 M10 新引入），故实际不触发。建议 emit 前把 phase/cipherSize/chunkSize/chunkCount/sha256Hex 全拷入局部、emit 序列不再解引用 `t` |
| P3 | 安全 | `processDeleteConversationRequest` 对非成员暴露“会话是否存在”的错误码差异（存在性 oracle） | 2026-09-14 M10 CodeReview | 权限校验顺序为 形态→限流→`getConversation`(存在性)→`isConversationMember`→群主校验；非成员探测时“存在但非成员”回 `PermissionDenied`、“不存在”回 `ConversationNotFound`，二者可区分。会话 ID 自增需先验知识、成员间本已互知，信息增益极低。建议（可选）对“非成员”与“不存在”统一错误码消除区分度 |
| P3 | 工程 | M12 消息页泵的"真正跨时间片"续做无确定性用例 | 2026-09-16 CodeReview | 分片预算 `kMessagePageSliceNs` 是编译期常量、无注入点，测试只能构造"大页"提高跨片概率（`largePageEmittedOnceInOrderAcrossSlices` 断言的是"整页单次 emit 且保序"，不依赖实际片数）。续做逻辑本身由泵的"只从队列头取、片内续用成员"结构保证，真机大页已验；如需锁死多片路径需为预算加测试接缝 |
| P3 | 工程 | 分发覆盖不到的成员无用户可见反馈 | 2026-09-17 本轮排查发现 | `buildGroupSenderKeyDistribution` 对“无密钥包”的成员（未注册 E2EE / 预密钥耗尽，`fetch_group_keys` 已跳过）只是记进 `skippedUsers` 日志后照常分发，而这些成员收不到 chain key → 解不开本端后续群消息，发送方与接收方都无 UI 提示。服务端跳过是为避免对预密钥耗尽者的连续失败，属既有缺口而非本轮引入；修复方向：把跳过名单回传 UI 或在下次发送前重试该成员，待定 |
| P3 | 功能 | 文件消息不支持转发（M11B B3 的可转发子集排除文件） | 2026-09-20 M11B | 转发文件消息需要把 `FileManifest`（含 32 字节文件密钥与 nonce）按**目标会话**重新加密后投递，并复用同一个 `fileId`（服务端按消息引用授权下载，新会话的消息引用即可让目标成员获得下载权）。当前实现把文件消息排除在可转发子集外并在结果提示里说明跳过条数——**刻意不做的原因是安全**：原样转发本会话清单等于把密钥材料投给另一个会话。补齐需 C++ 侧按消息取回清单（`manifestFor`）并在目标会话的 E2EE 上下文里重新加密，属"协议无改动、客户端工作量中等"的一项 |
| P3 | 工程 | 一次踢人最多触发 3 次 `get_group_info` | 2026-10-14 周度审查 | `GroupInfoPanel` 的“移除”按钮在发 `kickRequested` 的同时发 `refreshRequested`（①），叠加服务端响应驱动的 `groupMemberKicked`（②）与广播驱动的 `groupChanged`（③，服务端 `notifyGroupChanged` 不排除操作者），单次踢人走三次 `refreshGroupInfoIfOpen`。`get_group_info` 无服务端限流，故仅冗余不阻塞正确性；建议去重（如去掉面板内的乐观 `refreshRequested`，交由响应/广播驱动） |
| P3 | 工程 | 批量删除一次超过服务端 edit/delete 限流窗口（20/60s）时静默部分失败 | 2026-09-20 M11B 审查 | 多选批量删除按选中条数一次性并发下发，超限部分只回 `RateLimited`，用户看到若干条失败提示（后到的覆盖先到的）且没有自动续批。建议按窗口上限分批串行下发（或选中数超阈值时先行提示）；当前已在 §4.4 列为已知限制 |
| P3 | 工程 | 历史重放的老群分发每次登录都打 `unusable` 警告，与现网故障同形难分辨 | 2026-09-20 M12 验收排查 | 客户端每次登录会重放历史，重新处理老 `sender_key_distribution`；其引用的预密钥早已消费并被本地删除，故必然解密失败并打印 `[NetMgr] Group sender key distribution unusable … selfEntryPresent true`。该形态与**真实缺陷**（分发未覆盖本机、预密钥不匹配）产生的日志完全一致，排查时无法一眼区分——本次验收一度据此误判为现网缺陷。建议对"已消费预密钥/已使用 keyId"的情况降级为 debug 级，或在日志中带上本地迭代数与判定依据 |
| P3 | 功能 | "在线"为活跃时间启发式，空闲但已连接的客户端显示为离线 | 2026-10-15 M11C C2 | `online` 由 `sessions.last_active_at` 是否落在 120s 阈值内判定（`PROTOCOL.md` 已注明）。心跳（ping）按设计**无需认证、不刷新活跃时间**，故用户只要不在发消息/读消息（读也走 ack 刷新活跃）就会被判为离线——表现为"两个客户端都开着却互相显示离线"。只会**少报**在线、不会多报，属精度问题而非安全问题，但会影响验收观感。真在线需要服务端复用 `m_onlineSessions`（handler 各在独立线程，跨线程读需加锁）或新增 presence 推送 / 鉴权心跳，属协议扩展，故本次未做 |
| P3 | 功能 | “退出并删除本机记录”不跨设备；重加入后退出期间的消息会被取回且可能不可解密 | 2026-09-23 M13 | ① 清空是本机行为：服务端已在退群时移除本端成员资格，推送无从投递，同账号其他设备只能在下一次会话列表对账时把该群标记为"已退出但保留历史"（历史仍在），无法同步执行清空。跨设备一致需服务端支持"按用户隐藏历史"的每用户视图（当前 `messages` 为会话共享表，删除即对全员生效，与本操作语义相反）。② 清空水位只封住"早于水位"的本地历史，重加入后**退出期间**他人所发消息（ID 晚于水位）仍会被取回，其中群 E2EE 消息可能因 ratchet 已推进而显示为不可解密（有噪声但不算数据错误）。改进方向：重加入时以服务端会话列表的 `lastMessageId` 作为新水位（"只收重新加入之后的消息"，对齐 WhatsApp 语义） |

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
- 已知限制：hashing 与 `saveToFile` 解密曾在 GUI 线程同步（已由 M10 P4.4/P4.3 销账）；下载票据寿命与大文件下载不匹配、且客户端无断点续传（已由 2026-09-17 修复，见 §10）；数据面无请求体上限预检（§3 P2）；**双客户端实机联调尚未做**（属人工验证）。

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

### 4.4 M11B：消息交互增强（已完成 2026-09-21，含双客户端实机验收）

- **目标**：补齐消息维度的高级交互（多选/回复引用/转发/emoji）。
- **Phase 与执行顺序**：B1 多选模式 → B3 消息转发（转发复用多选的选择集，**依赖 B1**）；B2 消息回复/引用、B4 emoji 选择器各自独立可并行。
- **协议影响**：`send_message` 新增可选 `replyToMessageId`，响应/推送/历史/事件回传引用字段——**这是 M11 唯一的协议扩展与数据库迁移**（服务端 V11：`messages.reply_to_message_id`；客户端 `LocalStore` 同步加列）。除 B2 外其余 Phase 无协议变更。
- **关键约束**：群聊被引用消息的正文对服务端是密文，引用摘要**不得**由服务端填充——只回传 `replyToMessageId`/`replyToSender`，摘要由客户端本地解密被引用消息后渲染（服务端不接触明文）；引用字段与既有 E2EE、跳序密钥缓存的交互需回归（编辑会改正文，引用仍指向原消息 ID）。
- **交付**：
  - **B1 多选模式（纯 QML）**：消息右键"多选"进入（右键菜单同期向对方消息开放：复制/多选；编辑/删除仍仅自己的消息可见）；选择集为**视图级**状态（`ChatView.selectedIds`，`messageId → true` 整表重赋值驱动绑定，避免随 delegate 回收丢失）；选中期间由 `MessageBubble` 顶层的行级 `MouseArea` 接管整行左键；标题栏换成选择工具栏（已选 N 条 / 转发 / 删除 / Esc 退出）。工具栏可用性经属性绑定 `selectedRows → canDeleteSelected / hasForwardableSelected` 计算（**订正 2026-10-15**：原“不写成函数——QML 依赖跟踪不穿透函数体”的归因有误，实测会被穿透；真正要求整表重赋值的是 `selectedIds` 这类 `var` 对象的原地改动不触发 NOTIFY）：删除要求选中项**全部**是自己的未删除消息，转发只要有一条可转发即可用。
  - **B3 消息转发（纯 QML）**：`ForwardDialog` 从 `convList.conversationSummaries()` 的会话快照选择目标；转发即**以本端已解密正文向目标会话重新发送**（目标会话得到自己的 E2EE 密文，服务端无转发元数据），且**不带引用**（目标会话没有原消息）。`MainPage` 在 emit 发送信号前注入 `pendingTrackKey`/`pendingTrackConversationId`，由 `trackOutgoingMessage` 消费，把乐观气泡与在途登记落到**目标会话**（目标会话未打开时不建视图，避免挤掉用户视图）；草稿清理仅限当前会话发送。可转发子集排除系统消息、已删除、不可解密与文件消息。
  - **B2 回复/引用（协议 + V11 迁移）**：`send_message` 可选 `replyToMessageId`（形态校验先于限流消费；目标必须存在且属**同一会话**，"不存在"与"不属于本会话"回同一 `InvalidRequest` 以免成为跨会话存在性探测）；服务端只存/只回 `replyToMessageId` + 被引用者名，**不产出摘要**（群聊正文对服务端是密文）；响应/推送/`sync_messages`/`sync_events` 四条路径同口径。客户端以 `ChatView.replyPreviewMap` 渲染引用块（随编辑刷新、随删除转"已删除的消息"占位），未命中退回"原消息不可见"占位；点击引用块跳转 + 1.5s 高亮。`LocalStore` 消息表新增 `reply_to_message_id`/`reply_to_sender`、outbox 新增 `reply_to_message_id`（重启重发不丢引用）。
  - **B4 emoji 选择器（纯 QML）**：输入区 emoji `Popup`（8 列常用网格 + 最近使用两行），插入到光标处；最近使用去重前置、上限 16，经 `AppSettings.recentEmojis` 持久化。
- **验证**：`ctest` **14/14** 全绿（`TestDatabaseManager` +3：V11 列/版本、引用字段三条读路径往返、引用目标必须同会话；`TestTlsIntegration` +2：真实连接收发引用消息并跨账号回读、非法引用一律拒绝且不落库；`TestLocalStore` +3：引用字段回读与部分 upsert 不清空、outbox 保留引用目标、`upsertMessageRow` 逐列写入顺序与状态不回退；`TestNetworkManager` +1：重启恢复持久化 outbox 时引用目标被完整搬运，负向对照把 `item.replyToMessageId` 置 0 后失败精确落在 `replyTarget` 断言上）；`qmllint` 零错误；QML 行为用 Qt Quick Test 覆盖（`out/tst_m11b.qml` 19 项：选择集增删与可用性判定、服务端删除后选择集剔除、引用预览表随编辑/删除刷新、回复目标与发送回传引用、失败气泡重发带回引用、引用跳转定位到被引用消息且完整入视口、跳转命中/未命中、emoji 插入与上限、emoji 面板几何），**三处破坏的负向对照分别精确命中各自用例**；无头冒烟无 QML 错误。**2026-09-23 入库前复查**：8 个变更 QML 文件全量 `qmllint` 零错误，并修掉 `MessageInput.positionEmojiPanel` 里 `Item` 不存在的 `window` 属性（应为附加属性 `Window.window`，原写法使窗口兜底整块成为永不生效的死分支——`qmllint` 的 `[missing-property]` 属真缺陷，已写入 `AGENTS.md` 约定）；16 个新增/扩展信号的声明、发射点与处理器逐项比对，参数数量全部一致。
- **实机验收（2026-09-21，双客户端）**：发现并修复两处（详见 §10 变更记录）——emoji 面板位置/裁切/不可滚动、引用跳转目标取错（跳到了承载引用的消息而非被引用消息）。其余四项（多选→转发/删除、引用显示与摘要随编辑/删除刷新、emoji 插入与最近使用、失败重发保留引用）实机通过。
- **已知限制**：① 文件消息不支持转发（需按目标会话重加密 `FileManifest`，并避免把原会话密钥材料投给另一会话；已登记 §3）；② 引用摘要与引用跳转仅覆盖本地已缓存/已加载的消息（无跨页定位接口），被引用消息不在本地时显示发送者名 + "原消息不可见"；③ 批量删除受服务端 edit/delete 限流（20/60s）约束，一次选中过多时超出部分失败并提示。

### 4.5 M11C：群聊与布局完善（已完成 2026-10-15）

- **目标**：群聊体验优化（信息抽屉、最后在线时间）与宽屏布局适配。
- **Phase 与执行顺序**：C1 群信息抽屉（`Drawer` 替代弹出对话框，**已完成 2026-09-30**）→ C3 响应式布局（宽屏三栏由 C1 的抽屉组件常驻右侧实现，**依赖 C1**，**已完成 2026-10-15**）；C2 最后在线时间独立，需服务端回填，**已完成 2026-10-15**。
- **C1 交付**：`GroupInfoPanel`（纯内容面板，C3 常驻右栏直接复用，`showCloseButton` 可关）+ `GroupInfoDrawer`（右缘 `Drawer`，非模态无遮罩，`closePolicy` 收窄为仅 Escape——默认 `CloseOnPressOutside` 会让"边聊边看/切换跟随"失效），替代并删除 `GroupInfoDialog`；`MainPage` 生命周期守卫——`showGroupInfo` 只展示当前会话且未退出的结果、抽屉打开时切换会话跟随换载或收起、退群/被移出/会话删除/登出四路径收抽屉；`MainWindow` 接上 `groupChanged`/`groupMembersInvited`/`groupMemberKicked` 三信号实现抽屉原地刷新；邀请目标改取抽屉自身 `convId`。QML 行为用例 10 项（`out/tst_m11c.qml`）+ 负向对照 5 项。
- **C2/C3 交付**：**C3**——`Theme.wideLayoutBreakpoint`(1080) 与 `rightPanelWidth`(340) 两个 token；`MainPage` 第三栏由 `GroupInfoPanel` 常驻（`visible: wideLayout && groupInfoOpen`），抽屉与右栏**各持一份面板、只填当前生效的那份**（避免 200 人群每次刷新重建两份模型）；开合状态 `groupInfoOpen` 是**意图**状态，只由 `showGroupInfo`/`closeGroupInfoPanel`/抽屉 `aboutToHide` 写入，**不从抽屉的 `opened` 反推**（`opened` 要等动画收尾，会导致"关闭动画未收尾时再点开被迟到的 closed 覆盖"与"跨断点判断拿到动画中途假值"）；跨断点用 `lastGroupInfo` 缓存重填新容器，不为换容器再向服务端要一次。**C2**——服务端在 **SQL 内**判定在线（避免 `last_active_at` 的 SQLite 格式与 C++ 侧 ISODate 漂移）；客户端**不把这两个字段写入本地缓存**（瞬时状态，否则下次启动会显示上一次会话的陈旧"在线"），改在 `handleGetConversationsResponse` 按 `conversationId` 盖到缓存优先的合并列表上；展示在私聊头部第二行，**只用绝对时间**（无走时定时器，相对措辞会定住失真），服务端时间戳须补 `T`/`Z` 按 UTC 解析。
- **协议影响**：C1 无；C2 为 `get_conversations` 回填 `lastActiveAt`/`online`（复用既有 `sessions.last_active_at`，**无迁移**；`online` 为 120s 活跃阈值启发式，空闲但已连接的客户端会显示为离线——见 §3 与 `SECURITY.md`）；C3 无协议变更。
- **风险**：无边框窗口（QWindowKit）的 Snap Layout 与三栏布局、会话列表滑出的交互需实机验证不冲突。**已处理**：抽屉从 `y=0` 铺满整窗会盖住标题栏的拖拽区与窗口控制按钮，故显式让开 `Theme.titleBarHeight`；窗口默认可视宽度由 1000 提到 1180（三栏至少需 左 320 + 群信息 340 + 聊天 420）。
- **交付与验收**：`ctest` 14/14 全绿；`qmllint` 零错误。C1 QML 行为用例 10 项（`out/tst_m11c.qml`）+ 负向对照 5 项覆盖抽屉配置、切换跟随与信号路由；**C2/C3 行为用例 12 项**（C3 六项：断点判定、窄屏走抽屉且右栏不占位、宽屏走右栏且抽屉收起、跨断点双向转移不丢开合状态、关闭同时收起两种形态、抽屉顶边让开标题栏；C2 六项：在线优先、同一瞬间按本地时区渲染成当天时钟、跨年走完整日期、群聊/无会话/非法时间戳一律为空），**负向对照 4 项**分别精确命中各自用例；服务端 `TestDatabaseManager` 新增 C2 回填用例（无 session / 在线 / 陈旧 / 阈值边界 / 群会话不适用）。**实机验证（抽屉滑出收起、切换跟随、宽窄布局切换、暗色主题）仍待进行**。

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

### 4.7 M12：聊天流畅性与多会话体验（已完成）

- **目标**：修复两项用户报告的运行时问题——① 发送消息后消息列表滚出边界，需手动滚动才回到正确位置；② 点击/切换会话时整页加载阻塞 GUI 线程。要求任何加载与界面操作异步、不阻塞主线程；会话切换不得影响其它会话视图的状态。
- **Phase 与执行顺序**：M12.1 滚动越界修复（**已完成**，纯 QML，§10 2026-09-16 条目）→ M12.2 消息加载异步化（**已完成**：C++ 单线程时间片泵 + 批量落库 + `sync_messages` 多槽路由，含本地搜索脱敏补漏）→ M12.3 QML 增量应用消息（**已完成**：`ChatView.setMessages` 按 messageId 合并替换 clear+全量重建，未读分隔线/滚动位置/附件下载进度不再被刷新页重置，10 项 Qt Quick Test + 负向对照）→ M12.4 按会话堆叠 ChatView（**已完成**：`conversationId → ChatView` 实例映射 + `activeViewKey` 活跃指针，切换只改可见性不重建；新会话首条消息前以 `p<peerUserId>` 建"待绑定"视图、确认后迁移到 `c<conversationId>`；LRU 上限 8 个实例，驱逐转存输入框草稿并在重建时恢复；MainWindow 消息页/推送按 conversationId 路由、状态与传输进度按 messageId 落到所属视图；切回已打开会话不重载缓存页、只补收 `lastMessageId` 之后的增量；已读回执从"消息页到达"改挂 `conversationActivated` 激活点；8 项 Qt Quick Test + 负向对照 4 项失败；**审查修复**：页未取完（`hasMore`）时按本页末条 messageId 续拉（视图 `lastMessageId` 被实时消息推高后，不续拉会在中间留下永久空洞）、待绑定迁移遇目标键已占用时保留原视图并销毁待绑定实例（杜绝脱离`chatViews` 的孤儿视图）、`messageSendFailed` 带幂等键精确落到发起视图的气泡（加好友失败改走独立 `addContactFailed`）、`destroyView` 清理在途发送记录（新增 3 项用例，负向对照 3 项失败））。
- **协议影响**：无（纯客户端改动，无服务端改动与数据库迁移）。
- **关键约束**：M12.3/M12.4 必须尊重 M12.2 建立的 emit 契约（每页一次 `messagesSynced`、非空才 emit、页内全不可见消息时跳过）；堆叠视图的滚动位置、未读分隔线、文件进度绑定等状态由各 ChatView 自身持有，切换不得重置；LRU 驱逐复用 M11A 草稿机制保存输入。
- **交付与验收**：`ctest` 12/12 全绿、`qmllint` 零错误、无头冒烟启动无 QML 错误；实机验证发送后稳定贴底不越界、快速连点多个会话无卡顿且各会话消息与滚动位置互不污染；文档同步 ARCHITECTURE/SECURITY/ROADMAP。**验收结论（2026-09-20）**：上述四项结论在双客户端人工验收中全部成立（详见 §10 同日条目）；期间出现的两处客户端崩溃经定位为构建树混编（40/87 陈旧对象）所致的环境问题，与 M12 代码无关，重建后复验通过。

### 4.8 M13：退群语义与本机历史治理（已完成）

- **目标**：修复用户报告的缺陷——从群组退出后刷新会话列表，已退出的群会重新出现，历史仍可访问，新消息收不到也发不出去（属正常逻辑），但**无法从会话列表中删除**；并把退群时"本机聊天记录怎么办"的决定权交给用户（对齐主流 IM 的"退出 / 退出并删除聊天记录"）。
- **根因**：客户端把 `LocalStore` 会话缓存当作只增不减的展示源——退群后服务端不再返回该会话（成员行已删），但对账只做 upsert 不做标记，于是任何一次"缓存先行"（下次登录的 `emitCachedConversations`、已读游标推送、会话偏好推送）都会把这条失效会话重新显示出来；而它又不是"会话"——M10 的整表删除仅成员可发起、群聊仅群主可删，右键"删除会话"必被服务端拒绝，形成死结。
- **交付**：
  - **数据层**（客户端 `LocalStore`，无服务端迁移）：`conversations.left_group` 列（已退出但保留历史，存量库幂等补列）+ `conversation_clear_marks` 表（本地清空水位，**不随会话行删除**）+ `setConversationLeft`/`reconcileConversations`（服务端权威对账：落库返回的会话并清除其已退出标记、把缺席的群标记为已退出、私聊不参与）+ `purgeConversation`（清空本地行并写入水位）。
  - **链路层**（`NetworkManager`）：`leaveGroup(conversationId, deleteLocalHistory)` 在途请求改**多槽**（对齐 M9/M10 教训，并发退群不丢选择）；响应按选择分流（保留→打标记并立即 emit 本地列表；删除→清空并 emit `conversationDeleted`）；`handleGetConversationsResponse` 改为 emit **对账后的本地列表**（本缺陷根因所在）；群变更推送与 `sync_events` 的失权分支默认保留历史并发 `conversationLeft`；`syncMessages` 经 `applyClearMarkFloor` 把服务端起拉点抬到清空水位；新增仅删本机的 `deleteLocalConversation`；登出/断线两路径清理新增状态并对在途退群上报失败。
  - **UI**（QML）：新增 `LeaveGroupDialog`（仅退出 / 退出并删除记录，群信息对话框的"退出群聊"改为打开它）；`ChatView.readOnly` 只读态（输入区换提示条 + "删除聊天记录"出口、群信息入口与消息级操作收起、只读态一路传到消息气泡）；`ConversationList` 已退出条目预览位显示"你已退出该群聊"、右键菜单只留"删除聊天记录"并路由到本机删除、偏好项收起；已退出的群不作为转发目标。
- **协议影响**：**无**（退群请求与踢人推送形态不变；服务端本就独立拒绝非成员的一切成员态操作，客户端只读态是体验层收敛）。
- **验证**：`ctest` 14/14（`TestLocalStore` +3、`TestNetworkManager` +2，含"服务端列表不含已退出群时 `conversationsResult` 仍带 `leftGroup=true`"与"并发退群两个在途请求各自的选择不串台"两条回归锁定）；QML 行为用例 5 项（`out/tst_m13.qml`）；负向对照 4 项（回退只读绑定、回退删除路由、回退菜单收起、破坏对账标记）全部如实失败（另 2 项源码注入式负向对照被工具沙箱策略拦截，未执行）；`qmllint` 0 错误；clean 全量重建后 `ctest` 复跑全绿；无头启动无 QML 报错。
- **审查后加固（同轮）**：① 本地清空失败（库未开/SQLite 出错）不再谎报删除——退化为"保留历史"只读态并回 `conversationDeleteFailed`，避免"UI 已删而数据仍在"的再生路径；② 对账遇**空列表**不标记（服务端列表查询出错会返回空列表却回 `Ok`，否则会造成全体群聊批量误置只读）；③ 只读态补齐到悬停"回复"、失败气泡点击重发与多选工具栏（只读翻转时强制退出多选并清引用，避免留下必失败入口）；④ 标记已退出时一并清零免打扰（与置顶口径一致）；⑤ 退群请求纳入在途超时清扫，丢响应不再无提示。
- **已知限制**：本地清空仅作用于本机（"退出并删除记录"无法跨设备广播——服务端已移除本端成员资格，推送无从投递）；重新加入后按水位同步，退出期间他人所发的消息（ID 晚于水位）会被取回且可能因 ratchet 已推进而不可解密。见 §3 登记项。

## 5. 推荐执行顺序（2026-10-15 更新）

下一步候选按"安全欠账优先、横切能力其次、特性栈分批"排序；**M12 聊天流畅性与多会话体验（§4.7）与 M13 退群语义与本机历史治理（§4.8）已完成**；其余候选待讨论确定：

1. **解决历史遗留 P2 欠账**（§3 中 P2 优先）：**本仓可闭环的两项已于 2026-09-20 全部销账**——TLS 端到端集成测试与文件控制面 handler 级测试（`TestTlsIntegration` + `TestFileControlPlane`，`ctest` 14/14）。**余下 5 项 P2 均依赖部署形态或产品决策**：nonce 去重为单服务器内存态、TOFU 无带外验证与无密钥备份/设备迁移、非 Windows 平台密钥明文回退、对象存储单机无副本、数据面前置反向代理（进程内请求体上限经核验不可行——Qt 6.8.3 无任何限流/body 上限 API，已改为强制前置反向代理 + CLI 显式确认）。这些项须先定部署与产品策略再动，届时逐项评估。
2. **M11B 消息交互增强（§4.4）：已完成（2026-09-21 实机验收通过）**；**M13 退群语义与本机历史治理（§4.8）：已完成（2026-09-23）**。下一步候选为下方第 3 项。
3. **M11C 群聊与布局完善（§4.5）：已完成（2026-10-15）**——C1 抽屉 → C3 宽屏常驻右栏 → C2 最后在线时间；**M11C 至此收口，M11 三个切片（A/B/C）全部完成**。下一步候选为下方第 4 项。
4. **M11 稳定性剩余项（§4.6）**：指标监控、崩溃捕获与客户端日志上报、备份恢复演练、压测（持续，可与 M11B/M11C 并行）。
5. **M8 遗留收尾**：视频动态画面渲染（需自定义 QSGNode 渲染 `QVideoFrame`）、文件消息持久化 outbox、大群拉取/游标模式与改群名接口。
6. **M11 未涵盖的体验项**：国际化（.ts）——如需另立项。

## 6. 目录结构

> 该部分见 `ARCHITECTURE.md`。

## 7. 数据库演进

- **服务端**（SQLite，版本化迁移，当前 V11）：`schema_version`、`users`、`devices`、`sessions`、`login_audit`、`contacts`、`conversations`（V7 增 `name`）、`conversation_members`（V7 增 `role`、V9 增 `pinned`/`muted`）、`messages`（V9 增 `edited_at`/`deleted`，V10 增 `file_id`，**V11 增 `reply_to_message_id`**）、`message_receipts`、`sync_events`、`device_identity_keys`（M6，仅公钥）、`prekeys`（M6，仅公钥）、`sync_meta`（V8，清理水位线）、`files`（V10/M8：密文侧元数据，`blob_key` UNIQUE + 状态机 + 两个回收/配额索引）、`file_tickets`（V10/M8：只存票据 SHA-256 摘要，无明文列）。中长期若需多人并发/多实例部署，迁移 PostgreSQL/MySQL，并尽早抽象 Repository/DAO；对象存储已以 `IObjectStorage` 抽象，多实例部署时需同步换为共享存储。**M10 会话整表删除未引入迁移**（当时仍 V10）：复用既有表，删除在事务内按外键安全顺序显式执行（`message_receipts → messages → conversation_members → conversations`），不依赖 `PRAGMA foreign_keys`（该 pragma 仅在 `openDatabase()` 设置）。
- **客户端 LocalStore**（SQLite，按账号+设备隔离，正文加密落库）：`schema_meta`、`messages`（M9 增 `edited_at`/`deleted`，M11B 增 `reply_to_message_id`/`reply_to_sender`）、`conversations`（含群名/成员数、M9 增 `pinned`/`muted`，置顶按 pinned DESC 排序，**M13 增 `left_group`**）、`outbox`（含 `conversation_id`、M11B 增 `reply_to_message_id`）、`decrypt_cache`、`meta`（同步游标）、**`conversation_clear_marks`（M13：本地清空水位，`conversation_id` 主键 + `cleared_before_message_id`/`cleared_at`；不随会话行删除，重新加入时作为同步起拉下限）**、`sender_keys`（M7b：chain key/Ed25519 签名密钥对/迭代数，登出保留；"最新密钥"按 `rowid DESC` 选取）、`sender_key_skipped`（2026-09-09：跳序消息密钥缓存，整体密文 blob，与 `sender_keys` 同主键维度，退群一并清理）。

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
| 2026-09-16 | 修复 CI 构建缺陷：`certs` 目录自我拷贝竞态 | 上一修复后 CI 推进至 build 阶段，`TestGroupRepro` 的 POST_BUILD 失败于 `cmake -E copy_directory`：其源与目标同为 `build/Release/certs`——`qt_standard_project_setup()` 在 Windows 上把 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 统一指向构建根目录（理由：Windows 无 RPATH，DLL 须与 exe 同目录，见 `Qt6CoreMacros.cmake`），故本目标与 `Chat-Server` 输出到同一目录。自我拷贝本身可通过，但它要求源已存在，而 `Chat-Server` 的 `make_directory` POST_BUILD 与之并发（MSBuild `--parallel`），全新构建目录下必然抢跑失败。修为改用幂等的 `make_directory`，不再依赖目标构建顺序（本地已复刻全新构建目录场景：移走 `certs` + 强制重链，ninja 退出码 0）；顺带更正 `Chat-Server/CMakeLists.txt` 中"拷贝证书生成脚本"的失实注释。另为 Install Qt 步骤加 `cache: true` 缩短后续 CI。`ctest` 12/12 |
| 2026-09-16 | M12 前半完成：滚动越界修复 + 消息加载异步化 | ① 发送消息后视图滚出边界：贴底定位改以**末行 delegate 真实末端**（`layoutExtent`）为准并据此夹取 `contentY`（`ListView.contentHeight` 为按可见行估算，未创建行多时误差可达数百 px 幽灵空白），贴底请求统一经 `Qt.callLater` 合并延迟（在 delegate 布局信号内直接 `positionViewAtEnd` 是对布局逻辑的重入）；② 消息加载改**单线程时间片泵**（`QTimer(0)` + 8ms/片，逐条解密/落库/脱敏；与 M10 传输泵同口径），`sync_messages` 改 requestId→conversationId **多槽路由**（快速切会话不再丢页），落库整页单事务，每页仍只 emit 一次 `messagesSynced`；③ 本地消息搜索命中的文件清单补过脱敏口径（M11A A5 漏网，文件密钥曾随搜索结果进 JS 堆；负向对照验证用例有效）。`ctest` 12/12（`TestLocalStore` +2、`TestNetworkManager` +2）。M12 剩余：QML 增量应用消息、按会话堆叠 ChatView（§4.7） |
| 2026-09-16 | M12.3 完成：QML 增量应用消息 | `ChatView.setMessages` 由 `msgModel.clear()` + 全量重建改为**按 messageId 合并**：已存在的行原地更新（不销毁 delegate）、新消息按 id 序插入、未读分隔线只在首次填充放置；状态更新加秩比较（只前进不回退，与 `LocalStore::statusRank` 同口径），附件下载进度/状态不再被刷新页重置（此前 `fileState`/`fileProgress` 会随重建丢失）；首次填充即置贴底意图，消除服务端页在 50ms 定时器窗口内到达时误报 FAB 徽标。顺带修复旧实现的隐性丢行——clear+重建会丢弃不在到达页内的既有消息（如本地缓存有 100 条而服务端页只回 50 条时，前半截从视图消失）。10 项 Qt Quick Test（`out/`，负向对照对旧实现 6 项失败）、`qmllint` 零错误、无头冒烟无 QML 错误、`ctest` 12/12 |
| 2026-09-16 | M12.4 完成：会话视图堆叠（M12 结项） | 会话区由单个 `ChatView` 改为**按会话堆叠**：`chatViews`（键 `c<conversationId>`，新会话首条消息前为 `p<peerUserId>` 待绑定键）+ `activeViewKey` 活跃指针，已打开会话再切回只切可见性、实例与消息/滚动位置/输入框草稿/附件下载进度全部原样保留；`ChatView.clearMessages` 随之删除（视图不复用、只销毁）。细则：`openConversation` 复用既有视图（私聊会认领同对端的待绑定视图，避免同一会话两个实例）、`confirmSentMessage` 按幂等键找回发起视图并把新会话迁移到服务端 ID（补进 `currentConversationId` 与列表高亮）；LRU 上限 8 实例，驱逐最久未用的非活动视图并转存其输入框文本，重建时恢复；MainWindow 消息页/新消息推送/文件回显按 conversationId 分发，消息状态与传输进度按 messageId 扫描落到所属（可为隐藏）视图；切回已打开会话**不回放本地缓存页**，只 `loadMessagesRequested(convId, lastMessageId)` 补收离线窗口的增量（补 `ingestSyncEvents` 只落库不发 UI 事件留下的空洞）；已读回执从"当前会话消息页到达"改挂新增的 `conversationActivated` 信号（切回旧会话没有页到达，角标否则永不清零）；`messageSendFailed` 携带失败消息的幂等键，QML 精确标记发起视图的气泡，只有无键的整批失败才退回启发式；系统消息映射抽到 `components/ChatText.js`（气泡与会话预览共用唯一实现）。验证：新增 8 项 Qt Quick Test（`out/tst_mainpage_stack.qml`：实例复用、增量补收、LRU、草稿转存、待绑定迁移、隐藏视图传输路由、删除会话关闭视图、登出销毁全部视图），负向对照（把切换改回"销毁其它视图"）4 项失败；M12.3 的 10 项用例同步适配（`clearMessages` 已移除）；`qmllint` 无新增类别告警、无头冒烟无 QML 错误、`ctest` 12/12 |
| 2026-09-16 | M12.4 代码审查修复：分页续拉、孤儿视图、失败精确标记 | 代码审查发现并修复四项（均为 M12 引入）：① `messagesSynced` 的 `hasMore` 被丢弃——视图 `lastMessageId` 被实时消息推高后，下次激活的增量补收从更高 id 起算，中间那段成为取不回来的**永久空洞**，现按"本页末条 messageId"链式续拉直到取完（`MainPage.updateMessages`）；② `adoptPendingView` 无条件覆盖目标键，被覆盖视图脱离 `chatViews` 成为既不被淘汰也不被 `resetUi` 销毁的**孤儿**（已解密消息驻留到进程退出），现保留原视图、销毁待绑定实例并补收一次；③ `messageSendFailed` 不带幂等键导致失败标记靠猜测（加好友失败还会借道该信号误标在途气泡），现信号携带 `clientMessageId` 由 `pendingSentKeys` 精确定位，加好友失败改走独立 `addContactFailed`；④ `destroyView` 残留 `pendingSentKeys` 记录（会话键复用后会让失败标记落到新视图的无关气泡）。另修正 `LocalStore::upsertMessages` 注释的提交粒度口径（消息页泵按片提交、非整页）。验证：新增 Qt Quick Test 3 项（续拉起点、占用键迁移、失败精确标记）且负向对照各失败 1 项；`TestNetworkManager` 新增跨片整页单次 emit 用例（6 次重复无抖动）；`qmllint` 无新增类别、无头冒烟无 QML 错误、`ctest` 12/12。
| 2026-09-17 | 下载票据生命周期修复（销账 §3 P2「TTL 与大文件下载不匹配且中途不续期」） | 服务端下载票据改**按“最后一次使用”滑动续期**（新增 `DatabaseManager::renewFileTicket`，由 `FileHttpService::authorizeTicket` 在下载授权通过时调用），并以 `created_at + DownloadTicketMaxLifetimeSeconds(24h)` 为**绝对上限**封顶（`min()` 标量重载；仅对仍有效且未消费的票据生效，不复活失效票据；续期失败不改变本次授权结论，故授权路径不因它 fail-closed）。客户端 `FileTransferManager` 收到 401 时**重新申请票据并从断点续传**（保留 `downloadIndex`/`tmpPath`；续写前校验临时文件长度与记账一致，不符则退回从头下载；`MaxTicketRenewals=3` 单独封顶以防“申请－再 401”活锁，且该封顶是**连续**预算——任一分片成功落盘即清零，否则一台休眠过几次的机器下载大文件时会因累计满 3 次而永久失败并丢掉全部进度，正是本项要消除的症状；等待新票据期间清空票据，复用 `pumpNext` 的“票据非空”门防止用旧票据重打）。顺带修正 `FileProtocol.h` 将下载票据写成“一次性”的失实注释，并同步 `SECURITY.md`/`PROTOCOL.md` 口径。验证：`TestDatabaseManager` +1（滑动续期、绝对上限、不复活失效/已消费票据、入参兜底）、`TestFileHttpService` +1（一次 GET 后寿命被推回完整窗口且票据继续可用）、`TestFileTransfer` +2（首片落盘后吊销票据 → 重申请并断点续传且**字节级一致**；每张票据皆废 → 4 次申请后明确失败且不留半截缓存）；**三处修复分别做负向对照，失败点精确落在各自的 `ttlAfterRenew <= 300`、`ttlAfter >= Ttl-10`、`download did not finish` 断言上**；`ctest` 11/12——`TestNetworkManager::largePageEmittedOnceInOrderAcrossSlices` 因 5 秒预算超时失败，同一二进制 5 次实跑（3 次只跑该函数 + 2 次整卷）100% 复现、泵稳定需 14500-14900 ms（隔离与整卷一致，与负载无关，QTest 自身报告“约 14.6 s 即可满足”），已登记 §3 P3，与本轮改动无因果（该用例不触及本轮任何改动）。**第二轮复核**另修掉自己引入的 3 处缺陷：`FileProtocol.h` 注释对齐、`DatabaseManager.cpp` 函数间空行、以及 `ticketRenewals` 由**累计**改为**连续**预算（真问题，见上）。 |
| 2026-09-17 | 在途请求超时清扫（销账 §3 P2「编辑/删除在途请求无超时清扫」与 P3「`sync_messages` 在途请求与页队列无超时/上限」） | `NetworkManager` 新增**单表统一超时清扫**：`m_pendingDeadlines`（`requestId → 截止时刻`，30s）由惰性创建、表空即停的 `m_inFlightSweepTimer` 每 5s 扫一次，命中即清退并复用**断线路径已有的失败信号**（`messageEditFailed`/`messageDeleteFailed`/`conversationDeleteFailed`）让 UI 回退乐观态；消息页同步请求无对应信号，只回收登记项（该会话下次激活会重新请求）。之所以共用一张表而非给每个容器加字段：`requestId` 由 `nextRequestId()` 全局单调分配、永不复用，故无需改动容器值类型（改类型会波及既有用例与全部响应路径，收益为零）。覆盖范围除欠账点名的编辑/删除/同步外，**并入了 M10 后加的 `m_pendingDeleteConversationRequests`**（同一缺陷类的遗漏）**与私聊编辑等待 `fetch_keys` 传输槽的停摆**——后者是同类里更严重的一例：漏答时 `m_editFetchInFlight` 恒真，队列里所有后续私聊编辑会**静默停摆到下次重连**；清退方式完全复用既有的“密钥包不可用”分支（出队上报 + 泵送下一条），并释放槽位（槽位按 requestId 匹配，晚到的旧响应会被首行校验丢弃，不会重复消费）。另给 `m_messagePageQueue` 封顶 16（= 视图 LRU 8 的余量，含 `hasMore` 链式续拉），超限丢最旧且不发信号——与 `finishMessagePageJob` 的“页内全不可见消息时跳过 emit”同一口径，QML 本就不依赖每页必有响应。验证：`TestNetworkManager` +4（四类登记表到期清退且未到期的原样保留、响应到达即撤销截止登记并在表空时停表、页队列封顶且丢最旧、私聊编辑槽位超时后出队上报并续泵），**三处破坏的负向对照分别精确落在“清退计数”“截止登记未撤销”“队列尺寸”上**；`ctest` 11/12（唯一失败仍是既有的 `largePage…` 预算超时） |
| 2026-09-17 | 数据面绑定面修复（更正 §3 P2「请求体已缓冲」的错误前提） | **原判断有误**：欠账里写“`--http-host` 默认仅回环是正确的”，实际 `--http-host` 只设置下发给客户端的 `fileTransferBaseUrl`（`setAdvertisedHost`），而 `FileHttpService::start()` 无条件 `listen(QHostAddress::Any, port)`——**数据面默认监听全部网卡**。于是“未认证请求体在票据校验前被 `QHttpServer` 读进内存”这一放大面是默认对局域网敞开的，比原判断严重一档。修为：绑定地址与通告地址**分离**（`setBindAddress`/`setFileHttpBind`，默认仅回环），绑非回环须显式 `--http-allow-public` 并打警告，非法地址 **fail-closed 拒绝启动**（绝不静默退回 `Any`——否则“地址打错”与“有意对外”行为完全一致）；启动日志与 `file.http_started` 事件增加 `bindAddress` 字段。**进程内请求体上限经核验不可行**：Qt 6.8.3 的 `QAbstractHttpServer`/`QHttpServer` 无任何限流或 body 上限 API（核对公开头与私有头），自建请求头预检等于手写 HTTP 解析器（与真实解析器不一致即引入请求走私），`setReadBufferSize` 亦无效（解析器会持续抽干 socket 缓冲、把数据累积进自己的 body 缓冲）。故对外部署的前置条件仍是带 body 上限的反向代理，现已由 CLI 显式确认 + 启动警告强制可见；“已授权请求的并发数与字节速率上限”拆为 §3 P3（有意留白）。验证：`TestFileHttpService` +1（默认仅回环、非法地址拒绝、非回环未确认拒绝、确认后可绑）——**两轮独立负向对照**分别命中“默认回环”与“fail-closed 守卫”两条断言 |
| 2026-09-17 | 第二批代码复核 | 同上两轮内自审并修掉：`NetworkManager` 三处 `remove() > 0` 比较触发 MSVC `C4804`（Qt 各容器 `remove` 返回类型不一致，改用 `contains + remove`）；`FileProtocol.h` 常量注释对齐列；`DatabaseManager.cpp` 函数间空行。另记录一条可复用教训：**做负向对照时必须复刻“修复前的完整状态”而非单个 hunk**——本轮的默认绑定值若只改一半（默认改 `Any` 但保留 fail-closed 守卫），服务在 `initTestCase` 就起不来，失败点跑到与本用例无关的地方，会把结论引偏 |
| 2026-09-17 | 群 Sender-Key 分发可靠性修复（销账 §3 P2「大群单条 `sender_key_distribution` 超 16384 上限」与 P2「群 Sender-Key 先落盘后分发」；并销掉 P3「`largePage…` 5 秒预算不足」，`ctest` 首次恢复 12/12 全绿） | 两项 P2 是同一条链上的因与果，故同轮一并修。**① 分片分发**：新增 `GroupE2eeCrypto::encodeDistributionChunks`（预算 `MaxDistributionContentLength=15000`，对服务端 `MaxGroupMessageLength`(16384) 留约 8% 余量），按**序列化后的实际长度**累加切分而非估算——Compact 输出无空白，故 `[a,b]` 的长度严格等于 `1+len(a)+1+len(b)+1`，可精确预判追加下一条是否越界；把设备条目的 JSON 形态与根对象布局抽成 `distributionDeviceObject`/`encodeDistributionRoot` 供整包与分片两条路径共用，避免字段布局各自漂移。关键性质：每条分片都携带**同一把** `SenderKey`（同 `keyId`/`publicSigningKey`/`chainKey`，`devices` 只覆盖部分设备），接收端只在自己所属的那条里找条目，因此**无需重组**、同 `keyId` 重复安装天然幂等；单条条目自身超预算时仍单独成片——宁可让整次分发被服务端拒掉（调用方随后丢弃整把 key），也不能静默少发设备。**② ACK 后启用**：`buildGroupSenderKeyDistribution` 改为**不落盘、不进内存缓存**（原实现在生成时即 `saveSenderKey` + 写 `m_groupSenderKeys`，于是分发一旦被永久拒绝，本端已用他人拿不到的 `keyId` 加密，产出一批谁也解不开的群消息），改由 `beginGroupSenderKeyDistribution` 登记为待提交（分片 `clientMessageId`/`requestId` 三张映射表），全部分片 ACK 才由 `commitPendingGroupSenderKey` 落盘并激活；任一分片被拒/超时/断线走 `failGroupSenderKeyDistribution`——清退**全部分片**的在途登记（含已 ACK 的那片，否则其复核 ACK 会落进“普通消息”分支凭空 `emit messageSent`）、wipe 密钥材料、置 30s 退避并排一个重试定时器（单发护栏），断线/登出则由 `discardPendingGroupSenderKeys` 统一丢弃。pending 期间 `ensureGroupSenderKey` 对该群 **fail-closed**：既不能用尚未分发的新 key（其他成员无解），也不能在轮换期继续用旧 key（新成员无解、被移除成员仍可解）——这是“先落盘后分发”之外**额外封住**的第二个窗口，也是本项不能简单改成“ACK 前用旧 key 顶着”的原因。分片请求并入既有的单表在途超时清扫（30s），否则一次丢包就让该群的分发永久挂起，而挂起期间该群发不出消息。**③ 接收端幂等**：切分后同一 `keyId` 的分发由 1 条变 N 条，历史同步还可能重投，而 `decodeDistribution` 不回传 `iteration`（恒 0），故对本地 `iteration > 0`（已使用）的 `keyId` 拒绝再被安装，避免把已推进的 ratchet 打回 0。**本项末一条是被自己写的用例抓出来的**：初版按“本地 chainKey 是否等于分发里的那份”判定，而 ratchet 推进后本地 chainKey 早已不同，保护**恒不生效**（用例先失败、再改判定依据），说明该用例不是事后补的形式断言。验证：`TestGroupE2eeCrypto` +4（多片不超预算且不丢/不重条目、条目少时与整包编码**逐字相同**、单条超预算仍成片、非法入参返回空）、`TestNetworkManager` +5（在途分发时连既有旧 key 也不得使用且加密返回空、两片中只 ACK 一片不得启用、分片被拒即整次作废且晚到 ACK 不回显、漏答经超时清扫清退、`flushOutbox` 在在途/退避期不重拉且带正例对照）；**五处破坏的负向对照 + 幂等守卫的负向对照，失败点分别精确落在各自的断言上**；`ctest` 12/12（同轮把 `largePage…` 的 `QTRY_COMPARE` 改 `QTRY_COMPARE_WITH_TIMEOUT(…, 60000)`——Debug 下该大页泵稳定需约 14.7 s，默认 5000 ms 预算会在任何逻辑断言求值前先超时，属预算问题而非正确性缺陷）；无头冒烟无新增 QML 报错（仅既有 `QQmlBind::target` 非 NOTIFYable 告警）。**新增欠账**：分发覆盖不到的成员（未注册 E2EE / 预密钥耗尽）无用户可见反馈（§3 P3）。 |
| 2026-09-20 | M12 实机验收通过（双客户端）；验收途中两处客户端崩溃定位为**构建树混编**，非代码缺陷 | 服务端 + 两个 GUI 客户端、两个真实账号的人工验收通过：发送后稳定贴底不越界、快速切换会话无卡顿、各会话滚动位置/草稿/附件进度互不污染；群 E2EE 活链路正常——本端新分发的 sender key（`f4706df1…`）被对端成功安装，新消息可解。验收期间先遇两处崩溃（`Q_ASSERT "numBuckets > 0"` 与登出时 `QHash<qint64,qint64>::clear` 访问违例），经 cdb 挂起复现 + MiniDump 离线分析定位：崩溃成员 `m_fileSeqByRequestId` 的 `d` 指针指向 `AppSettings::vftable` 前 1 字节——**该内存本就是 AppSettings 对象**。成因：`main.cpp.obj`（13:10）早于 `NetworkManager.h`（17:46），`main()` 按旧 `sizeof(NetworkManager)` 为栈对象留位，使紧随其后的 `appSettings` 与 NetworkManager 尾部成员重叠，而 `NetworkManager.cpp` 按新偏移读写。全树核查：**87 个 .obj 中 40 个陈旧**。处置：`pwsh`（PowerShell 7，`Build.ps1` 在 Windows PowerShell 5.1 下会因 UTF-8 无 BOM + 执行策略解析失败）执行 `cmake --preset` + `ninja -t clean` + 全量重建，118/118 成功，`ctest` 12/12 复验（此前那次 12/12 系混编二进制所出）。残留的 `unusable`/`No sender key` 警告经核对全部来自**历史重放的老分发**（其预密钥早已消费，属"历史消息不可恢复"口径），活链路不受影响；日志噪音已登记 §3 |
| 2026-09-20 | 销两条 P2 欠账：主通道端到端 TLS 集成测试 + 文件控制面 handler 级测试 | 两项同轮补齐，`ctest` 12/12 → **14/14**。**① `TestTlsIntegration`（6 用例，7.7 s）**：真实证书 + 真实 `Server` 进程内启动 + 真实 `QSslSocket` 客户端（CA 注入与验证口径与生产 `NetworkManager::initTls` 逐项一致），覆盖"未启用 TLS 时拒绝启动"与"客户端无 CA 握手必败"两条 fail-closed 断言、跨连接两用户的完整链路（帧编解码 → 重放保护 → 会话鉴权 → DB），并断言登录响应下发的 `fileTransferBaseUrl` 就是 Server 实际监听的（OS 分配的）数据面地址。**② `TestFileControlPlane`（11 用例，1.7 s）**：进程内真实回环 + 真实 `RequestHandler`，按协议 90-99 手工发包，覆盖五个文件处理器——入参形态先于限流与配额、上传配额（8）与限流窗口（20/60s）是两道独立闸门、查询反映已收分片、完成与取消的幂等、finalize 的"未齐备 3016 附已收索引"与"数据故障 3015 并回收"分类、以及"不存在"与"不是你的"必须回同一码的枚举预言机合并；另断言三个响应都不含 `blobKey`（存储键不出服务端）。**测试接缝两处（不改生产行为）**：数据目录可用 `XYCHAT_SERVER_DATA_DIR` 重定向（handler 的连接名按 this 指针生成，无法注入 `:memory:`，否则会写开发者真实库）；`Server::setFileHttpEndpoint(0, …)` 接受 0 = 交 OS 分配（CLI 仍把 0 判为非法），避免与在跑的开发服务抢 12346。**负向对照**：把重放保护短路、并发配额放宽到 1000、所有权错误码拆出 `PermissionDenied`，三处破坏分别精确命中 `replayProtectionPrecedesAuth`/`createEnforcesQuotaThenRateWindow`/`queryMergesNotExistAndNotYours`（8 通过 3 失败），还原后复验 11/11 |
| 2026-09-20 | M11B 完成：消息交互增强（多选 / 转发 / 回复引用 / emoji） | 四个 Phase 按 B1 → B3 → B2 → B4 顺序落地，**只有 B2 是协议扩展**。**B1 多选**：选择集为视图级状态（`ChatView.selectedIds`，整表重赋值驱动绑定，避免随 delegate 回收丢失），行级 `MouseArea` 在选中期接管整行左键，标题栏换成选择工具栏（转发/删除/Esc），可用性经属性绑定（`selectedRows → canDeleteSelected / hasForwardableSelected`）计算而非函数调用（**2026-10-15 订正**：当时所作的归因“QML 依赖跟踪不穿透函数体”不成立，实测会被穿透；“选人后不刷新”的真因是 `selectedIds` 原地改动不触发 NOTIFY，须整表重赋值）。**B3 转发**：以本端已解密正文向目标会话重新发送（服务端无转发元数据、不带引用），`MainPage` 在 emit 前注入 `pendingTrackKey`/`pendingTrackConversationId` 把乐观气泡与在途登记落到目标会话（目标会话未打开时不建视图），草稿清理仅限当前会话发送；可转发子集排除系统/已删除/不可解密/文件消息。**B2 回复/引用（协议 + V11）**：`send_message` 可选 `replyToMessageId`，形态校验先于限流消费、语义校验要求目标存在且属**同一会话**（"不存在"与"不属于本会话"回同一 `InvalidRequest`，避免跨会话存在性探测）；服务端只回 ID 与被引用者名、**不产出摘要**（群聊正文对服务端是密文），响应/推送/历史/事件四条路径同口径；客户端以 `ChatView.replyPreviewMap` 渲染引用块（随编辑刷新、随删除转占位），未命中退回"原消息不可见"；点击跳转 + 1.5s 高亮；`LocalStore` 消息表两列 + outbox 一列（重启重发不丢引用）。**B4 emoji**：输入区 `Popup`（常用网格 + 最近使用两行），插入到光标处，最近使用去重前置、上限 16、经 `AppSettings.recentEmojis` 持久化。验证：`ctest` **14/14**（`TestDatabaseManager` +3、`TestTlsIntegration` +2、`TestLocalStore` +2）、`qmllint` 零错误、Qt Quick Test 14 项（`out/tst_m11b.qml`，**三处破坏的负向对照分别精确命中各自用例**）、无头冒烟无 QML 错误。已知限制三项（文件消息不转发/引用仅覆盖本地已有消息/批量删除受 20·60s 限流）见 §4.4，文件转发登记为 §3 P3 |
| 2026-09-20 | M11B 代码审查与修复 | 审查（子代理）发现两项 P1：① **本地库列/绑定错位**——`LocalStore::upsertMessageRow` 的 11 个绑定值里 8-11 号与列清单（`status_rank`/`undecryptable`/`client_message_id`/`created_at`）顺序颠倒（M6.5 引入的既有缺陷，本次恰好在其后追加了引用两列才被看到）——后果是本地缓存页的时间戳被写成 `"0".."3"`、`undecryptable` 标志失效、`upsert` 的"回执只前进"判定基于垃圾秩值；修为对齐绑定序并补 `upsertWritesColumnsInOrder` 用例（逐列往返 + 状态不得回退），该缺陷此前被"批量 vs 单条同源对比"式的用例掩盖；② **失败气泡重发丢引用**——`ChatView` 的 delegate 仍按 2 参调用已扩为 3 参的 `resendRequested`，QML 对缺省实参静默按 0 处理，回复重发会退化成普通消息；修为补 `model.replyToMessageId` 并新增用例（直接触发 delegate 信号，与点击状态图标同一条接线）。另收紧四项 P2/P3：多选可用性在"别的设备删除消息"后不陈旧（`updateMessageRow` 置 `deleted` 时同步剔除选择集）、转发后 `pendingTrackKey` 兜底清理（发送入口提前返回时不留脏目标）、转发提示改"正在转发"（服务端确认是异步的）、引用校验改用 `DatabaseManager::isMessageInConversation`（此前该函数无生产调用者，文档与实现口径不一）、`parseReplyTarget` 加 `isfinite` 守卫、去掉两个无调用者的函数、乐观/失败气泡不可被回复（`messageId > 0` 才允许）、Enter 发送时收起 emoji 面板。复验：`ctest` 14/14（`TestLocalStore` 31 用例）、QML 用例 16 项、`qmllint` 零错误、无头冒烟无 QML 错误 |
| 2026-09-21 | M11B 实机验收（双客户端）：修复 emoji 面板定位/裁切与引用跳转目标 | 验收发现两处（均为纯 QML 缺陷，已在同轮修复并补回归用例）：**① emoji 面板位置错误、内容超出边界且不可滚动**——面板原先用 `x/y` 绑定定位并依赖打开瞬间的内容隐式高度：(a) 绑定的首次求值可能早于窗口/版式就绪（`mapToItem` 得到 0），之后依赖不再变化，面板永久停在兜底位置（实测落在会话列表上方）；(b) 内容里的 GridView 要等抛光才有真实高度，打开瞬间定下的高度偏小，实际渲染时底部溢出窗口（有「最近使用」时面板高出约一截，最下两行看不到）。修为：位置改由 `aboutToShow`/`onOpened`/`onHeightChanged` 调 `positionEmojiPanel()` 显式计算并挂 `Overlay.overlay`（窗口坐标系），面板宽度按 `8 × 单元宽 + 内边距` 精确给出（原 328 未扣内边距，第 8 列被右缘压住），高度显式绑定并在窗口过矮被挤压时让网格转为可滚动。**② 点引用块跳到错误位置**——`MessageBubble.replyJumpRequested()` 原先不带参数、delegate 用 `model.messageId` 跳转，于是跳到「承载引用的那条消息」自身（把点击处重新居中、高亮也落错气泡），而非被引用消息；修为信号携带被引用消息 ID（`replyJumpRequested(int messageId)`）并由 delegate 透传，新增用例以真实鼠标点击引用块断言高亮落在被引用消息且目标行完整进入视口。另：`LocalStore` 列/绑定错位与失败重发丢引用的修复见上一行。验证：QML 行为用例 19 项全绿、`qmllint` 零错误、服务端真实库 V10→V11 迁移成功、其余四项验收（多选→转发/删除、引用摘要随编辑/删除刷新、emoji 插入与最近使用、失败重发保留引用）实机通过 |
| 2026-09-23 | M11B 入库前复查：`qmllint` 全量扫描 + 信号参数比对 + 补 outbox 重启恢复用例 | 对 8 个变更 QML 文件做全量 `qmllint`：**0 错误**，252 警告全部落在既有类别（`[unqualified]` 上下文属性 224、`[Quick.layout-positioning]` 24、QWindowKit 的 `[import]`/`[missing-type]` 误报 4）。扫描**抓出一处真缺陷**：`MessageInput.positionEmojiPanel` 的窗口兜底写在 `messageInput.window` 上，而 `Item` 并无 `window` 属性（附加属性才是 `Window.window`）——该访问不报错、恒为 `undefined`，于是 `if (messageInput.window)` 整块是**永不生效的死分支**（实际由 `emojiPopup.parent`（`Overlay.overlay`）的尺寸兜住，故实机验收看不出问题）。probe 实测确认（`rect.window` 为 `undefined`、`Window.window` 有值）后改用 `Window.window`，该 `[missing-property]` 警告随之消失，并作为"`[missing-property]` 是真缺陷、与 QWindowKit 误报不同"的约定写入 `AGENTS.md`。**信号参数比对**：对本次新增/扩展的 16 个信号逐一列出声明、发射点与处理器，参数数量全部一致（`sendMessage` 2 参、`resendRequested` 3 参、`sendMessageRequested`/`sendGroupMessageRequested` 3 参、`forwardConfirmed` 4 参、`replyJumpRequested(int)` 1 参及 8 个零参信号；`MessageInput.sendMessage()` 与 `ChatView.sendMessage` 同名但一为函数一为信号，逐个核对不混）。**补用例**：`TestNetworkManager::openLocalStoreRestoresOutboxReplyTarget`——预置"一条带引用的回复 + 一条普通消息"于持久化 outbox，经 `openLocalStore()` 载入后断言内存 `m_outbox` 的引用目标复原为 4242 / 0。原 `TestLocalStore::outboxPreservesReplyTarget` 只覆盖库层 close→open 往返，**未覆盖 `LocalStore → 内存 outbox` 的搬运**（该搬运正是"重启后重发的回复是否退化"的真正决定点）。**负向对照**：把搬运处的 `item.replyToMessageId` 置 0，用例精确失败在 `Actual (replyTarget): 0 / Expected: 4242`，还原后通过。复验：全量重建并核对 103 个 .obj 相对 5 个变更头文件**无陈旧对象**（混编排查式自查）、`ctest` **14/14**、无头冒烟无 QML 报错 |
| 2026-09-23 | M13 完成：退群语义与本机历史治理（修复"退群后会话复活且无法删除"） | 用户报告的缺陷：退群后刷新会话列表，已退出的群重新出现（可看历史、收不到也发不出去消息），且**无法从会话列表删除**。根因：客户端把本地缓存当只增不减的展示源——服务端不再返回该会话（成员行已删），但对账只做 upsert 不标记，任何一次缓存先行（下次登录、已读游标推送、会话偏好推送）都会把它显示回来；而它已非"会话"（M10 整表删除仅成员可发起、群聊仅群主可删），右键"删除会话"必被服务端拒绝。修复同时把本机历史的去向交还用户（对齐主流 IM）：主动退群二选一【仅退出（保留历史、会话留列表但只读）/ 退出并删除本机记录（清空本地缓存并从列表移除）】，被管理员移出默认保留历史。落地：`LocalStore` 新增 `left_group` 列与 `conversation_clear_marks` 清空水位表 + `reconcileConversations` 服务端权威对账 + `purgeConversation`；`NetworkManager` 退群在途请求改多槽并携带用户选择、`handleGetConversationsResponse` 改 emit 对账后的本地列表（根因所在）、失权分支默认保留历史、`applyClearMarkFloor` 把服务端起拉点抬到清空水位、新增仅删本机的 `deleteLocalConversation`；QML 新增 `LeaveGroupDialog` 与 `ChatView.readOnly` 只读态（输入区换提示条 + 删除出口、群信息入口与消息级操作收起）、`ConversationList` 已退出条目与菜单收敛、已退出的群不作转发目标。**无线上协议变更、无服务端改动**。验证：`ctest` 14/14（`TestLocalStore` +3、`TestNetworkManager` +1）、QML 行为用例 5 项（`out/tst_m13.qml`）、**负向对照 4 项**（回退只读绑定→"只读时输入框应收起"失败；回退删除路由→"应发出本机删除信号"失败；回退菜单收起→列出 4 项操作；破坏对账标记→`leftGroup` 断言失败）全部如实失败、`qmllint` 0 错误（353 警告均为既有类别）、无头启动无 QML 报错 |
| 2026-09-30 | M11C C1 完成：群信息抽屉替代弹出对话框 | 群信息从 `AppDialog` 弹窗改为**右缘滑出抽屉**（对齐 Telegram Desktop），并为 C3 宽屏常驻右栏做组件拆分：`GroupInfoPanel`（纯内容面板：群名/成员/邀请/退出/移除，`showCloseButton` 可关，C3 直接复用）+ `GroupInfoDrawer`（`Drawer` 包装，非模态无遮罩），原 `GroupInfoDialog` 删除。生命周期守卫（`MainPage`）：`showGroupInfo` 只展示当前会话**且未退出**的结果；抽屉打开时切换会话跟随换载（活跃在群）或收起（私聊/已退出的群）；待绑定私聊（`openChatWithUser`）显式收起；退群/被移出/会话删除/登出四路径收抽屉；邀请目标改取抽屉自身 `convId`。**CodeReview 修复 1 项 P1 + 4 项 P2**：① **P1 默认 `closePolicy` 使"边聊边看/切换跟随"不成立**——`CloseOnPressOutside` 让第一次点击聊天区（含点会话列表条目）就收起抽屉，跟随换载实质是死代码，收窄为 `Popup.CloseOnEscape`（探针实证后修复）；② `interactive` 限定 `convId > 0`，防止拖出空抽屉（邀请/退出会作用于 0 号会话）；③ 常开期间内容滞后——`MainWindow` 接上此前无人连接的 `groupChanged`/`groupMembersInvited`/`groupMemberKicked`，抽屉开着且正是该群时重发 `get_group_info`；④ 被踢后迟到的 `get_group_info` 响应会把抽屉重新打开成刷不掉的陈旧面板，`showGroupInfo` 增加 `leftGroup` 丢弃；⑤ `openChatWithUser` 绕过跟随逻辑（抽屉继续展示上一个群）。面板取色全走 `Theme`（Basic 样式 Popup 默认取系统调色板）。**无协议变更、无服务端改动、无安全面变化**（数据仍走 `get_group_info`，服务端成员校验不变）。验证：QML 行为用例 10 项（`out/tst_m11c.qml`：抽屉配置与 CloseOnEscape、外部点击不收起、空抽屉不可拖开、showInfo 渲染与原地刷新、角色门控三态、移除/退出/邀请信号路由、关闭按钮）；**负向对照 5 项**（`modal` 改 true、`canKick` 恒真、断开关闭接线、删 `closePolicy`、`interactive` 恒真）全部如实失败；`ctest` 14/14、`qmllint` 零错误、无头冒烟无 QML 错误。用例层两条经验：匹配按钮须限定 AppButton 本体（其内部 IconLabel 的 `visible` 不随按钮隐藏翻转，会把隐藏按钮误判为可见）；模型变更后须等一帧再检查 delegate（旧实例销毁前 model 上下文已剥离，此时点击它会把 `model.userId` 发成 undefined）。实机验证（抽屉滑出/收起、切换跟随、暗色主题）待 C3 一并进行 |
| 2026-10-15 | 构建树工具链换装修复、文档一致性专项与一项误判订正 | **P0 构建环境**：VS Enterprise 被 Professional 取代（装于 2026-10-08）后，`out/build/debug/CMakeCache.txt` 仍固化 Enterprise 的 `CMAKE_CXX_COMPILER`/`CMAKE_MAKE_PROGRAM` 绝对路径，`cmake --build` 以“找不到 ninja.exe”告败；清空构建树重新配置后恢复，`Build.ps1 -Preset Qt-Debug -Build` 全流程复验（104 个 .obj 无陈旧对象、`ctest` 14/14、无头冒烟无 QML 报错），教训写入 `AGENTS.md`。**P2 文档一致性 6 项**：§1 标题日期 09-16→09-30、§0 导航 §4.1-§4.3→§4.1-§4.8、§1.2 补录 M11C C1、§4.5「交付与验收」剔除 C2/C3 项并改标“仅 C1 部分已完成”、§3 销账 P3「桌面通知；简化图片消息」（两项均已由 M11A 与 M8.3a/b 交付）、`AGENTS.md` VS 版本更正。**P3 一项误判订正**：上一轮审查提出的“`GroupInfoPanel` 的 `visible` 是函数调用式绑定、`myRole` 变化不触发重算”经实测**不成立**——最小探针（本对象函数不限定名读 / 他对象函数限定名读 / 外部 `.js` helper 读入参）与**真组件**（把 `myRole` 原地改值、不重建 model，移除按钮可见数随 0→2→1 正确重算）双重验证：QML 绑定**会**穿透函数体登记依赖，Qt 6.8.3 上无此缺陷，故该改动已回退。据此订正三处旧归因（`ARCHITECTURE.md`、§4.4、§10 的 2026-09-20 条）并写入 `AGENTS.md`：真正的陷阱是 `var` 持有 JS 对象的**原地改动**不触发 NOTIFY（同一探针验证：`bag.b = 2` 后直接读与经函数读**都不刷新**），M11B B1 当年归因的“不穿透函数体”实为误诊。另登记一项 P3「一次踢人触发三次 `get_group_info`」。 |
| 2026-10-15 | M11C 完成：C3 响应式布局（宽屏常驻右栏）+ C2 最后在线时间，并修复抽屉遮挡标题栏 | **C3**：新增 `Theme.wideLayoutBreakpoint`(1080)/`rightPanelWidth`(340) 两个 token，`MainPage` 第三栏由 `GroupInfoPanel` 常驻（`visible: wideLayout && groupInfoOpen`），窄屏沿用 C1 抽屉；抽屉与右栏**各持一份面板、只填当前生效的那份**。**开合状态刻意与动画解耦**——`groupInfoOpen` 是意图状态，只由 `showGroupInfo`/`closeGroupInfoPanel`/抽屉 `aboutToHide` 写入，不从抽屉的 `opened`/`position` 反推：`opened` 要等打开动画收尾才置位，耦合会同时造成两个缺陷——关闭动画未收尾时再点开，迟到的 `closed` 把刚设上的状态覆盖成 false（"点了没反应"）；跨断点判断拿到动画中途的假值。跨断点（`onWideLayoutChanged`）把形态连开合一起转移，内容用 `lastGroupInfo` 缓存重填，不为换容器再向服务端要一次；判据用 `position > 0` 而非 `opened`。**C2**：`get_conversations` 为 private 会话回填 `lastActiveAt`/`online`，在线与否**在 SQL 内**判定（`datetime('now', '-120 seconds')`）——`last_active_at` 由 SQLite `datetime('now')` 写入、而 C++ 侧其他时间戳是 `Qt::ISODate`，在 SQL 里比对可避免两侧格式漂移；无迁移。客户端**不把这两个字段写进本地缓存**（瞬时状态，否则下次启动会显示上一次会话的陈旧"在线"），改在 `handleGetConversationsResponse` 按 `conversationId` 盖到缓存优先的合并列表上；展示在私聊头部第二行，**只用绝对时间**（本值只在会话列表刷新时更新、无走时定时器，相对措辞会定住失真），服务端时间戳须补 `T`/`Z` 按 UTC 解析（空格形式会被 `Date` 当成本地时区，直接把时差体现成错误的最后在线）。**顺带修复（用户报告）**：抽屉从 `y=0` 铺满整窗会盖住自定义标题栏的拖拽区与窗口控制按钮，改为显式让开 `Theme.titleBarHeight`；窗口默认可视宽度由 1000 提到 1180，否则新装用户（默认 1000）永远落在窄屏分支、看不到 C3。验证：`ctest` **14/14**（`TestDatabaseManager` +1：无 session/在线/陈旧/阈值边界/群会话不适用）、QML 行为用例 **12 项**（C3 六项含跨断点双向转移与抽屉让开标题栏，C2 六项含本地时区渲染与非法时间戳降级）、**负向对照 4 项分别精确命中各自用例**（去标题栏偏移→0 vs 32；跨断点不关抽屉；右栏不按宽度门控；时间戳不按 UTC 解析→两条时间用例各差 8 小时）、`qmllint` 零错误、无头冒烟无 QML 报错。**新增欠账**：`online` 为活跃时间启发式，空闲但已连接的客户端显示为离线（§3 P3）。 |
