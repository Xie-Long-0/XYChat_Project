#pragma once

#include <QObject>
#include <QTcpServer>

class ConnectionServer : public QTcpServer
{
    Q_OBJECT

public:
    using QTcpServer::QTcpServer;

signals:
    void socketAccepted(qintptr socketDescriptor);

protected:
    void incomingConnection(qintptr socketDescriptor) override;
};

class Server : public QObject
{
    Q_OBJECT

public:
    explicit Server(QObject *parent = nullptr);
    bool start(quint16 port);

private slots:
    void onSocketAccepted(qintptr socketDescriptor);

private:
    ConnectionServer *tcpServer;
};
