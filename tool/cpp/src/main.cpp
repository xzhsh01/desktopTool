#include <QApplication>
#include "app/EmojiFont.h"
#include <QIcon>
#include <QMessageBox>
#include <QFile>
#include <QMetaType>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include "app/MainWindow.h"
#include "core/Labels.h"
#include "core/Logger.h"
#include "core/Settings.h"
#include "connections/ConnectionManager.h"
#include "redis/RedisClient.h"
#include "database/DatabaseClient.h"
#include "database/DriverInstaller.h"
#include "mail/MailSelfTest.h"
#include "mail/MailAccountManager.h"
#include "mail/ScheduledQueue.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("bambooRat");
    QApplication::setOrganizationName("KFrame");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setWindowIcon(QIcon(":/icons/app.png"));

    // ── 注册 emoji 字体回退：解决 Windows 缺字体时 UI 里 emoji 显示为方框 ──
    EmojiFont::install();

    // ── 一次性数据目录迁移：DesktopTool -> bambooRat ──
    // applicationName 改名后 AppDataLocation 由 %APPDATA%/KFrame/DesktopTool
    // 变为 %APPDATA%/KFrame/bambooRat，旧数据（设置/邮件账号/连接配置）整体搬移
    {
        const QString appData = QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation);
        const QString oldDir = QFileInfo(appData).absolutePath() + "/DesktopTool";
        if (QDir(oldDir).exists()) {
            QDir parent(QFileInfo(appData).absolutePath());
            if (!QDir(appData).exists()) {
                parent.rename("DesktopTool", "bambooRat");
            } else if (QDir(appData).isEmpty()) {
                // 新目录只是空壳（曾以新名启动过），删除后接管旧数据
                QDir(appData).removeRecursively();
                parent.rename("DesktopTool", "bambooRat");
            }
        }
    }

    // ── 自测模式：--mail-selftest ──
    bool selftest = false;
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == "--mail-selftest") {
            selftest = true;
            break;
        }
    }
    if (selftest) {
        Logger::instance().info("Application starting in mail-selftest mode", "app");
        Settings::instance();
        mailSelfTest::run();
        Logger::instance().info("selftest finished, exiting", "app");
        return 0;
    }

    // ── 调试：--mail-cleanup 仅清理自测残留账号（不启动 UI） ──
    bool cleanup = false;
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == "--mail-cleanup") {
            cleanup = true; break;
        }
    }
    if (cleanup) {
        Settings::instance();
        auto& mgr = MailAccountManager::instance();
        QStringList ids;
        for (const auto& a : mgr.accounts()) {
            if (a.email.contains("selftest@") ||
                a.email.contains("@invalid.example.com") ||
                a.email.contains("test@")) {
                ids.append(a.id);
            }
        }
        for (const auto& id : ids) {
            mgr.remove(id);
            Logger::instance().info(QString("cleanup: removed %1").arg(id), "app");
        }
        Logger::instance().info(QString("cleanup done, remaining=%1").arg(mgr.accounts().size()), "app");
        return 0;
    }

    // High DPI support (Qt6 默认启用，这里显式确认)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // 注册跨线程信号所需的自定义元类型
    qRegisterMetaType<Logger::Entry>("Logger::Entry");
    qRegisterMetaType<RedisClient::CommandResult>("RedisClient::CommandResult");
    qRegisterMetaType<RedisClient::KeyInfo>("RedisClient::KeyInfo");
    qRegisterMetaType<QList<RedisClient::KeyInfo>>("QList<RedisClient::KeyInfo>");
    qRegisterMetaType<DatabaseClient::QueryResult>("DatabaseClient::QueryResult");
    qRegisterMetaType<DatabaseClient::TableInfo>("DatabaseClient::TableInfo");
    qRegisterMetaType<QList<DatabaseClient::TableInfo>>("QList<DatabaseClient::TableInfo>");

    // 初始化核心模块（单例，首次访问时加载配置）
    Logger::instance().info("Application starting", "app");
    Settings::instance();

    // ── UI 文本外部化：先于任何 UI 模块构造触发加载
    //    Labels::instance() 内部 lazy，但这里显式构造以：
    //      ① 让 Logger 立刻记录加载数量 / 用户覆盖文件路径
    //      ② 让 labels/default.json 解析错误尽早暴露（而不是等到第一个 UI 标签时）
    //    必须在 Settings 之后：用户 labels.json 路径依赖 AppDataLocation 已就位
    //    必须在 ConnectionManager/MainWindow 之前：UI 模块构造时会查询 Labels
    Labels::instance();
    {
        const bool userExists = QFileInfo(Labels::instance().userFilePath()).exists();
        Logger::instance().info(
            QString("Labels loaded: %1 keys; user overlay=%2")
                .arg(Labels::instance().size())
                .arg(userExists ? Labels::instance().userFilePath() : QStringLiteral("(none)")),
            "app");
    }

    ConnectionManager::instance();

    // 本地数据库运行库已就绪则加入 DLL 搜索路径（必须先于任何 QSqlDatabase::drivers()
    // 调用——drivers() 只读插件元数据，addDatabase 才真正加载插件 DLL，失败后进程内不重试）
    DriverInstaller::preparePostgresPath();
    DriverInstaller::prepareOraclePath();

    MainWindow win;
    win.show();

    // 启动定时发送队列：构造单例 + 从磁盘恢复待发邮件 + 启动定时器
    ScheduledQueue::instance();

    int ret = QApplication::exec();
    Logger::instance().info(QString("Application exited with code %1").arg(ret), "app");
    return ret;
}
