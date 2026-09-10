#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace XYChat::Protocol
{

// M8: 媒体与文件传输的共享协议约定
//
// 通道划分：控制面（申请上传、查询已收分片、宣告完成、取消、申请下载票据）复用
// 既有 TCP 主通道（消息类型 90-99，受 MaxPayloadSize 4 MiB 约束，只承载 JSON
// 元数据）；数据面（分片字节流）走独立 HTTP(S) 上传下载服务，不挤占消息长连接
// （见 ROADMAP §4.1 与 §10）。
//
// 隐私边界：文件字节在客户端加密后才上传，服务端只见密文与其自身元数据
// （字节数、分片参数、密文整体 SHA-256、上传者）。文件名、MIME、明文大小与
// 多媒体尺寸/时长属敏感元数据，只存在于 FileManifest 中，随消息正文经既有
// E2EE（私聊 envelope / 群聊 Sender-Key）加密，服务端不可见。
//
// 分片口径：本协议中的 chunkSize 一律指密文分片大小（含 16 字节 GCM 标签），
// 即客户端实际 PUT 到 HTTP 服务的字节数；服务端不需要也不应当知道明文分片边界。

inline constexpr int FileManifestVersion = 1;

// 分片与体积上限（客户端与服务端共用同一组常量，避免两侧校验口径漂移）
inline constexpr qint64 DefaultChunkSize = 1 * 1024 * 1024;     // 1 MiB 密文分片
inline constexpr qint64 MinChunkSize = 64 * 1024;               // 64 KiB
inline constexpr qint64 MaxChunkSize = 4 * 1024 * 1024;         // 4 MiB
inline constexpr qint64 MaxFileSize = 2LL * 1024 * 1024 * 1024; // 2 GiB 密文总量
inline constexpr int MaxChunkCount = 4096;                      // 单文件分片数上限
inline constexpr int MaxFileNameLength = 255;                   // 明文字符数
// 内联缩略图密文上限。不能取大值：清单随消息正文走 envelope/Sender-Key，
// 而群消息正文受 MaxGroupMessageLength(16384 字符) 约束，base64 后约 1.34 倍膨胀，
// 加上 envelope 头部开销，清单明文必须控在万字符以内。更大的缩略图应作为
// 独立文件上传并在清单里引用其 fileId（待多媒体元数据阶段实施）
inline constexpr int MaxThumbnailBytes = 4096;

// 票据与生命周期
inline constexpr int UploadTicketTtlSeconds = 24 * 3600;   // 上传票据：覆盖大文件慢速上传
inline constexpr int DownloadTicketTtlSeconds = 300;       // 下载票据：一次性、短时效
inline constexpr int StaleUploadHours = 48;                // 未完成上传保留期（超期回收）
inline constexpr int MaxConcurrentUploadsPerUser = 8;      // 每用户并发上传配额

// files.status 取值
namespace FileStatus
{
inline constexpr const char *Uploading = "uploading"; // 已创建、分片上传中
inline constexpr const char *Ready = "ready";         // 分片齐备且整体校验通过，可下载
inline constexpr const char *Cancelled = "cancelled"; // 上传者取消，数据待回收
inline constexpr const char *Failed = "failed";       // 校验失败或存储故障，数据待回收
}

// file_tickets.kind 取值
namespace FileTicketKind
{
inline constexpr const char *Upload = "upload";
inline constexpr const char *Download = "download";
}

// 文件消息清单：文件消息的密文明文形态
//
// 一条文件消息的正文（envelope/Sender-Key 解密后得到的 plaintext）不是用户文本，
// 而是本结构的 JSON 序列化。收发双方以 messages.file_id 是否大于 0 作为判别依据
// （服务端权威、随消息同步），不靠正文内容猜测，避免用户文本恰好是 JSON 时误判。
struct FileManifest
{
    qint64 fileId = 0;     // 服务端 files.id，下载凭它申请票据
    QString name;          // 原始文件名（E2EE，服务端不可见）
    QString mime;          // MIME 类型（E2EE；服务端下载恒按二进制密文投递）
    qint64 plainSize = 0;  // 加密前字节数（供 UI 显示与下载后校验）
    qint64 cipherSize = 0; // 加密后字节数，等于服务端 files.size_bytes
    QString sha256Hex;     // 密文整体 SHA-256（hex 小写），下载重组后自校验
    QByteArray key;        // 32 字节 AES-256 文件密钥（原始字节，序列化时 base64）
    QByteArray iv;         // 12 字节 nonce 前缀，分片 nonce 由它与分片序号派生

    // 预留字段：多媒体元数据。生成逻辑延后实施（需引入 QtMultimedia 与缩略图
    // 管线），字段先行定义以免后续扩展清单时破坏已有消息的兼容性
    int width = 0;         // 图片/视频宽（像素），未知为 0
    int height = 0;        // 图片/视频高（像素），未知为 0
    qint64 durationMs = 0; // 音频/视频时长（毫秒），未知为 0
    QByteArray thumbnail;  // 缩略图密文（小图内联，上限 MaxThumbnailBytes），空表示无

    // 结构与取值自检（字段齐备、长度合法、分片口径自洽）
    bool isValid() const;
};

// 序列化为 JSON 字符串；清单非法时返回空串（调用方据此拒绝发送）
QString encodeFileManifest(const FileManifest &manifest);

// 解析清单。版本不符、字段缺失、base64 非法、长度越界或分片口径不自洽时返回
// 默认构造对象且置 ok=false（fail-closed：宁可判为不可解，不渲染半截元数据）
FileManifest decodeFileManifest(const QString &json, bool *ok = nullptr);

// 快速判断一段已解密正文是否为文件清单（只看标记字段，不校验完整性）
bool looksLikeFileManifest(const QString &content);

// 分片数学：客户端加密/续传与服务端校验共用，确保两侧算出的分片数一致

// 密文分片数 = ceil(cipherSize / chunkSize)；参数非法时返回 0
int chunkCountFor(qint64 cipherSize, qint64 chunkSize);

// 明文分片字节数 = 密文分片字节数 - GCM 标签；不足一个标签时返回 -1
qint64 plainSizeOfChunk(qint64 cipherChunkBytes);

// 密文分片字节数 = 明文字节数 + GCM 标签
qint64 cipherSizeOfChunk(qint64 plainChunkBytes);

// 分片参数是否可被服务端接受：chunkSize 在区间内、cipherSize 为正且不超上限、
// chunkCount 与 ceil 一致且不超分片数上限。任一条不满足即 false
bool isChunkingValid(qint64 cipherSize, qint64 chunkSize, int chunkCount);

// 第 index 个密文分片应有的字节数：非末片恒为 chunkSize，末片为余量。
// index 越界或分片参数非法时返回 -1。HTTP 数据面据此拒绝长度不符的 PUT，
// 防止客户端自行选择分片边界绕过体积与分片数校验
qint64 expectedChunkBytes(qint64 cipherSize, qint64 chunkSize, int chunkCount, int index);

// 64 位十六进制小写摘要串是否形态合法（长度与字符集），供校验和入参预检
bool isSha256Hex(const QString &hex);

} // namespace XYChat::Protocol
