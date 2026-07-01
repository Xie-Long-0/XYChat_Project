#pragma once

#include <QMainWindow>

namespace Ui
{
class LoginWindow;
}

class NetworkManager;

class LoginWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit LoginWindow(QWidget *parent = nullptr);
    ~LoginWindow();

private slots:
    void onLoginButtonClicked();
    void onLoginSuccessful();
    void onLoginFailed(const QString &errorMessage);

private:
    Ui::LoginWindow *ui;
    NetworkManager *m_networkManager;
};
