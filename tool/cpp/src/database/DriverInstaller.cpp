#include "database/DriverInstaller.h"
#include "core/Logger.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFile>
#include <QDir>
#include <QSettings>
#include <QProcess>
#include <QStandardPaths>
#include <QSqlDatabase>
#include <QUrl>
#include <QMessageBox>
#include <QProgressDialog>
#include <QEventLoop>

// Oracle Instant Client 21.22 直链（无需登录；21.3 已被官方移除返回 404）
static const char* IC_BASIC_URL =
    "https://download.oracle.com/otn_software/nt/instantclient/2122000/instantclient-basic-windows.x64-21.22.0.0.0dbru.zip";
static const char* IC_ODBC_URL =
    "https://download.oracle.com/otn_software/nt/instantclient/2122000/instantclient-odbc-windows.x64-21.22.0.0.0dbru.zip";
// 注册到 HKCU 的 ODBC 驱动名（与官方 instant client 命名一致）
static const char* IC_DRIVER_NAME = "Oracle in instantclient_21_22";

// Microsoft ODBC Driver 18 for SQL Server (x64) MSI — 官方稳定 fwlink，重定向最新 18.x
static const char* MS_ODBC18_URL = "https://go.microsoft.com/fwlink/?linkid=2187106";
// MySQL Connector/ODBC (x64) MSI 直链（无需登录）。
// 注意：MySQL CDN 会移除旧版本文件（8.0.33 已 404）；目录为两段式版本号，
// 版本淘汰后需更新此 URL（可从 dev.mysql.com/downloads/connector/odbc/ 查最新版）
static const char* MYSQL_ODBC_URL =
    "https://cdn.mysql.com/Downloads/Connector-ODBC/26.7/mysql-connector-odbc-26.7.1-winx64.msi";
// PostgreSQL 16 Windows 二进制包（EDB，含 libpq；无需登录）
static const char* PG_BINARIES_URL =
    "https://get.enterprisedb.com/postgresql/postgresql-16.6-1-windows-x64-binaries.zip";

DriverInstaller::DriverInstaller(QObject* parent) : QObject(parent) {
}

DriverInstaller::~DriverInstaller() {
    cancel();
    cleanupIo();
}

// ── 静态查询 ─────────────────────────────────────────────────────────────────

QString DriverInstaller::findOdbcDriver(const QString& keyword, const QString& preferred) {
#ifdef Q_OS_WIN
    QString best;
    // 同时查 HKLM（系统级）和 HKCU（用户级，本应用注册的）
    const QStringList roots = {
        R"(HKEY_LOCAL_MACHINE\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers)",
        R"(HKEY_CURRENT_USER\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers)"
    };
    for (const QString& root : roots) {
        QSettings reg(root, QSettings::NativeFormat);
        for (const QString& name : reg.childKeys()) {
            if (!name.contains(keyword, Qt::CaseInsensitive)) continue;
            if (!preferred.isEmpty() && name.contains(preferred, Qt::CaseInsensitive)) {
                return name;
            }
            if (best.isEmpty()) best = name;
        }
    }
    return best;
#else
    Q_UNUSED(keyword)
    Q_UNUSED(preferred)
    return {};
#endif
}

QString DriverInstaller::driversRoot() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/drivers";
    QDir().mkpath(dir);
    return dir;
}

QString DriverInstaller::instantClientDir() {
    return driversRoot() + "/oracle_instantclient";
}

QString DriverInstaller::libpqDir() {
    return driversRoot() + "/libpq";
}

bool DriverInstaller::isLocalInstantClientReady() {
    return QFile::exists(instantClientDir() + "/sqora32.dll")
        && QFile::exists(instantClientDir() + "/oci.dll");
}

bool DriverInstaller::isLocalLibpqReady() {
    return QFile::exists(libpqDir() + "/libpq.dll");
}

// ── 带 UI 的驱动确保 ─────────────────────────────────────────────────────────

