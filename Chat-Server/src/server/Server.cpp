#include "Server.h"
#include "RequestHandler.h"
#include <QTcpSocket>

Server::Server(QObject *parent)
    : QObject(parent)
    , tcpServer(new QTcpServer(this))
{
    connect(tcpServer, &QTcpServer::newConnection, this, &Server::onNewConnection);
}

bool Server::start(quint16 port)
{
    if (tcpServer->listen(QHostAddress::Any, port))
    {
        qDebug() << "Listen" << port;
        return true;
    }
    qDebug() << "Failed to listen" << port << tcpServer->errorString();
    return false;
}

void Server::onNewConnection()
{
    QTcpSocket *clientSocket = tcpServer->nextPendingConnection();
    if (clientSocket)
    {
        RequestHandler *handler = new RequestHandler(clientSocket->socketDescriptor(), this);
        connect(handler, &RequestHandler::finished, handler, &RequestHandler::deleteLater);
        handler->start();
    }
}
