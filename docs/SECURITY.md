# XYChat 安全文档

## 当前安全状态（M10 完成后，2026-09-14 对齐）

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
- **密文不落库/不显示**：解密失败的 envelope 原文（私聊 `e2ee`、群聊 `group_e2ee` 与 `sender_key_distribution`）绝不作为正文写入本地库或渲染到 UI（统一清空并标记 undecryptable，UI 显示“无法解密”占位）；`upsertMessage` 落库拦截与 `healEnvelopeLeaks` 打开时自愈均覆盖上述三类 envelope，历史污染行自动检出并清空（2026-09-03 修复：此前群 envelope 未纳入拦截/自愈，且 `sync_messages` 曾 emit 未解密数组致密文当正文显示）。**读取侧纵深防御（2026-09-10）**：`loadConversations` 回填会话预览时再过一道同类拦截，命中则优先用持久化解密缓存回填真实明文、否则占位，使历史污染或尚未同步的预览也不会把密文 JSON 展示给用户。
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

### 会话整表删除安全（M10，2026-09-14）

- **权限收紧（群聊仅群主）**：删除会话是**不可逆的破坏性操作**，仅会话成员可发起；**群聊额外要求发起者为群主**（`groupRole == owner`），普通成员整表删除全群会销毁其他成员的数据，故拒绝并提示改用“退出群聊”（`PermissionDenied`）；私聊任一方可删除整个会话。越权（非成员）同样回 `PermissionDenied`（成员关系已是前置门，不以枚举防护为首要目标）。
- **硬删除的原子性与 FK 安全顺序**：`DatabaseManager::deleteConversation` 在单事务内**显式按 FK 安全顺序**删除回执（`message_receipts`）→消息（`messages`）→成员（`conversation_members`）→会话（`conversations`）四步，任一步失败即 `rollback` 返回 false。**刻意不依赖 `PRAGMA foreign_keys` 的 CASCADE**——该 pragma 是连接级设置，若某连接未开启，CASCADE 会静默失效而遗留孤儿行；显式有序删除使正确性不受连接配置影响。
- **fail-closed 广播**：仅当 `deleteConversation` 返回 true（库内确已删除）才向成员广播删除事件；失败时回 `InternalError`、**不广播**、记 `conversation.delete_failed` 结构化日志（与 M9 `message.delete_failed` 同范式，避免“库内未变却广播删除”的服务端与事件流分歧）。
- **通知全体前成员避免幽灵会话**：删除前先取全体成员 ID（成员行删除后无法再取），成功后向**全体前成员**（含操作者本人的其他设备）推 `ConversationDeletedNotification` + 各自写 `conversation_deleted` 事件到 `sync_events`（离线补偿）；发起设备按 `originDeviceId` 自行忽略推送（已凭响应本地清理），其余设备经推送或增量同步移除本地会话，避免“服务端已删而某端仍显示”的幽灵会话。
- **本地缓存清理**：客户端 `LocalStore::deleteConversation` 清除该会话的本地消息/会话缓存行；存储密钥与 E2EE 密钥材料按既有策略保留（会话删除不销毁密钥材料）。
- **文件对象的兜底回收**：会话删除**不直接触碰对象存储**——消息引用的 `files` 行随 `messages` 删除而失去引用，由 M8 维护任务按“已就绪但无引用”原子迁入终态、下一轮销毁磁盘对象（见“文件与媒体传输安全”节的回收竞态防护）。因此删除会话不会立即抹掉密文字节，但也不产生新引用，回收链路照常收敛。
- **限流**：与 `edit_message`/`delete_message` 共用 `m_editDeleteWindow`（每连接 60 秒 20 次），先校验入参形态后消费配额；破坏性且触发 O(N) 成员事件写入，必须限流防刷。

### “正在输入”指示安全与隐私（M10，2026-09-14）

