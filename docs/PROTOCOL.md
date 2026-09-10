# XYChat 协议文档

## 当前协议状态（M7b 完成后，2026-09-02 对齐）

M5 在 M3 基础上新增了传输层加密（TLS 1.2+）与重放保护；**M5.5（2026-08-03 实施）完成了安全加固**：TLS 改为 fail-closed、timestamp/nonce 改为强制必填并全局 TTL 去重、会话/消息接口全部先授权再查询、越权注销接口改为仅能终止本人其他会话、发送消息新增 `clientMessageId` 幂等键、回执改为按接收者/设备维度记录、新增账号级 `sync_events` 游标同步。**M6（2026-08-17 实施）完成了一对一聊天端到端加密**：简化 Signal 方案（X25519 身份密钥 + 一次性预密钥 + 每消息临时密钥 ECDH + HKDF-SHA256 + AES-256-GCM），消息正文以不透明 envelope 密文传输，服务端 fail-closed 只存密文。**M6.5（2026-08-21 实施）为纯客户端本地持久化（本地加密缓存与持久化 outbox），未变更任何线上协议**：复用既有 `sync_events` 游标接口（客户端登录后自动增量拉取并持久化游标）与 `clientMessageId` 幂等语义（持久化 outbox 重启后重发）。**M7a 子任务一（2026-08-21 实施）完成了明文群聊的协议定义与服务端数据模型**：新增群组请求/响应消息类型（60-70）与群组错误码（3009-3012），数据库迁移至 V7（`conversations.name` + `conversation_members.role`）。**M7a 子任务二（2026-08-21 实施）完成了群组业务处理器与 fan-out**：建群/邀请/退群（群主自动转让）/踢人（层级保护）/群信息全部服务端落地，`send_message` 按 `conversationId`/`toUserId` 分流（群聊明文 fan-out，私聊维持 envelope fail-closed），群成员变更产生系统消息与 `group_changed` 事件，回执聚合改为按接收者人数（新增送达/已读计数）。**M7a 子任务三（2026-08-21 实施）完成客户端接入与群聊 UI**（无线上协议变更）：`NetworkManager` 群组五接口与群消息 outbox 分流，`LocalStore` 会话缓存新增群名/成员数，QML 建群/群信息/邀请对话框与系统消息渲染。**M7b（2026-09-02 入库）完成了群聊端到端加密（Sender Keys）**：新增 `FetchGroupKeysRequest/Response`（消息类型 71/72）一次性拉取全群成员 E2EE 密钥包；群消息新增 `contentType=e2ee_group`（chain-key ratchet + AES-256-GCM + Ed25519 签名的群 envelope）与 `contentType=sender_key_distribution`（chain key 经 M6 pairwise envelope 逐设备加密分发）；服务端对两类正文 fail-closed 校验（非法返回 3008），只见密文。上述变更均有自动化测试覆盖。**M9 特性栈（2026-09-05 实施）完成了会话置顶/免打扰与消息编辑/删除**：新增 `SetConversationPrefsRequest/Response (81/82)`、`ConversationPrefsNotification (83)`、`EditMessageRequest/Response (84/85)`、`DeleteMessageRequest/Response (86/87)`；数据库迁移至 V9（`conversation_members.pinned/muted`、`messages.edited_at/deleted`）；编辑/删除仅发送者可操作、编辑正文须保持原 contentType 且经服务端 fail-closed 密文校验（私聊 pairwise envelope、群 e2ee_group，拒绝明文注入）；新增 `conversation_prefs`/`message_edited`/`message_deleted` 三类 `sync_events` 事件实现多端与离线同步，删除为软删除留墓碑（幂等）。

仍属非生产级的部分：nonce 去重为单服务器内存缓存（重启清空）、认证状态仍为连接级内存态（但自 2026-09-02 起每个已认证请求逐包携带并校验 token，`validateSession()` 逐请求回查 `sessions` 表并对过期/终止/续期换代即时失效）、媒体消息尚未 E2EE（M8 目标）、设备信任为 TOFU（无安全码比对）。会话自动续期与失效自动重登已落地（2026-09-04：客户端解析 `expiresAt` 过期前自动 `renewToken`，失效回登录页）。群成员变更的 Sender-Key healing 与失权回收已于 2026-09-02 实施（成员变更触发轮换+重分发）。

### 固定包头

所有多字节整数使用大端序。包头长度为 20 字节：

```text
magic:u32 | version:u16 | messageType:u16 | requestId:u64 | payloadLength:u32 | payload
```

字段说明：

| 字段 | 当前值/说明 |
| --- | --- |
| `magic` | `0x58594350`，ASCII 语义为 `XYCP` |
| `version` | 当前协议版本为 `1` |
| `messageType` | 见下表 |
| `requestId` | 客户端生成的请求 ID；响应沿用请求 ID |
| `payloadLength` | JSON payload 字节数，当前最大 4 MiB |

### 消息类型