bool DriverInstaller::runInstallUi(QWidget* parent, const QString& title,
                                    const QString& tip, const QString& successTip,
                                    const std::function<void(DriverInstaller&)>& start) {
    auto ret = QMessageBox::question(parent, title, tip,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (ret != QMessageBox::Yes) return false;

    QProgressDialog dlg("正在准备下载...", "取消", 0, 100, parent);
    dlg.setWindowTitle(title);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setAutoClose(false);
    dlg.setMinimumWidth(360);

    DriverInstaller installer(parent);
    bool ok = false;
    QString message;
    QObject::connect(&installer, &DriverInstaller::progressChanged, &dlg,
        [&dlg](const QString& stage, int percent) {
            dlg.setLabelText(stage);
            dlg.setValue(percent);
        });
    QObject::connect(&installer, &DriverInstaller::installFinished, &dlg,
        [&ok, &message, &dlg](bool o, const QString& m) {
            ok = o;
            message = m;
            dlg.close();
        });
    QObject::connect(&dlg, &QProgressDialog::canceled, &installer, &DriverInstaller::cancel);

    start(installer);
    dlg.exec();

    if (ok) {
        QMessageBox::information(parent, "安装完成", successTip);
    } else if (!message.isEmpty()) {
        QMessageBox::warning(parent, "驱动安装失败", message);
    }
    return ok;
}

bool DriverInstaller::ensureOdbcDriver(const QString& dbType, QWidget* parent) {
    // SQL Server：优先官方 ODBC Driver 18，其次 Windows 自带 "SQL Server"
    if (dbType == "mssql") {
        if (!findOdbcDriver("sql server").isEmpty()) return true;
        return runInstallUi(parent, "缺少 SQL Server 驱动",
            "未检测到 SQL Server ODBC 驱动。\n\n"
            "是否自动下载并安装 Microsoft ODBC Driver 18 for SQL Server？\n"
            "（约 10MB，安装需要管理员授权，弹一次 UAC）",
            "SQL Server ODBC 驱动安装完成。\n可以重新尝试连接。",
            [](DriverInstaller& inst) {
                inst.installMsiPackage(MS_ODBC18_URL, "msodbcsql.msi",
                    { "/qn", "IACCEPTMSODBCSQLLICENSETERMS=YES", "ADDLOCAL=ALL" },
                    "sql server", "odbc driver");
            });
    }

    // MySQL：QMYSQL 插件缺失时走 QODBC（MySQL Connector/ODBC）
    if (dbType == "mysql") {
        if (!findOdbcDriver("mysql odbc").isEmpty()) return true;
        return runInstallUi(parent, "缺少 MySQL 驱动",
            "Qt MySQL 驱动插件（QMYSQL）未编译，将改用 ODBC 方式连接，\n"
            "但未检测到 MySQL ODBC 驱动。\n\n"
            "是否自动下载并安装 MySQL Connector/ODBC？\n"
            "（约 20MB，安装需要管理员授权，弹一次 UAC）",
            "MySQL ODBC 驱动安装完成。\n可以重新尝试连接。",
            [](DriverInstaller& inst) {
                inst.installMsiPackage(MYSQL_ODBC_URL, "mysql-connector-odbc.msi",
                    { "/qn" }, "mysql odbc", "unicode");
            });
    }

    if (dbType != "oracle") return true;

    // 仅 HKLM 注册的驱动对 ODBC 驱动管理器可见（HKCU 注册不生效，不认可）
    {
        QSettings reg(R"(HKEY_LOCAL_MACHINE\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers)",
                      QSettings::NativeFormat);
        for (const QString& name : reg.childKeys()) {
            if (name.contains("oracle", Qt::CaseInsensitive)) return true;
        }
    }

    return runInstallUi(parent, "缺少 Oracle 驱动",
        "未检测到 Oracle ODBC 驱动。\n\n"
        "是否自动下载 Oracle Instant Client（约 90MB）并安装？\n"
        "（安装到当前用户目录，无需管理员权限）",
        QString("Oracle ODBC 驱动安装完成。\n安装目录: %1\n可以重新尝试连接。")
            .arg(QDir::toNativeSeparators(instantClientDir())),
        [](DriverInstaller& inst) {
            inst.installOracleOdbc();
        });
}

bool DriverInstaller::ensurePostgresRuntime(QWidget* parent) {
    // QPSQL 插件已可加载（libpq 在 PATH 或系统已装）
    if (QSqlDatabase::drivers().contains("QPSQL")) return true;

    if (!runInstallUi(parent, "缺少 PostgreSQL 驱动运行库",
        "Qt PostgreSQL 插件已编译，但缺少 libpq 客户端运行库。\n\n"
        "是否自动下载 PostgreSQL 客户端库（约 330MB）并配置？\n"
        "（下载到当前用户目录，无需管理员权限）",
        QString("libpq 运行库安装完成。\n安装目录: %1\n\n"
                "需要重启应用后生效。").arg(QDir::toNativeSeparators(libpqDir())),
        [](DriverInstaller& inst) {
            inst.installPostgresRuntime();
        })) {
        return false;
    }
    // 本进程内 QFactoryLoader 已初始化（插件加载失败不会重试），需重启生效
    return false;
}

void DriverInstaller::preparePostgresPath() {
#ifdef Q_OS_WIN
    // QPSQL 插件依赖 LIBPQ.dll；本地已就绪则加入 DLL 搜索路径。
    // 注意：不得调用 QSqlDatabase::drivers()——插件加载失败后进程内不会重试，
    // 必须保证本函数在首次 drivers() 之前执行。
    if (!isLocalLibpqReady()) return;
    QString dir = QDir::toNativeSeparators(libpqDir());
    QString path = QString::fromLocal8Bit(qgetenv("PATH"));
    if (!path.contains(dir, Qt::CaseInsensitive)) {
        qputenv("PATH", QString("%1;%2").arg(dir, path).toLocal8Bit());
    }
#endif
}

void DriverInstaller::prepareOraclePath() {
#ifdef Q_OS_WIN
    // QOCI 插件依赖 oci.dll；本地 Instant Client 已就绪则加入 DLL 搜索路径。
    // 注意：oci.dll 加载失败会导致 QOCI 插件加载失败且进程内不重试，
    // 必须在首次 QSqlDatabase::drivers() / addDatabase() 之前执行。
    if (!isLocalInstantClientReady()) return;
    QString dir = QDir::toNativeSeparators(instantClientDir());
    QString path = QString::fromLocal8Bit(qgetenv("PATH"));
    if (!path.contains(dir, Qt::CaseInsensitive)) {
        qputenv("PATH", QString("%1;%2").arg(dir, path).toLocal8Bit());
    }
#endif
}

// ── 异步下载安装 ─────────────────────────────────────────────────────────────

void DriverInstaller::installOracleOdbc() {
    m_task = TaskOracle;
    if (isLocalInstantClientReady()) {
        // 已下载过，直接注册并返回
        extractAndRegister();
        return;
    }

    m_urls = { IC_BASIC_URL, IC_ODBC_URL };
    QString tmp = QDir::tempPath();
    m_files = { tmp + "/ic_basic.zip", tmp + "/ic_odbc.zip" };
    m_index = 0;
    m_receivedTotal = 0;
    m_cancelled = false;

    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    downloadNext();
}

void DriverInstaller::installMsiPackage(const QString& url, const QString& fileName,
                                         const QStringList& msiArgs,
                                         const QString& verifyKeyword,
                                         const QString& verifyPreferred) {
    m_task = TaskMsi;
    m_msiArgs = msiArgs;
    m_verifyKeyword = verifyKeyword;
    m_verifyPreferred = verifyPreferred;
    m_urls = { url };
    m_files = { QDir::tempPath() + "/" + fileName };
    m_index = 0;
    m_receivedTotal = 0;
    m_cancelled = false;

    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    downloadNext();
}

void DriverInstaller::installPostgresRuntime() {
    m_task = TaskLibpq;
    m_urls = { PG_BINARIES_URL };
    m_files = { QDir::tempPath() + "/pg_binaries.zip" };
    m_index = 0;
    m_receivedTotal = 0;
    m_cancelled = false;

    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    downloadNext();
}

void DriverInstaller::cancel() {
    m_cancelled = true;
    if (m_reply) {
        m_reply->abort();
    }
}

void DriverInstaller::downloadNext() {
    if (m_cancelled) {
        fail("已取消");
        return;
    }
    if (m_index >= m_urls.size()) {
        switch (m_task) {
        case TaskOracle:  extractAndRegister(); break;
        case TaskMsi:     runMsiInstall();      break;
        case TaskLibpq:   extractLibpq();       break;
        default:          fail("内部错误：未知安装任务"); break;
        }
        return;
    }

    emit progressChanged(QString("正在下载驱动包 %1/%2...").arg(m_index + 1).arg(m_urls.size()),
                         m_index * 45);

    QNetworkRequest req(QUrl(m_urls[m_index]));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    m_file = new QFile(m_files[m_index]);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail("无法写入临时文件: " + m_files[m_index]);
        return;
    }

    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (m_file) m_file->write(m_reply->readAll());
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 recv, qint64 total) {
        // 每个包约占 45% 进度
        int base = m_index * 45;
        int p = base + (total > 0 ? static_cast<int>(45.0 * recv / total) : 0);
        emit progressChanged(QString("正在下载驱动包 %1/%2（%3 MB）")
                                 .arg(m_index + 1).arg(m_urls.size())
                                 .arg(recv / 1048576),
                             qMin(p, 90));
    });
    connect(m_reply, &QNetworkReply::finished, this, &DriverInstaller::onDownloadFinished);
}

