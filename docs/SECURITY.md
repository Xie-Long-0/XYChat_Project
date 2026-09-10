# XYChat 安全文档

## 当前安全状态（M9 特性栈完成后，2026-09-05 对齐）

### 端到端加密（M6）

一对一文本消息采用简化 Signal 方案实现端到端加密，服务端只能转发密文，无法读取正文：

- **密钥体系**：每台设备生成 X25519 身份密钥对（长期）并批量上传 20 个一次性预密钥公钥（私钥永不离开客户端）；余量低于 5 时自动补齐。
- **每条消息独立密钥**：发送方为每设备生成临时 X25519 密钥对，`shared = ECDH(eph, prekey) ‖ ECDH(eph, identity)`，经 HKDF-SHA256（salt="xychat-e2ee-v1"）派生 AES-256-GCM 消息密钥（随机 12B IV）。临时密钥用后即弃，具备前向安全。
- **预密钥生命周期**：服务端认领即消费（`unused -> claimed -> used`）；消息入库与预密钥消费同事务；认领后 10 分钟未消费自动回退（防泄漏）；身份公钥变更时旧世代预密钥全部废弃；`fetch_keys` 有连接级频率限制（60s/20 次）防耗尽攻击。
- **fail-closed**：`send_message` 强制校验 envelope 格式与预密钥归属/状态（非法返回 3008），数据库中的消息正文均为密文。
- **私钥存储**：客户端密钥持久化于 `AppDataLocation/e2ee/<account>_<device>.key`，Windows 下经 DPAPI（CryptProtectData）按当前用户加密保护；写入采用临时文件+替换避免崩溃损坏；非 Windows 平台明文回退并告警。
- **发送方自身拷贝（2026-08-20 修复）**：每条消息额外附带一个仅用发送方身份密钥加密的本机拷贝（`prekeyId=0`，不消费预密钥），发送方登出重登/多端同步后仍能解密自己发出的消息。
- **解密缓存**：已解密消息的明文按 messageId 持久化于本地，一次性预密钥删除后重新同步时由缓存兜底；自 M6.5 起归口本地加密库 `LocalStore`（AES-256-GCM 加密落库，存储密钥 DPAPI 保护），遗留 `e2ee/<account>_<device>.cache` 首次登录自动迁入并删除；等价于本地消息存储，不削弱传输侧/服务端不可读的安全属性。
- **离线发送**：对方尚未注册密钥时消息保留在客户端 outbox 并定期重试，不丢弃；M6.5 起 outbox 加密持久化，发送方退出应用/重启后未送达消息不再丢失。
- **设备信任**：TOFU（首次信任），本地记录对方身份公钥 SHA-256 指纹，变更时通过 `peerIdentityChanged` 信号告警（不阻塞发送）；尚无安全码/二维码带外验证。
- **历史消息不可恢复**：仅限真正丢失密钥材料的场景（更换设备/清除应用数据）：新设备登录无法解密旧消息（无对应预密钥私钥），UI 显示“无法解密此消息”占位；同一设备登出重登由持久化解密缓存与自身拷贝兜底，不受影响。密钥备份/设备间迁移属后续里程碑。
- **内存安全**：私钥、共享密钥、DPAPI 解密出的明文 blob 使用后立即经 `SecureMemory` 清零。

### 本地存储安全（M6.5）

客户端本地持久化缓存（`LocalStore`，AppData/localstore，按账号+设备隔离的 SQLite）遵循与传输侧一致的最小暴露原则：

