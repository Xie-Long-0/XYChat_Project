#include "DatabaseManager.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>
#include "../CommonModule/encryption/EncryptionManager.h"

DatabaseManager::DatabaseManager()
{
    openDatabase();
}

bool DatabaseManager::openDatabase()
{
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName("resources/db/chatapp.db");

    if (!db.open())
    {
        qCritical() << "Failed to open database:" << db.lastError().text();
        return false;
    }

    QSqlQuery query;
    if (!query.exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, username TEXT UNIQUE, password TEXT)"))
    {
        qCritical() << "Failed to create table:" << query.lastError().text();
        return false;
    }
    query.prepare("INSERT INTO users (username, password) VALUES (:username, :passwd)");
    query.bindValue(":username", "admin");
    query.bindValue(":passwd", EncryptionManager::encryptPassword("passwd"));
    query.exec();
    return true;
}

bool DatabaseManager::verifyUser(const QString &username, const QString &encryptedPassword)
{
    QSqlQuery query;
    query.prepare("SELECT password FROM users WHERE username = :username");
    query.bindValue(":username", username);

    if (query.exec() && query.next())
    {
        QString storedPassword = query.value(0).toString();
        return storedPassword == encryptedPassword;
    }

    return false;
}

void DatabaseManager::closeDatabase()
{
    QSqlDatabase::database().close();
}
