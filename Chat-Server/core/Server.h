#pragma once

#include <QObject>
#include <QTcpServer>

class Server : public QObject
{
    Q_OBJECT

public:
    explicit Server(QObject *parent = nullptr);
    bool start(quint16 port);

private slots:
    void onNewConnection();

private:
    QTcpServer *tcpServer;
};