void DriverInstaller::onDownloadFinished() {
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;

    if (m_file) {
        m_file->flush();
        m_file->close();
    }

    if (m_cancelled) {
        cleanupIo();
        fail("已取消");
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QString err = reply->errorString();
        cleanupIo();
        // 按任务给出正确的手动安装指引（避免 MySQL 失败却提示装 Oracle）
        QString manual;
        switch (m_task) {
        case TaskOracle: manual = "Oracle Instant Client"; break;
        case TaskMsi:    manual = "对应的 ODBC 驱动安装包"; break;
        case TaskLibpq:  manual = "PostgreSQL 客户端库"; break;
        default:         manual = "驱动安装包"; break;
        }
        fail(QString("下载失败: %1\nURL: %2\n请检查网络后重试，或通过上方 URL 手动下载安装%3。")
                 .arg(err, m_urls.value(m_index), manual));
        return;
    }
    reply->deleteLater();

    m_index++;
    downloadNext();
}

void DriverInstaller::extractAndRegister() {
    emit progressChanged("正在解压...", 92);

    QString targetDir = instantClientDir();
    QDir().mkpath(targetDir);

    // 用系统自带 tar（Windows 10+）解压 zip；zip 内有 instantclient_21_22/ 顶层目录
    for (const QString& zip : m_files) {
        if (!QFile::exists(zip)) continue;
        QProcess proc;
        proc.start("tar", { "-xf", zip, "-C", targetDir, "--strip-components=1" });
        if (!proc.waitForFinished(120000) || proc.exitCode() != 0) {
            fail("解压失败: " + QString::fromLocal8Bit(proc.readAllStandardError()));
            return;
        }
        QFile::remove(zip);
    }

    if (!isLocalInstantClientReady()) {
        fail("解压后未找到 Oracle 驱动文件（sqora32.dll）");
        return;
    }

    emit progressChanged("正在注册 ODBC 驱动（需要管理员授权）...", 96);

#ifdef Q_OS_WIN
    // ODBC 驱动管理器只从 HKLM 枚举驱动（HKCU 的 ODBCINST.INI 不生效），必须写 HKLM
    QString dir = QDir::toNativeSeparators(targetDir);
    const QString driverName = IC_DRIVER_NAME;
    const QString hklmBase = R"(HKEY_LOCAL_MACHINE\SOFTWARE\ODBC\ODBCINST.INI)";

    auto isRegistered = [&]() {
        QSettings check(hklmBase + "\\" + driverName, QSettings::NativeFormat);
        return check.value("Driver").toString().endsWith("sqora32.dll", Qt::CaseInsensitive);
    };

    // 1) 应用以管理员运行时可直接写 HKLM
    if (!isRegistered()) {
        QSettings drvKey(hklmBase + "\\" + driverName, QSettings::NativeFormat);
        drvKey.setValue("Driver", dir + "\\sqora32.dll");
        drvKey.setValue("Setup", dir + "\\sqoras32.dll");
        drvKey.setValue("APILevel", "1");
        drvKey.setValue("SQLLevel", "1");
        drvKey.setValue("ConnectionFunctions", "YYN");
        drvKey.setValue("DriverODBCVer", "03.51");
        QSettings listKey(hklmBase + R"(\ODBC Drivers)", QSettings::NativeFormat);
        listKey.setValue(driverName, "Installed");
        drvKey.sync();
        listKey.sync();
    }

    // 2) 无权限时生成 .reg 文件，通过提权的 reg import 写入（弹一次 UAC）
    if (!isRegistered()) {
        QString regFile = QDir::tempPath() + "/oracle_odbc_register.reg";
        QString esc = QString(dir).replace("\\", "\\\\");
        QString content =
            "Windows Registry Editor Version 5.00\r\n\r\n"
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\ODBC\\ODBCINST.INI\\" + driverName + "]\r\n"
            "\"Driver\"=\"" + esc + "\\\\sqora32.dll\"\r\n"
            "\"Setup\"=\"" + esc + "\\\\sqoras32.dll\"\r\n"
            "\"APILevel\"=\"1\"\r\n"
            "\"SQLLevel\"=\"1\"\r\n"
            "\"ConnectionFunctions\"=\"YYN\"\r\n"
            "\"DriverODBCVer\"=\"03.51\"\r\n\r\n"
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\ODBC\\ODBCINST.INI\\ODBC Drivers]\r\n"
            "\"" + driverName + "\"=\"Installed\"\r\n";

        QFile f(regFile);
        if (f.open(QIODevice::WriteOnly)) {
            // .reg (5.00 格式) 要求 UTF-16 LE 带 BOM
            const char bom[2] = { '\xFF', '\xFE' };
            f.write(bom, 2);
            f.write(reinterpret_cast<const char*>(content.utf16()),
                    static_cast<qint64>(content.size()) * 2);
            f.close();

            QProcess::execute("powershell.exe", {
                "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                QString("Start-Process reg.exe -ArgumentList 'import','\"%1\"' -Verb RunAs -Wait")
                    .arg(regFile)
            });
            QFile::remove(regFile);
        }
    }

    if (!isRegistered()) {
        fail("注册 ODBC 驱动失败：需要管理员权限（请在 UAC 弹窗中允许）");
        return;
    }

    // 清理旧版本遗留的 HKCU 注册（驱动管理器不读取，避免混淆）
    QSettings hklcuBase(R"(HKEY_CURRENT_USER\SOFTWARE\ODBC\ODBCINST.INI)", QSettings::NativeFormat);
    hklcuBase.remove(driverName);
    hklcuBase.remove("ODBC Drivers/" + driverName);

    // Instant Client 目录加入当前进程 PATH（oci.dll 依赖解析）
    QString path = QString::fromLocal8Bit(qgetenv("PATH"));
    if (!path.contains(targetDir, Qt::CaseInsensitive)) {
        qputenv("PATH", QString("%1;%2").arg(targetDir, path).toLocal8Bit());
    }
