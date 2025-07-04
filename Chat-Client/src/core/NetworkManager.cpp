#include "NetworkManager.h"
#include <QJsonObject>
#include <QJsonDocument>

NetworkManager::NetworkManager(QObject *parent) :
    QObject(parent),
    tcpSocket(new QTcpSocket(this))
{
    connect(tcpSocket, &QTcpSocket::readyRead, this, &NetworkManager::onReadyRead);
    tcpSocket->connectToHost("127.0.0.1", 12345);
}

void NetworkManager::login(const QString &username, const QString &encryptedPassword)
{
    QJsonObject json;
    json["type"] = "login";
    json["username"] = username;
    json["password"] = encryptedPassword;

    if (tcpSocket->waitForConnected())
    {
        tcpSocket->write(QJsonDocument(json).toJson());
    }
    else
    {
        emit loginFailed("Connection timeout");
    }
}

void NetworkManager::onReadyRead()
{
    QByteArray data = tcpSocket->readAll();
    QJsonDocument responseDoc = QJsonDocument::fromJson(data);
    QJsonObject response = responseDoc.object();

    qDebug() << response;

    if (response["status"] == "success")
    {
        emit loginSuccessful();
    }
    else
    {
        auto msg = response["message"].toString();
        if (!msg.isEmpty())
            emit loginFailed(msg);
        else
            emit loginFailed("Unknown error");
    }
}
