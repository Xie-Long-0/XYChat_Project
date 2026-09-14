# XYChat 架构概览

> 本文档描述当前架构实态（截至 2026-09-11，M8.2 数据面与客户端文件传输完成后）；历次里程碑的演进过程与修复记录见下文各记录节，完整时间线见 `docs/ROADMAP.md` 变更记录表。

## 当前组件（M8.2 完成后）

```text
Chat-Client ── QSslSocket/PacketCodec/JSON ── Chat-Server ── SQLite
     │        (TLS 1.2+，fail-closed)          │
     │                                     ├─ IObjectStorage（M8：密文分片/最终对象）
     │                                     └─ 维护任务（sync_events 与文件回收）
     ├─ FileTransferManager (M8.2) ─ HTTP(S) + 票据 ─> FileHttpService ─> IObjectStorage
     └──── CommonModule（protocol/encryption/security）────┘
```

控制面（申请上传/续传查询/宣告完成/取消/下载票据，类型 90-99）走 TCP 主通道；数据面（分片字节流）走独立 HTTP(S) 服务。两者在客户端由 `NetworkManager` 与 `FileTransferManager` 分工，经“请求信号 + seq 回调”协作。

TLS 采用 fail-closed 策略：不存在静默降级路径（服务端无证书拒启，客户端无 CA 拒连；开发明文需显式开关）。

- `Chat-Client`：Qt 桌面客户端，**UI 已全面采用 QML/Qt Quick**（M4 完成，M4.5 完善），通过 `QWindowKit::Quick` 实现无边框窗口；登录窗口与主窗口为**两个独立根窗口**（均由 `main.cpp` 经 `engine.load()` 加载，主窗口在任务栏独立显示）；C++ 后端层为 `core/NetworkManager`（网络状态机、协议编解码、TLS、M6 起集成 E2EE 引导/加密发送/接收解密/TOFU，M6.5 起接入本地缓存与持久化 outbox，M7a 起接入群组五接口/群消息 outbox 分流/群变更推送，M7b 起实现群 Sender-Key 生成/分发/加解密与 `FetchGroupKeys` 协议交互，M9 起接入会话偏好置顶/免打扰与消息编辑/删除的请求/响应/推送/sync_events 全链路）、`core/KeyStorage`（M6：DPAPI 保护的本地密钥与 TOFU 指纹存储；M6.5：LocalStore 存储密钥）、`core/LocalStore`（M6.5：按账号+设备隔离的加密本地缓存；M7a：会话缓存新增群名/成员数字段；M7b：新增 `sender_keys` 表保存 chain key 与 Ed25519 签名密钥对；M9：会话缓存新增 `pinned`/`muted` 并按 pinned DESC 排序、消息缓存新增 `edited_at`/`deleted` 与 `updateMessageContent`/`markMessageDeleted`/`clearDecryptedContent`）、`core/ThemeSettings`（主题偏好持久化）与 `models/User`。
- `Chat-Server`：Qt TCP 服务端，`ConnectionServer`（QTcpServer）接受连接，每连接一个 `RequestHandler`（QThread）处理注册/登录/登出/续期/联系人/消息/密钥交换/群组管理请求（M6 新增 register_keys/fetch_keys；M7a 新增建群/邀请/退群/踢人/群信息五个处理器与群消息 fan-out；M7b 新增 fetch_group_keys 处理器，一次性返回群内所有成员 E2EE 密钥包；M9 新增会话偏好/消息编辑/消息删除三个处理器；M11 前置：发消息/搜索连接级限流 RateWindow + 结构化审计日志；**M8.1 新增文件控制面五个处理器**（申请上传/续传查询/宣告完成/取消/下载票据）与 `send_message` 的 `fileId` 校验），管理 session 路由与在线状态，访问 SQLite。**M8.1 新增 `storage/`**：`IObjectStorage` 抽象与 `LocalFileStorage` 实现（由 `Server` 创建并注入各 handler，存储初始化失败则不注入，文件相关接口一律 fail-closed 回 `FileStorageFailed`）；`Server` 维护连接新增 `pruneFileUploads`（与 `pruneSyncEvents` 共用定时器）。
- `CommonModule`：客户端和服务端共享代码：
  - `protocol/`：`Packet` / `PacketCodec` 长度前缀帧协议；`FileProtocol`（M8.1：`FileManifest` 编解码、分片数学与体积/分片/票据常量，客户端与服务端共用同一组常量以免校验口径漂移）；
  - `encryption/`：`EncryptionManager`（PBKDF2 慢哈希 + Token 生成）、`E2eeCrypto`（M6：X25519/HKDF/AES-256-GCM/envelope 编解码；M8.1 新增带 AAD 的 GCM 原语）、`GroupE2eeCrypto`（M7b：Sender-Key 生成/chain-key ratchet/群消息 AES-256-GCM + Ed25519 签名/分发消息 pairwise envelope 编解码；2026-09-09 新增跳序消息密钥缓存）、`FileCrypto`（M8.1：文件密钥/nonce 前缀生成、分片独立 AEAD 加解密、流式 SHA-256、票据与其摘要）；
  - `security/`：`TlsHelper`（证书生成/加载）、`LogSanitizer`（日志脱敏）、`SecureMemory`（敏感内存清零）、`StructuredLogger`（M11 前置：单行 JSON 结构化日志，统一字段 + 复用 LogSanitizer 脱敏）。
- 客户端还包含 `core/FileTransferManager`（M8.2：文件上传下载引擎，不持有 socket，经信号/回调与控制面协作）、`core/ThumbnailMaker`（M8.3a：图片尺寸与内联缩略图，纯 QtGui；M8.3b 抽出 `encodeThumbnail` 供视频封面复用）、`core/FileImageProvider`（M8.3c：应用内大图查看器图像源 `image://xyfile/<id>`）、`core/MediaMetadataExtractor`（M8.3b：音视频时长/分辨率/封面异步提取，QtMultimedia）、`core/DecryptingIODevice` 与 `core/MediaPlaybackManager`（M8.3b：密文缓存流式逐片解密播放，明文不落盘）。
- `docs`：路线图、协议、安全和架构说明。
- `tests`：Qt Test 单元测试（PacketCodec、EncryptionManager、DatabaseManager（含 M7a 群组数据层、V7-V10 迁移与 M8 文件元数据/票据/访问控制/回收）、Security（含 M11 前置 RateWindow 限流窗口与 StructuredLogger 结构化日志/脱敏）、LocalStore、GroupE2eeCrypto（M7b）、NetworkManager（2026-09-10：客户端链路层回归，friend 注入）、**FileProtocol 与 ObjectStorage（M8.1）、FileHttpService 与 FileTransfer（M8.2 集成测试，起真实 HTTP 回环 + 真实对象存储 + 内存 SQLite）、ThumbnailMaker（M8.3a）**，共 12 套均纳入 CTest）；`tests/e2e/TestGroupRepro` 为 M7b 双客户端群 E2EE 端到端复现工具（不纳入 CTest，需手动启动服务端）。

## 服务端运行模型