- **成员校验**：仅会话成员可发 typing（鉴权 + 限流通过后回查 `isConversationMember`），非成员回 `PermissionDenied`；入参 `conversationId <= 0` 先回 `InvalidRequest`（形态校验先于限流消费，畸形请求不占额度，与 send/edit/prefs 一致）。
- **限流防刷屏放大**：连接级 `m_typingWindow` 限流 10/10s（`MaxTypingPerWindow=10`/`TypingWindowSeconds=10`）。typing 是高频信号且服务端按会话成员数 fan-out（O(N) 放大），未限流可被用于刷屏/放大攻击；客户端另有 `sendTyping` 每会话 4s 节流（`TypingThrottleMs=4000`）作为第一道闸。
- **不落库、不写 sync_events**：typing 是纯瞬时状态，服务端**不写数据库、不写 `sync_events`**（无需离线补偿），仅在线直推给会话其他成员；接收端 5s 无新信号自动隐藏。因此 typing 不产生持久化足迹，也不进入增量同步流。
- **隐私考量（username 与实时行为暴露）**：`TypingNotification` payload 携带发起者 `userId` 与 `username`（`usernameById`），仅推送给**同会话成员**——会话成员本就能看到彼此用户名，故不构成额外泄露；但需注意 typing 会暴露“某成员当前在线且正在输入”这一**实时行为信号**（与主流 IM 一致的产品取舍，本次未提供关闭开关）。

### 文件与媒体传输安全（M8.1，2026-09-10）

文件字节在客户端加密后才上传，服务端只见密文与密文侧元数据：

