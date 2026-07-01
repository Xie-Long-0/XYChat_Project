#pragma once

#include <QObject>
#include <QTcpSocket>

class NetworkManager : public QObject
{
    Q_OBJECT

public:
    explicit NetworkManager(QObject *parent = nullptr);
    void login(const QString &username, const QString &encryptedPassword);

signals:
    void loginSuccessful();
    void loginFailed(const QString &errorMessage);

private slots:
    void onReadyRead();

private:
    QTcpSocket *tcpSocket;
};