#endif

    Logger::instance().success(QString("Oracle ODBC 驱动已安装: %1").arg(targetDir), "database");
    emit progressChanged("完成", 100);
    emit installFinished(true, {});
}

void DriverInstaller::runMsiInstall() {
    emit progressChanged("正在安装驱动（需要管理员授权）...", 95);

    const QString msi = QDir::toNativeSeparators(m_files.first());

    // 提权静默安装：Start-Process msiexec -Verb RunAs（弹一次 UAC）
    // 单引号内为 msiexec 的完整参数串，路径含空格时用内嵌双引号包裹
    QStringList argList = QStringList() << "/i" << ("\"" + msi + "\"") << m_msiArgs;
    QString psCmd = QString("Start-Process msiexec.exe -ArgumentList '%1' -Verb RunAs -Wait")
                        .arg(argList.join(' '));
    QProcess::execute("powershell.exe",
                      { "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", psCmd });
    QFile::remove(m_files.first());

    // 通过注册表验证安装结果（比退出码更可靠：UAC 拒绝时静默失败）
    const QString drv = findOdbcDriver(m_verifyKeyword, m_verifyPreferred);
    if (drv.isEmpty()) {
        fail(QString("驱动安装后仍未检测到（关键字: %1）。\n"
                     "请确认 UAC 弹窗已允许，或手动安装后重试。").arg(m_verifyKeyword));
        return;
    }

    Logger::instance().success(QString("ODBC 驱动已安装: %1").arg(drv), "database");
    emit progressChanged("完成", 100);
    emit installFinished(true, drv);
}