- **每文件独立密钥**：每个文件一把随机 AES-256 密钥 + 12 字节 nonce 前缀，严禁跨文件复用。密钥只写入 `FileManifest`，随消息正文经既有 E2EE（私聊 pairwise envelope / 群聊 Sender-Key）分发，服务端无从获得。
- **分片独立 AEAD + 位置绑定**：第 i 片 nonce = `iv` 后 4 字节 XOR 大端 `i`，AAD = 大端 4 字节 `i`。AAD 把分片绑到其序号上，重排、截断或以他片冒替均在 GCM 认证阶段被拒；nonce 唯一性由 `iv` 随机性与 XOR 对固定 `iv` 的双射性共同保证。各片互相独立，因此上传/下载可流式、可断点续传、内存占用恒定。
- **刻意不复用消息 ratchet**：文件密钥与 Sender Key 解耦后，分片没有必须按序消费的链状态，不会出现“链已推进导致早先分片永久不可解”的不可逆损坏（M9 消息编辑踩过的坑）；转发与多端重复下载同一文件也不需要重新加密。**代价**：文件密钥不具备链式前向安全（拿到清单即可解该文件的全部内容），这是“可重复下载/可转发”与“逐片前向安全”之间的取舍，与主流 IM 的媒体加密一致。
- **元数据分层**：服务端可见密文字节、密文体积、分片参数、密文整体 SHA-256、上传者与其设备；不可见文件名、MIME、明文大小、多媒体尺寸/时长与文件密钥（这些只在清单里，走 E2EE）。
- **认证失败清零**：`decryptChunk` 认证失败（篡改、密钥错、分片序号错）返回空并清零已产出明文。
- **枚举预言机防护**：`fileId` 为顺序整数，因此“不存在”与“不是你的”在上传控制面（`requireOwnedFile`）与下载授权（`canUserAccessFile`）两处均合并为同一错误码 `FileNotFound`，真实原因只进服务端结构化日志；下载票据同样先发授权后取记录。票据校验的四种失败原因（不存在/过期/类型不符/已使用）也统一回 `InvalidFileTicket`，避免被用来探测票据库。
- **票据**：上传/下载票据为高熵随机串，明文只在签发响应中出现一次；服务端只存 SHA-256 摘要（与 session token 同一套做法，库泄露不等于凭据泄露）。上传票据 TTL 24 小时（覆盖大文件慢速上传），下载票据 TTL 300 秒；过期票据由维护任务清理。
- **配额与限流**：每用户并发上传配额 8（只数 `uploading`），与插入在同一条 `INSERT...SELECT` 内原子校验（分步“先读计数后插入”存在 TOCTOU，同一用户多设备并发创建会集体读到“未满”而全部放行，使软配额形同虚设）；新建上传每连接 60 秒 20 次、查询/完成/取消/下载票据共用 60 秒 60 次，入参形态校验先于限流消费（畸形请求不占额度）。体积上限 2 GiB、分片大小 64 KiB-4 MiB、分片数上限 4096，`chunkCount` 必须等于 `ceil(cipherSize / chunkSize)`（否则可用少报分片数把超大文件拆到上限之外）；非末片恒为 `chunkSize` 字节、末片为余量，防止客户端自选分片边界绕过校验。
- **存储层安全**：`blobKey` 由服务端分配（16 字节随机数的十六进制），不含任何用户可控成分；每次访问前重新校验形态，用户可控字符串永不参与路径拼接（从根源排除路径穿越）。所有写入均为“临时文件 + 原子改名”，进程崩溃或磁盘写满不会留下被当作完整数据的半截文件；临时文件名带随机后缀，两次意外并发的组装不会互相覆写。同一 `blobKey` 的 `finalize`/`remove` 经固定条带锁（64 条）串行：两条线程同时组装会因交错写入产出损坏对象，一条组装而另一条删除则产出“元数据 ready 而对象缺失”的不可自愈状态。`blobKey` 不回传客户端，以免它成为可枚举的对象路径。
- **完整性校验**：`finalize` 流式组装并逐片核对长度、整体核对 SHA-256，全部通过才转 `ready`（fail-closed：宁可要求重传，也不把损坏对象推上下载路径）。结果分类区分“数据故障”（长度/摘要不符 → 标 `failed` 并回收磁盘，重传同批分片只会得到同样结果）与“存储故障”（写满/改名失败 → 保留现场让客户端稍后重试）；把后者误报成校验和错误会使客户端无限重传。
- **回收与数据留存**：维护任务三轮清理——超期未完成上传（48 小时）先删盘后落状态；终态行（`cancelled`/`failed`）先删盘后删行；已就绪但无引用的行（附件所在消息被软删除，或上传完成后发送始终未发生）**只原子地迁入终态、不直接碰磁盘**，销毁推到下一轮。引用判定与迁移合并为单条语句：分步版本存在 TOCTOU，并发 `sendMessage` 可能在两步之间引用该文件，随后磁盘数据被删掉，留下一条指向空数据的消息（用户侧表现为附件永久打不开）。迁入终态后不可能再被新消息引用——**这一不变量单独并不成立**：发送侧的文件校验与消息写入同样存在跨线程窗口（每连接一个线程，维护任务在另一线程用独立连接）。因此两侧都做了防护：发送侧把“文件仍为 `ready`”下推为 `INSERT` 的守卫子查询（单语句原子，SQLite 写者串行：要么消息先落库使迁移的 `NOT EXISTS` 放弃，要么迁移先提交使插入查不到 `ready` 行而回 `FileNotReady`）；回收侧终态那一轮在删盘前再判一次引用，宁可留下一条指向仍在盘上对象的 `cancelled` 行（可修复），也不销毁仍被引用的数据（不可恢复）。宽限期以 `completed_at` 为基准（缺失时退回 `created_at`）：续传可能跨越数天，按 `created_at` 算会使刚完成的大文件被立即当成孤儿删掉。
- **取消顺序**：取消接口先落状态再删磁盘。反序会与并发的完成请求交错出“DB=ready 而 blob 已删”的不可自愈状态（下载票据能正常签发、数据面必然读失败）；本序最坏只留下“DB=cancelled 而分片仍在盘上”的隐形孤儿，由维护任务的终态回收兜底。已完成的文件不走取消接口（可能已被消息引用，撤回会让接收方的下载票据指向已消失的对象）。
- **文件消息不得静默降级**：存储未注入时 `send_message` 携带 `fileId` 一律拒绝（`FileStorageFailed`）而非按普通消息投递——正文其实是清单 JSON，降级投递会让接收端把文件密钥当文本渲染。文件状态除前置校验（存在/本人/`ready`）外，还在插入语句内原子复核（守卫未命中则不写入并回 `FileNotReady`），避免产出一条指向已回收文件的消息。
- **内联缩略图（M8.3a，2026-09-11）**：图片消息的清单携带原图尺寸与最长边 ≤160px 的 JPEG 缩略图。缩略图是**明文**字节（清单整体已经既有 E2EE 加密，再单独加一层无安全收益），也正因此它**不含任何密钥**，可以经 `sanitizeForUi` 交给 QML 以 base64 渲染（`data:image/jpeg;base64,`），使接收方在下载原图之前就能预览；文件密钥与 nonce 仍只留在 C++ 侧。提取只用 QtGui（`QImageReader`/`QImage`）而不依赖平台多媒体后端；`setAutoTransform` 校正 EXIF 方向（否则手机竖拍照片会得到横向缩略图）。体积硬约束：压不进 `MaxThumbnailBytes`（4096）就**不内联**（UI 回退到文件图标），绝不放宽上限，因为清单超长会使整条文件消息被服务端拒收。元数据提取失败（非图片/损坏/编码器缺失）一律留空字段，**绝不阻断文件发送**。
- **音视频元数据与应用内播放（M8.3b/c，2026-09-11）**：音视频时长/分辨率/视频封面由 `MediaMetadataExtractor`（QtMultimedia `QMediaPlayer` + `QVideoSink`）异步提取，同样只在清单内随 E2EE 分发、服务端不可见，提取失败/超时留空绝不阻断发送。应用内大图查看器（`FileImageProvider`，`image://xyfile/<id>`）与音视频播放器（`MediaPlaybackManager` + `DecryptingIODevice`）均从密文缓存逐片解密后在内存中解码/播放，**明文不落盘**（与“磁盘上不存在可读明文”口径一致，看原图/播放不再必须“另存为”）；`DecryptingIODevice` 作为只读 `QIODevice` 喂给 `QMediaPlayer::setSourceDevice`，支持 seek（拖动进度条时按 plainPos 定位分片重新解密）；内存解码上限 64 MiB，超限回退“另存为”。渲染线程与密钥的跨线程访问经 `manifestFor` 加锁拷贝清单、IO 与 GCM 认证在锁外。历史图片消息（清单 thumb 为空）下载后由 `localThumbnailForMessage` 本地生成缩略图缓存到 `<cacheRoot>/thumbs/`（JPEG 明文，不含密钥，与密文缓存分目录）。**已知限制**：视频播放无动态画面（QML VideoOutput 无法绑定 C++ QVideoSink，只输出音频轨 + 静态封面）。

