// M8.3: 图片元数据与内联缩略图生成测试
//
// 只依赖 QtGui 的图像编解码，不需要平台多媒体后端，因此可在无头环境与 CI 中
// 运行。JPEG 编码在 Qt 中由插件提供，若目标环境缺失该插件，与缩略图字节相关的
// 断言会 QSKIP（尺寸提取走 PNG 内建编解码，仍然完整验证）。
#include <QtTest>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QTemporaryDir>

#include "core/ThumbnailMaker.h"

#include "FileProtocol.h"

using namespace XYChat::Client;
namespace Protocol = XYChat::Protocol;

namespace
{
// 生成测试图片。noise=true 时为高熵随机像素（几乎不可压缩），用于验证
// "压不进上限就不内联"的收敛逻辑；否则为纯色（极易压缩）
bool writePng(const QString &path, int width, int height, bool noise)
{
    QImage image(width, height, QImage::Format_RGB32);
    if (noise) {
        quint32 state = 0x9E3779B9u;
        for (int y = 0; y < height; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < width; ++x) {
                state = state * 1664525u + 1013904223u;
                line[x] = qRgb(static_cast<int>((state >> 24) & 0xFF),
                               static_cast<int>((state >> 16) & 0xFF),
                               static_cast<int>((state >> 8) & 0xFF));
            }
        }
    } else {
        image.fill(QColor(40, 120, 200));
    }
    return image.save(path, "PNG");
}

bool hasJpegWriter()
{
    return QImageWriter::supportedImageFormats().contains("jpeg");
}
} // namespace

class TestThumbnailMaker : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void extractsSizeAndDecodableThumbnail();
    void thumbnailNeverExceedsByteLimit();
    void smallLimitYieldsNoThumbnailInsteadOfOversize();
    void rejectsNonImageFile();
    void handlesMissingFileAndBadArguments();
    void thumbnailFitsWithinManifestBudget();

private:
    QTemporaryDir *m_dir = nullptr;
    QString m_plainPath;   // 纯色 800x600
    QString m_noisePath;   // 高熵 800x600
};

void TestThumbnailMaker::initTestCase()
{
    m_dir = new QTemporaryDir();
    QVERIFY(m_dir->isValid());
    m_plainPath = m_dir->path() + "/plain.png";
    m_noisePath = m_dir->path() + "/noise.png";
    QVERIFY(writePng(m_plainPath, 800, 600, false));
    QVERIFY(writePng(m_noisePath, 800, 600, true));
    QVERIFY(QImageReader::supportedImageFormats().contains("png"));
}

void TestThumbnailMaker::extractsSizeAndDecodableThumbnail()
{
    const auto result = ThumbnailMaker::make(m_plainPath);
    QVERIFY(result.isImage);
    QCOMPARE(result.width, 800);
    QCOMPARE(result.height, 600);

    if (!hasJpegWriter()) {
        QSKIP("JPEG encoder plugin is unavailable in this environment");
    }
    QVERIFY(!result.thumb.isEmpty());
    QVERIFY(result.thumb.size() <= Protocol::MaxThumbnailBytes);

    // 缩略图必须是可解码的 JPEG，且最长边不超过默认上限
    QImage decoded;
    QVERIFY(decoded.loadFromData(result.thumb, "JPEG"));
    QVERIFY(!decoded.isNull());
    QVERIFY(decoded.width() <= 160 && decoded.height() <= 160);
    // 宽高比应保持（800x600 -> 4:3），否则 UI 会显示变形的预览
    const qreal ratio = static_cast<qreal>(decoded.width()) / decoded.height();
    QVERIFY(qAbs(ratio - (800.0 / 600.0)) < 0.15);
}

void TestThumbnailMaker::thumbnailNeverExceedsByteLimit()
{
    if (!hasJpegWriter()) {
        QSKIP("JPEG encoder plugin is unavailable in this environment");
    }
    // 高熵图像几乎不可压缩：这正是"逐步降质量再降尺寸"要收敛的场景
    const auto result = ThumbnailMaker::make(m_noisePath);
    QVERIFY(result.isImage);
    QCOMPARE(result.width, 800);
    QCOMPARE(result.height, 600);
    // 要么压进上限，要么留空；绝不返回超限的缩略图（清单会撑破群消息正文长度）
    QVERIFY(result.thumb.size() <= Protocol::MaxThumbnailBytes);
}

void TestThumbnailMaker::smallLimitYieldsNoThumbnailInsteadOfOversize()
{
    if (!hasJpegWriter()) {
        QSKIP("JPEG encoder plugin is unavailable in this environment");
    }
    // 上限压到极小：宁可不内联缩略图（UI 回退到文件图标），也不放宽上限
    const auto result = ThumbnailMaker::make(m_noisePath, 160, 200);
    QVERIFY(result.isImage);
    QVERIFY(result.thumb.size() <= 200);
    // 尺寸信息仍然可得（它与缩略图编码成败无关）
    QCOMPARE(result.width, 800);
    QCOMPARE(result.height, 600);
}

void TestThumbnailMaker::rejectsNonImageFile()
{
    const QString textPath = m_dir->path() + "/not-an-image.txt";
    QFile file(textPath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("this is not an image, just some bytes\n");
    file.close();

    const auto result = ThumbnailMaker::make(textPath);
    // 非图片：不得报错也不得产出缩略图，元数据缺失绝不阻断文件发送
    QVERIFY(!result.isImage);
    QVERIFY(result.thumb.isEmpty());
    QCOMPARE(result.width, 0);
    QCOMPARE(result.height, 0);
}

void TestThumbnailMaker::handlesMissingFileAndBadArguments()
{
    QVERIFY(!ThumbnailMaker::make(m_dir->path() + "/does-not-exist.png").isImage);
    QVERIFY(!ThumbnailMaker::make(QString()).isImage);
    // 非法参数一律安全返回，不做任何 IO
    QVERIFY(!ThumbnailMaker::make(m_plainPath, 0).isImage);
    QVERIFY(!ThumbnailMaker::make(m_plainPath, -1).isImage);
    QVERIFY(!ThumbnailMaker::make(m_plainPath, 160, 0).isImage);
    QVERIFY(!ThumbnailMaker::make(m_plainPath, 160, -1).isImage);
}

void TestThumbnailMaker::thumbnailFitsWithinManifestBudget()
{
    if (!hasJpegWriter()) {
        QSKIP("JPEG encoder plugin is unavailable in this environment");
    }
    // 清单要作为群消息正文投递，受 MaxGroupMessageLength（16384 字符）约束，
    // 而 thumb 以 base64 编码（约 1.34 倍膨胀）。本用例锁定"缩略图上限 +
    // base64 膨胀"仍在正文预算之内，避免图片消息被服务端以超长为由拒收
    const auto result = ThumbnailMaker::make(m_plainPath);
    QVERIFY(result.isImage);
    if (result.thumb.isEmpty()) {
        return;  // 压不进上限时不内联，天然满足预算
    }
    const int base64Chars = static_cast<int>(result.thumb.toBase64().size());
    QVERIFY(base64Chars <= Protocol::MaxThumbnailBytes * 2);
    // 清单其余字段（密钥/iv/摘要/文件名等）远小于群消息上限，
    // 因此缩略图 base64 之后仍需留出充足余量
    QVERIFY(base64Chars < 16384 / 2);
}

QTEST_GUILESS_MAIN(TestThumbnailMaker)
#include "TestThumbnailMaker.moc"
