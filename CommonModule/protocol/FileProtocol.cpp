#include "FileProtocol.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include "security/SecureMemory.h"

namespace XYChat::Protocol
{

namespace
{
// 与 E2eeCrypto/GroupE2eeCrypto 的 AES-256-GCM 参数保持一致：
// 文件密钥 32 字节、nonce 12 字节、认证标签 16 字节
constexpr int FileKeySize = 32;
constexpr int FileIvSize = 12;
constexpr int GcmTagSize = 16;
constexpr int Sha256HexLength = 64;
const char ManifestKind[] = "file";

QString toBase64(const QByteArray &data)
{
    return QString::fromLatin1(data.toBase64());
}

// 严格 base64 解码：含非法字符时返回空，交由长度校验一并拒绝
QByteArray fromBase64(const QString &data)
{
    if (data.isEmpty()) {
        return {};
    }
    return QByteArray::fromBase64(data.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
}

// 仅接受小写十六进制：QByteArray::toHex() 的输出即小写，放宽大小写会让
// 客户端与服务端的校验和比对出现两种等价写法
bool isHexLower(const QString &hex)
{
    for (QChar c : hex) {
        const bool isDigit = c >= '0' && c <= '9';
        const bool isLowerHex = c >= 'a' && c <= 'f';
        if (!isDigit && !isLowerHex) {
            return false;
        }
    }
    return true;
}
} // namespace

bool FileManifest::isValid() const
{
    if (fileId <= 0) {
        return false;
    }
    if (key.size() != FileKeySize || iv.size() != FileIvSize) {
        return false;
    }
    if (cipherSize <= 0 || cipherSize > MaxFileSize) {
        return false;
    }
    if (plainSize < 0 || plainSize > MaxFileSize) {
        return false;
    }
    // 每个分片都会追加一个 GCM 标签，密文必然长于明文至少一个标签；
    // 等长或更短说明清单被篡改或加密环节被跳过
    if (cipherSize < plainSize + GcmTagSize) {
        return false;
    }
    if (!isSha256Hex(sha256Hex)) {
        return false;
    }
    // 分片口径必须由清单自带且自洽：chunkSize 在合法区间内，按它算出的
    // 分片数不超上限。缺失（旧清单或被篡改）一律拒绝，不猜默认值：
    // 接收方需要它才能切分密文并逐片解密
    if (chunkSize < MinChunkSize || chunkSize > MaxChunkSize) {
        return false;
    }
    const int chunks = chunkCountFor(cipherSize, chunkSize);
    if (chunks <= 0 || chunks > MaxChunkCount) {
        return false;
    }
    // 文件名必填：接收端要靠它落地到磁盘，空名会让下载路径退化为用户目录
    if (name.isEmpty() || name.size() > MaxFileNameLength) {
        return false;
    }
    if (mime.size() > MaxFileNameLength) {
        return false;
    }
    if (width < 0 || height < 0 || durationMs < 0) {
        return false;
    }
    if (thumbnail.size() > MaxThumbnailBytes) {
        return false;
    }
    return true;
}

QString encodeFileManifest(const FileManifest &manifest)
{
    if (!manifest.isValid()) {
        return {};
    }

    QJsonObject obj;
    obj["v"] = FileManifestVersion;
    obj["kind"] = ManifestKind;
    obj["fileId"] = manifest.fileId;
    obj["name"] = manifest.name;
    obj["mime"] = manifest.mime;
    obj["plainSize"] = manifest.plainSize;
    obj["cipherSize"] = manifest.cipherSize;
    obj["chunkSize"] = manifest.chunkSize;
    obj["sha256"] = manifest.sha256Hex;
    obj["key"] = toBase64(manifest.key);
    obj["iv"] = toBase64(manifest.iv);
    obj["width"] = manifest.width;
    obj["height"] = manifest.height;
    obj["durationMs"] = manifest.durationMs;
    // 缩略图为可选字段：无缩略图时不写入，避免每条文件消息都带一个空串
    if (!manifest.thumbnail.isEmpty()) {
        obj["thumb"] = toBase64(manifest.thumbnail);
    }
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

FileManifest decodeFileManifest(const QString &json, bool *ok)
{
    if (ok) {
        *ok = false;
    }
    if (json.isEmpty()) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }
    const QJsonObject obj = doc.object();

    // 版本与类型先行判定：不认识的清单版本直接拒绝，不做尽力解析
    if (obj.value("v").toInt(0) != FileManifestVersion) {
        return {};
    }
    if (obj.value("kind").toString() != ManifestKind) {
        return {};
    }

    FileManifest manifest;
    manifest.fileId = obj.value("fileId").toVariant().toLongLong();
    manifest.name = obj.value("name").toString();
    manifest.mime = obj.value("mime").toString();
    manifest.plainSize = obj.value("plainSize").toVariant().toLongLong();
    manifest.cipherSize = obj.value("cipherSize").toVariant().toLongLong();
    manifest.chunkSize = obj.value("chunkSize").toVariant().toLongLong();
    manifest.sha256Hex = obj.value("sha256").toString();
    manifest.key = fromBase64(obj.value("key").toString());
    manifest.iv = fromBase64(obj.value("iv").toString());
    manifest.width = obj.value("width").toInt(0);
    manifest.height = obj.value("height").toInt(0);
    manifest.durationMs = obj.value("durationMs").toVariant().toLongLong();
    manifest.thumbnail = fromBase64(obj.value("thumb").toString());

    if (!manifest.isValid()) {
        // 非法清单不留半截状态：已解出的密钥材料就地清零后整体丢弃
        XYChat::Security::SecureMemory::wipe(manifest.key);
        XYChat::Security::SecureMemory::wipe(manifest.thumbnail);
        return {};
    }

    if (ok) {
        *ok = true;
    }
    return manifest;
}

bool looksLikeFileManifest(const QString &content)
{
    // 廉价启发式判别：只用于避免把清单 JSON 当正文渲染，完整校验在 decode 中。
    // 不依赖键顺序（QJsonObject 序列化按字典序），只做子串探测
    if (content.size() < 32 || !content.startsWith('{')) {
        return false;
    }
    return content.contains("\"kind\":\"file\"") && content.contains("\"cipherSize\":");
}

QString filePreviewText(const QString &content)
{
    // 只取文件名，绝不回落成清单原文：清单含 32 字节文件密钥，字符串一旦进入
    // JS 堆或本地库就无法可靠清零（见 FileProtocol.h 里本函数的说明）
    bool ok = false;
    const FileManifest manifest = decodeFileManifest(content, &ok);
    if (!ok || manifest.name.isEmpty()) {
        return QStringLiteral("[File]");
    }
    return QStringLiteral("[File] ") + manifest.name;
}

int chunkCountFor(qint64 cipherSize, qint64 chunkSize)
{
    // 先夹住取值区间再运算：MaxFileSize + MaxChunkSize 远小于 qint64 上界，
    // 由此保证 ceil 的加法不会溢出
    if (cipherSize <= 0 || cipherSize > MaxFileSize) {
        return 0;
    }
    if (chunkSize < MinChunkSize || chunkSize > MaxChunkSize) {
        return 0;
    }
    const qint64 count = (cipherSize + chunkSize - 1) / chunkSize;
    return static_cast<int>(count);
}

qint64 plainSizeOfChunk(qint64 cipherChunkBytes)
{
    if (cipherChunkBytes < GcmTagSize) {
        return -1;
    }
    return cipherChunkBytes - GcmTagSize;
}

qint64 cipherSizeOfChunk(qint64 plainChunkBytes)
{
    if (plainChunkBytes < 0) {
        return -1;
    }
    return plainChunkBytes + GcmTagSize;
}

bool isChunkingValid(qint64 cipherSize, qint64 chunkSize, int chunkCount)
{
    if (cipherSize <= 0 || cipherSize > MaxFileSize) {
        return false;
    }
    if (chunkSize < MinChunkSize || chunkSize > MaxChunkSize) {
        return false;
    }
    if (chunkCount <= 0 || chunkCount > MaxChunkCount) {
        return false;
    }
    // 分片数必须与体积自洽，否则可用少报分片数把超大文件拆到上限之外
    return chunkCountFor(cipherSize, chunkSize) == chunkCount;
}

qint64 expectedChunkBytes(qint64 cipherSize, qint64 chunkSize, int chunkCount, int index)
{
    if (!isChunkingValid(cipherSize, chunkSize, chunkCount)) {
        return -1;
    }
    if (index < 0 || index >= chunkCount) {
        return -1;
    }
    if (index < chunkCount - 1) {
        return chunkSize;
    }
    return cipherSize - static_cast<qint64>(chunkCount - 1) * chunkSize;
}

bool isSha256Hex(const QString &hex)
{
    return hex.size() == Sha256HexLength && isHexLower(hex);
}

} // namespace XYChat::Protocol
