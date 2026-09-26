#include "database/DbSessionPane.h"
#include "connections/ConnectionManager.h"
#include "core/Logger.h"

DbSessionPane::DbSessionPane(QWidget* parent) : QWidget(parent) {}

QString DbSessionPane::currentConnId() const   { return m_connId; }
QString DbSessionPane::currentConnName() const { return m_connName; }
QString DbSessionPane::currentDbType() const   { return m_dbType; }
bool    DbSessionPane::isConnected() const     { return m_connected; }

void    DbSessionPane::disconnectFromUI()      { m_connected = false; }

void DbSessionPane::connectTo(const ConnectionManager::Connection& conn) {
    m_connId   = conn.id;
    m_connName = conn.name;
    m_dbType   = ConnectionManager::dbTypeToString(conn.dbType);
    m_connected = false;

    emit connecting(m_connId, m_connName);
    Logger::instance().info(
        QString("DbSessionPane::connectTo placeholder: %1 (%2)")
            .arg(conn.name, m_dbType),
        "database");

    // placeholder：原实现使用真实驱动连接各 DB，这里不连
    emit disconnected(m_connId);
}