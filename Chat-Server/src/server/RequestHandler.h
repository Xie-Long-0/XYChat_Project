#pragma once

#include <QThread>
#include <QTcpSocket>
#include <QJsonObject>

class RequestHandler : public QThread
{
    Q_OBJECT

public:
    explicit RequestHandler(qintptr socketDescriptor, QObject *parent = nullptr);
    void run() override;

signals:
    void finished();

private slots:
    void onReadyRead();

private:
    void processRequest(const QJsonObject &request);

private:
    QTcpSocket *m_socket = nullptr;
    qintptr m_socketDescriptor;
};