- **落盘必加密（fail-closed）**：消息正文、会话预览、outbox 正文与解密缓存均以 AES-256-GCM（随机 12B IV，认证标签防篡改）加密后写入（格式 `enc1:<iv>:<密文+标签>`）；加密失败时拒绝写入，磁盘上不存在可读的消息明文；解密失败返回空并标记 undecryptable，绝不回退原文。
- **存储密钥**：每账号+设备随机生成 32 字节密钥（OpenSSL RAND_bytes），经 `KeyStorage` DPAPI 保护（`localstore/<account>_<device>.key`）；密钥仅驻留进程内存，关闭时安全清零。密钥无法持久化时禁用缓存（宁可无缓存不落明文）；密钥文件存在但 DPAPI 还原失败时拒绝启用，绝不用新密钥覆盖导致旧密文永久不可解。
- **登出清除**：登出/切换账号时清除用户可见数据（消息/会话/outbox/同步游标）；**解密缓存与存储密钥作为 E2EE 密钥材料保留**——一次性预密钥消费后不可恢复，登出重登必须依靠解密缓存兜底（与 M6 产品承诺一致，等价于主流 IM 的本地密钥材料留存）；E2EE 身份密钥同样由 `KeyStorage` 保留复用。彻底销毁（删库+删密钥，旧密文不可再恢复）仅供显式销毁场景。
- **幂等防重**：持久化 outbox 重发沿用 `clientMessageId` 服务端幂等去重，重启/断线重连不产生重复消息。
- **密文不落库/不显示**：解密失败的 envelope 原文（私聊 `e2ee`、群聊 `group_e2ee` 与 `sender_key_distribution`）绝不作为正文写入本地库或渲染到 UI（统一清空并标记 undecryptable，UI 显示“无法解密”占位）；`upsertMessage` 落库拦截与 `healEnvelopeLeaks` 打开时自愈均覆盖上述三类 envelope，历史污染行自动检出并清空（2026-09-03 修复：此前群 envelope 未纳入拦截/自愈，且 `sync_messages` 曾 emit 未解密数组致密文当正文显示）。
- **边界**：本地缓存为展示层缓存，权威数据以服务端为准；拥有本机用户权限者可经 DPAPI 还原存储密钥进而读取缓存（与主流 IM 本地存储模型一致，不抵抗本机管理员）。

### 群聊端到端加密（M7b）

群消息采用简化 Signal Sender-Key 方案，服务端只见密文：