```text
ConnectionServer(主线程) ── socketAccepted ──> RequestHandler(QThread, 每连接一个)
     │                                              ├── QSslSocket（handler 线程内创建）
     │                                              ├── 发送代理 QObject（handler 线程亲和，M5.5）
     │                                              ├── DatabaseManager（每线程独立连接名）
     │                                              └── PacketCodec + 认证状态(内存)
     └── Server(主线程)：在线路由表 userId -> {sessionId -> handler} + 全局 NonceCache
            ├── onMessageForUser: 向目标用户所有在线 handler 转发数据包
            └── onSessionTerminated: 终止本人其他会话时断开对应连接
```

- 每连接一线程模型在低连接数下可行；连接数上升后成本高（ROADMAP 风险清单已记录）。
- 跨线程推送统一投递到 handler 线程内的发送代理对象（M5.5 修复：此前 `QMetaObject::invokeMethod(this)` 的 `this` 是主线程亲和的 QThread 对象，导致写 socket 发生在错误线程）。

## 当前能力边界

### 协议层（M1）

- 长度前缀帧协议（magic + version + messageType + requestId + payloadLength + payload）。
- 统一响应结构（code / message / data），所有请求通过 requestId 匹配。
- ping/pong 心跳与空闲超时（90 秒）。

### 账户体系（M2）

- 注册、登录、登出、token 续期、强制下线。
- 密码存储使用 PBKDF2-HMAC-SHA256（100,000 次迭代 + 16 字节随机盐）。
- 登录后签发 session token，服务端维护 `sessions` 表。
- 登录失败限流：同一 IP 5 分钟 10 次、同一用户 5 分钟 5 次。
- 会话终止仅限本人其他会话（`terminate_session`，M5.5），被终止连接由服务端主动断开。
- **限制**：断线重连必须重新登录。认证已加固（2026-09-02 P1）：每个已认证请求逐包携带 token，`validateSession()` 在连接级内存态之外逐请求回查 `sessions` 表并比对 token 哈希 + 校验过期（fail-closed，`token_renew` 豁免过期门），登出/终止/续期换代后即时失效；会话失效已闭环（2026-09-04）：客户端解析 `expiresAt` 并在过期前 1 天自动续期，续期被拒或业务请求返回 `SessionInvalid/SessionExpired` 时安全清零并回登录页提示重新登录。

### 即时通信（M3 + M5.5 加固）

- 用户搜索、联系人关系（双向）。
- 会话模型（conversations / conversation_members）。
- 消息表（messages），服务端递增 ID；送达/已读权威记录在 `message_receipts`（按接收者/设备维度，M5.5），`messages.status` 为回执聚合出的展示值。
- 服务端实现 send_message（`clientMessageId` 幂等去重）/ ack_message（先授权再写回执）/ sync_messages（先授权再查询）/ sync_events（账号级游标同步）。
- 客户端实现会话列表、聊天窗口、消息气泡、持久化 outbox（M6.5：加密落库，未确认消息重启后登录成功自动重发，幂等键保证不重复）。
- M6.5 本地缓存接入：登录后立即展示上一周期的缓存会话列表，打开会话先展示本地缓存再由服务端数据覆盖；登录后基于 `sync_events` 游标自动增量同步（hasMore 自动续拉），事件写入本地缓存并推进游标。
- M9 多端同步与离线一致性：`ack_message(read)` 触发已读者自身 `read_cursor` 事件 + `ReadCursorNotification` 推送，同账号其他设备经 `markConversationRead` 重算未读角标、推进消息已读态（状态只前进）；`sync_events` 按 30 天保留期每小时清理（`sync_meta` 水位线），落后于水位的设备由 `needsFullSync` 触发全量回退（重置游标 + `get_conversations`，历史消息经 `sync_messages` 从 messages 表补齐）。
- M4.5 客户端体验完善：搜索用户直接发起对话（虚拟会话 + 首条消息 ACK 后绑定 conversationId）、发送乐观显示（发送中→已发送→已送达→已读实时流转）、显式已读回执、日期分隔线、会话选中高亮与未读角标本地实时更新、侧边栏用户信息栏与登出入口、亮/暗主题切换（`Theme.qml` darkMode 驱动 + `ThemeSettings` QSettings 持久化）。
- 离线消息通过 sync_messages（afterId 游标）按会话增量同步；离线期间的消息/联系人/回执变更可经 sync_events 兜底补齐。
- 会话/消息接口全部先授权再查询（`isConversationMember()` / `canAccessMessage()`，M5.5）。
- M7a 群聊（明文，服务端与客户端均已落地）：服务端建群（创建者为 owner，初始成员去重/上限 200）/邀请（仅成员，已在群中拒绝）/退群（群主自动转让给最早入群成员）/踢人（层级保护：owner 可移除 admin/member，admin 仅可移除 member）/群信息查询（仅成员）；`send_message` 按 `conversationId`/`toUserId` 分流，群消息明文入库后逐成员在线直推（小群 fan-out）+ 全员 sync_events 兜底；成员变更产生 `contentType=system` 系统消息与 `GroupChangedNotification`/`group_changed` 事件；回执聚合改为按接收用户人数（多设备去重），`MessageStatusUpdate` 携带 `deliveredCount`/`readCount`；`get_conversations` 群会话携带 `name`/`memberCount`。客户端（子任务三）：`NetworkManager` 群组五接口 + 群消息 outbox 分流（明文直发不依赖 E2EE 引导，确定性错误移除待发项避免无限重试）；`LocalStore` 会话缓存群名/成员数（存量库幂等补列）；QML 建群（联系人多选）/群信息（成员列表/层级踢人/退群）/邀请（搜索多选）三个对话框，会话列表群样式与成员数标识，系统消息居中胶囊渲染。M7b 完成后群聊天区顶部“暂未端到端加密”横幅已改为群 E2EE 状态提示。
- **限制**：本地缓存仅供快速展示与离线查看，权威数据仍以服务端为准；群聊仅小群直推 fan-out（无大群拉取模式）；发消息/搜索限流已于 M11 前置实施（连接级 `RateWindow`：`send_message` 30/10s、`search_users` 20/60s，超限返回 `RateLimited (1003)`，客户端瞬时失败退避重刷不丢消息）；成员加入/退出的 Sender-Key healing 与失权回收已实现（2026-09-02 P1：成员变更触发本端轮换+重分发，离线经 sync_events 补偿），残留大群分发上限与“先落盘后分发”窗口（P2）。会话整表删除仍未实现（消息编辑/删除与置顶/免打扰已于 2026-09-05 实施，见 M9 小节）。

### 端到端加密（M6）

- 简化 Signal 方案：X25519 身份密钥（每设备长期）+ 一次性预密钥（客户端批量上传公钥，私钥留本地）+ 每消息临时密钥 ECDH + HKDF-SHA256 + AES-256-GCM。
- 发送链路：`sendMessage` → outbox → `fetch_keys`（服务端事务内逐设备认领预密钥）→ 逐设备加密为 envelope（另附发送方自身拷贝条目，仅身份密钥加密）→ `send_message`（服务端 fail-closed 校验后入库并同事务消费预密钥）；对方尚未注册密钥时保留 outbox 并每 30 秒重试，不丢弃。
- 接收链路：`NewMessageNotification` / `sync_messages` / `sync_events` 统一解密；按 `deviceId` 定位本机条目（自身拷贝用身份密钥解密，接收方条目逐本地预密钥试解密，GCM 标签验证），成功后删除该预密钥私钥；解密结果持久化缓存（DPAPI），重复投递/重新登录同步时由缓存兜底。
- 预密钥生命周期：认领后 10 分钟未消费自动回退；身份公钥变更时旧世代全部废弃；`fetch_keys` 连接级限流（60s/20 次）。
- 私钥存储：`KeyStorage`（AppData/e2ee，Windows DPAPI 保护，临时文件+替换原子写入）；TOFU 指纹存于 `e2ee/trust.json`，变更时 `peerIdentityChanged` 告警；解密缓存自 M6.5 起归口本地加密库 `LocalStore`（遗留 `e2ee/<account>_<device>.cache` 首次登录时自动迁入并删除，本地库不可用时回退旧文件兼容路径）。
- 产品取舍：历史消息不可恢复仅限真正丢失密钥材料的场景（更换设备/清数据）；同一设备登出重登由自身拷贝 + 解密缓存兜底；M6 前存量明文保持可读。
- **限制**：仅一对一文本消息；TOFU 无带外验证；无密钥备份/设备间迁移。

