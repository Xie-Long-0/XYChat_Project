# XYChat 协议文档

## M1 当前协议状态

当前客户端与服务端已经使用长度前缀帧协议，解决 TCP 粘包/拆包问题。Payload 仍使用 JSON，后续可替换为 Protobuf/FlatBuffers。

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
| `messageType` | `1=LoginRequest`，`2=LoginResponse`，`3=Ping`，`4=Pong`，`5=Error` |
| `requestId` | 客户端生成的请求 ID；响应沿用请求 ID |
| `payloadLength` | JSON payload 字节数，当前最大 4 MiB |

### 登录请求

```json
{
  "type": "login",
  "username": "admin",
  "password": "<sha256-hex>",
  "clientVersion": "0.1.0",
  "platform": "windows",
  "deviceId": "<machine-id-hex>"
}
```

### 标准响应

所有响应 payload 统一为：

```json
{
  "code": 0,
  "message": "OK",
  "data": {}
}
```

当前错误码：

| code | 名称 | 含义 |
| --- | --- | --- |
| `0` | `Ok` | 成功 |
| `1000` | `InvalidRequest` | 请求格式、类型或 payload 非法 |
| `1001` | `UnsupportedVersion` | 协议版本不支持 |
| `2001` | `AuthenticationFailed` | 用户名或密码错误 |
| `9001` | `Timeout` | 连接空闲超时 |
| `9002` | `InternalError` | 服务端内部错误 |

### 心跳

客户端连接后定时发送 `Ping`，服务端返回相同 `requestId` 的 `Pong`。服务端连接空闲 90 秒会发送 `Timeout` 错误并断开连接。

## 已知限制

- 当前登录凭据仍是客户端 SHA-256 摘要，尚未升级到 M2 的带盐慢哈希与 session token。
- 当前传输仍是明文 TCP，尚未启用 M4 的 TLS。
- 当前心跳只做连接保活和空闲断开，尚未包含重放保护、nonce 或会话绑定。
- 当前协议兼容策略只支持版本 `1`，后续版本升级需要扩展协商或降级策略。

## 测试覆盖

- `TestPacketCodec::parsesManyConsecutiveSmallPackets` 覆盖连续 1000 个小包解析。
- `TestPacketCodec::waitsForSplitLargePacket` 覆盖单个大包拆成多次到达后的解析。
- 客户端登录响应按 `requestId` 匹配，不处理不属于当前登录请求的响应。