- **密钥体系**：每个发送方在每个群独立生成 SenderKey（32 字节 chain key + Ed25519 签名密钥对，`keyId` = SHA-256(签名公钥) hex 前 32 字符）；chain key 经 HKDF-SHA256 ratchet（salt `xychat-grp-chain`）逐条派生消息密钥，具备链式前向安全。
- **消息加密与认证**：AES-256-GCM（随机 12B IV）加密，发送方 Ed25519 私钥签名覆盖 `iv || ciphertext`，接收方验签失败/iteration 回滚/篡改均拒绝解密。
- **密钥分发**：chain key 复用 M6 pairwise E2EE（X25519 身份/预密钥）逐成员逐设备加密，以 `contentType=sender_key_distribution` 群消息投递；`fetch_group_keys` 仅限群成员且与 `fetch_keys` 共享连接级限流（60s/20 次），防预密钥池耗尽。分发经 `send_message` 入库后消费其引用的预密钥（`claimed→used`，与单聊 `processSendMessage` 一致），否则 `claimed` 预密钥 10 分钟超时回收为 `unused` 被重复 claim，而接收方首次解密已删除本地私钥，致轮换后的新分发永久不可解（2026-09-03 修复）。
- **DoS 防护**：单次解密 ratchet 跳跃上限 `MaxRatchetSteps = 2000`，envelope `iteration` 绝对上界 `MaxMessageIteration = 1e8`；恶意超大 iteration 在触发任何 HKDF 运算前即被拒绝（2026-09-02 安全审查修复）。
- **乱序容忍与跳序消息密钥缓存（2026-09-09）**：chain-key ratchet 单向不可逆，而“密文可被事后覆写”的消息编辑会使 `iteration` 与 `message_id` 顺序解耦：被编辑消息获得比其后发送消息更大的 `iteration`，而 `sync_messages` 按 `message_id ASC` 返回，接收端先解到高 `iteration` 后，后到的低 `iteration` 消息会被回滚检查**永久拒绝**（明文不可恢复）。为此 `GroupE2eeCrypto::decryptMessage` 接受可选的跳序消息密钥缓存（Signal skipped-message-keys 语义）：ratchet 跨越迭代时缓存途中派生的消息密钥，使乱序/在途消息仍可解密。安全约束：① 容量上限 `MaxSkippedMessageKeys = 1000`，超限丢弃 `iteration` 最小者（内存与落库均有界）；② 缓存条目**命中即删**（一次性消费），不削弱重放拒绝；③ **fail-closed 提交**——仅当 GCM 解密与 Ed25519 验签全部通过后才提交链状态与新缓存，伪造密文/签名既不推进 `iteration` 也不写入任何派生密钥；④ 命中缓存时不改动 `chainKey`/`iteration`。**前向安全权衡**：缓存保留了尚未接收消息的密钥，在这些消息到达前它们不具备前向安全性（与 Signal 一致的可接受折中）；密钥仅驻内存与本地密文库，经 `LocalStore.sender_key_skipped` 表以存储密钥 AES-256-GCM 加密落库（磁盘无可读密钥），退群随 `sender_keys` 一并清理。
- **服务端 fail-closed**：`e2ee_group` 与 `sender_key_distribution` 正文入库/fan-out 前强制 decode 校验（含 `senderDeviceId` 非空、条目非空、`groupId` 与会话一致），非法返回 `E2eeInvalidEnvelope (3008)`，无静默放行路径；群系统消息（`contentType=system`）仅含元数据不含用户正文，不加密。
- **本地存储**：接收方 chain key 与签名密钥对写入 `LocalStore.sender_keys` 表（存储密钥 AES-256-GCM 加密落库，DPAPI 保护）；登出作为 E2EE 密钥材料保留（与解密缓存一致，否则重登后无法解密/签名）。跳序消息密钥缓存写入 `sender_key_skipped` 表（整体密文 blob，与 `sender_keys` 同主键维度），同样属 E2EE 密钥材料：登出保留、退群清理。“最新密钥”按 `rowid DESC` 选取（`INSERT OR REPLACE` 每次写入取得更大 rowid）；旧实现按秒级 `updated_at` 排序并以随机 hex `key_id` 作并列破口，同秒写入两把密钥（轮换场景）时选中哪把完全随机，可能用陈旧密钥加密而接收方无法解密（2026-09-09 修复）。
- **成员变更 healing（2026-09-02 P1 修复）**：`member_added/removed/left` 群变更通知（及离线期间的 `sync_events` 补偿）触发本端 sender key 轮换（`generateSenderKey` 生成新 `keyId`）并向现任成员重分发；新成员因此获得当前密钥、被移除成员因密钥轮换失去后续消息的解密能力（后向安全）。轮换去重（同群在途/已排队不重复触发）、单发槽位队列化、瞬时失败（限流/超时）延迟重试；退群时清除本端该群 sender key（内存 wipe + `removeSenderKeysForGroup`）。
- **遗留限制**：大群（成员设备数约 >60）单条 `sender_key_distribution` 可能超 16384 字符上限致分发失败；轮换采用“先落盘后分发”，分发永久失败时存在群解密不可用窗口；群路径服务端仍兼容接受 `contentType=text` 明文（M7a 遗留形态，客户端已不产生，收紧为拒绝属后续选项）。上述均登记为 ROADMAP 欠账（P2/P3）。

### 消息编辑与删除安全（M9）

