#include "Server.h"
#include "RequestHandler.h"

void ConnectionServer::incomingConnection(qintptr socketDescriptor)
{
    emit socketAccepted(socketDescriptor);
}

Server::Server(QObject *parent)
    : QObject(parent)
    , tcpServer(new ConnectionServer(this))
{
    connect(tcpServer, &ConnectionServer::socketAccepted, this, &Server::onSocketAccepted);
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

void Server::onSocketAccepted(qintptr socketDescriptor)
{
    RequestHandler *handler = new RequestHandler(socketDescriptor, this);
    connect(handler, &RequestHandler::finished, handler, &RequestHandler::deleteLater);
    handler->start();
}