### 群端到端加密（M7b）

- 简化 Signal Sender-Key 方案：每个发送方在每个群中独立生成一个 `SenderKey`，包含 32 字节 chain key 与 Ed25519 签名密钥对（`publicSigningKey`/`privateSigningKey`）。chain key 通过 HKDF-SHA256  ratchet 派生消息密钥（`salt="xychat-grp-chain"`），消息密钥与当前迭代次数绑定。
- 发送链路：发送群消息前 `NetworkManager` 调用 `ensureGroupSenderKey()` 生成或加载本群 sender-key；`GroupE2eeCrypto::encryptMessage()` 用当前 chain key 派生 AES-256-GCM 消息密钥，加密明文后对 `iv || ciphertext` 做 Ed25519 签名；`encodeGroupMessage()` 将密文、IV、keyId、iteration、`senderDeviceId` 编码为 JSON envelope，经 `send_message` 以 `conversationId` 为目标发送。发送后 chain key 前 ratchet 到新值并持久化。
- 分发链路：首次发送或 chain key 不存在时，`NetworkManager` 通过 `FetchGroupKeysRequest`（协议类型 71）获取群内所有成员 E2EE 密钥包，再为每个成员设备生成 pairwise envelope：用 M6 X25519 身份密钥/预密钥 ECDH 协商对称密钥，加密 base64 编码的 chain key；`encodeDistribution()` 将所有 pairwise 条目与发送方公钥打包为 `contentType=sender_key_distribution` 的群消息发送。接收方 `processGroupSenderKeyDistribution()` 用本机私钥解密对应条目（遍历所有属于本机的条目逐个尝试，预密钥条目失败时自身拷贝条目 `prekeyId=0` 兜底），base64 解码后得到 32 字节 chain key，连同公钥一起存入 `LocalStore::saveSenderKey()`。服务端 `processSendGroupMessage` 对 `sender_key_distribution` 入库后消费其引用的预密钥（`claimed→used`，与单聊一致），杜绝 `claimed` 超时回收后被 `fetch_group_keys` 重复 claim 导致接收方一次性预密钥已删而轮换分发永久不可解（2026-09-03 修复）。
- 接收链路：`decryptGroupMessageObject()` 解析群消息 envelope 得到 `senderDeviceId`、keyId、iteration；从 `LocalStore` 加载对应发送方的 chain key 与公钥；`GroupE2eeCrypto::decryptMessage()` 用 chain key ratchet 到消息迭代派生消息密钥，AES-256-GCM 解密并验证 Ed25519 签名；解密成功后将更新后的 chain key/iteration 写回 `LocalStore`。解密失败时 content 清空并标记 `undecryptable`，禁止 envelope 原文入库。
- 持久化：`LocalStore` 新增 `sender_keys` 表，字段包括 `group_id`、`sender_user_id`、`sender_device_id`、`key_id`、`chain_key_enc`、`public_signing_key`、`private_signing_key_enc`、`iteration`、`updated_at`，chain key 与签名私钥经存储密钥 AES-256-GCM 加密后落库（`_enc` 后缀列为密文）。登出时保留 sender-key 材料（与 M6 解密缓存策略一致），避免重登后无法解密或签名。
- 安全属性：服务端数据库中群消息正文为密文；篡改、错误 chain key、错误签名均导致解密失败；一对一 E2EE 与群 E2EE 使用独立密钥路径，互不影响。
- DoS 防护与服务端 fail-closed（2026-09-02 安全审查修复）：单次解密 ratchet 跳跃上限 `MaxRatchetSteps = 2000`、envelope `iteration` 绝对上界 `MaxMessageIteration = 1e8`，恶意超大 iteration 在触发 HKDF 运算前即被拒绝；服务端 `processSendGroupMessage` 对 `e2ee_group`（`decodeGroupMessage` 且 `senderDeviceId` 非空）与 `sender_key_distribution`（`decodeDistribution` 且条目非空、`groupId` 与会话一致）入库/fan-out 前强制校验，非法返回 `E2eeInvalidEnvelope (3008)`，无静默放行路径。
- **成员变更 healing（2026-09-02 P1）**：`handleGroupChangedNotification` 在 `member_added/removed/left` 时调用 `healGroupSenderKey` 轮换本端 sender key（新 `keyId`）并经 `fetch_group_keys` → `buildGroupSenderKeyDistribution` 重分发；离线期间的变更由 `ingestSyncEvents` 处理 `group_changed` 事件补偿；含同群去重、单发槽位队列（`m_healQueue`/`drainHealQueue`）、瞬时失败延迟重试，E2EE 就绪（`register_keys` 响应）后排空队列；退群响应清除本端该群 sender key（wipe + `removeSenderKeysForGroup`）。服务端 `MessageType::Error` 回包由 `handleErrorResponse` 清理在途槽位，避免 healing 队列卡死。
- **限制**：大群（成员设备数约 >60）单条 `sender_key_distribution` 可能超 16384 字符上限致分发失败；轮换“先落盘后分发”，分发永久失败时存在群解密不可用窗口；一次成员变更触发全员各自轮换（O(N²) 重分发，已加同群去重/节流）；群路径服务端仍兼容接受 `contentType=text` 明文（M7a 遗留形态，客户端已不产生）。上述登记为 ROADMAP 欠账（P2/P3）。

### 会话偏好与消息编辑/删除（M9）