void DriverInstaller::extractLibpq() {
    emit progressChanged("正在解压 libpq...", 92);

    const QString tmp = QDir::tempPath() + "/pg_extract";
    QDir(tmp).removeRecursively();
    QDir().mkpath(tmp);

    // 只提取 pgsql/bin 与 pgsql/lib 两个目录（bsdtar 支持成员过滤，避免解压整个包）
    QProcess proc;
    proc.start("tar", { "-xf", m_files.first(), "-C", tmp, "pgsql/bin", "pgsql/lib" });
    if (!proc.waitForFinished(300000) || proc.exitCode() != 0) {
        fail("解压失败: " + QString::fromLocal8Bit(proc.readAllStandardError()));
        return;
    }
    QFile::remove(m_files.first());

    // 复制全部 DLL 到 libpq 目录（libpq 及其依赖 libssl/libcrypto/libintl 等）
    const QString target = libpqDir();
    QDir().mkpath(target);
    int copied = 0;
    for (const QString& sub : { QString("/pgsql/bin"), QString("/pgsql/lib") }) {
        QDir d(tmp + sub);
        for (const QString& dll : d.entryList({ "*.dll" }, QDir::Files)) {
            QFile::remove(target + "/" + dll);
            if (QFile::copy(d.filePath(dll), target + "/" + dll)) copied++;
        }
    }
    QDir(tmp).removeRecursively();

    if (copied == 0 || !isLocalLibpqReady()) {
        fail("解压后未找到 libpq.dll");
        return;
    }

    Logger::instance().success(QString("libpq 运行库已安装: %1").arg(target), "database");
    emit progressChanged("完成", 100);
    emit installFinished(true, {});
}

void DriverInstaller::fail(const QString& message) {
    Logger::instance().error("数据库驱动安装失败: " + message, "database");
    emit installFinished(false, message);
}

void DriverInstaller::cleanupIo() {
    if (m_reply) {
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    if (m_file) {
        delete m_file;
        m_file = nullptr;
    }
}
