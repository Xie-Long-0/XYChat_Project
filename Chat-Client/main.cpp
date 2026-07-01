#include <QApplication>
#include "views/LoginWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    auto loginWindow = new LoginWindow();
    loginWindow->show();
    return app.exec();
}