- **会话偏好（置顶/免打扰）**：按成员×会话维度存储于 `conversation_members.pinned/muted`（本人多设备共享，非设备级）。客户端 `NetworkManager::setConversationPrefs` 发 `SetConversationPrefsRequest`（类型 81），服务端仅会话成员可设（越权 `PermissionDenied`）后写库，向本人所有在线设备推 `ConversationPrefsNotification`（83）并写 `conversation_prefs` 事件；`get_conversations` 回填 `pinned`/`muted`。本地 `LocalStore.setConversationPrefs` 更新缓存，`loadConversations` 按 `pinned DESC` 排序置顶会话在前。
- **消息编辑**：仅发送者可编辑、已删除与系统消息不可编辑、编辑正文 `contentType` 须与原消息一致（私聊 `text`、群 `e2ee_group`，拒绝借编辑切换形态）。服务端 fail-closed 密文校验（`e2ee_group` 经 `decodeGroupMessage` 且 `senderDeviceId` 为当前设备、`text` 经 `decodeEnvelope`）拒绝明文注入，写库 `messages.edited_at`；向会话成员推 `message_edited` 事件与专用推送 `MessageEditedNotification (88)`（2026-09-10 前曾复用 `EditMessageResponse` messageType 靠 `requestId==0` 区分响应与推送，现已拆分），事件/推送携带 `senderId`（群聊解密寻址所需）与 `originDeviceId`（发起设备去重），且**不排除操作者本人**以保障其名下其他设备实时一致。客户端群聊编辑同步 Sender-Key 重加密、私聊编辑经 `m_privateEditQueue` 队列串行消费 `fetch_keys` 传输槽后 `encryptForUser` 重加密提交；解密侧**不预先清缓存**，直接解新密文，成功则覆盖本地明文与解密缓存，失败则保留既有可读正文与缓存、仅推进 `edited_at`（2026-09-10 幂等回退：预密钥一次性/群 ratchet 已推进，离线重放时新密文无法二次解密，不得写空覆盖）。
- **群聊编辑的解密寻址与乱序容忍（2026-09-09 修复）**：`decryptGroupMessageObject` 在 `senderId` 缺失时按（群, 设备, keyId）反查发送者（兼容修复前已落库的旧事件）；`GroupE2eeCrypto::decryptMessage` 接受可选跳序消息密钥缓存（Signal skipped-message-keys），使编辑抬高 `iteration` 后按 `message_id ASC` 补收的后续消息不被回滚检查永久拒绝；缓存经 `LocalStore.sender_key_skipped` 表加密持久化，退群与 `sender_keys` 同一事务清理。
- **消息删除**：仅发送者可删、系统消息不可删；软删除（`messages.deleted=1` + 正文清空）留墓碑，幂等（重复删除返回成功）；写入失败 fail-closed（回 `InternalError`、不广播事件、记 `message.delete_failed` 日志）。向会话成员推 `message_deleted` 事件与专用推送 `MessageDeletedNotification (89)`（语义同 88）。客户端 `markMessageDeleted` 置本地占位“已删除”。
- **并发响应匹配（2026-09-10）**：编辑/删除的在途请求由单发槽位改为多槽 `m_pendingEdits`（requestId→上下文）+ `m_pendingDeleteRequestIds` 集合，连续操作不再静默丢弃；登出（`resetAuthState`）与断线（`onDisconnected`，自动重连不经前者）两路径均清空容器并复位 `m_editFetchInFlight`，否则在途标记恒真会永久堵死编辑泵。
- **多端与离线同步**：`conversation_prefs`/`message_edited`/`message_deleted` 三类 `sync_events` 事件 + 实时推送双通道，`ingestSyncEvents` 解密后更新本地缓存并通知 UI（编辑事件同样先失效旧缓存再解密）。
- **UI**：`ConversationList` 会话右键菜单（置顶/取消置顶、免打扰/取消免打扰）+ 置顶/免打扰角标；`MessageBubble` 消息右键菜单（编辑/删除，仅自己消息）+ “已编辑”标记与“已删除”占位；`MainPage` 编辑对话框与删除确认对话框；`MainWindow` 接线 `setConversationPrefs`/`editMessage`/`deleteMessage` 与五个新信号。

### 客户端本地加密持久化缓存（M6.5）

- `LocalStore`（AppData/localstore，SQLite，按账号+设备隔离）：会话/消息/持久化 outbox/解密缓存/sync_events 游标；消息正文与会话预览以 AES-256-GCM 加密后落库（格式 `enc1:<iv>:<密文+标签>`），磁盘上不存在可读明文；M7a 起会话缓存额外携带群名/成员数，持久化 outbox 支持群消息目标（conversationId）。
- 存储密钥：每账号+设备随机生成 32 字节密钥，经 `KeyStorage` DPAPI 保护（`localstore/<account>_<device>.key`）；密钥无法持久化时 fail-closed 禁用缓存；密钥文件存在但 DPAPI 还原失败时拒绝启用（绝不用新密钥覆盖导致旧密文永久不可解）。
- 写入路径：发送确认（含正文）、`sync_messages`/`NewMessageNotification`/`sync_events` 解密后入库、`MessageStatusUpdate` 与回执事件更新状态（状态只前进不回退，`status_rank` 比较）；已解密正文同步写入解密缓存表，供后续 envelope 重复投递命中。
- 展示路径：登录后立即 emit 缓存会话列表（服务端响应到达后刷新，预览为占位符时先从解密缓存回填真实明文）；`syncMessages` 首页拉取先 emit 本地缓存再由服务端**解密后的**消息覆盖；`NewMessageNotification`/`sync_messages`/`sync_events` 三入口统一先解密再交 UI，解密失败标记 `undecryptable` 显示占位符，绝不把 envelope 原文当正文（2026-09-03 修复 `sync_messages` 曾 emit 未解密数组的缺陷）。
- 生命周期：登出时 `clearUserData()` 清除用户可见数据（消息/会话/outbox/同步游标）；**解密缓存与存储密钥作为 E2EE 密钥材料保留**——一次性预密钥消费后不可恢复，登出重登必须依靠解密缓存兜底（与 M6 产品承诺一致）；E2EE 身份密钥同样由 `KeyStorage` 保留复用；切换账号同样只清用户数据不毁密钥材料；`closeAndDestroy()`（删库+删密钥）仅保留给彻底销毁场景。
- **限制**：本地缓存为展示层缓存，不提供离线发送以外的完整离线能力；联系人列表仍按需从服务端拉取。

### 传输层安全（M5 + M5.5 fail-closed）

- 服务端 `QSslSocket` + TLS 1.2+，开发环境自签 CA（`certs/` 脚本生成）；初始化失败拒绝启动（`--allow-plaintext` 显式开发开关）。
- 客户端校验服务端证书，证书错误时断开；CA 缺失拒绝连接（`XYCHAT_ALLOW_PLAINTEXT=1` 显式开发开关）。
- 业务请求强制携带 timestamp/nonce（缺失/格式错误/超时/重复一律拒绝），nonce 由服务端全局 TTL 缓存（`NonceCache`）跨连接去重。
- 日志脱敏（`LogSanitizer`）；敏感内存清零（`SecureMemory`）；结构化日志（M11 前置 `StructuredLogger`：单行 JSON 统一 ts/level/event/requestId/userId/deviceId/code/durationMs/ip 字段，`sendResponse` 中央审计 + 安全事件带 reason，敏感字段脱敏）。
- **限制**：nonce 缓存与限流窗口均为单服务器/单连接内存态（重启清空、多实例不共享）；文件传输的控制面、数据面与客户端引擎均已落地（M8.1/M8.2），但上传 hashing 与保存解密在 GUI 线程同步执行、下载票据 TTL 对大文件不足且无断点续传（见 ROADMAP §3）。

### 文件与对象存储（M8.1，2026-09-10）

```text
客户端                       Chat-Server                      IObjectStorage
  │ ① file_upload_create ─────> RequestHandler ── allocateBlobKey ──> 分配存储键
  │ <── fileId + 上传票据 ────  createFileRecord（配额原子校验）+ issueFileTicket
  │ ② PUT 分片 ───────────> 数据面 HTTP(S)（待实施）── putChunk ──> parts/<b0b1>/<key>/<i>.part
  │ ③ file_upload_query ────> receivedChunks（断点续传：只补传缺的片）
  │ ④ file_upload_complete ─> finalize（流式组装 + 逐片长度 + 整体 SHA-256）
  │                            └─ 通过才 markFileReady → blobs/<b0b1>/<key>.bin
  │ ⑤ send_message(fileId) ─> checkMessageFile（存在/本人/ready）→ messages.file_id
  │ ⑥ file_download_ticket ─> canUserAccessFile → 签发一次性短时效票据
  │ ⑦ GET/Range ──────────> 数据面（待实施）── readRange
```

