#include <QCoreApplication>
#include <QCommandLineParser>
#include "core/Server.h"
#include "TlsHelper.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName("XYChat");
    app.setApplicationName("XYChat-Server");

    // M5.5: 开发明文模式必须显式开启（默认关闭，生产禁用）
    QCommandLineParser parser;
    parser.setApplicationDescription("XYChat Server (TLS fail-closed by default)");
    parser.addHelpOption();
    QCommandLineOption plaintextOption(
        QStringList() << "allow-plaintext",
        "Development only: allow plaintext TCP when TLS is unavailable. "
        "Never use in production.");
    parser.addOption(plaintextOption);
    // M8.2: 文件传输数据面（HTTP(S)）的监听端口与对外通告主机名。
    // 主机名必须由运维指定：服务端无法自知 NAT/反向代理后的对外地址，
    // 而客户端是根据登录响应里的 fileTransferBaseUrl 去连数据面的
    QCommandLineOption httpPortOption(
        QStringList() << "http-port",
        "Listening port of the file transfer data plane (HTTP(S)). Default: 12346.",
        "port", "12346");
    QCommandLineOption httpHostOption(
        QStringList() << "http-host",
        "Host name advertised to clients in fileTransferBaseUrl "
        "(set this when behind NAT or a reverse proxy). Default: 127.0.0.1.",
        "host", "127.0.0.1");
    // 2026-09-17: 数据面的实际绑定地址。默认仅回环——未认证的请求体会在票据校验
    // 之前被 QHttpServer 读进内存，故"对外可达"必须是有意识的选择，而不是默认行为
    QCommandLineOption httpBindOption(
        QStringList() << "http-bind",
        "Numeric IP address the data plane binds to. Default: 127.0.0.1 (loopback only). "
        "Binding a non-loopback address also requires --http-allow-public.",
        "address", QString());
    QCommandLineOption httpAllowPublicOption(
        QStringList() << "http-allow-public",
        "Confirm that the data plane may bind a non-loopback address. Only use this "
        "with a reverse proxy that enforces a request-body limit in front of it: "
        "unauthenticated request bodies are buffered in memory before the ticket check.");
    parser.addOption(httpPortOption);
    parser.addOption(httpHostOption);
    parser.addOption(httpBindOption);
    parser.addOption(httpAllowPublicOption);
    parser.process(app);

    bool httpPortOk = false;
    const uint httpPort = parser.value(httpPortOption).toUInt(&httpPortOk);
    if (!httpPortOk || httpPort == 0 || httpPort > 65535) {
        qCritical() << "Invalid --http-port:" << parser.value(httpPortOption);
        return -1;
    }

    Server server;

    // M5: 初始化 TLS（自动生成开发证书）
    const QString certDir = XYChat::Security::TlsHelper::defaultCertDir();
    if (!server.initTls(certDir)) {
        qWarning() << "[Main] TLS init failed.";
    }

    // M8.2: 数据面端口与对外通告地址（须在 start() 前设定）
    server.setFileHttpEndpoint(static_cast<quint16>(httpPort), parser.value(httpHostOption));
    // 绑定地址（默认回环；非回环需 --http-allow-public，见 FileHttpService）
    server.setFileHttpBind(parser.value(httpBindOption), parser.isSet(httpAllowPublicOption));

    // M5.5: fail-closed：TLS 不可用且未显式允许明文时拒绝启动
    if (!server.start(12345, parser.isSet(plaintextOption)))
    {
        qCritical() << "Failed to start server";
        return -1;
    }
    return app.exec();
}