| 值 | 名称 | 说明 |
| --- | --- | --- |
| `1` | `LoginRequest` | 登录请求 |
| `2` | `LoginResponse` | 登录响应 |
| `3` | `Ping` | 心跳请求 |
| `4` | `Pong` | 心跳响应 |
| `5` | `Error` | 错误 |
| `10` | `RegisterRequest` | 注册请求 |
| `11` | `RegisterResponse` | 注册响应 |
| `12` | `LogoutRequest` | 登出请求 |
| `13` | `LogoutResponse` | 登出响应 |
| `14` | `TokenRenewRequest` | Token 续期请求 |
| `15` | `TokenRenewResponse` | Token 续期响应 |
| `16` | `ForceLogoutRequest` | 会话终止请求（M5.5 起仅允许终止本人其他会话，兼容 `terminate_session` 类型名） |
| `17` | `ForceLogoutResponse` | 会话终止响应 |
| `20` | `SearchUsersRequest` | 用户搜索请求 |
| `21` | `SearchUsersResponse` | 用户搜索响应 |
| `22` | `AddContactRequest` | 添加联系人请求 |
| `23` | `AddContactResponse` | 添加联系人响应 |
| `24` | `GetContactsRequest` | 获取联系人列表请求 |
| `25` | `GetContactsResponse` | 获取联系人列表响应 |
| `30` | `GetConversationsRequest` | 获取会话列表请求 |
| `31` | `GetConversationsResponse` | 获取会话列表响应 |
| `32` | `SendMessageRequest` | 发送消息请求 |
| `33` | `SendMessageResponse` | 发送消息响应 |
| `34` | `NewMessageNotification` | 新消息通知（服务端推送） |
| `35` | `AckMessageRequest` | 消息确认请求 |
| `36` | `AckMessageResponse` | 消息确认响应 |
| `37` | `SyncMessagesRequest` | 同步消息请求 |
| `38` | `SyncMessagesResponse` | 同步消息响应 |
| `39` | `MessageStatusUpdate` | 消息状态更新（服务端推送，M5.5 起由回执聚合触发） |
| `40` | `SyncEventsRequest` | 账号级增量同步请求（M5.5） |
| `41` | `SyncEventsResponse` | 账号级增量同步响应（M5.5） |
| `50` | `RegisterKeysRequest` | E2EE 密钥注册请求（M6：身份公钥 + 预密钥公钥） |
| `51` | `RegisterKeysResponse` | E2EE 密钥注册响应（M6） |
| `52` | `FetchKeysRequest` | E2EE 密钥包拉取请求（M6） |
| `53` | `FetchKeysResponse` | E2EE 密钥包拉取响应（M6） |
| `60` | `CreateGroupRequest` | 创建群组请求（M7a） |
| `61` | `CreateGroupResponse` | 创建群组响应（M7a） |
| `62` | `InviteGroupMembersRequest` | 邀请群成员请求（M7a） |
| `63` | `InviteGroupMembersResponse` | 邀请群成员响应（M7a） |
| `64` | `LeaveGroupRequest` | 退出群组请求（M7a） |
| `65` | `LeaveGroupResponse` | 退出群组响应（M7a） |
| `66` | `KickGroupMemberRequest` | 移除群成员请求（M7a） |
| `67` | `KickGroupMemberResponse` | 移除群成员响应（M7a） |
| `68` | `GetGroupInfoRequest` | 获取群信息请求（M7a） |
| `69` | `GetGroupInfoResponse` | 获取群信息响应（M7a） |
| `70` | `GroupChangedNotification` | 群变更通知（服务端推送，M7a：成员变更/系统消息） |
| `71` | `FetchGroupKeysRequest` | 群 E2EE 密钥包拉取请求（M7b：一次性返回全群成员密钥包） |
| `72` | `FetchGroupKeysResponse` | 群 E2EE 密钥包拉取响应（M7b） |
| `80` | `ReadCursorNotification` | 已读游标推送（服务端推送，M9：已读者自身多端已读同步） |
| `81` | `SetConversationPrefsRequest` | 会话偏好设置请求（M9：置顶/免打扰） |
| `82` | `SetConversationPrefsResponse` | 会话偏好设置响应（M9） |
| `83` | `ConversationPrefsNotification` | 会话偏好变更通知（服务端推送，M9：本人多端同步） |
| `84` | `EditMessageRequest` | 消息编辑请求（M9） |
| `85` | `EditMessageResponse` | 消息编辑响应（M9：仅本端请求响应，requestId 命中在途编辑才处理） |
| `86` | `DeleteMessageRequest` | 消息删除请求（M9） |
| `87` | `DeleteMessageResponse` | 消息删除响应（M9：仅本端请求响应，requestId 命中在途删除才处理） |
| `88` | `MessageEditedNotification` | 消息编辑实时推送（服务端推送，M9 欠账修复：覆盖全体成员 **含操作者本人的其他设备**，发起设备按 payload.`senderId`+`originDeviceId` 自行去重；requestId=0。旧方案复用 `EditMessageResponse`靠 `requestId==0` 区分，现已拆分） |
| `89` | `MessageDeletedNotification` | 消息删除实时推送（服务端推送，M9 欠账修复：语义同 `88`，与 `DeleteMessageResponse` 分离） |

### 注册请求

```json
{
  "type": "register",
  "username": "newuser",
  "password": "<plaintext-password>",
  "email": "user@example.com",
  "phone": "13800000000"
}
```

- `username`：必填，3-32 字符
- `password`：必填，至少 6 字符，服务端使用 PBKDF2-HMAC-SHA256 存储
- `email`：可选
- `phone`：可选

### 注册响应

```json
{
  "code": 0,
  "message": "Account created successfully",
  "data": {
    "userId": 1,
    "username": "newuser"
  }
}
```

### 登录请求

```json
{
  "type": "login",
  "username": "admin",
  "password": "<plaintext-password>",
  "clientVersion": "0.2.0",
  "platform": "windows",
  "deviceId": "<machine-id-hex>",
  "timestamp": 1753977600,
  "nonce": "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
}
```

- 当前客户端（`NetworkManager::sendLoginRequest`）直接传输 QML 输入的**原始密码**，依赖 TLS 保护传输过程；`encryptPassword()`（SHA-256 摘要）虽存在但登录流程未调用
- 服务端使用 PBKDF2-HMAC-SHA256（100,000 次迭代 + 16 字节随机盐）存储密码验证数据
- 登录成功后返回 session token

> 2026-08-03 校正：旧版本文档描述登录传输 SHA-256 摘要，与代码实现不符。SHA-256 预散列对传输安全没有实质增益（摘要本身即成为传输凭据），保持明文 + TLS 的做法与主流 IM 一致；在 TLS 强制开启（fail-closed）之前，登录凭据存在明文传输风险。

### 登录响应

```json
{
  "code": 0,
  "message": "OK",
  "data": {
    "username": "admin",
    "userId": 1,
    "token": "<session-token-hex>",
    "expiresAt": "2026-08-05T12:00:00"
  }
}
```

### 登出请求

需要已认证 session。

```json
{
  "type": "logout"
}
```

### Token 续期请求

需要已认证 session。M5.5 起服务端会校验请求携带的 `token` 与当前 session 的 token 哈希是否一致，不一致返回 `SessionInvalid`。

```json
{
  "type": "token_renew",
  "token": "<current-session-token>"
}
```

### 会话终止请求（原强制下线）

需要已认证 session。M5.5 起 `force_logout` 的越权语义已移除：请求只能以 `sessionId` 或 `deviceId` 为目标，且目标必须属于**本人**的其他会话（不能终止当前会话，当前会话请用 `logout`）。新类型名 `terminate_session` 与旧名 `force_logout`、`messageType=16` 均兼容。

```json
{
  "type": "terminate_session",
  "sessionId": 5
}
```

```json
// 响应 data
{ "terminatedSessionId": 5 }
```

- 携带非本人的 `userId` 将被拒绝（`PermissionDenied`）。
- 被终止会话对应的连接会被服务端主动断开。
- 管理员踢人能力不在此接口范围内，需独立鉴权通道（未实现）。

### 标准响应

所有响应 payload 统一为：

```json
{
  "code": 0,
  "message": "OK",
  "data": {}
}
```

### 错误码