### 数据面与客户端缓存安全（M8.2，2026-09-11）

- **数据面 TLS 与主通道同口径**：HTTP(S) 服务由 `QSslServer` 承载，复用主通道同一份 `QSslConfiguration`（开发自签 CA + `TlsV1_2OrLater` + `VerifyPeer`）；TLS 不可用且未显式允许明文则拒启。客户端 `QNetworkAccessManager` 复用同一份 CA 配置，**不连 `sslErrors` 去 `ignoreSslErrors()`**，也不在此处放宽校验；重定向策略为 `NoLessSafeRedirectPolicy`。
- **票据不进 URL**：上传/下载票据只经请求头 `X-XYChat-Ticket` 传递。放 query 会被反向代理、访问日志与 `Referer` 记下，等于把凭据写进日志；服务端只存并比对 SHA-256 摘要。四种票据失败统一 401 且响应体一致，真实原因只进服务端日志。
- **上传票据即时吊销**：上传完成/取消/标失败后服务端立即 `revokeFileTickets`（TTL 24 小时而分片已组装回收，持票也无处可用）。下载票据刻意允许 TTL（300 秒）内重用以支持 `Range` 分段，因此不消费。
- **数据面限流只计失败**：per-IP 30 次/60 秒的窗口只统计授权失败与畸形请求，成功的数据搬运不计入（合法上传一个大文件需多达 4096 次 PUT，按请求数限流会挡住正常业务）。**残留风险**：`QHttpServer` 在进 handler 前已完整缓冲请求体，未认证客户端可用巨大 `Content-Length` 造成内存放大（已登记为 ROADMAP §3 P2，建议部署在带 body 上限的反向代理之后，`--http-host` 默认仅回环）。
- **客户端本地缓存存的是密文（经调研后定调）**：下载得到的字节本就是客户端加密的密文，因此**原样落盘即可**（`<AppData>/XYChat/filecache/<sha256[0..1]>/<sha256>`，无后缀）：零额外加密开销、磁盘上天然不是明文，与本文“磁盘上不存在可读明文”的口径一致。文件名用密文 SHA-256（协议已有该摘要，一份值兼作缓存键、完整性校验值与不可预测文件名），前两位分 256 桶。**明文只在用户显式“另存为”时写出**（临时名 + 改名，不留半截明文）；应用内预览在内存中解密，不落盘。
  - **调研依据**：Signal Desktop（`attachments.noindex/` 下加密存储，按消息内 `localKey`/`iv` 解密并校 SHA-256）、Telegram Desktop（`tdata/<账号>/cache` 与 `media_cache` 加密存储，`PlaceFromId()` 随机 14 字符名 + 前 2 字节分桶）、Signal/Session Android（`app_parts/*.mms`，AES-CTR + HMAC-SHA256）——**无一家采用“明文 + 混淆文件名”**。随机/无后缀命名只用于防索引与防目录浏览时识别类型，不承担保密职责（文件头魔数使类型探测成本极低，属隐匿式安全）。关键论证：若附件明文缓存，则 `LocalStore` 对消息正文的加密在“含附件的会话”上被完全绕过，加密就失去意义。
  - **不用 MD5 命名**：MD5 不抗碰撞，且协议已统一用 SHA-256，再引入一套摘要口径只增加漂移风险。
  - **无法跨文件去重**：每文件独立随机密钥使相同明文产生不同密文，因此缓存去重只在“同一文件重复下载”层面生效（这是 E2EE 的固有性质，也是隐私优点：服务端无法凭摘要判断两人发了同一个文件）。
