#pragma once

#include <QWidget>
#include <QDialog>

class QScrollArea;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QTextEdit;
class QFormLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class QFrame;
class ConnectionManager;

/**
 * ConnectionsWidget: 应用管理
 * 对应原 src/views/Connections.vue
 * 按分类（SSH/数据库/Redis/RDP/邮箱）分块展示连接卡片 + 新建/编辑对话框
 */
class ConnectionsWidget : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionsWidget(QWidget* parent = nullptr);

private slots:
    void refresh();

public slots:
    // 弹出新建流程（顶部「新建」按钮和本页「新建连接」共用）
    void showAddDialog();

private:
    void setupUI();
    bool eventFilter(QObject* obj, QEvent* event) override;
    void editConnection(const QString& id);
    void deleteConnection(const QString& id);
    void connectById(const QString& id);
    // 按指定类型直接新建（分类块上的「＋」按钮），跳过类型选择页
    void addByType(const QString& type, const QString& dbType = QString());

    // 分类区块（标题 + 卡片网格），返回该区块容器；无内容时返回 nullptr
    QFrame* makeCategorySection(const QString& type, const QString& title,
                                const QString& icon, const QString& color,
                                const QStringList& cardIds);
    // 单张连接卡片（双击连接）
    QFrame* makeCard(const QString& id, const QString& kind,
                     const QString& name, const QString& subtitle,
                     const QString& color, const QString& badge);

    QScrollArea* m_scroll = nullptr;
    QWidget* m_content = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;
    QLineEdit* m_searchEdit = nullptr;   // 关键字搜索（名称/主机/用户/邮箱）
    QComboBox* m_typeFilter = nullptr;   // 分类筛选
};

/**
 * AppTypeSelectorDialog: 选择应用类型页面
 * 点击「新建连接」后先弹出，让用户选择应用类型（SSH/SFTP、邮箱、数据库等），
 * 数据库可继续细分 MySQL/Oracle/PostgreSQL 等。
 * 选择完成后根据类型打开 ConnectionDialog 并预设类型。
 */
class AppTypeSelectorDialog : public QDialog {
    Q_OBJECT

public:
    explicit AppTypeSelectorDialog(QWidget* parent = nullptr);

    // 选择结果：连接类型（ssh/mail/database/redis/rdp）
    QString selectedType() const { return m_type; }
    // 数据库子类型（mysql/oracle/postgres/mssql/sqlite），仅 database 有效
    QString selectedDbType() const { return m_dbType; }

private:
    void buildDatabaseSubPage();
    void selectApp(const QString& type, const QString& dbType = QString());

    QString m_type;
    QString m_dbType;

    QStackedWidget* m_stack = nullptr;   // 主应用页 / 数据库子类型页
    QLabel* m_titleLbl = nullptr;
    QPushButton* m_backBtn = nullptr;    // 数据库子页返回
};

/**
 * ConnectionDialog: 新建/编辑连接对话框
 * 对应原 Connections.vue 中的 el-dialog 表单
 */
class ConnectionDialog : public QDialog {
    Q_OBJECT

public:
    // mode: "add" 或 "edit"
    ConnectionDialog(const QString& editId = QString(), QWidget* parent = nullptr);

    // 预选连接类型（ssh/database/redis/rdp/mail），并锁定类型选择
    void presetType(const QString& type);
    // 预选数据库子类型（mysql/oracle/...），锁定数据库类型选择
    void presetDbType(const QString& dbType);

    // 对话框关闭后获取表单数据
    QVariantMap formData() const;

private slots:
    void browsePrivateKey();
    void browseSqliteFile();
    void browseInstantClient();
    void testConnection();
    void accept() override;

private:
    void setupUI();
    void loadFromConnection(const QString& id);
    void updateFieldVisibility();

    // 测试连接辅助（同步、带超时）
    bool testTcpPort(const QString& host, int port, QString* err);
    bool testSshService(const QString& host, int port, QString* info);
    bool testRedisService(const QString& host, int port, QString* err);
    bool testDatabase(QString* err);

    // 解析「数据库连接」输入：完整 ODBC 串原样保留，
    // 简式 host[:port][/database] 解析到对应字段
    void parseConnectionInput(const QString& input, QString& host, int& port,
                              QString& database, QString& connStr) const;

    QString m_editId;

    // 动态行的 label（用于控制可见性）
    QLabel* m_dbTypeLabel = nullptr;
    QLabel* m_dbLabel = nullptr;
    QLabel* m_authLabel = nullptr;
    QLabel* m_keyLabel = nullptr;
    QLabel* m_clientLabel = nullptr;
    QLabel* m_passLabel = nullptr;
    QLabel* m_hostLabel = nullptr;
    QLabel* m_portLabel = nullptr;
    QLabel* m_connStrLabel = nullptr;

    // 表单控件
    QLineEdit* m_nameEdit = nullptr;
    QComboBox* m_typeCombo = nullptr;
    QComboBox* m_dbTypeCombo = nullptr;
    QLineEdit* m_hostEdit = nullptr;
    QSpinBox* m_portSpin = nullptr;
    QLineEdit* m_connStrEdit = nullptr;
    QLineEdit* m_userEdit = nullptr;
    QLineEdit* m_passEdit = nullptr;
    QLineEdit* m_dbEdit = nullptr;
    QComboBox* m_authTypeCombo = nullptr;
    QLineEdit* m_keyPathEdit = nullptr;
    QLineEdit* m_instantClientEdit = nullptr;
    QTextEdit* m_descEdit = nullptr;
    QPushButton* m_testBtn = nullptr;
};