- **职责分层**：`RequestHandler` 只做鉴权、入参校验、限流与元数据；字节流全部经 `IObjectStorage` 抽象（`allocateBlobKey`/`putChunk`/`receivedChunks`/`readChunk`/`finalize`/`isFinalized`/`blobSize`/`readRange`/`remove`），M8 以本地文件系统实现（`LocalFileStorage`），后续可替换为 S3/MinIO 而不改动业务代码。
- **隐私边界**：服务端只见密文与密文侧元数据（体积/分片参数/整体 SHA-256/上传者）；文件名、MIME、明文大小、多媒体尺寸/时长与文件密钥只在 `FileManifest` 中，随消息正文经既有 E2EE（私聊 envelope / 群聊 Sender-Key）分发。收发双方以 `messages.file_id > 0` 判别文件消息，不靠正文内容猜测。发送时文件需满足存在/本人上传/已 `ready`，且该状态在插入语句内原子复核。
- **分片加密**：每文件一把独立 AES-256 密钥 + 12 字节 nonce 前缀；第 i 片 nonce = `iv` 后 4 字节 XOR 大端 `i`、AAD = 大端 `i`，各片独立认证且绑定位置（重排/截断/冒替均被拒），因此可流式、可续传、内存恒定。**刻意不复用消息 ratchet**（避开 M9 编辑踩过的“链已推进→早先分片永久不可解”不可逆损坏）。
- **目录布局与崩溃安全**：`<root>/parts/<b0b1>/<blobKey>/<index>.part`（上传中分片）、`<root>/tmp/<b0b1>/<blobKey>.<rand>.tmp`（组装中间产物）、`<root>/blobs/<b0b1>/<key>.bin`（最终对象）；blobKey 前两位十六进制作分桶，把单目录项数摊平到 256 个桶。所有写入为“临时文件 + 原子改名”，崩溃或写满不会留下被当作完整数据的半截文件；用户可控字符串永不参与路径拼接。
- **并发**：存储实例由各连接线程共享，同一 blobKey 的 `finalize`/`remove` 经固定条带锁（64 条）串行（同时组装会交错写入产出损坏对象；一边组装一边删除会产出“元数据 ready 而对象缺失”的不可自愈状态）；`putChunk` 不入锁（单片写入已原子，与组装交叠只会让 finalize 的长度/摘要关卡判失败）。
- **回收**：`Server::pruneFileUploads` 三轮（超期未完成上传 → 终态行收尾 → 已就绪但无引用的行原子迁入终态），均遵循“先保证不产生孤儿数据、再销毁”；详见 `docs/PROTOCOL.md` M8 章节与 `docs/SECURITY.md`。
- **回收与发送的竞态防护（两侧）**：“终态行不可能再被引用”并不成立——每连接一个线程，发送侧的文件校验与消息写入之间存在窗口，维护任务可在其中把无引用的 `ready` 文件迁入终态。因此发送侧把“文件仍为 `ready`”下推为 `INSERT` 的守卫子查询（`sendMessage` 单语句原子，SQLite 写者串行，守卫未命中则不写入并回 `FileNotReady`），回收侧终态那一轮在删盘前再判一次引用（宁可留下可修复的 `cancelled` 行 + 盘上对象，也不销毁仍被引用的数据）。
- **已知限制**：视频播放无动态画面（QML VideoOutput 无法绑定 C++ QVideoSink，只输出音频轨 + 静态封面，需自定义 QSGNode）；音视频元数据提取与播放依赖平台解码后端（Windows Media Foundation），CI/无头不可验证。

### 图片元数据与内联缩略图（M8.3a，2026-09-11）

- `Chat-Client/core/ThumbnailMaker`：**纯 QtGui**（`QImageReader` 读尺寸 + 缩放解码，`QImage` 编码 JPEG），不依赖平台多媒体后端，因此在无头环境与 CI 中可稳定验证（音视频时长/封面需 Media Foundation 等后端，CI 不可验证，故拆为 M8.3b）。最长边 160px，逐步降质量 70/55/40/25/15，质量到底仍超限再折半降尺寸（下限 32px）；**压不进 `MaxThumbnailBytes`（4096）就不内联**（绝不放宽上限，否则清单撑破群消息正文长度会使整条文件消息被拒收）。`setAutoTransform` 校正 EXIF 方向并据此修正上报宽高；`setScaledSize` 先缩放再解码，避免把大图完整读进内存。
- 接入路径：`FileTransferManager::uploadAndSend` 提取并写入清单 `width`/`height`/`thumb`（失败留空，**元数据缺失不阻断发送**）→ `NetworkManager::attachFileInfo` 补脱敏字段 `fileWidth`/`fileHeight`/`fileThumb`（base64 JPEG，不含密钥）→ `ChatView` 角色透传 → `MessageBubble` 以 `data:image/jpeg;base64,` 渲染（仅 `status === Image.Ready` 时显示，解码失败不留空白）。
- 安全口径：缩略图是 JPEG **明文**字节（清单整体已经 E2EE），因不含密钥而可进 QML，使接收方**在下载原图之前**就能预览（零流量）；服务端仍全程不可见。

### 音视频元数据与应用内播放（M8.3b/c，2026-09-11）

- `Chat-Client/core/MediaMetadataExtractor`：QtMultimedia（`QMediaPlayer` + `QVideoSink`）异步提取音频时长、视频时长/分辨率与封面帧（seek 到 min(1000,duration/2)ms 抓 `QVideoFrame::toImage` → `ThumbnailMaker::encodeThumbnail` 压缩）；5 秒超时护栏，失败/超时留空。依赖平台解码后端（Windows Media Foundation），CI 不可验证。
- `uploadAndSend` 异步三阶段（extracting → hashing → creating）：图片同步提取，音视频走 `extracting` 阶段异步提取（同一时间一个，其余排队 `m_pendingExtractTokens`），完成后 `beginHashing` 回调继续；元数据缺失绝不阻断发送。
- 应用内查看/播放（**明文不落盘**）：`FileImageProvider`（`image://xyfile/<id>`，图片大图）与 `MediaPlaybackManager` + `DecryptingIODevice`（音视频，`QMediaPlayer::setSourceDevice` 流式逐片解密，支持 seek 拖动进度）。`DecryptingIODevice` 按 plainPos 定位分片解密，明文只在内存；`MediaPlaybackManager` 经 context property `mediaPlayer` 暴露给 QML 播放器对话框。
- 历史图片缩略图补齐：`localThumbnailForMessage` 在清单 thumb 为空且原图就绪时本地生成缩略图缓存到 `<cacheRoot>/thumbs/`，`attachFileInfo` 回填 `fileThumb`，`clearCache` 递归清理。
- 已知限制：视频播放无动态画面（QML VideoOutput 无法绑定 C++ QVideoSink，只输出音频轨 + 静态封面）。

### 文件数据面与客户端传输引擎（M8.2，2026-09-11）

