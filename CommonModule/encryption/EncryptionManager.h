#pragma once

#include <QString>

class EncryptionManager
{
public:
    static QString encryptPassword(const QString &password);
};