- **仅发送者可操作**：编辑/删除均校验 `messages.sender_id == 当前用户`，否则 `PermissionDenied`；系统消息（`contentType=system`）不可编辑/删除；已删除消息不可再编辑。
- **编辑 fail-closed 密文校验**：编辑正文必须与原消息 `contentType` 一致（私聊 `text`、群 `e2ee_group`），拒绝借编辑切换形态注入非法内容；`e2ee_group` 须通过 `GroupE2eeCrypto::decodeGroupMessage` 且 `senderDeviceId` 为当前设备、`text` 须通过 `E2eeCrypto::decodeEnvelope`——服务端只见密文，明文注入一律 `E2eeInvalidEnvelope`，与 `send_message` 的 envelope 强校验保持一致。
- **软删除留墓碑**：删除后 `messages.deleted=1` 且正文清空，messageId/发送者/时间保留供客户端渲染“已删除”占位；删除幂等（重复删除返回成功）。不物理删除消息行，审计可追溯。**写入 fail-closed（2026-09-09）**：`deleteMessage` 真实写入失败时返回 `InternalError` 且不广播事件（旧实现忽略返回值，会在库内状态未变的情况下向全员广播删除，造成服务端与事件流分歧），失败记 `message.delete_failed` 结构化日志。
- **事件寻址字段完整性（2026-09-09 修复的高危缺陷）**：群聊正文为 `e2ee_group` 密文，接收端必须凭（群, 发送者 userId, 发送者 deviceId, keyId）四元组定位 Sender Key；旧实现的 `message_edited` 事件与推送 payload **不带 `senderId`**（客户端甚至硬置 `senderId = 0`），而 `LocalStore::loadSenderKey` 对 `senderUserId <= 0` 直接 fail-closed → 群消息一旦被编辑，**所有接收端解密失败**；更严重的是解密失败前已执行 `m_decryptCache.remove` + `clearDecryptedContent`，随后以空正文回写 → 接收端**原本可读的正文被清成“无法解密”**。修复：服务端事件/推送补 `senderId`；客户端去除硬编码，并在 `senderId` 缺失时按（群, 设备, keyId）反查发送者（兼容修复前已落库的旧事件；keyId 为签名公钥指纹，全局唯一，同机双用户共用 deviceId 也不会误匹配）。**教训**：新增会改动已有密文的事件时，必须先确认 payload 携带解密所需的全部寻址字段。
- **解密缓存一致性（客户端）**：编辑/删除事件与响应处理时，先失效该 messageId 的旧解密缓存（内存 `m_decryptCache` + LocalStore `clearDecryptedContent`）再解密新密文或标记删除，避免编辑后仍显示编辑前明文；本端编辑以乐观明文落库并覆盖解密缓存。
- **多端与离线一致性**：`conversation_prefs`/`message_edited`/`message_deleted` 事件经 `sync_events` 与实时推送双通道投递，离线设备上线经 `ingestSyncEvents` 补偿；编辑正文仍为密文传输，服务端不接触明文。**推送覆盖操作者本人（2026-09-09）**：旧实现在服务端按 `memberId != 操作者` 排除整个用户，使操作者名下其他设备得不到实时推送（仅能等下次增量同步），与已读游标/会话偏好的推送策略不一致；现改为推送给全体成员，由客户端按 `originDeviceId` 去重（实时推送与 `sync_events` 补偿两路径均去重），既保障多端实时一致又避免发起设备回显自身操作（群聊下回显会用已推进的 ratchet 状态重试解密并误清正文）。
- **并发响应匹配完整性（M9 欠账修复，2026-09-10）**：旧客户端实现用单发槽位 `m_pendingEditMessageRequestId`/`m_pendingDeleteMessageRequestId` 只保存最后一个 requestId，连续编辑/删除时前一条响应因 `requestId != pending` 被静默丢弃（无失败信号/重试/outbox 兜底 → 本地与服务端分歧直到下次全量同步）；私聊编辑走 `fetch_keys` 异步回调，等待期间再次编辑会覆盖在途上下文（可能用错 messageId 或丢失前一次编辑）。现改为多槽 `m_pendingEdits`（requestId→上下文，镜像已验证的 `m_pendingSendByRequestId`）+ `m_pendingDeleteRequestIds` 集合，私聊编辑经 `m_privateEditQueue` 队列串行消费 `fetch_keys` 传输槽（`m_editFetchInFlight` 占位，与 healing 队列同范式），异步回调按 requestId 取对应上下文不再互相覆盖；登出经 `resetAuthState`、断线经 `onDisconnected` 两路径均清空这些容器并复位 `m_editFetchInFlight`（自动重连不经过 `resetAuthState`，故断线路径必须单独清理，否则在途标记恒真会永久堵死编辑泵），并对已入队/已发出但未收到响应的编辑/删除上报失败以便 UI 回退乐观态。同时新增专用推送类型 `MessageEditedNotification (88)`/`MessageDeletedNotification (89)`，响应与推送彻底分离，消除靠 `requestId==0` 区分带来的误处理风险。

