#include "LoginWindow.h"
#include "ui_LoginWindow.h"
#include "MainWindow.h"
#include <QMessageBox>
#include "core/NetworkManager.h"
#include "EncryptionManager.h"

LoginWindow::LoginWindow(QWidget *parent) :
    QMainWindow(parent)
    , ui(new Ui::LoginWindow)
    , m_networkManager(new NetworkManager(this))
{
    setAttribute(Qt::WA_DeleteOnClose);
    ui->setupUi(this);
    connect(ui->loginButton, &QPushButton::clicked, this, &LoginWindow::onLoginButtonClicked);
    connect(m_networkManager, &NetworkManager::loginSuccessful, this, &LoginWindow::onLoginSuccessful);
    connect(m_networkManager, &NetworkManager::loginFailed, this, &LoginWindow::onLoginFailed);
}

LoginWindow::~LoginWindow()
{
    delete ui;
}

void LoginWindow::onLoginButtonClicked()
{
    QString username = ui->usernameLineEdit->text();
    QString password = ui->passwordLineEdit->text();

    if (username.isEmpty() || password.isEmpty())
    {
        QMessageBox::critical(this, tr("错误"), tr("用户名或密码不能为空！"));
        return;
    }

    ui->loginButton->setEnabled(false);

    QString encryptedPassword = EncryptionManager::encryptPassword(password);

    m_networkManager->login(username, encryptedPassword);
}

void LoginWindow::onLoginSuccessful()
{
    auto mainWindow = new MainWindow();
    mainWindow->show();
    close();
}

void LoginWindow::onLoginFailed(const QString &errorMessage)
{
    QMessageBox::critical(this, tr("登录失败"), errorMessage);
    ui->passwordLineEdit->clear();
    ui->loginButton->setEnabled(true);
}