- **文件密钥的暴露面（M8.2 修复的 P0）**：`FileManifest` 含 32 字节文件密钥，它的明文形态只得存在于 C++ 侧。旧实现只在“实时推送”一条路径上脱敏，而 `sync_messages`（历史翻页与离线补收）、本地缓存回填、`sync_events` 三条路径会把含密钥的清单 JSON 当正文渲染进气泡（可截图、可复制转发，且字符串进入 JS 堆后无法可靠清零）——拿到它就能直接解出服务端上的文件密文，此时密文等同明文。现修为：
  - **唯一脱敏出口** `sanitizeForUi`，四条通向 UI 的路径全部经它：先 `attachFileInfo` 登记清单（密钥留在 C++ 侧的传输引擎）并补脱敏展示字段（`fileName`/`fileMime`/`fileSizeBytes`/`fileSha256`/`fileCipherSize`，均非秘密），再把 `content` 置空。M12 起 `sync_messages` 与本地缓存回填在时间片泵内**逐条**调用它（页内消息不再整批一次处理），出口唯一性不变；
  - **兜底防线**：即使 `fileId` 缺失、清单解析失败或字段不自洽，只要正文形态像清单（`looksLikeFileManifest`）就一律置空并告警，使将来新增的消息出口不会重蹈覆辙（宁可不展示一个附件）；
  - **M12 补漏：本地消息搜索（M11A A5）同样过出口**。`LocalStore::searchMessages` 的解密正文原先直接返回 QML 并由搜索结果列表逐条渲染，文件消息命中时会把含 32 字节文件密钥的清单原文送进 JS 堆（字符串进入 JS 堆后无法可靠清零）；现与 `loadConversations` 同口径，命中清单时降级为 `Protocol::filePreviewText`（`[File] <名>`），降级实现只保留这一处；
  - **落库与 emit 分离**：本地库写入的是原始清单（由存储密钥 AES-256-GCM 加密），emit 给 UI 的是脱敏副本；会话预览用 `[File] <名>` 而不是清单 JSON；
  - **回归锁定**：`TestNetworkManager::fileManifestNeverReachesUiLayer` 断言四条路径的 `content` 为空、不含 `key`/`iv`、普通文本不被误伤，并断言密钥确实登记到了 C++ 侧。