| code | 名称 | 含义 |
| --- | --- | --- |
| `0` | `Ok` | 成功 |
| `1000` | `InvalidRequest` | 请求格式、类型或 payload 非法 |
| `1001` | `UnsupportedVersion` | 协议版本不支持 |
| `1002` | `ReplayRejected` | 重放保护拒绝：timestamp/nonce 缺失、格式错误、超时或重复（M5.5） |
| `1003` | `RateLimited` | 非登录类请求频率超限：发消息 / 搜索 / 密钥拉取 / 消息编辑删除 / 会话偏好（M11 前置 + M9 欠账修复） |
| `2001` | `AuthenticationFailed` | 用户名或密码错误 |
| `2002` | `AccountAlreadyExists` | 用户名已存在 |
| `2003` | `AccountNotFound` | 用户不存在 |
| `2004` | `SessionExpired` | Session 已过期 |
| `2005` | `SessionInvalid` | Session 无效或未认证 |
| `2006` | `LoginRateLimited` | 登录失败次数过多，触发限流 |
| `2007` | `TooManyDevices` | 设备数量超限 |
| `3001` | `ContactAlreadyExists` | 联系人已存在 |
| `3002` | `ContactNotFound` | 联系人不存在 |
| `3003` | `ConversationNotFound` | 会话不存在 |
| `3004` | `MessageNotFound` | 消息不存在 |
| `3005` | `CannotSendToSelf` | 不能给自己发送消息 |
| `3006` | `PermissionDenied` | 越权访问被拒绝：非会话成员、非本人会话等（M5.5） |
| `3007` | `KeyBundleUnavailable` | 对方无可用设备或预密钥耗尽，无法建立加密会话（M6） |
| `3008` | `E2eeInvalidEnvelope` | 消息密文 envelope 非法：格式错误、预密钥无效或重复设备条目（M6） |
| `3009` | `GroupLimitExceeded` | 群数量或成员数超限（M7a） |
| `3010` | `MemberAlreadyExists` | 被邀请者已在群中（M7a） |
| `3011` | `MemberNotFound` | 目标不是群成员（M7a） |
| `3012` | `NotGroupOwner` | 仅群主可执行的管理操作（M7a） |
| `9001` | `Timeout` | 连接空闲超时 |
| `9002` | `InternalError` | 服务端内部错误 |

### 心跳

客户端连接后定时发送 `Ping`，服务端返回相同 `requestId` 的 `Pong`。服务端连接空闲 90 秒会发送 `Timeout` 错误并断开连接。

### 认证流程

1. 客户端连接服务端（TLS fail-closed：服务端无证书拒绝启动，客户端无 CA 拒绝连接；开发明文需显式开关）
2. 发送 `LoginRequest`，服务端验证密码后返回 session token
3. 后续每个已认证请求在 payload 携带 `token` 字段（当前 session token）；服务端 `validateSession()` 逐包回查 `sessions` 表并比对 `hashToken(token)` 与 `token_hash`，同时校验 `expires_at` 未过期（`token_renew` 豁免过期门），任一不符回 `Error`（`SessionInvalid`）（2026-09-02 P1 修复；此前仅连接级内存态 + 续期校验 token）
4. 客户端可发送 `TokenRenewRequest` 续期 token（旧 session 删除，新 session 生效）
5. 客户端发送 `LogoutRequest` 主动登出；或用 `terminate_session` 终止本人其他设备的会话
6. 服务端在连接断开时自动清理 session

> 已知限制：断线重连后必须重新登录。逐包验 token 已于 2026-09-02 实施（每个已认证请求携带 `token`，服务端逐包回查 `sessions` 表 + 比对哈希 + 过期校验，撤销/过期即时生效）。客户端自动续期与失效自动重登已于 2026-09-04 实施（解析 `expiresAt` 过期前自动 `renewToken`，失效经 `sessionExpired` 回登录页）。后续方向：可选 TLS channel 绑定进一步加固。

### 限流策略

限流均为**连接级**（一条连接对应一个已认证用户+设备），采用固定窗口计数（M11 前置统一为 `RateWindow`）。

- **登录**（`login`）：同一 IP 5 分钟内最多 10 次失败、同一用户 5 分钟内最多 5 次失败；触发返回 `LoginRateLimited (2006)`，并记录 `login_audit`。
- **发消息**（`send_message`，私聊/群聊同一入口）：每连接 10 秒内最多 30 条；超限返回 `RateLimited (1003)`。客户端视为瞬时失败——保留 outbox 并短退避后自动重刷，不丢消息。
- **搜索**（`search_users`）：每连接 60 秒内最多 20 次；超限返回 `RateLimited (1003)`，抑制用户名枚举/刷库。
- **密钥拉取**（`fetch_keys` 与 `fetch_group_keys` 共享窗口）：每连接 60 秒内最多 20 次；超限返回 `RateLimited (1003)`（M11 前由 `LoginRateLimited` 迁移而来），防止恶意耗尽他人预密钥池。
- **消息编辑/删除**（`edit_message` 与 `delete_message` 共享窗口，M9 欠账修复）：每连接 60 秒内最多 20 次；超限返回 `RateLimited (1003)`。这类操作每次按会话成员数写 `sync_events` + fan-out（O(N) 放大），需限流防刷库/DB 膨胀。
- **会话偏好**（`set_conversation_prefs`，M9 欠账修复）：每连接 60 秒内最多 30 次；超限返回 `RateLimited (1003)`。

> `LoginRateLimited (2006)` 自 M11 起仅用于登录限流；其余请求限流统一使用通用 `RateLimited (1003)`。

## 已知限制

- 当前协议兼容策略只支持版本 `1`，后续版本升级需要扩展协商或降级策略。
- Session token 自 2026-09-02 起逐包校验：每个已认证请求携带 `token`，服务端 `validateSession()` 回查 `sessions` 表并比对哈希 + 过期（此前仅连接级内存态 + `TokenRenewRequest` 校验）。
- nonce 去重缓存为单服务器内存 TTL 缓存（跨连接共享），服务端重启后清空；多服务器部署时需改为持久化存储。
- E2EE 覆盖一对一文本消息（M6）与群聊消息（M7b Sender Keys）；媒体消息仍为服务端可见形态（M8 目标）；群路径服务端仍兼容接受 `contentType=text` 明文（M7a 遗留形态，客户端已不产生，收紧为拒绝属后续选项）。
- 群成员变更的 Sender-Key healing 与失权回收已实现（2026-09-02）：`member_added/removed/left` 触发本端 sender key 轮换（新 `keyId`）+ 重分发，新成员获得当前密钥、被移除成员因轮换失去后续消息解密能力（后向安全）；离线期间的成员变更经 `sync_events` 补偿。残留：大群（成员设备数约 >60）单条分发消息可能超 16384 字符上限（见 ROADMAP 欠账 P2）。
- 设备信任为 TOFU，无安全码/二维码带外验证；密钥备份与设备间迁移未实现（更换设备/清除应用数据后无法解密历史消息，但同一设备登出重登不受影响）。
- 预密钥超时回收阈值为 10 分钟；发送方在认领后 10 分钟内仍可正常消费。
- 客户端解密缓存与本地持久化 outbox 均已实现（M6.5：`LocalStore` 加密落库，重启后自动重发且幂等不重复）。
- 消息编辑/删除与会话置顶/免打扰已于 2026-09-05 实施（M9 特性栈：协议类型 81-87 + V9 迁移 + `conversation_prefs`/`message_edited`/`message_deleted` 三类 `sync_events` 事件，多端与离线一致）；`sync_events` 保留清理已于 2026-09-04 落地（30 天保留 + 落后设备全量回退）。会话整表删除仍未实现（无对应接口/事件，属后续）。

## M5 新增：传输层加密

- 服务端使用 `QSslSocket` + TLS 1.2+ 监听。
- 客户端使用 `connectToHostEncrypted()` 建立加密连接。
- 开发环境自动生成自签名 CA + 服务端证书（SAN: localhost, 127.0.0.1）。
- 证书错误时客户端拒绝连接并提示用户。

