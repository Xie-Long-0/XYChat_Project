#include "EncryptionManager.h"
#include <openssl/sha.h>

QString EncryptionManager::encryptPassword(const QString &password)
{
    QByteArray passwordBytes = password.toUtf8();
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(passwordBytes.data()), passwordBytes.size(), hash);

    QByteArray hashBytes(reinterpret_cast<char *>(hash), SHA256_DIGEST_LENGTH);
    return hashBytes.toHex();
}