- **文件消息不进持久化 outbox**：outbox 表无 `file_id` 列，若把清单落库，重启后重发会以 `fileId=0` 投出一条“正文是清单”的普通消息，等于把文件密钥当文本发给对方。因此文件消息只进内存 outbox，应用重启后需重新上传（本地源文件仍在）。
- **会话生命周期**：登出（`resetAuthState`）与断线（`onDisconnected`，自动重连不经前者）两条路径均调 `FileTransferManager::reset()`：标全部任务为已取消、断开在途 reply 的回调并显式复位（`abort()` 会同步触发 `finished`，不断开就会在登出途中发新请求）、删临时文件、**清零已登记清单里的文件密钥**（不得跨会话驻留）。

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
- **消息编辑/删除**（M9 欠账修复）：`edit_message` 与 `delete_message` 共享窗口，每连接 60 秒内最多 20 次，超限返回 `RateLimited (1003)`。此类操作每次按会话成员数写 `sync_events` + fan-out（O(N) 放大）且事件 30 天才清理，未限流前可被用于刷库/DB 膨胀；编辑仅需重放已捕获的合法 envelope，攻击成本低，故必须限流。M10 的 `delete_conversation`（破坏性、按成员数写 `conversation_deleted` 事件）也共用此窗口。
- **会话偏好**（M9 欠账修复）：`set_conversation_prefs` 每连接 60 秒内最多 30 次，超限返回 `RateLimited (1003)`。
- **“正在输入”**（M10）：`typing` 每连接 10 秒内最多 10 次（`m_typingWindow`），超限返回 `RateLimited (1003)`。typing 高频且服务端按成员数 fan-out（O(N) 放大），需独立紧窗口防刷屏；客户端另有每会话 4s 节流作为第一道闸。
- **文件上传**（M8.1）：`file_upload_create` 每连接 60 秒内最多 20 次（每次新建都会在 DB 写入元数据行与票据行，并占用并发上传配额），超限返回 `RateLimited (1003)`。
- **文件操作**（M8.1）：`file_upload_query`/`file_upload_complete`/`file_upload_cancel`/`file_download_ticket` 共用一个窗口，每连接 60 秒内最多 60 次，超限返回 `RateLimited (1003)`。`complete` 会触发服务端流式读盘与整体 SHA-256 计算（IO 密集），`download_ticket` 会写票据行，两者均需限流。入参形态校验先于限流消费，畸形请求不占额度。
- **并发上传配额**（M8.1）：每用户最多 8 个 `uploading` 状态的文件（`MaxConcurrentUploadsPerUser`），超限返回 `FileQuotaExceeded (3021)`；配额与插入在单条 `INSERT...SELECT` 内原子完成，避开“先读计数后插入”的 TOCTOU。
- 所有限流窗口为连接级（`RateWindow`，`Chat-Server/core`），与既有 fetch_keys 内联窗口语义一致；`LoginRateLimited` 现仅用于登录。多服务器部署时限流状态需共享/持久化（与 nonce 缓存同为单实例内存态限制）。

### 数据库安全

- 数据库使用版本化迁移机制（`schema_version` 表，当前 V10），禁止隐式 schema 变更；V7（M7a）仅新增 `conversations.name` 与 `conversation_members.role` 两列，V9（M9 特性栈）新增 `conversation_members.pinned/muted` 与 `messages.edited_at/deleted`，V10（M8.1）新增 `files`/`file_tickets` 两表与 `messages.file_id` 列，存量数据不受影响。
- 每个线程使用独立数据库连接名，避免多线程竞争；写并发启用 5 秒 busy timeout。
- 表结构：`users`、`devices`、`sessions`、`login_audit`、`contacts`、`conversations`、`conversation_members`、`messages`、`message_receipts`、`sync_events`；M6 新增 `device_identity_keys`（仅存身份公钥）、`prekeys`（仅存预密钥公钥，服务端不接触任何私钥）；M8.1 新增 `files`（密文侧元数据：`blob_key` UNIQUE、体积、分片参数、整体 SHA-256、状态与时间戳）、`file_tickets`（只存票据 SHA-256 摘要，无票据明文列）。
- Session 表存储 token 哈希而非明文；文件票据同理（`file_tickets.ticket_hash`）。
- M6 起新私聊消息的 `messages.content` 为 pairwise E2EE envelope 密文；M7b 起新群消息为 `e2ee_group`/`sender_key_distribution` envelope 密文；M6/M7a 时期的存量明文消息保持原样（历史遗留，不做转换）。

