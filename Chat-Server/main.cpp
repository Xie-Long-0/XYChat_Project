#include <QCoreApplication>
#include "core/Server.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    Server server;
    if (!server.start(12345))
    {
        qCritical() << "Failed to start server";
        return -1;
    }
    return app.exec();
}
