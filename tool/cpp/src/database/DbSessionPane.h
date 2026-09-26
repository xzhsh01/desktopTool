#pragma once
#include <QString>
#include <QWidget>
#include "connections/ConnectionManager.h"

/**
 * DbSessionPane — 数据库会话 UI（重构中 placeholder 实现）
 *
 * 历史版本包含驱动安装、SQL 编辑器、表数据浏览等复杂 UI，
 * 当前实文件丢失，重建最小可编译版本：仅承载"会话状态 + 信号"，UI 不渲染。
 * UI 部分后续根据产品需求补回。
 */
class DbSessionPane : public QWidget {
    Q_OBJECT
public:
    explicit DbSessionPane(QWidget* parent = nullptr);

    // 当前会话状态查询
    QString currentConnId()   const;
    QString currentConnName() const;
    QString currentDbType()   const;
    bool    isConnected()     const;

    // UI 操作
    void    disconnectFromUI();

public slots:
    // 发起一次数据库连接（UI 上不可见，当前为 placeholder：不实际连接）
    void    connectTo(const ConnectionManager::Connection& conn);

signals:
    void connecting(const QString& connId, const QString& displayName);
    void connected(const QString& connId, const QString& serverVersion);
    void disconnected(const QString& connId);

private:
    QString m_connId;
    QString m_connName;
    QString m_dbType;
    bool    m_connected = false;
};