### 会话与认证加固（2026-09-02）

- **逐包验 token**：每个已认证请求在 payload 携带当前 session token（客户端 `addReplayProtection` 在已认证态统一附加）；服务端 `validateSession()` 逐包比对 `hashToken(token)` 与 `sessions.token_hash`，不再仅依赖连接级内存态。
- **回查 sessions 表（fail-closed）**：`validateSession()` 逐请求 `getSessionById` 回查，session 被 `logout`/`terminate_session`/续期换代删除后立即失效；`expires_at` 以 `Qt::ISODate` 解析并校验未过期，格式不可解析一律拒绝（fail-closed，不放行）；`token_renew` 豁免过期门以允许对已过期会话续期。
- **失效回包处理**：鉴权门失败以 `MessageType::Error`（`SessionInvalid`）回包；客户端新增 `handleErrorResponse` 清理在途单发槽位（`fetch_group_keys`/`fetch_keys`/`register_keys`），避免 Error 回包（非对应响应类型）导致群密钥拉取槽位与 healing 队列卡死。
- **残留**：已闭环（2026-09-04）。客户端解析登录/续期响应的 `expiresAt`，过期前 1 天自动 `renewToken`（续期响应 60 秒看门狗兜底、瞬时失败 5 分钟退避重试），续期换代先 `SecureMemory::wipe` 旧 token；服务端 `SessionInvalid/SessionExpired` 回包（业务请求与续期响应两路径）触发客户端安全清零 + `sessionExpired` 信号回登录页提示重新登录。断线重连重登后自动重新调度续期。

### 传输层安全 (TLS)

- 服务端使用 `QSslSocket` + TLS 1.2+ 加密所有客户端连接。
- 客户端使用 `QSslSocket` + `connectToHostEncrypted()`，强制验证服务端证书。
- 证书错误时客户端明确拒绝连接并向用户提示错误信息。
- 开发环境：服务端首次启动自动生成自签名 CA + 服务端证书（RSA 2048，SHA-256 签名，SAN: localhost/127.0.0.1）。
- 生产环境：替换 `certs/` 目录下的证书文件为正式 CA 签发的证书即可。
- 证书生成使用 OpenSSL X509 API，文件 I/O 通过内存 BIO + Qt QFile 避免 Windows applink 问题。

### 重放保护

- 所有业务请求携带 `timestamp`（Unix 秒级时间戳）和 `nonce`（UUID v4），M5.5 起强制必填。
- 服务端拒绝时间戳偏差超过 5 分钟的请求。
- 服务端维护全局共享的 `NonceCache`（跨连接生效，TTL 600 秒，上限 100000 条），拒绝重复 nonce（M5.5 从每连接缓存升级为全局去重）。

### 日志脱敏与结构化日志

- 提供 `LogSanitizer` 工具类，对密码、token、消息正文、IP 地址、邮箱进行掩码处理。
- 日志中不输出明文密码、完整 token、私钥或完整消息正文。
- IP 地址只保留前两段（如 `192.168.*.*`）。
- **结构化日志（M11 前置）**：服务端 `StructuredLogger`（`CommonModule/security`）以单行紧凑 JSON 输出统一字段——时间戳 `ts`、级别 `level`、事件 `event`、请求 ID `requestId`、用户 `userId`、设备 `deviceId`、错误码 `code`、耗时 `durationMs`、来源 IP `ip`（脱敏）。`sendResponse` 对每个请求响应输出一条中央审计日志（成功 `info`、失败 `warning`，据此可统计失败率与延迟）；鉴权/会话/重放/envelope/限流等安全事件另以结构化事件记录并携带 `reason`。敏感字段经 `LogSanitizer` 脱敏后写入，绝不记录 token/密码/私钥/完整正文；已移除登录/注册日志中的明文 username 与明文 IP。

