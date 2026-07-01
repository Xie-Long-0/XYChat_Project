#include <QtTest/QtTest>
#include <QRegularExpression>

#include "EncryptionManager.h"

class TestEncryptionManager : public QObject
{
    Q_OBJECT

private slots:
    void sha256DigestIsStable();
    void sha256DigestLengthIsHexEncoded();
};

void TestEncryptionManager::sha256DigestIsStable()
{
    QCOMPARE(EncryptionManager::encryptPassword("passwd"),
             QStringLiteral("0d6be69b264717f2dd33652e212b173104b4a647b7c11ae72e9885f11cd312fb"));
}

void TestEncryptionManager::sha256DigestLengthIsHexEncoded()
{
    const QString digest = EncryptionManager::encryptPassword("any-password");
    QCOMPARE(digest.size(), 64);
    QVERIFY(digest.contains(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))));
}

QTEST_MAIN(TestEncryptionManager)
#include "TestEncryptionManager.moc"