```text
发送方                                              接收方
FileTransferManager                                FileTransferManager
  │ ① 第一遍流式加密：算密文整体 SHA-256          │
  │ ② file_upload_create ─(TCP)─> RequestHandler   │
  │ ③ 逐片加密 PUT ─(HTTP)─> FileHttpService        │
  │ ④ file_upload_complete ─> finalize + ready      │
  │ ⑤ 清单作正文经 E2EE 发送（带 fileId） ───────> ⑥ 解密得清单
  │                                                 │    → attachFileInfo 登记（密钥留 C++）
  │                                                 │    → sanitizeForUi 置空正文后给 QML
  │                                                 ⑦ file_download_ticket ─> 票据
  │                                                 ⑧ 逐片 Range GET → 临时密文
  │                                                 ⑨ 整体 SHA-256 自校验 → 原子改名进缓存
  │                                                 ⑩ “另存为”时逐片解密写明文
```

- **职责划分**：`NetworkManager` 只负责 TCP 控制面（五个请求/响应 + `requestId`→`seq` 映射）与清单登记/脱敏；`FileTransferManager` 只负责 HTTP 数据面与分片加解密/缓存，**不持有 socket**（经信号请求控制面、经回调接收结果），因此可脱离网络单测。引擎经 `main.cpp` 注册为 QML context property `fileTransfer`。
- **隐私边界（关键）**：清单含 32 字节文件密钥，**只在 C++ 侧流转**。所有通向 QML 的消息经唯一脱敏出口 `sanitizeForUi`（四条路径：实时推送、`sync_messages` 历史/离线补收、`sync_events`、本地缓存回填），正文置空、只给脱敏展示字段；另有“形态像清单就置空”兜底，使将来新增出口不会重蹈覆辙（M8.2 P0 教训，由 `TestNetworkManager::fileManifestNeverReachesUiLayer` 锁定）。
- **本地缓存**：密文原样落盘（`<AppData>/XYChat/filecache/<sha256[0..1]>/<sha256>`，无后缀），零额外加密开销且磁盘上不是明文；明文只在用户“另存为”时写出。缓存命中判定只看“存在且字节数相符”，内容完整性由入库前的整体 SHA-256 与解密时的逐片 GCM 认证两道关卡保证（损坏则删缓存并回退到可重下状态，一次性自愈）。
- **串行调度与重入护栏**：一次只跑一个分片（避免带宽争抢、内存峰值与服务端 per-IP 限流）；`pumpNext` 先取 token 快照再遍历（循环体内可能同步 `failTask` → `erase` 当前节点）+ `m_pumping` 防嵌套；`reset()` 用 `m_resetting` 护栏并显式复位在途 reply（`abort()` 会同步触发回调）；`finishTask`/`failTask` 先取 `token` 副本再 `erase`（否则 `emit` 时读已释放内存）。
- **重试与恢复**：分片失败先问控制面“服务端实际收了哪些片”再决定跳过/重传（处理“响应丢失但数据已落盘”）；重试预算双层（分片 3 次 + 总恢复轮次 5 轮），后者防止数据面持续 5xx 而控制面正常时“成功的查询”不断清零预算而形成活锁。

## 数据库 Schema（V10，M8.1 文件元数据迁移）

- `schema_version`：数据库迁移版本控制
- `users`：用户基础信息（username, email, phone, password_hash）
- `devices`：设备信息（device_id, device_name, platform, public_key）——`public_key` 仅为预留列，当前无已落地的 E2EE 公钥流程
- `sessions`：登录会话（token_hash, login_ip, expires_at, last_active_at）
- `login_audit`：登录审计日志（ip_address, success, failure_reason）
- `contacts`：联系人关系（双向记录）
- `conversations`：会话信息（type, updated_at；M7a 新增 `name` 群名列，private 会话为 NULL）
- `conversation_members`：会话成员（conversation_id, user_id, last_read_message_id，读游标只前进；M7a 新增 `role` 成员角色列，取值 owner/admin/member，存量行默认 member；M9 新增 `pinned`/`muted` 会话偏好列，按成员×会话维度）
- `messages`：消息主体（conversation_id, sender_id, content, status, created_at, client_message_id, sender_device_id；M9 新增 `edited_at`/`deleted`；M8.1 新增 `file_id` 引用 `files.id`，NULL 表示普通消息）——M6 起新消息正文为 E2EE envelope 密文，存量旧消息为明文；文件消息的正文为 `FileManifest` 的密文
- `message_receipts`（V4 新增）：送达/已读回执（message_id, user_id, device_id, delivered_at, read_at，UNIQUE(message_id, user_id, device_id)）
- `sync_events`（V4 新增）：账号级同步事件流（seq 全局自增, user_id, event_type, payload），索引 (user_id, seq)；event_type 含 message/contact_added/receipt/group_changed/read_cursor（M9）/conversation_prefs/message_edited/message_deleted（M9 特性栈）
- `sync_meta`（V8 新增，M9）：单行清理水位线（id=1, pruned_below_seq），记录已被 `pruneSyncEvents` 清理的最大 seq，供落后设备 `needsFullSync` 判定
- `device_identity_keys`（V5 新增）：设备身份公钥（user_id, device_id, identity_pub, UNIQUE(user_id, device_id)）——仅存公钥
- `prekeys`（V5 新增）：一次性预密钥公钥（user_id, device_id, pub, status: unused/claimed/used, claimed_at）——仅存公钥，认领超时回退靠 `claimed_at`（V6 迁移兼容补齐该列）
- `sender_keys`（M7b 新增，客户端 `LocalStore` 本地表）：群 Sender-Key 本地加密存储（group_id, sender_user_id, sender_device_id, key_id, chain_key_enc, public_signing_key, private_signing_key_enc, iteration, updated_at，主键 (group_id, sender_user_id, sender_device_id, key_id)）——chain key 与签名私钥经 `LocalStore` 存储密钥加密后落库，登出保留；“最新密钥”按 `rowid DESC` 选取
- `sender_key_skipped`（2026-09-09 新增，客户端 `LocalStore` 本地表）：跳序消息密钥缓存（与 `sender_keys` 同主键维度 + `skipped_keys_enc` 整体密文 blob）——属 E2EE 密钥材料：登出保留、退群与 `sender_keys` 同一事务清理
- `files`（V10 新增，M8.1）：文件密文侧元数据（blob_key UNIQUE, uploader_id, uploader_device_id, size_bytes, chunk_size, chunk_count, sha256_hex, status: uploading/ready/cancelled/failed, created_at, completed_at）；索引 `idx_files_uploader_status`（配额计数）与 `idx_files_status_created`（回收扫描）。不存文件名/MIME 等敏感元数据（只在客户端清单里）
- `file_tickets`（V10 新增，M8.1）：上传/下载票据（ticket_hash UNIQUE, file_id, user_id, kind: upload/download, used, expires_at, created_at）——**无票据明文列**，只存 SHA-256 摘要（与 `sessions.token_hash` 同一做法）；索引 `idx_file_tickets_expires` 供过期清理

V10 还为 `messages.file_id` 建 `idx_messages_file`，使“文件是否被未删除消息引用”的回收判定不全表扫描。

messages 表幂等唯一约束：`UNIQUE(sender_id, sender_device_id, client_message_id)`（部分索引，仅对非空幂等键生效，存量旧数据不受影响）。

## 客户端架构（M4 已落地，M4.5 完善）