### 安全内存

- 提供 `SecureMemory` 工具类，使用 `OPENSSL_cleanse()` 安全清零内存。
- 密码、token 等敏感数据在使用完毕后立即安全清除。
- 客户端登出时对 session token、待处理密码执行安全清零。

### 密码存储

- 客户端对密码执行 SHA-256 摘要后发送给服务端。
- 服务端使用 **PBKDF2-HMAC-SHA256** 对密码进行慢哈希存储。
- 存储格式：`v1:<iterations>:<salt-hex>:<hash-hex>`
  - `v1`：参数版本号，便于后续升级迭代
  - `iterations`：当前默认 100,000 次迭代
  - `salt`：16 字节随机盐，每次注册/修改密码独立生成
  - `hash`：32 字节 PBKDF2 派生结果
- 密码验证使用常数时间比较，防止时序攻击。

### Session Token

- 登录成功后服务端生成 32 字节随机 token（OpenSSL RAND_bytes）。
- 服务端存储 token 的 SHA-256 摘要，而非明文 token。
- Session 默认有效期 7 天。
- 支持 token 续期（重新生成 token 并替换旧 session）。
- 支持主动登出和强制下线。

### 限流（连接级固定窗口）

- **登录**：同一 IP 5 分钟内最多 10 次失败、同一用户 5 分钟内最多 5 次失败；触发返回 `LoginRateLimited (2006)`；所有登录尝试（成功/失败）记录到 `login_audit` 表。
- **发消息**（M11 前置）：`send_message`（私聊/群聊同一入口）每连接 10 秒内最多 30 条，超限返回 `RateLimited (1003)`；客户端视为瞬时失败保留 outbox 并短退避重刷，不丢消息。抑制刷消息/DoS。
- **搜索**（M11 前置）：`search_users` 每连接 60 秒内最多 20 次，超限返回 `RateLimited (1003)`，抑制用户名枚举/刷库。
- **密钥拉取**：`fetch_keys` 与 `fetch_group_keys` 共享窗口，每连接 60 秒内最多 20 次，超限返回 `RateLimited (1003)`（原用 `LoginRateLimited`，M11 迁至通用码），防预密钥池耗尽。
- **消息编辑/删除**（M9 欠账修复）：`edit_message` 与 `delete_message` 共享窗口，每连接 60 秒内最多 20 次，超限返回 `RateLimited (1003)`。此类操作每次按会话成员数写 `sync_events` + fan-out（O(N) 放大）且事件 30 天才清理，未限流前可被用于刷库/DB 膨胀；编辑仅需重放已捕获的合法 envelope，攻击成本低，故必须限流。
- **会话偏好**（M9 欠账修复）：`set_conversation_prefs` 每连接 60 秒内最多 30 次，超限返回 `RateLimited (1003)`。
- 所有限流窗口为连接级（`RateWindow`，`Chat-Server/core`），与既有 fetch_keys 内联窗口语义一致；`LoginRateLimited` 现仅用于登录。多服务器部署时限流状态需共享/持久化（与 nonce 缓存同为单实例内存态限制）。

### 数据库安全

