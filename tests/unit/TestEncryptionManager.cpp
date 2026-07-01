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
             QStringLiteral("0d0061a9553d6e34eac7b2af18a6c65b9e78dd953662c574dc3c6719ed598c9b"));
}

void TestEncryptionManager::sha256DigestLengthIsHexEncoded()
{
    const QString digest = EncryptionManager::encryptPassword("any-password");
    QCOMPARE(digest.size(), 64);
    QVERIFY(digest.contains(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))));
}

QTEST_MAIN(TestEncryptionManager)
#include "TestEncryptionManager.moc"