```text
Chat-Client
  ├── QML UI 层（resources/）
  │     ├── main.qml（登录窗口根，objectName=loginRoot）
  │     ├── pages/（LoginPage.qml, MainPage.qml, MainWindow.qml 主窗口根，objectName=mainWindow）
  │     ├── components/（TitleBar, ConversationList, ChatView, MessageInput, MessageBubble, QWKButton）
  │     └── theme/（Theme.qml 单例，darkMode 驱动亮/暗双配色，qmldir 注册）
  ├── C++ 后端层
  │     ├── core/NetworkManager（连接状态机 + TLS + 协议，注册为 QML 上下文对象；sendMessage 返回 clientMessageId 供乐观消息跟踪；M6 起登录后自动引导 E2EE 密钥注册，发送前 fetch_keys 加密、接收后解密；M6.5 起接入 LocalStore 缓存与持久化 outbox；M7a 起提供群组五接口与 sendGroupMessage（outbox 分流，群消息明文直发）；M7b 起实现 ensureGroupSenderKey/buildGroupSenderKeyDistribution/encryptGroupMessage/decryptGroupMessageObject 等群 Sender-Key E2EE 接口与 FetchGroupKeys 协议交互；2026-09-04 起接入会话续期：解析 `expiresAt` 过期前自动 `renewToken`（60 秒看门狗 + 失败退避）、失效发 `sessionExpired` 回登录页；M9 起接入 setConversationPrefs/editMessage/deleteMessage 与 conversationPrefsChanged/messageEdited/messageDeleted 信号）
  │     ├── core/KeyStorage（M6：身份/预密钥私钥持久化，Windows DPAPI 保护；TOFU 指纹存储；M6.5：LocalStore 存储密钥）
  │     ├── core/LocalStore（M6.5：按账号+设备隔离的 SQLite 加密本地缓存，M7a 含群会话字段，M7b 新增 sender_keys 表保存 chain key 与 Ed25519 签名密钥对，M9 新增会话 pinned/muted 与消息 edited_at/deleted 及 updateMessageContent/markMessageDeleted/clearDecryptedContent，见上文）
  │     ├── core/ThemeSettings（QSettings 主题持久化，注册为 QML 上下文对象）
  │     └── models/User
  └── QWindowKit（QWK::Quick WindowAgent：无边框、拖拽、Snap Layout；标题栏自定义按钮需 setHitTestVisible 注册）
```

窗口组织（M4.5 调整）：

- `main.cpp` 依次 `engine.load()` 加载 `main.qml`（登录窗口）与 `pages/MainWindow.qml`（主窗口），两者均为独立根窗口；主窗口按 `objectName` 查找后注入登录窗口的 `mainWindow` 属性。**不能把主窗口声明在登录窗口 QML 内部**，否则会成为 transient 子窗口而不在 Windows 任务栏显示。
- 窗口流转：启动→登录窗口→（登录成功）隐藏登录窗口并显示主窗口；登出→隐藏主窗口并重新显示登录窗口；关闭主窗口退出应用，主窗口打开时关闭登录窗口仅隐藏。
- 主题：`Theme.qml` 全部颜色属性为 `darkMode ? 暗色 : 亮色` 绑定表达式，`main.qml` 用 `Binding` 将 `Theme.darkMode` 绑定到 `themeSettings.darkMode`，标题栏切换按钮写入 `themeSettings` 即全局生效并持久化。
- 聊天区：`ChatView` 消息列表直接用 `ListView`（不用外层 ScrollView 包 `height: contentHeight` 的 ListView，否则不可滚动）；自动贴底由 50ms Timer + `stayAtBottom`/`programmaticScroll` 标志实现（用户手动上滚时暂停贴底）。

M7a 群聊 UI（子任务三新增）：

- `ConversationList` 侧边栏新增建群按钮；会话模型携带 type/name/memberCount，群会话显示群名、成员数与圆角方形头像。
- `MainPage` 新增三个对话框：建群（群名 + 联系人多选，打开时拉取联系人）、群信息（成员列表/角色/层级踢人/邀请入口/退群，由 get_group_info 响应驱动）、邀请（搜索用户多选，搜索结果按 searchMode 路由）。
- `ChatView` 群会话顶部显示群 E2EE 状态横幅（当前文案：“群聊消息已启用端到端加密（Sender Keys），服务端仅存储密文”；高度随可见性折叠，避免私聊下锚点链残留空隙），系统消息（contentType=system）以居中胶囊渲染，发送按会话类型分流（群聊走 sendGroupMessage，M7b 起走群 E2EE 加密路径）。

与旧文档的差异说明：

- 亮/暗主题切换已于 M4.5 实现（单一 `Theme.qml` 双配色 + `ThemeSettings` 持久化），不再需要独立的 `DarkTheme.qml`/`LightTheme.qml`。

- 客户端自 M6.5 起具备本地数据库（`LocalStore`，仅作加密展示缓存）；`models/User` 仍是登录态数据对象，未引入独立模型层。

## 架构问题修复状态（2026-08-03 审查 → M5.5 修复）

| 级别 | 问题 | 状态与落地方式 |
| --- | --- | --- |
| P0 | 会话/消息接口缺成员授权 | ✅ 已修复：新增 `isConversationMember()` / `canAccessMessage()`，sync_messages / ack_message 先授权再查询，越权返回 `PermissionDenied` |
| P0 | `force_logout` 接受任意 `userId`，构成越权注销 | ✅ 已修复：改为 `terminate_session`，仅允许终止本人其他会话（指定他人 userId 被拒绝），被终止连接由服务端断开 |
| P0 | TLS 可静默降级 | ✅ 已修复：fail-closed（服务端拒启 / 客户端拒连），开发明文改为显式开关（`--allow-plaintext` / `XYCHAT_ALLOW_PLAINTEXT=1`） |
| P0 | timestamp/nonce 非必填，可整体绕过 | ✅ 已修复：强制必填 + 格式校验，拒绝返回 `ReplayRejected`；nonce 由服务端全局 `NonceCache`（TTL）跨连接去重 |
| P1 | 认证依赖 handler 内存状态，token 语义不完整 | ✅ 已修复（2026-09-02）：每个已认证请求逐包携带并校验 token，`validateSession()` 回查 `sessions` 表 + 过期 fail-closed（`token_renew` 豁免过期门）；TLS channel 绑定为可选后续加固 |
| P1 | `sendRawData` 排队写存在线程风险 | ✅ 已修复：发送投递到 handler 线程内的发送代理 QObject，socket 只在其所属线程被访问 |
| P1 | 消息无客户端幂等键，无 outbox | ✅ 已修复：`clientMessageId` + 部分唯一索引去重；客户端内存 outbox 登录成功后自动重发（本地持久化 outbox 随本地缓存一并补齐） |
| P1 | 单值 `messages.status` 无法多设备聚合 | ✅ 已修复：`message_receipts` 按接收者/设备记录，`messages.status` 改为回执聚合展示值 |
| P1 | `sync_messages` 单会话拉取 | ✅ 已补充：新增 `sync_events` 账号级游标同步（消息/联系人/回执）；sync_messages 保留为会话内历史分页 |