- 数据库使用版本化迁移机制（`schema_version` 表，当前 V9），禁止隐式 schema 变更；V7（M7a）仅新增 `conversations.name` 与 `conversation_members.role` 两列，V9（M9 特性栈）新增 `conversation_members.pinned/muted` 与 `messages.edited_at/deleted`，存量数据不受影响。
- 每个线程使用独立数据库连接名，避免多线程竞争；写并发启用 5 秒 busy timeout。
- 表结构：`users`、`devices`、`sessions`、`login_audit`、`contacts`、`conversations`、`conversation_members`、`messages`、`message_receipts`、`sync_events`；M6 新增 `device_identity_keys`（仅存身份公钥）、`prekeys`（仅存预密钥公钥，服务端不接触任何私钥）。
- Session 表存储 token 哈希而非明文。
- M6 起新私聊消息的 `messages.content` 为 pairwise E2EE envelope 密文；M7b 起新群消息为 `e2ee_group`/`sender_key_distribution` envelope 密文；M6/M7a 时期的存量明文消息保持原样（历史遗留，不做转换）。

### 传输层

- 已启用 TLS 1.2+，所有客户端-服务端通信均加密。
- 服务端使用 `QSslSocket::startServerEncryption()`，客户端使用 `connectToHostEncrypted()`。
- 开发证书自动生成，有效期 10 年。
- 拓包无法直接看到登录凭据或消息正文。

## 风险

- 开发环境使用自签证书，生产环境必须替换为正式 CA 证书。
- Session token 认证已加固（2026-09-02）：逐包携带并校验 token + `validateSession()` 回查 `sessions` 表，过期/终止/续期换代即时失效（此前仅连接级内存态、除续期外不逐包校验、不回查 DB，存量连接在 token 失效后仍可能通过校验——现已闭环，详见“会话与认证加固”节）。会话失效的客户端自动续期与自动重登 UX 已于 2026-09-04 落地（过期前自动续期、失效回登录页提示），无残留。
- 媒体消息尚未 E2EE（M8 目标）。群聊已经 M7b 实现 Sender-Key E2EE，成员变更的密钥 healing 与失权回收已于 2026-09-02 实施（`member_added/removed/left` 触发轮换+重分发，被移除成员失去后续消息解密能力）；残留：大群单条分发消息可能超 16384 字符上限、轮换“先落盘后分发”的失败窗口（P2）；群路径服务端仍兼容接受 `contentType=text` 明文（M7a 遗留形态，客户端已不产生）。群组接口均遵循先授权再操作（仅成员可发言/邀请/查询，踢人带角色层级保护），输入长度与批量大小受限（群名 ≤64、单批邀请 ≤100、群成员 ≤200、群消息 ≤16384 字符）；客户端本地缓存的群消息/群会话与 sender-key 同样经存储密钥加密落库。
- 设备信任为 TOFU，首次通信无法抵抗服务端中间人；需后续引入安全码带外验证。
- 客户端私钥文件在非 Windows 平台为明文存储（仅 Windows 有 DPAPI 保护）；LocalStore 存储密钥与解密缓存同受此限制。
- 本地缓存（M6.5）含经存储密钥加密的消息明文，拥有本机用户权限者可经 DPAPI 还原后读取，与主流 IM 本地存储模型一致。
- 重放保护的 nonce 缓存为单服务器内存实现，服务端重启后清空；多服务器部署需持久化。

## 后续要求

- 认证加固：`validateSession()` 回查 `sessions` 表 + 逐包验 token 已于 2026-09-02 实施（撤销/过期即时生效）；客户端自动续期与失效自动重登 UX 已于 2026-09-04 实施（`renewToken()` 定时续期 + `sessionExpired` 回登录页）。后续可选 TLS channel 绑定进一步加固。
- 设备信任升级：安全码/二维码带外验证；密钥备份与设备间迁移策略。
- 群成员变更的 Sender-Key healing 与失权回收已于 2026-09-02 实施（后向安全闭环）；后续：大群分片分发/提高分发上限、轮换改为 ACK 后启用（消除分发失败窗口）、群路径收紧为拒绝 `text` 明文。
- 媒体文件客户端加密上传（M8）。
- 后续可考虑将 PBKDF2 升级为 Argon2id。
- 生产部署时应启用证书自动续期或 ACME 协议。
- 可考虑增加 HSTS 或证书固定 (Certificate Pinning) 策略。