### 传输层

- 已启用 TLS 1.2+，所有客户端-服务端通信均加密。
- 服务端使用 `QSslSocket::startServerEncryption()`，客户端使用 `connectToHostEncrypted()`。
- 开发证书自动生成，有效期 10 年。
- 拓包无法直接看到登录凭据或消息正文。

## 风险

- 开发环境使用自签证书，生产环境必须替换为正式 CA 证书。
- Session token 认证已加固（2026-09-02）：逐包携带并校验 token + `validateSession()` 回查 `sessions` 表，过期/终止/续期换代即时失效（此前仅连接级内存态、除续期外不逐包校验、不回查 DB，存量连接在 token 失效后仍可能通过校验——现已闭环，详见“会话与认证加固”节）。会话失效的客户端自动续期与自动重登 UX 已于 2026-09-04 落地（过期前自动续期、失效回登录页提示），无残留。
- 文件传输控制面与存储层（M8.1）、数据面 HTTP(S) 上传下载服务与客户端上传/下载引擎（M8.2，2026-09-11）、多媒体元数据与应用内预览（M8.3，2026-09-11）均已落地并有单测覆盖，具备可用的文件消息端到端路径；M10（2026-09-14）进一步把上传 hashing 与“另存为”解密改为单线程时间片增量泵（不再阻塞 GUI 线程、进度可见可取消）。残留：下载票据 TTL（300s）对大文件不足且中途不续期、`QHttpServer` 进 handler 前已缓冲整个请求体（见 ROADMAP §3）。群聊已经 M7b 实现 Sender-Key E2EE，成员变更的密钥 healing 与失权回收已于 2026-09-02 实施（`member_added/removed/left` 触发轮换+重分发，被移除成员失去后续消息解密能力）；残留：大群单条分发消息可能超 16384 字符上限、轮换“先落盘后分发”的失败窗口（P2）；群路径服务端仍兼容接受 `contentType=text` 明文（M7a 遗留形态，客户端已不产生）。群组接口均遵循先授权再操作（仅成员可发言/邀请/查询，踢人带角色层级保护），输入长度与批量大小受限（群名 ≤64、单批邀请 ≤100、群成员 ≤200、群消息 ≤16384 字符）；客户端本地缓存的群消息/群会话与 sender-key 同样经存储密钥加密落库。
- 设备信任为 TOFU，首次通信无法抵抗服务端中间人；需后续引入安全码带外验证。
- 客户端私钥文件在非 Windows 平台为明文存储（仅 Windows 有 DPAPI 保护）；LocalStore 存储密钥与解密缓存同受此限制。
- 本地缓存（M6.5）含经存储密钥加密的消息明文，拥有本机用户权限者可经 DPAPI 还原后读取，与主流 IM 本地存储模型一致。
- 重放保护的 nonce 缓存为单服务器内存实现，服务端重启后清空；多服务器部署需持久化。
- 对象存储为单机本地文件系统（`LocalFileStorage`）：无副本/无冗余，磁盘损坏即文件丢失；存储根目录的访问控制依赖操作系统文件权限（数据为密文，但删除/改写仍可造成可用性损失）。多实例部署需换为共享对象存储（`IObjectStorage` 已抽象，可接 S3/MinIO），届时限流与并发配额也需从单实例内存/单库口径改为全局口径。

### 桌面集成安全（M11A）

M11A 新增的设置页、系统托盘、桌面通知、草稿与本地消息搜索均为纯客户端功能，不涉及协议变更，但引入以下安全考量：

