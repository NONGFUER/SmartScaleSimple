#include <QGuiApplication>
#include <QCursor>
#include <QQmlApplicationEngine>
#include <Qt>
#include <QQmlContext>

// 日志落盘
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <cstdio>

// 硬件层：称重通讯（飞功 Modbus 协议）
#include "hardware/WeightSensor.h"

// ============================================================================
// 日志：同时输出到控制台(stderr) 与 data/smartscale.log（带时间+级别）
// ============================================================================
static QFile  g_logFile;
static QMutex g_logMutex;

static void smartScaleMessageHandler(QtMsgType type,
                                     const QMessageLogContext &ctx,
                                     const QString &msg)
{
    if (msg.contains("Qt6CTPlatformTheme"))
        return;

    QString level;
    switch (type) {
    case QtDebugMsg:    level = "DEBUG"; break;
    case QtInfoMsg:     level = "INFO";  break;
    case QtWarningMsg:  level = "WARN";  break;
    case QtCriticalMsg: level = "CRIT";  break;
    case QtFatalMsg:    level = "FATAL"; break;
    default:            level = "???";   break;
    }
    const QString text = QString("[%1] %2: %3")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
        .arg(level)
        .arg(msg);

    fprintf(stderr, "%s\n", qPrintable(text));
    fflush(stderr);

    QMutexLocker locker(&g_logMutex);
    if (g_logFile.isOpen()) {
        QTextStream ts(&g_logFile);
        ts << text << Qt::endl;
    }
}

int main(int argc, char *argv[])
{
    qInstallMessageHandler(smartScaleMessageHandler);

    qputenv("QT_IM_MODULE", QByteArray("qtvirtualkeyboard"));

    QGuiApplication app(argc, argv);

    // 注册内嵌 PingFang SC 字体
    {
        QFile fontFile(":/resources/fonts/PingFangSC-Regular.ttf");
        if (fontFile.open(QIODevice::ReadOnly)) {
            const int fontId = QFontDatabase::addApplicationFontFromData(fontFile.readAll());
            if (fontId == -1)
                qWarning() << "[Main] PingFang SC 字体加载失败";
            else
                qInfo() << "[Main] 已注册字体族:" << QFontDatabase::applicationFontFamilies(fontId);
        }
    }

    // 注册内嵌 DIN Bold 字体（用于称重数值显示）
    {
        QFile dinFile(":/resources/fonts/din-bold-2.ttf");
        if (dinFile.open(QIODevice::ReadOnly)) {
            const int dinId = QFontDatabase::addApplicationFontFromData(dinFile.readAll());
            if (dinId == -1)
                qWarning() << "[Main] DIN 字体加载失败";
            else
                qInfo() << "[Main] 已注册字体族:" << QFontDatabase::applicationFontFamilies(dinId);
        }
    }

    QFont defaultFont("PingFang SC");
    defaultFont.setPixelSize(30);
    app.setFont(defaultFont);

    // 打开日志文件
    {
        const QString logPath = QCoreApplication::applicationDirPath()
                                + "/data/smartscale.log";
        QDir().mkpath(QFileInfo(logPath).absolutePath());
        g_logFile.setFileName(logPath);
        QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text;
        mode |= (QFile::exists(logPath) && QFile(logPath).size() > 2 * 1024 * 1024)
                ? QIODevice::Truncate
                : QIODevice::Append;
        if (!g_logFile.open(mode))
            qWarning() << "[Main] 无法打开日志文件:" << logPath;
        else
            qInfo() << "[Main] 日志文件:" << logPath;
    }

    // 触摸屏环境：全局隐藏鼠标光标
    QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));

    QQmlApplicationEngine engine;

    // ============================================================
    // 硬件层：双路称重传感器（飞功 Modbus RTU 通讯）
    //   左: /dev/ttyAMA0 (不传参, 保持环境变量 SMARTSCALE_SERIAL_PORT 旧行为)
    //   右: /dev/ttyAMA1 (可用环境变量 SMARTSCALE_SERIAL_PORT_R 覆盖)
    //   两路参数相同: 9600 8N1, Modbus slave=1
    // ============================================================
    WeightSensor *weightSensorLeft = new WeightSensor(QString(), 9600, 1, &app);

    const QByteArray rightPortEnv = qgetenv("SMARTSCALE_SERIAL_PORT_R");
    const QString rightPort = rightPortEnv.isEmpty()
            ? QStringLiteral("/dev/ttyAMA1")
            : QString::fromLocal8Bit(rightPortEnv);
    WeightSensor *weightSensorRight = new WeightSensor(rightPort, 9600, 1, &app);

    // ============================================================
    // 注入到 QML 全局环境（左右两路独立单例）
    // ============================================================
    qmlRegisterSingletonInstance("App.Backend", 1, 0, "WeightManagerLeft",  weightSensorLeft);
    qmlRegisterSingletonInstance("App.Backend", 1, 0, "WeightManagerRight", weightSensorRight);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("SmartScale", "Main");

    return QCoreApplication::exec();
}
