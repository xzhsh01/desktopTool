#pragma once

#include <QMainWindow>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QList>
#include <QToolButton>
#include <QPushButton>
#include <QAbstractButton>
#include <QLabel>

class DashboardWidget;
class ConnectionsWidget;
class SSHTermWidget;
class FileManagerWidget;
class RedisWidget;
class RDPWidget;
class DatabaseWidget;
class MailWidget;
class WeChatWidget;
class DocumentWidget;
class SettingsWidget;
class LogsWidget;
class AboutWidget;

/**
 * MainWindow: 主窗口
 * 对应原 Electron frameless window + MainLayout.vue
 * 无边框窗口 + 侧边栏导航 + 状态栏 + 系统托盘
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

    // 供其他视图调用的导航/操作接口
    void openSSHTerminal(const QString& connectionId);
    void navigateToPage(const QString& pageId);

protected:
    // 无边框窗口拖拽移动
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void navigateTo(const QString& pageId);
    void onMinimize();
    void onMaximize();
    void onClose();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onSettingsChanged(const QVariantMap& settings);
    void updateStatusBar();

private:
    void setupUI();
    void setupTitleBar();
    void setupSidebar();
    void setupPages();
    void setupStatusBar();
    void setupTray();
    void applyTheme();
    // 根据已配置的连接/邮箱动态显示侧边栏功能项
    void refreshSidebar();
    // 收缩/展开侧边栏（收缩后仅显示图标）
    void setSidebarCollapsed(bool collapsed);

    // 无边框窗口
    QPoint m_dragStartPos;
    bool m_dragging = false;
    bool m_isQuitting = false;

    // Title bar
    QWidget* m_titleBar = nullptr;
    QLabel* m_appLabel = nullptr;

    // Sidebar
    QWidget* m_sidebar = nullptr;
    QLabel* m_funcLabel = nullptr;          // 「功能」分组标签（收缩时隐藏）
    QPushButton* m_collapseBtn = nullptr;   // 收缩/展开按钮
    QList<QAbstractButton*> m_navButtons;   // 侧边栏导航 + 顶部系统导航
    bool m_sidebarCollapsed = false;
    QString m_currentPage;

    // Content
    QStackedWidget* m_contentStack = nullptr;

    // Pages
    DashboardWidget* m_dashboard = nullptr;
    ConnectionsWidget* m_connections = nullptr;
    SSHTermWidget* m_sshTerm = nullptr;
    FileManagerWidget* m_fileManager = nullptr;
    RedisWidget* m_redis = nullptr;
    RDPWidget* m_rdp = nullptr;
    DatabaseWidget* m_database = nullptr;
    MailWidget* m_mail = nullptr;
    WeChatWidget* m_wechat = nullptr;
    DocumentWidget* m_documents = nullptr;
    SettingsWidget* m_settings = nullptr;
    LogsWidget* m_logs = nullptr;
    AboutWidget* m_about = nullptr;

    // Status bar
    QLabel* m_statusConnections = nullptr;
    QLabel* m_statusVersion = nullptr;

    // Tray
    QSystemTrayIcon* m_tray = nullptr;
};