剩余已知问题（非阻塞，完整清单见 ROADMAP 欠账节）：nonce 去重为单服务器内存缓存（多服务器部署需持久化）；服务端每连接一线程模型在高连接数下成本高；群路径仍兼容 `text` 明文；大群 `sender_key_distribution` 超 16384 上限、轮换“先落盘后分发”窗口、healing O(N²) 重分发（P2/P3）。

## M6 代码审查修复记录（2026-08-17）

M6 首次实现后经代码审查发现并修复：

| 级别 | 问题 | 修复方式 |
| --- | --- | --- |
| P0 | claimed 预密钥无释放机制 + fetch_keys 无限流，可被耗尽且不自愈 | 预密钥表新增 `claimed_at`，认领 10 分钟未消费自动回退 unused；fetch_keys 连接级频率限制（60s/20 次）；消息入库与预密钥消费同事务 |
| P0 | 身份密钥轮换后旧预密钥仍可被认领，导致消息静默丢失 | `upsertIdentityKey` 检测公钥变更时废弃该设备全部 unused/claimed 预密钥（附回归测试） |
| P1 | claimPrekeys 并发竞争整体回滚 + 客户端瞬时失败即永久删除待发项 | 单设备竞争失败改为跳过；SQLite busy timeout 5 秒；客户端仅对确定性错误（3007/3005/2003）删除 outbox，瞬时错误延迟重试 |
| P1 | 预密钥补齐仅看本地计数；身份密钥损坏时发送永久阻塞 | 补齐同时参考服务端 `remainingPrekeys`；损坏的身份密钥自动重新生成并丢弃旧预密钥 |
| P2 | KeyStorage 非原子写入；DPAPI 解密中间明文未清零 | 临时文件+替换写入；中间 blob 使用后经 SecureMemory 清零 |

## M6 运行期缺陷修复记录（2026-08-20）

双客户端同机联调发现并修复：

| 问题 | 根因 | 修复方式 |
| --- | --- | --- |
| 对方未上线时发送的加密消息永久丢失 | `fetch_keys` 返回 AccountNotFound/KeyBundleUnavailable 时客户端将消息从 outbox 删除，而对方尚未注册密钥属可恢复状态 | 保留 outbox，按目标用户 30 秒退避重试，对方首次登录注册密钥后自动送达；仅 CannotSendToSelf 才删除 |
| 登出重登后自己发出的消息无法解密 | envelope 只含对方设备条目，发送方本机无密文拷贝 | 发送时追加自身拷贝条目（`prekeyId=0`，仅身份密钥加密，不消费预密钥）；服务端校验放行发送方设备的该类条目（同机 deviceId 相同时与接收方条目分开去重） |
| 登出重登后对方发来的消息无法解密 | 一次性预密钥解密后即删除，内存解密缓存随登出清空 | 解密缓存按账号+设备持久化（DPAPI 保护，`e2ee/<account>_<device>.cache`），登录后加载；预密钥删除后重新同步由缓存兜底 |
| 中间版本数据库兼容 | 早期构建创建的 prekeys 表可能缺 `claimed_at` 列 | 新增 V6 迁移补齐该列 |

回归测试：新增 `selfCopyEnvelopeRoundTrip`（prekeyId=0 条目编解码与仅身份密钥加解密往返）；4 组测试套件全部通过。

## M6.5 本地持久化实施与审查修复记录（2026-08-21）

新增 `LocalStore` 与持久化 outbox，实现后经代码审查发现并修复：

| 级别 | 问题 | 修复方式 |
| --- | --- | --- |
| 警告 | 服务端会话列表的占位预览 `[Encrypted message]` 覆盖本地已解密预览，缓存预览被架空 | upsert 前用本地解密缓存按 `lastMessageId` 回填真实明文（UI 同步受益） |
| 警告 | 存储密钥不区分“文件缺失”与“DPAPI 还原失败”，瞬时失败会用新密钥覆盖致整库永久不可解 | 密钥文件存在但还原失败时拒绝启用缓存（fail-closed），仅文件缺失时生成新密钥 |
| 建议 | upsert 无条件覆盖 status，滞后同步可能回退已读状态 | 新增 `status_rank` 列，状态只前进不回退（附回归测试 `statusOnlyMovesForward`） |

另修复的构建期缺陷：`QJsonValue::toString()` 对缺失字段返回 null QString，Qt SQLite 驱动将其绑定为 SQL NULL 导致 NOT NULL 约束失败——统一规范化为非 null 空串。自动化测试：新增 TestLocalStore（落盘密文不可读、outbox 持久化、upsert 语义、遗留迁移、登出销毁等），5 组测试套件全部通过。

## M6.5 运行期缺陷修复记录（2026-08-21 联调）

双客户端互发消息后登出重登，对方消息无法解密且 UI 显示 envelope 密文原文。两个叠加缺陷：

| 问题 | 根因 | 修复方式 |
| --- | --- | --- |
| 登出重登后对方消息永久无法解密 | 登出时 `closeAndDestroy()` 整库销毁（含 decrypt_cache）并删存储密钥，而 M6 的“登出重登靠持久化解密缓存兜底”前提是缓存跨越登出存活；一次性预密钥已消费不可恢复 | 新增 `clearUserData()`：登出/切换账号只清用户可见数据（消息/会话/outbox/游标），解密缓存与存储密钥作为 E2EE 密钥材料保留 |
| envelope 密文伪装成正文显示 | 解密失败时 content 残留 envelope 原文（仅靠 undecryptable 标志），`upsertMessage` 将其当明文加密落库，缓存先行展示时又无标志 | `upsertMessage` 对 undecryptable 或 `looksLikeEnvelope` 的 content 一律清空并标记；新增 `healEnvelopeLeaks()` 打开时自愈历史污染行 |

回归测试：新增 `logoutClearsUserDataButKeepsDecryptCache`、`upsertNeverPersistsEnvelopeCiphertext`、`healsLegacyEnvelopeLeakRows`；另修复同类的空密文 null 绑定问题（content_enc/last_message_enc）。5 组测试套件全部通过。

## 架构调整依据

修复方向参考主流 IM 的公开技术方案：

- **Telegram**：MTProto 以 auth_key 绑定加密通道、`random_id` 幂等去重、`getDifference/getChannelDifference` 差分同步、`sessions.killSession` 设备管理。
- **WhatsApp**：per-recipient 送达/已读回执（蓝勾模型）、客户端消息 ID 去重。
- **Signal**：预密钥（pre-key）离线密钥协商、消息级 MAC 防篡改（M6 E2EE 参考）。

共同原则：先授权再查询（authorization-before-query）、fail-closed 的传输安全、幂等写 + 游标拉（idempotent write, cursor-based pull）、推送只做通知、数据靠增量同步兜底。本项目 M5.5 修复与后续 M9 同步模型均按这些原则设计。

## 下一步演进

M0-M7b、M9 与 M8.1/M8.2（文件与对象存储地基 + 数据面与客户端）已完成（明细见上文各节与 `docs/ROADMAP.md` §2 已完成能力摘要）。后续演进方向以 ROADMAP 为唯一权威来源：

- 候选任务与建议执行顺序见 `docs/ROADMAP.md` §5（M8.3 多媒体元数据与预览、M10 搜索/通知/体验、M11 稳定性与可运维）。
- 集中登记的欠账与风险见 `docs/ROADMAP.md` §3（P1/P2/P3 分级）。

本文档不再维护逐里程碑的演进流水账，新增架构实态变化时直接更新对应章节。
