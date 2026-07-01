#include "RequestHandler.h"
#include "database/DatabaseManager.h"
#include <QJsonDocument>
#include <QJsonObject>

RequestHandler::RequestHandler(qintptr socketDescriptor, QObject *parent)
    : QThread(parent)
    , m_socketDescriptor(socketDescriptor)
{
}

void RequestHandler::run()
{
    m_socket = new QTcpSocket(this);
    if (!m_socket->setSocketDescriptor(m_socketDescriptor)) {
        qDebug() << "Failed to set socket descriptor";
        emit finished();
        return;
    }

    connect(m_socket, &QTcpSocket::readyRead, this, &RequestHandler::onReadyRead);
    exec();
}

void RequestHandler::onReadyRead()
{
    QByteArray requestData = m_socket->readAll();
    QJsonDocument jsonDoc = QJsonDocument::fromJson(requestData);
    QJsonObject json = jsonDoc.object();
    processRequest(json);
}

void RequestHandler::processRequest(const QJsonObject &json)
{
    QString type = json["type"].toString();
    qDebug() << m_socket->peerAddress().toString() << "request" << type;

    QJsonObject response;
    if (type == "login")
    {
        QString username = json["username"].toString();
        QString encryptedPassword = json["password"].toString();

        DatabaseManager dbManager;
        if (dbManager.verifyUser(username, encryptedPassword))
        {
            response["status"] = "success";
            qDebug() << username << "login successful";
        }
        else
        {
            response["status"] = "failure";
            response["message"] = "Invalid username or password";
            qDebug() << username << "login failed";
        }
    }
    else
    {
        response["status"] = "failure";
        response["message"] = "Invalid request type";
        qDebug() << "Invalid request type";
    }

    m_socket->write(QJsonDocument(response).toJson());
    m_socket->flush();
}