**M5.5 fail-closed 策略**：

- 服务端：`initTls()` 失败时 `start()` 拒绝启动；仅当显式传入 `--allow-plaintext` 时才允许明文 TCP（仅开发用途，启动日志明确告警）。
- 客户端：找不到或无法加载 CA 时拒绝连接并通过登录/注册失败信号提示用户；仅当显式设置环境变量 `XYCHAT_ALLOW_PLAINTEXT=1` 时才允许明文连接。
- 因此不存在静默降级路径：要么 TLS，要么显式声明的开发明文。

## M5 新增：重放保护

所有业务请求（登录、注册、登出、消息等）的 JSON payload 中携带以下字段（**M5.5 起均为必填**）：

```json
{
  "type": "login",
  "timestamp": 1753977600,
  "nonce": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  ...
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `timestamp` | int64 | Unix 秒级时间戳，必填；服务端拒绝缺失、非数值或偏差超过 300 秒的请求 |
| `nonce` | string | UUID v4 随机字符串，必填（≤128 字符）；服务端拒绝重复 nonce |

服务端重放保护实现（M5.5）：
- 缺失、格式错误、超时、重复的业务请求一律拒绝，返回 `ReplayRejected (1002)`
- 时间戳容差：±300 秒（5 分钟）
- nonce 去重：服务端全局共享的 `NonceCache`（跨连接生效），TTL 600 秒惰性清理，上限 100000 条
- Ping/Pong 心跳不要求重放保护字段

## 测试覆盖

- `TestPacketCodec::parsesManyConsecutiveSmallPackets` 覆盖连续 1000 个小包解析。
- `TestPacketCodec::waitsForSplitLargePacket` 覆盖单个大包拆成多次到达后的解析。
- `TestEncryptionManager` 覆盖 PBKDF2 哈希、验证、token 生成；M6 新增：X25519 密钥对生成/重建、ECDH 双向一致性、HKDF 确定性、AES-GCM 加解密往返、篡改密文/IV/错误密钥必须失败、公钥指纹、envelope 编解码往返与非法输入拒绝、完整发送方/接收方密钥协商流程。
- `TestDatabaseManager` 覆盖迁移（V1-V9，含 M7a 群组数据层、M9 `sync_meta` 与 V9 置顶/免打扰/编辑/删除列）、用户注册、session 管理（含按 ID 查询 token 哈希）、登录审计、设备管理、联系人、会话、消息；M5.5 新增：会话成员/消息访问授权、`clientMessageId` 幂等去重、回执聚合、读游标单调前进、`sync_events` 游标；M6 新增：身份密钥 upsert、预密钥上传/计数、每设备一次性认领与耗尽、claimed 校验与消费、删除设备清除密钥材料、身份变更废弃旧预密钥；M7a 新增：群组创建/成员管理/角色白名单/按用户去重回执计数等 8 个用例；M9 新增：`sync_events` 保留清理、`read_cursor` 事件往返、V9 列存在性、会话偏好 set/get/回填、消息编辑/删除软删幂等（共 49 个）。
- `TestLocalStore` 覆盖本地加密缓存：磁盘字节级密文校验、持久化 outbox 幂等、登出语义、群字段与群 outbox、sender-key 持久化（M7b）、`markConversationRead` 未读重算与状态只前进（M9）。
- `TestGroupE2eeCrypto`（M7b）20 个用例：Sender-Key 原语、chain ratchet、篡改/回滚/错误签名拒绝、DoS 上限（超限 iteration 拒绝）、分发与群消息 envelope 编解码、fail-closed 校验。
- `TestSecurity` 覆盖日志脱敏、安全内存清零、TLS 证书生成与加载；M5.5 新增：nonce 首次接受/重复拒绝/空值拒绝/TTL 过期。
- 客户端登录响应按 `requestId` 匹配，不处理不属于当前登录请求的响应。
- `tests/e2e/TestGroupRepro`：双客户端群 E2EE 端到端复现工具（建群→分发→加密收发→登出重登→再发），需手动启动服务端，不纳入 CTest。
- 尚缺：自动化真实 TLS 客户端-服务端集成测试（纳入 CTest 的 e2e）。

### M3 新增接口

#### 用户搜索

```json
// 请求
{ "type": "search_users", "query": "admin" }
// 响应 data
{ "users": [{ "userId": 1, "username": "admin" }] }
```

#### 添加联系人

```json
// 请求
{ "type": "add_contact", "userId": 2 }
// 响应 data
{ "contactUserId": 2 }
```

#### 获取联系人列表

```json
// 请求
{ "type": "get_contacts" }
// 响应 data
{ "contacts": [{ "userId": 2, "username": "bob", "addedAt": "..." }] }
```

#### 获取会话列表

```json
// 请求
{ "type": "get_conversations" }
// 响应 data
{ "conversations": [{ "conversationId": 1, "type": "private", "peerUserId": 2, "peerUsername": "bob", "lastMessage": "...", "unreadCount": 0 }] }
```

#### 发送消息

```json
// 私聊请求（M5.5 起 clientMessageId 必填；M6 起 content 必须为 E2EE envelope 密文）
{ "type": "send_message", "toUserId": 2, "content": "{\"v\":1,\"devices\":[...]}", "contentType": "text", "clientMessageId": "<uuid>" }
// M7b 群聊请求（conversationId > 0 走群路径；客户端正常发送为 e2ee_group 密文，envelope 同样受 ≤16384 字符上限约束）
{ "type": "send_message", "conversationId": 9, "content": "{\"v\":1,\"type\":\"group_e2ee\",...}", "contentType": "e2ee_group", "clientMessageId": "<uuid>" }
// 响应 data
{ "messageId": 1, "conversationId": 1, "clientMessageId": "<uuid>", "status": "sent" }
```

M7a 分流规则：请求携带 `conversationId > 0` 时走群聊路径（会话必须存在且 type=group，发送者必须是成员，否则 `ConversationNotFound`/`PermissionDenied`；幂等重试语义与私聊一致）；否则走私聊 `toUserId` 路径。同一请求不得混用两种目标。

M7b 群消息 contentType 约束：群路径仅接受 `text`（M7a 兼容形态，客户端已不再产生）、`sender_key_distribution` 与 `e2ee_group` 三类（正文 ≤16384 字符限制适用于全部类型）；`e2ee_group` 与 `sender_key_distribution` 入库前必须通过服务端 fail-closed 校验（见“M7b 新增：群聊端到端加密”），非法返回 `E2eeInvalidEnvelope (3008)`；服务端内部产生的系统消息为 `contentType=system`。

`clientMessageId` 为客户端生成的 UUID 幂等键（参考 Telegram `random_id`/WhatsApp 客户端消息 ID）：服务端以 `(sender_id, sender_device_id, client_message_id)` 唯一约束去重，重试/重连重发返回已存储的同一条消息（幂等重试优先于 envelope 校验，因为重试时引用的预密钥可能已被首次发送消费）；客户端维护 outbox，登录成功后自动重发未确认消息。

M6 fail-closed 校验：`content` 必须解析为合法的 v1 envelope（见“M6 新增：端到端加密”）；每个接收方设备条目引用的 `prekeyId` 必须属于接收方且处于 `claimed` 状态，接收方条目无重复设备；允许额外携带一个发送方自身设备的拷贝条目（`deviceId` 为发送方设备 + `prekeyId=0`，仅身份密钥加密，不消费预密钥）；至少需要一个接收方条目，否则返回 `E2eeInvalidEnvelope (3008)`。消息入库与预密钥消费（`claimed -> used`）在同一事务内完成，保证一次性投递；服务端全程只见密文。

#### 新消息通知（服务端推送）

```json
{ "messageId": 1, "conversationId": 1, "senderId": 2, "content": "Hi!", "contentType": "text", "createdAt": "..." }
```

M7a 群消息额外携带 `senderUsername`（便于 UI 展示发送者）；群系统消息的 `contentType` 为 `system`，`content` 为结构化 JSON（见“M7a 新增：群组接口”）。

#### 消息确认（回执）

```json
// 请求
{ "type": "ack_message", "messageId": 1, "status": "delivered" }
// 响应 data
{ "messageId": 1, "status": "delivered" }
```

M5.5 行为：
- 先授权再更新：请求者必须是消息所属会话的成员，否则返回 `PermissionDenied (3006)`。
- `status` 仅接受 `delivered` / `read`。
- 回执写入 `message_receipts(message_id, user_id, device_id, delivered_at, read_at)`，按接收者/设备维度记录（参考 WhatsApp per-recipient 回执模型）；多设备各自回执互不覆盖。
- 服务端根据回执聚合更新 `messages.status` 展示值，并向发送方推送 `MessageStatusUpdate`，同时写入发送方 `sync_events`；M9 起 `status=read` 时额外向已读者自身写入 `read_cursor` 事件并经 `ReadCursorNotification` 推送给其所有在线设备，实现同账号多端已读同步。

M7a 聚合语义：展示状态按**接收用户人数**聚合（接收者 = 会话成员中除发送方外的全体，私聊为 1 人）：全员送达才达 `delivered`，全员已读才达 `read`；同一用户多设备回执按用户去重（`receiptUserCount`），不会提前达成。`MessageStatusUpdate` 与发送方 `receipt` 事件额外携带 `deliveredCount`/`readCount`（群消息送达/已读计数）。

#### 同步消息

```json
// 请求
{ "type": "sync_messages", "conversationId": 1, "afterId": 0, "limit": 100 }
// 响应 data
{ "conversationId": 1, "messages": [...], "hasMore": false }
```

M5.5 行为：先授权再查询 —— 非会话成员返回 `PermissionDenied (3006)`；拉取仅前进成员读游标，不再隐式修改全局消息状态（已读回执由显式 `ack_message` 产生）。

#### 账号级增量同步（M5.5 新增）

```json
// 请求
{ "type": "sync_events", "afterSeq": 0, "limit": 200 }
// 响应 data
{ "events": [{ "seq": 1, "type": "message", "payload": { ... }, "createdAt": "..." }], "lastSeq": 1, "hasMore": false, "needsFullSync": false }
```

- 事件流按账号维度严格递增（`seq`），客户端保存 `lastSeq` 游标做增量拉取（参考 Telegram 差分同步模型）。
- 当前事件类型：`message`（新消息，M6 起私聊 payload.content 为 envelope 密文，M7a 群聊为明文；群系统消息 contentType=system）、`contact_added`（联系人变更）、`receipt`（送达/已读回执，M7a 起含 deliveredCount/readCount）、`group_changed`（M7a：群成员变更，payload 同 `GroupChangedNotification`）、`read_cursor`（M9：已读者自身读游标，payload `{conversationId, readMessageId}`，供其其他设备同步未读角标与消息已读态）、`conversation_prefs`（M9：会话偏好变更，payload `{conversationId, pinned, muted}`）、`message_edited`（M9：消息编辑，payload 含 `messageId`/`conversationId`/**`senderId`**/`originDeviceId`/重新加密的 `content`/`contentType`/`editedAt`）、`message_deleted`（M9：消息删除，payload `{messageId, conversationId, senderId, originDeviceId, deletedAt}`）。
  - **`senderId` 为群聊解密的必需字段**（2026-09-09 补）：`e2ee_group` 密文靠（群, 发送者 userId, 发送者 deviceId, keyId）四元组定位本地 Sender Key，缺 `senderId` 即无法解密；为兼容修复前已落库的旧事件，客户端在 `senderId` 缺失时会按（群, 设备, keyId）反查发送者。
  - **`originDeviceId`** 为发起该操作的本端设备 ID；实时推送与离线补偿两路径均覆盖操作者本人（保障其名下其他设备实时一致）。接收端须**同时比对 `senderId == 本端 userId` 且 `originDeviceId == 本端 deviceId`** 才忽略，避免发起设备回显自身操作。注意：`deviceId` 为机器级标识（`QSysInfo::machineUniqueId`），同机多账号共享，仅比对 `deviceId` 会把同机其他账号误判为“本设备”而丢事件（2026-09-09 修正）。
- M9 保留清理：服务端按 30 天保留期每小时清理过期 `sync_events`（`sync_meta` 表记录清理水位线 `pruned_below_seq`）；设备游标落后于水位线（`0 < afterSeq < prunedBelowSeq`）时响应 `needsFullSync=true` + `fullSyncSeq`，客户端重置游标并全量重拉会话（`get_conversations`）与消息（`sync_messages` 从 messages 表补齐，不受事件清理影响）。
- 实时推送（`NewMessageNotification`/`MessageStatusUpdate`）仅作为通知，离线或丢推送时由 `sync_events` 兜底补齐。

### M6 新增：端到端加密

#### 密码学方案（简化 Signal）

| 环节 | 算法 |
| --- | --- |
| 身份/预密钥/临时密钥 | X25519（32 字节原始格式，传输/存储用 Base64） |
| 密钥协商 | `shared = ECDH(eph_priv, peer_prekey_pub) ‖ ECDH(eph_priv, peer_identity_pub)` |
| 密钥派生 | HKDF-SHA256(shared, salt="xychat-e2ee-v1") → 32 字节消息密钥 |
| 消息加密 | AES-256-GCM，随机 12 字节 IV，16 字节认证标签附在密文末尾 |
| 设备信任 | TOFU：首次记录对方身份公钥 SHA-256 指纹（前 16 字节 hex），变更时告警不阻塞 |

每条消息都使用全新的临时密钥对（前向安全）；一次性预密钥被服务端认领即消费，解密成功后客户端删除对应预密钥私钥。

#### 密钥注册（register_keys）

需要已认证 session；`deviceId` 取自 session，不信任请求参数（只能注册自己的密钥）。

```json
// 请求（prekeys 可选；单批 ≤100 个，每设备未认领总量上限 500）
{ "type": "register_keys", "identityPub": "<base64-32B>", "prekeys": ["<base64-32B>", ...] }
// 响应 data
{ "deviceId": "<device-id>", "uploadedPrekeys": 20, "remainingPrekeys": 20 }
```

- 身份公钥变更（重装/密钥丢失后重新生成）时，服务端自动废弃该设备旧世代的全部 `unused`/`claimed` 预密钥，避免发送方认领到接收方无法解密的旧预密钥。
- 客户端登录成功后自动引导：加载/生成身份密钥 → 注册 → 预密钥余量（本地或服务端报告）低于 5 时补齐到 20。

#### 密钥包拉取（fetch_keys）

```json
// 请求
{ "type": "fetch_keys", "userId": 2 }
// 响应 data（每设备一个 bundle：身份公钥 + 一个认领的预密钥）
{ "userId": 2, "bundles": [{ "deviceId": "...", "identityPub": "<b64>", "prekeyId": 7, "prekeyPub": "<b64>" }] }
```

- 服务端在事务内为目标用户每个有库存的设备原子认领（`unused -> claimed`）一个预密钥；认领后 10 分钟未被消费自动回退为 `unused`（防泄漏）。
- 目标无设备返回 `AccountNotFound`；无可用预密钥返回 `KeyBundleUnavailable (3007)`。
- 连接级频率限制（60 秒内 ≤20 次），超限返回 `RateLimited (1003)`，防止恶意耗尽他人预密钥池。
- 认领的密钥包仅供一条消息使用：消息入库时预密钥转为 `used`；发送方放弃时由超时回收兜底。

#### 消息 envelope 格式

`send_message` 的 `content` 为以下 JSON 的紧凑序列化（多设备时 `devices` 逐设备一个条目）：

```json
{
  "v": 1,
  "devices": [
    {
      "deviceId": "<receiver-device-id>",
      "prekeyId": 7,
      "eph": "<base64 发送方临时 X25519 公钥>",
      "iv": "<base64 12B GCM IV>",
      "ct": "<base64 密文 + 16B GCM 标签>"
    },
    {
      "deviceId": "<sender-device-id>",
      "prekeyId": 0,
      "eph": "<base64 另一个临时公钥>",
      "iv": "<base64 12B GCM IV>",
      "ct": "<base64 密文 + 16B GCM 标签>"
    }
  ]
}
```

- 接收方条目（`prekeyId > 0`）：接收方按 `deviceId` 找到自己的条目，逐个本地预密钥尝试解密（GCM 认证标签验证正确性）；成功即删除该预密钥私钥。
- 发送方自身拷贝（`prekeyId = 0`，2026-08-20 修复新增）：仅用发送方本人身份密钥加密（`shared = ECDH(eph, identity) ‖ ECDH(eph, identity)`），不消费预密钥；使发送方重新登录或多端同步后仍能解密自己发出的消息。服务端对该条目不校验预密钥，仅要求属于发送方当前设备且最多一条。
- 无本机条目或缺少对应预密钥私钥（新设备/历史消息）时显示“无法解密此消息”占位；**历史消息不可恢复**仅限真正丢失密钥材料的场景；已解密过的消息由客户端持久化解密缓存（DPAPI 保护）兜底，登出重登后仍可显示。
- M6 前的存量明文消息保持原样展示；会话列表预览对 envelope 显示 `[Encrypted message]`。
- 对方尚未注册密钥（从未登录）时，`fetch_keys` 返回 `AccountNotFound`/`KeyBundleUnavailable`；客户端保留消息在 outbox 并每 30 秒重试，对方首次登录注册密钥后自动送达（不会丢弃）。

### 消息状态

| 状态 | 含义 |
| --- | --- |
| `sending` | 客户端正在发送 |
| `sent` | 服务端已接收并存储 |
| `delivered` | 接收方已收到 |
| `read` | 接收方已读 |
| `failed` | 发送失败 |

> M5.5 说明：送达/已读的权威记录在 `message_receipts`（按接收者/设备维度，支持多设备聚合）；`messages.status` 仅作为由回执聚合得出的展示值，发送链路状态（sending/sent/failed）仍由客户端维护。

## 协议演进记录（M5.5 已实施）

以下变更基于 2026-08-03 代码审查与主流 IM（Telegram MTProto、WhatsApp/Signal 的同步与回执模型）参考，已在 M5.5 落地并附自动化测试：

| 变更 | 状态 | 参考 |
| --- | --- | --- |
| `timestamp`/`nonce` 强制必填 + 全局 TTL 去重 | ✅ 已实施 | Signal/Telegram 的 msg_id + salt 防重放 |
| `clientMessageId` 幂等键 + 唯一约束 + 客户端 outbox | ✅ 已实施 | Telegram `random_id`、WhatsApp 客户端消息 ID |
| `sync_events` + 账号游标接口 | ✅ 已实施（消息/联系人/回执） | Telegram updates 差分同步 |
| `ack_message` 接收者回执模型（`message_receipts`） | ✅ 已实施 | WhatsApp 蓝勾模型（per-recipient receipt） |
| `force_logout` → `terminate_session`（仅本人会话） | ✅ 已实施 | Telegram sessions.killSession |
| 续期接口真正校验 token | ✅ 已实施 | OAuth2 access token 验证 |
| 全部命令逐包携带并验证 access token / TLS channel 绑定 | ⬜ 未实施（后续） | MTProto auth_key 绑定 |
| 一对一 E2EE：X25519 身份密钥 + 一次性预密钥 + 每消息临时密钥 | ✅ 已实施（M6） | Signal PreKey 消息模式（简化） |
| envelope fail-closed：服务端只存/只转密文 | ✅ 已实施（M6） | Signal 服务端不可见明文 |
| 预密钥认领超时回收 + 身份变更废弃旧世代 + fetch_keys 限流 | ✅ 已实施（M6，代码审查后修复） | Signal 预密钥生命周期管理 |
| 客户端本地加密持久化缓存 + 持久化 outbox（无线上协议变更） | ✅ 已实施（M6.5） | Telegram/WhatsApp 本地存储模型；复用 sync_events 游标与 clientMessageId 幂等 |
| M7a 明文群聊：群组接口 + send_message 分流 fan-out + 系统消息 + 按人数回执聚合 | ✅ 已实施（M7a 子任务一/二） | Telegram/WhatsApp 群模型（服务消息、per-recipient 回执聚合） |
| M7b 群聊 E2EE：Sender Keys 分发 + `fetch_group_keys` + 群 envelope fail-closed + DoS 上限 | ✅ 已实施 | Signal Sender Keys（简化）；ratchet 跳跃上限参考 Signal skipped-key 策略 |
| M9 特性栈：会话置顶/免打扰 + 消息编辑/删除（软删除留墓碑）+ 三类 sync_events 事件 | ✅ 已实施（2026-09-05） | Telegram/WhatsApp 会话静音与消息编辑/删除语义；编辑/删除经 sync_events 多端同步 |

后续协议方向：大群拉取/游标模式、改群名接口（`name_changed`）；媒体分片上传走独立通道（M8）。会话整表删除接口仍缺（无对应事件类型）。

## M7a 新增：群组接口（三个子任务均已完成：定义/服务端处理器/客户端接入）

`send_message` 按会话类型分流：private 会话维持 M6 pairwise envelope fail-closed；group 会话自 M7b 起客户端发送 `contentType=e2ee_group` 密文（服务端仍兼容 `text` 明文形态，见下文 M7b 章节与“已知限制”）。所有群组请求均需已认证 session 并携带 timestamp/nonce。

### 创建群组（create_group）

```json
// 请求（name 1-64 字符；memberIds 不含创建者自身，服务端自动去重/剔除；单次邀请 ≤100）
{ "type": "create_group", "name": "项目群", "memberIds": [2, 3], "timestamp": ..., "nonce": "..." }
// 响应 data
{ "conversationId": 9, "name": "项目群", "memberCount": 3 }
```

创建者自动成为群主（role=`owner`）；成员数含创建者上限 200（超限拒绝，`GroupLimitExceeded`）；初始成员必须均为已注册用户（否则 `AccountNotFound`）。建群成功后产生 `group_created` 系统消息与群变更通知。

### 邀请成员（invite_group_members）

```json
// 请求（仅群成员可邀请；已在群中的用户跳过）
{ "type": "invite_group_members", "conversationId": 9, "userIds": [4] }
// 响应 data
{ "conversationId": 9, "added": [4], "memberCount": 4 }
```

### 退群（leave_group）

```json
// 请求
{ "type": "leave_group", "conversationId": 9 }
// 响应 data
{ "conversationId": 9 }
```

已落地语义（降级策略）：群主退群时若仍有其他成员，群主身份自动转让给最早入群的成员（产生 `owner_transferred` 系统消息与通知）；退出者本人不再接收后续群消息与变更通知。

### 移除成员（kick_group_member）

```json
// 请求（层级保护：owner 可移除 admin/member；admin 仅可移除 member；不能移除自己，越权返回 NotGroupOwner）
{ "type": "kick_group_member", "conversationId": 9, "userId": 4 }
// 响应 data
{ "conversationId": 9, "removedUserId": 4 }
```

非成员目标返回 `MemberNotFound`；被移除者不再接收后续群消息与变更通知（其本地会话由客户端清理）。

### 获取群信息（get_group_info）

```json
// 请求（仅群成员可查询，越权返回 PermissionDenied）
{ "type": "get_group_info", "conversationId": 9 }
// 响应 data
{ "conversationId": 9, "name": "项目群", "memberCount": 3, "myRole": "owner", "members": [{ "userId": 2, "username": "bob", "role": "member", "joinedAt": "..." }] }
```

### 群变更通知（GroupChangedNotification，服务端推送）

```json
{ "conversationId": 9, "changeType": "member_added", "operatorId": 1, "targetUserId": 4, "memberCount": 4 }
```

`changeType` 取值：`group_created` / `member_added` / `member_removed` / `member_left` / `owner_transferred`（后续可扩展 `name_changed`）。每次成员变更同时产生一条 `contentType=system` 的系统消息（`content` 为结构化 JSON，如 `{"event":"member_added","operatorId":1,"targetUserIds":[4]}`），与群变更通知一起写入成员 `sync_events`，离线成员上线后可经游标同步补齐。

### 群聊与既有接口的兼容约定

- `get_conversations` 响应中 group 会话额外携带 `name` 与 `memberCount`；private 会话字段不变（已实现）。
- `send_message`/`sync_messages`/`ack_message`/`sync_events` 对 group 会话沿用既有语义（成员授权、幂等键、游标）；群消息送达/已读计数已由 `MessageStatusUpdate` 的 `deliveredCount`/`readCount` 提供（已实现）。
- `NewMessageNotification` 对群消息额外携带 `senderUsername`（便于 UI 展示发送者，已实现）。
- 群消息 fan-out 策略：当前为小群直推（逐成员在线推送 + 全员 sync_events 兜底）；大群拉取/游标模式随规模需求再引入。

### 角色模型

| role | 权限（已实现） |
| --- | --- |
| `owner` | 全部管理权限：邀请/移除成员（含 admin）、退群时自动转让 |
| `admin` | 邀请成员、移除普通成员 |
| `member` | 收发消息、邀请新成员、退群 |

> 改群名接口（`name_changed`）未纳入子任务二，数据层 `setGroupName` 已就绪，接口层留待后续。

## M7b 新增：群聊端到端加密（Sender Keys）

简化 Signal Sender-Key 方案：每个发送方在每个群独立生成 `SenderKey`（32 字节 chain key + Ed25519 签名密钥对，`keyId` = SHA-256(签名公钥) hex 前 32 字符）；每条群消息由 chain key 经 HKDF-SHA256 ratchet（salt `xychat-grp-chain`）派生消息密钥，AES-256-GCM 加密并由发送方私钥签名（覆盖 `iv || ciphertext`）。服务端只存/只转密文，无法读取群消息正文。

### 密钥包拉取（fetch_group_keys，类型 71/72）

需要已认证 session 且仅限群成员（越权返回 `PermissionDenied`）；与 `fetch_keys` 共享连接级限流窗口（60 秒 ≤20 次，超限返回 `RateLimited (1003)`）。

```json
// 请求
{ "type": "fetch_group_keys", "conversationId": 9, "timestamp": ..., "nonce": "..." }
// 响应 data（bundles 按用户 ID 分组，每设备一个 bundle；事务内逐设备认领预密钥，语义同 fetch_keys）
{ "conversationId": 9, "bundles": { "2": [{ "deviceId": "...", "identityPub": "<b64>", "prekeyId": 7, "prekeyPub": "<b64>" }], "3": [...] } }
```

### Sender-Key 分发（contentType=sender_key_distribution）

发送方首次在某群发言前，先拉取全群成员密钥包，将 chain key（base64）用 M6 pairwise envelope 逐成员逐设备加密，以一条群消息分发（同样占用 `clientMessageId` 幂等键，经群 fan-out 投递）：

```json
{
  "v": 1,
  "type": "sender_key_distribution",
  "groupId": 9,
  "senderUserId": 1,
  "senderDeviceId": "<sender-device>",
  "keyId": "<sha256-hex-32>",
  "publicSigningKey": "<b64 Ed25519 公钥>",
  "devices": [
    { "userId": 2, "deviceId": "<receiver-device>", "prekeyId": 7, "eph": "<b64>", "iv": "<b64>", "ct": "<b64 chain key 密文>" }
  ]
}
```

接收方解密出 chain key 后写入本地 `LocalStore.sender_keys` 表（加密落库，登出保留）；分发消息只处理、不展示、不作为普通消息落库。服务端 fail-closed：`decodeDistribution` 解析失败、条目为空或 `groupId` 与会话不一致时拒绝入库（`E2eeInvalidEnvelope 3008`）。

### 群消息 envelope（contentType=e2ee_group）

```json
{
  "v": 1,
  "type": "group_e2ee",
  "keyId": "<sha256-hex-32>",
  "iteration": 5,
  "senderDeviceId": "<sender-device>",
  "iv": "<b64 12B GCM IV>",
  "ct": "<b64 密文 + 16B GCM 标签>",
  "sig": "<b64 Ed25519 签名，覆盖 iv || ciphertext>"
}
```

- 接收方按 `(groupId, senderUserId, senderDeviceId, keyId)` 定位本地 chain key，ratchet 前进到 `iteration` 派生消息密钥解密，验签失败/回滚（iteration 倒退）/篡改均拒绝。
- DoS 防护：单次解密 ratchet 跳跃超过 `MaxRatchetSteps = 2000` 拒绝；`iteration` 超过绝对上界 `MaxMessageIteration = 1e8` 直接判非法（恶意超大 iteration 不会触发 HKDF 运算）。
- 服务端 fail-closed：`decodeGroupMessage` 解析失败或 `senderDeviceId` 为空时拒绝入库与 fan-out（`E2eeInvalidEnvelope 3008`）。
- 接收方无对应 chain key（新成员/新设备未收到分发）时显示“无法解密此消息”占位；healing（成员变更触发重分发）已于 2026-09-02 实施：`member_added/removed/left` 使本端轮换 sender key 并重新分发，离线经 `sync_events` 补偿。
- 群系统消息（`contentType=system`，服务端内部产生）不加密：仅含成员 ID/事件类型等元数据，不含用户正文。

## M9 新增：会话偏好与消息编辑/删除（2026-09-05）

### 会话偏好（置顶/免打扰）

偏好按**成员×会话**维度存储（本人多设备共享同一偏好），非设备级。

```json
// 请求（pinned/muted 接受布尔或 0/1）
{ "type": "set_conversation_prefs", "conversationId": 9, "pinned": true, "muted": false, "timestamp": ..., "nonce": "..." }
// 响应 data
{ "conversationId": 9, "pinned": true, "muted": false }
```

- 仅会话成员可设置（越权返回 `PermissionDenied`）。
- 服务端写入 `conversation_members.pinned/muted` 后，向本人所有在线设备推送 `ConversationPrefsNotification (83)`（requestId=0），并写入本人 `sync_events`（`conversation_prefs` 事件）供离线设备补偿；`get_conversations` 响应每会话携带 `pinned`/`muted` 回填。

### 消息编辑（edit_message）

```json
// 请求（content 为重新加密后的密文，contentType 须与原消息一致）
{ "type": "edit_message", "messageId": 42, "content": "<重新加密的 envelope/e2ee_group>", "contentType": "text", "timestamp": ..., "nonce": "..." }
// 响应 data（仅本端请求响应）
{ "messageId": 42, "conversationId": 9, "senderId": 3, "originDeviceId": "dev-a1", "content": "<密文>", "contentType": "text", "editedAt": "..." }
// 实时推送（MessageEditedNotification 88，requestId=0）payload 同上，覆盖全体成员含操作者本人
```

- 仅消息发送者可编辑；已删除消息与系统消息（`contentType=system`）不可编辑（`MessageNotFound`/`PermissionDenied`）。
- `contentType` 必须与原消息一致（私聊 `text`、群 `e2ee_group`），拒绝借编辑切换形态注入非法内容；`content` 长度受 `MaxGroupMessageLength` 上限约束。
- **fail-closed 密文校验**：`e2ee_group` 经 `GroupE2eeCrypto::decodeGroupMessage` 且 `senderDeviceId` 须为当前设备；`text` 经 `E2eeCrypto::decodeEnvelope` 校验为合法 envelope——服务端只见密文，拒绝明文注入（`E2eeInvalidEnvelope`）。
- 成功后服务端 `messages.edited_at = datetime('now')`，向会话全体成员写 `message_edited` 事件并专用推送 `MessageEditedNotification (88)`（requestId=0；M9 欠账修复前曾复用 `EditMessageResponse` messageType 靠 `requestId==0` 区分，现已与响应拆分）。**事件与推送必须携带 `senderId`**（群聊解密寻址所需，2026-09-09 补）与 `originDeviceId`（连同 `senderId` 供发起设备去重）；推送不再排除操作者本人，以保障其名下其他设备实时一致。
- 客户端编辑路径：群聊同步用 Sender-Key 重加密提交；私聊异步 `fetch_keys` 拉取对方密钥包后 `encryptForUser` 重加密提交（含发送方自身拷贝，使本人其他设备可解）。解密侧**不预先清缓存**，直接解密新密文（绕过缓存，`decryptEditContent`）；解密成功则覆盖本地明文（`messages.content_enc`）与持久化解密缓存（`decrypt_cache`），失败则保留既有可读明文与缓存、仅推进 `edited_at`。**幂等回退（2026-09-10）**：私聊预密钥一次性、群 ratchet 已推进，离线重放（重登后 `sync_events` 补发 `message_edited`，或实时推送与同步事件重复投递）时新密文无法二次解密；此时**不写空覆盖、不清缓存**既有可读正文，确保重登后仍能像普通消息一样按持久化解密缓存恢复明文，而非显示"无法解密"。
- **群聊乱序容忍（2026-09-09）**：编辑会消耗一个新的 ratchet 迭代，使被编辑消息的 `iteration` 大于其后发送的消息，而 `sync_messages` 按 `message_id ASC` 返回；接收端为此维护**跳序消息密钥缓存**（skipped message keys，上限 `MaxSkippedMessageKeys=1000`，本地 `sender_key_skipped` 表加密持久化），使先解到高 `iteration` 后仍能解出低 `iteration` 的在途/乱序消息；缓存命中即一次性消费，不推进链状态。

### 消息删除（delete_message）

```json
// 请求
{ "type": "delete_message", "messageId": 42, "timestamp": ..., "nonce": "..." }
// 响应 data（仅本端请求响应）
{ "messageId": 42, "conversationId": 9, "senderId": 3, "originDeviceId": "dev-a1", "deletedAt": "..." }
// 实时推送（MessageDeletedNotification 89，requestId=0）payload 同上，覆盖全体成员含操作者本人
```

- 仅消息发送者可删除；系统消息不可删（`PermissionDenied`）。
- **软删除留墓碑**：`messages.deleted = 1`、正文清空，保留 messageId/发送者/时间供客户端渲染“已删除”占位；幂等（重复删除返回成功）。
- **写入 fail-closed（2026-09-09）**：`deleteMessage` 真实写入失败时返回 `InternalError` 且**不广播事件**（旧实现忽略返回值，会在库内状态未变的情况下向全员广播删除，造成服务端与事件流分歧）；失败记 `message.delete_failed` 结构化日志。
- 成功后向会话全体成员写 `message_deleted` 事件并专用推送 `MessageDeletedNotification (89)`（requestId=0；M9 欠账修复前曾复用 `DeleteMessageResponse` messageType，现已与响应拆分）；payload 同样携带 `senderId` 与 `originDeviceId`，推送不排除操作者本人（发起设备按 `senderId`+`originDeviceId` 客户端去重）。


