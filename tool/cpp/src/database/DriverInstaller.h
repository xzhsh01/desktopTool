#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QFile;

/**
 * DriverInstaller: 数据库驱动自动下载安装
 *
 * - 检测系统已安装的 ODBC 驱动（HKLM + HKCU 注册表）
 * - Oracle Instant Client（basic + odbc）自动下载、解压、注册到 HKLM
 *   （驱动管理器只从 HKLM 枚举驱动；无权限时通过提权 reg import 写入，弹一次 UAC）
 * - Microsoft ODBC Driver 18 for SQL Server：下载 MSI 后提权 msiexec 静默安装
 * - MySQL Connector/ODBC：QMYSQL 插件缺失时走 QODBC，同样 MSI 自动安装
 * - PostgreSQL libpq 运行库：qsqlpsql 插件存在但 LIBPQ.dll 缺失时自动下载配置
 * - 提供带 UI 的 ensureOdbcDriver() / ensurePostgresRuntime()：
 *   驱动缺失时询问用户并自动安装
 */
class DriverInstaller : public QObject {
    Q_OBJECT

public:
    explicit DriverInstaller(QObject* parent = nullptr);
    ~DriverInstaller() override;

    // ── 静态查询 ─────────────────────────────────────────────
    // 查找已安装 ODBC 驱动（HKLM + HKCU），命中 preferred 优先返回
    static QString findOdbcDriver(const QString& keyword, const QString& preferred = {});

    // 本应用管理的驱动根目录 / Instant Client 目录 / libpq 目录
    static QString driversRoot();
    static QString instantClientDir();
    static QString libpqDir();

    // Instant Client 是否已就绪（本应用下载安装的）
    static bool isLocalInstantClientReady();
    // 本地 libpq 运行库是否已就绪（本应用下载的）
    static bool isLocalLibpqReady();

    // ── 带 UI 的确保驱动可用 ─────────────────────────────────
    // dbType: "oracle" / "mssql" / "mysql"
    // 返回 true 表示驱动可用（原本已装或刚装好）
    static bool ensureOdbcDriver(const QString& dbType, QWidget* parent);
    // QPSQL 插件加载失败（缺 LIBPQ.dll）时下载 libpq 并配置；
    // 安装成功后仍需重启应用生效，此时返回 false
    static bool ensurePostgresRuntime(QWidget* parent);

    // 启动时调用（须在首次 QSqlDatabase::drivers() 之前）：
    // 本地 libpq 已就绪则把目录加入 DLL 搜索路径，使 QPSQL 插件可加载
    static void preparePostgresPath();
    // 同上：本地 Oracle Instant Client 已就绪则加入 DLL 搜索路径，
    // 使 QOCI 插件可加载、ODBC sqora32.dll 的依赖（oci.dll 等）可解析
    static void prepareOraclePath();

    // ── 异步安装 ─────────────────────────────────────────────
    void installOracleOdbc();
    // 下载 MSI 并提权 msiexec 静默安装，完成后按注册表验证
    void installMsiPackage(const QString& url, const QString& fileName,
                           const QStringList& msiArgs,
                           const QString& verifyKeyword, const QString& verifyPreferred);
    // 下载 PostgreSQL 二进制包并提取 libpq 运行库
    void installPostgresRuntime();
    void cancel();

signals:
    void progressChanged(const QString& stage, int percent);
    void installFinished(bool ok, const QString& message);

private:
    // 通用安装 UI 流程：询问 → 进度对话框 → 启动安装 → 结果提示
    static bool runInstallUi(QWidget* parent, const QString& title,
                             const QString& tip, const QString& successTip,
                             const std::function<void(DriverInstaller&)>& start);

    void downloadNext();
    void onDownloadFinished();
    void extractAndRegister();
    void runMsiInstall();
    void extractLibpq();
    void fail(const QString& message);
    void cleanupIo();

    enum Task { TaskNone, TaskOracle, TaskMsi, TaskLibpq };

    QStringList m_urls;        // 待下载 URL
    QStringList m_files;       // 对应本地临时文件
    int m_index = 0;
    qint64 m_receivedTotal = 0;
    Task m_task = TaskNone;    // 下载完成后的处理方式
    QStringList m_msiArgs;     // msiexec 额外参数（/i 与路径之外）
    QString m_verifyKeyword;   // MSI 安装后注册表验证关键字
    QString m_verifyPreferred;
    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply* m_reply = nullptr;
    QFile* m_file = nullptr;
    bool m_cancelled = false;
};
