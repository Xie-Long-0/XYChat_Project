#pragma once

#include <QString>

class DatabaseManager
{
public:
    DatabaseManager();
    bool verifyUser(const QString &username, const QString &encryptedPassword);

private:
    bool openDatabase();
    void closeDatabase();
};
