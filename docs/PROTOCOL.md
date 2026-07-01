# XYChat 协议文档

## M0 当前协议状态

当前客户端与服务端使用 `QTcpSocket` 直接传输 JSON 文本。登录请求的核心字段如下：

```json
{
  "type": "login",
  "username": "admin",
  "password": "<sha256-hex>"
}
```

服务端根据 `type == "login"` 进行用户校验，并返回 JSON 响应。

## 已知限制

- TCP 是字节流，当前直接 `readAll()` 无法可靠处理粘包和拆包。
- 请求没有 `requestId`，客户端无法稳定匹配响应。
- 响应结构尚未统一为标准错误码与数据对象。
- 协议没有版本字段。

## M1 目标协议方向

后续将引入长度前缀帧协议：

```text
magic | version | messageType | requestId | payloadLength | payload(JSON)
```

Payload 初期继续使用 JSON，所有响应统一为：

```json
{
  "code": 0,
  "message": "OK",
  "data": {}
}
```
