# XYChat 架构概览

## 当前组件

```text
Chat-Client ── QTcpSocket/JSON ── Chat-Server ── SQLite
     │                              │
     └──── CommonModule/encryption ─┘
```

- `Chat-Client`：Qt Widgets 桌面客户端，负责登录界面、主窗口与网络请求。
- `Chat-Server`：Qt TCP 服务端，负责监听连接、读取登录请求、访问 SQLite 用户表。
- `CommonModule`：客户端和服务端共享代码，目前提供 `EncryptionManager` 密码 SHA-256 摘要函数。
- `docs`：路线图、协议、安全和架构说明。
- `tests`：自动化测试入口。

## 当前边界

- 客户端与服务端当前直接发送 JSON 文本，尚未实现帧协议。
- 服务端当前使用 SQLite，适合本地开发和原型验证。
- 当前密码处理仅为演示级 SHA-256 摘要，不满足生产密码存储要求。

## 下一步演进

M1 将优先引入共享协议层，解决 TCP 粘包/拆包、协议版本和请求响应匹配问题。M2 将升级账户体系与密码安全存储。
