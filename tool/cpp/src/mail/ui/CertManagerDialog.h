#pragma once

#include <QDialog>
#include <QString>
#include "mail/crypto/CertificateManager.h"

class QTabWidget;
class QTableWidget;
class QLabel;

/**
 * CertManagerDialog: 邮件 S/MIME 证书管理对话框
 *
 * 两个 tab：
 *   1) 个人证书（含私钥，PKCS#12）：用于解密别人发给我的 S/MIME 邮件
 *   2) 联系人公钥（X.509 PEM）：用于给对方发 S/MIME 加密邮件
 *
 * 存储：%APPDATA%/KFrame/bambooRat/mail/certs/
 *   personal/<id>.p12  contacts/<sha1fp>.pem  index.json
 */
class CertManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit CertManagerDialog(QWidget* parent = nullptr);

private slots:
    void refreshTables();
    void importPersonal();
    void importContact();
    void removeSelectedPersonal();
    void removeSelectedContact();
    void exportSelectedPersonal();     // 导出个人证书到 PFX
    void exportSelectedContact();      // 导出联系人公钥到 PEM

private:
    void buildUi();
    void fillPersonalTable();
    void fillContactsTable();

    QTabWidget*  m_tabs        = nullptr;
    QTableWidget* m_personalTb = nullptr;
    QTableWidget* m_contactsTb = nullptr;
    QLabel*       m_statusLabel = nullptr;
};