- **桌面通知正文脱敏**：通知正文经 `sanitizeForUi` 统一出口脱敏后才交给 `TrayManager.showMessage`，文件消息只显示 "[File] 文件名" 而不含清单密钥；消息预览开关（`messagePreviewEnabled`）关闭时正文替换为 "[新消息]"，防止敏感内容在锁屏/投屏场景下泄露。免打扰会话（`muted=true`）与当前活动会话的通知被抑制。
- **本地消息搜索**：`LocalStore.searchMessages` 在内存中解密后匹配，明文不经 QML/JS 引擎（搜索结果以 `QJsonArray` 经 C++ 信号传递，QML 侧只见脱敏后的展示字段）；搜索范围限于本地缓存（已经 E2EE 解密的消息），不触及服务端。性能上限：扫描行数 `limit × 10`，千级消息量下可接受，万级需考虑 FTS5 或异步搜索。
- **草稿**：内存级 JS 对象（`MainPage.drafts`），退出应用后不保留；草稿文本为消息正文明文，与输入框中的内容同一安全等级（进程内存，不落盘）。
- **AppSettings 持久化**：经 `QSettings`（Windows 注册表 / INI）存储，仅含布尔偏好（darkMode/通知开关/预览开关/托盘开关），不含密钥或敏感数据。
- **系统托盘**：`QSystemTrayIcon` 为操作系统级组件，托盘图标与菜单不携带敏感信息；最小化到托盘时窗口隐藏但进程继续运行，会话 token 与 E2EE 密钥仍在内存中（与最小化到任务栏同一安全模型）。

### 客户端消息视图内存驻留（M12.4，2026-09-16）

M12.4 将聊天区由"单视图、切换即清空"改为**按会话堆叠**（每会话一个 `ChatView` 实例，切换只改可见性），纯客户端改动、无协议影响，安全口径变化如下：

- **明文驻留范围扩大**：切换会话不再销毁视图，故最多 `maxCachedViews`（8）个会话的**已解密消息**同时驻留 QML 模型（此前只有当前会话的视图持有明文）。这些都是当前登录用户本就有权查看的内容，与本地加密库中的解密缓存同一等级；超限淘汰与登出（`resetUi`）都会 `destroy()` 视图释放模型。
- **不新增落盘**：视图状态只驻内存；输入框草稿仍走既有 M11A 机制（淘汰时转存到内存 `drafts`，不落盘）。
- **脱敏口径不变**：喂给堆叠视图的消息仍逐条经 `sanitizeForUi`（文件密钥不进 QML/JS 堆）；切回已打开会话的增量补收复用同一出口。
- **已读回执时机**：回执由"消息页到达"改挂 `conversationActivated`（激活即上报），语义仍是"用户正停留在该会话"，不产生额外信息泄露。

## 后续要求

- 认证加固：`validateSession()` 回查 `sessions` 表 + 逐包验 token 已于 2026-09-02 实施（撤销/过期即时生效）；客户端自动续期与失效自动重登 UX 已于 2026-09-04 实施（`renewToken()` 定时续期 + `sessionExpired` 回登录页）。后续可选 TLS channel 绑定进一步加固。
- 设备信任升级：安全码/二维码带外验证；密钥备份与设备间迁移策略。
- 群成员变更的 Sender-Key healing 与失权回收已于 2026-09-02 实施（后向安全闭环）；后续：大群分片分发/提高分发上限、轮换改为 ACK 后启用（消除分发失败窗口）、群路径收紧为拒绝 `text` 明文。
- 媒体文件客户端加密上传：协议、加密原语、服务端控制面与存储层（M8.1）、数据面 HTTP(S) 上传下载服务与客户端上传/下载引擎（M8.2）、多媒体元数据与应用内预览（M8.3）均已落地；M10 补齐上传 hashing 与“另存为”解密的异步化（单线程时间片泵）。后续可选：下载票据大文件续期、数据面请求体大小上限（反向代理）。
- 后续可考虑将 PBKDF2 升级为 Argon2id。
- 生产部署时应启用证书自动续期或 ACME 协议。
- 可考虑增加 HSTS 或证书固定 (Certificate Pinning) 策略。

