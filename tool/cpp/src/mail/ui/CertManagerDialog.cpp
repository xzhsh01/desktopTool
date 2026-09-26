#include "CertManagerDialog.h"
#include "mail/crypto/CertificateManager.h"
#include "core/Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QLineEdit>
#include <QClipboard>
#include <QGuiApplication>
#include <QDateTime>
#include <QRegularExpression>

CertManagerDialog::CertManagerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("邮件加密证书管理"));
    resize(760, 480);
    buildUi();
    refreshTables();

    // 监听证书变更，跨进程/跨对话框自动刷新
    connect(&CertificateManager::instance(), &CertificateManager::certAdded,
            this, [this]{ refreshTables(); });
    connect(&CertificateManager::instance(), &CertificateManager::certRemoved,
            this, [this]{ refreshTables(); });
}

void CertManagerDialog::buildUi() {
    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(QStringLiteral(
        "<b>S/MIME 证书</b> 用于邮件加密（X.509 标准）。<br>"
        "• <b>个人证书</b>（含私钥）：解密别人发给你的 S/MIME 邮件 / 签名你的邮件<br>"
        "• <b>联系人公钥</b>：给对方发加密邮件（需先获取对方的 .pem/.crt 公钥证书）<br>"
        "<small>存储目录：%APPDATA%/KFrame/bambooRat/mail/certs/</small>"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    m_tabs = new QTabWidget;
    root->addWidget(m_tabs, 1);

    // ── Tab 1: 个人证书 ──
    auto* p1 = new QWidget;
    auto* l1 = new QVBoxLayout(p1);
    m_personalTb = new QTableWidget(0, 5);
    m_personalTb->setHorizontalHeaderLabels(
        {QStringLiteral("CN"), QStringLiteral("邮箱"), QStringLiteral("颁发者"),
         QStringLiteral("过期时间"), QStringLiteral("SHA-1 指纹")});
    m_personalTb->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_personalTb->horizontalHeader()->setStretchLastSection(true);
    m_personalTb->setSelectionBehavior(QTableWidget::SelectRows);
    m_personalTb->setEditTriggers(QTableWidget::NoEditTriggers);
    l1->addWidget(m_personalTb, 1);

    auto* p1Bar = new QHBoxLayout;
    auto* importP12Btn = new QPushButton(QStringLiteral("导入个人证书 (.p12/.pfx)"));
    auto* exportP12Btn = new QPushButton(QStringLiteral("导出 PFX"));
    auto* delP12Btn    = new QPushButton(QStringLiteral("删除选中"));
    p1Bar->addWidget(importP12Btn);
    p1Bar->addWidget(exportP12Btn);
    p1Bar->addWidget(delP12Btn);
    p1Bar->addStretch();
    l1->addLayout(p1Bar);
    connect(importP12Btn, &QPushButton::clicked, this, &CertManagerDialog::importPersonal);
    connect(exportP12Btn, &QPushButton::clicked, this, &CertManagerDialog::exportSelectedPersonal);
    connect(delP12Btn,    &QPushButton::clicked, this, &CertManagerDialog::removeSelectedPersonal);

    // ── Tab 2: 联系人公钥 ──
    auto* p2 = new QWidget;
    auto* l2 = new QVBoxLayout(p2);
    m_contactsTb = new QTableWidget(0, 5);
    m_contactsTb->setHorizontalHeaderLabels(
        {QStringLiteral("CN"), QStringLiteral("邮箱"), QStringLiteral("颁发者"),
         QStringLiteral("过期时间"), QStringLiteral("SHA-1 指纹")});
    m_contactsTb->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_contactsTb->horizontalHeader()->setStretchLastSection(true);
    m_contactsTb->setSelectionBehavior(QTableWidget::SelectRows);
    m_contactsTb->setEditTriggers(QTableWidget::NoEditTriggers);
    l2->addWidget(m_contactsTb, 1);

    auto* p2Bar = new QHBoxLayout;
    auto* importPemBtn = new QPushButton(QStringLiteral("导入联系人公钥 (.pem/.crt)"));
    auto* exportPemBtn = new QPushButton(QStringLiteral("导出 PEM"));
    auto* delPemBtn    = new QPushButton(QStringLiteral("删除选中"));
    p2Bar->addWidget(importPemBtn);
    p2Bar->addWidget(exportPemBtn);
    p2Bar->addWidget(delPemBtn);
    p2Bar->addStretch();
    l2->addLayout(p2Bar);
    connect(importPemBtn, &QPushButton::clicked, this, &CertManagerDialog::importContact);
    connect(exportPemBtn, &QPushButton::clicked, this, &CertManagerDialog::exportSelectedContact);
    connect(delPemBtn,    &QPushButton::clicked, this, &CertManagerDialog::removeSelectedContact);

    m_tabs->addTab(p1, QStringLiteral("个人证书"));
    m_tabs->addTab(p2, QStringLiteral("联系人公钥"));

    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet("color:#888;");
    root->addWidget(m_statusLabel);

    auto* btnBox = new QHBoxLayout;
    btnBox->addStretch();
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"));
    btnBox->addWidget(closeBtn);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    root->addLayout(btnBox);
}

void CertManagerDialog::refreshTables() {
    fillPersonalTable();
    fillContactsTable();
}

void CertManagerDialog::fillPersonalTable() {
    m_personalTb->setRowCount(0);
    auto list = CertificateManager::instance().listPersonal();
    m_personalTb->setRowCount(list.size());
    int row = 0;
    const QDateTime now = QDateTime::currentDateTime();
    for (const auto& c : list) {
        auto* cn  = new QTableWidgetItem(c.commonName);
        auto* em  = new QTableWidgetItem(c.email);
        auto* isr = new QTableWidgetItem(c.issuer);
        auto* fp  = new QTableWidgetItem(c.sha1Fingerprint);
        // 过期时间列：附"距 X 天/已过期"标签 + 红/橙色
        QString expText = c.notAfter.toString("yyyy-MM-dd");
        QColor  expColor;
        if (!c.notAfter.isValid()) {
            expText += QStringLiteral(" (无日期)");
            expColor = QColor("#888");
        } else if (c.notAfter < now) {
            int ago = now.daysTo(c.notAfter); // 负数
            expText += QStringLiteral("  (已过期 %1 天)").arg(-ago);
            expColor = QColor("#d33");
        } else {
            int days = now.daysTo(c.notAfter);
            expText += QStringLiteral("  (%1 天)").arg(days);
            if (days < 30)      expColor = QColor("#e80");
            else if (days < 90) expColor = QColor("#a80");
        }
        auto* exp = new QTableWidgetItem(expText);
        if (expColor.isValid()) {
            exp->setForeground(QBrush(expColor));
            QFont f = exp->font(); f.setBold(true); exp->setFont(f);
        }
        cn->setData(Qt::UserRole, c.id);
        m_personalTb->setItem(row, 0, cn);
        m_personalTb->setItem(row, 1, em);
        m_personalTb->setItem(row, 2, isr);
        m_personalTb->setItem(row, 3, exp);
        m_personalTb->setItem(row, 4, fp);
        ++row;
    }
    m_statusLabel->setText(QStringLiteral("个人证书: %1  联系人公钥: %2")
        .arg(list.size()).arg(CertificateManager::instance().listContacts().size()));
}

void CertManagerDialog::fillContactsTable() {
    m_contactsTb->setRowCount(0);
    auto list = CertificateManager::instance().listContacts();
    m_contactsTb->setRowCount(list.size());
    int row = 0;
    const QDateTime now = QDateTime::currentDateTime();
    for (const auto& c : list) {
        auto* cn  = new QTableWidgetItem(c.commonName);
        auto* em  = new QTableWidgetItem(c.email);
        auto* isr = new QTableWidgetItem(c.issuer);
        auto* fp  = new QTableWidgetItem(c.sha1Fingerprint);
        // 过期时间列同上
        QString expText = c.notAfter.toString("yyyy-MM-dd");
        QColor  expColor;
        if (!c.notAfter.isValid()) {
            expText += QStringLiteral(" (无日期)");
            expColor = QColor("#888");
        } else if (c.notAfter < now) {
            int ago = now.daysTo(c.notAfter);
            expText += QStringLiteral("  (已过期 %1 天)").arg(-ago);
            expColor = QColor("#d33");
        } else {
            int days = now.daysTo(c.notAfter);
            expText += QStringLiteral("  (%1 天)").arg(days);
            if (days < 30)      expColor = QColor("#e80");
            else if (days < 90) expColor = QColor("#a80");
        }
        auto* exp = new QTableWidgetItem(expText);
        if (expColor.isValid()) {
            exp->setForeground(QBrush(expColor));
            QFont f = exp->font(); f.setBold(true); exp->setFont(f);
        }
        cn->setData(Qt::UserRole, c.id);
        m_contactsTb->setItem(row, 0, cn);
        m_contactsTb->setItem(row, 1, em);
        m_contactsTb->setItem(row, 2, isr);
        m_contactsTb->setItem(row, 3, exp);
        m_contactsTb->setItem(row, 4, fp);
        ++row;
    }
}

void CertManagerDialog::importPersonal() {
    QString path = QFileDialog::getOpenFileName(this,
        QStringLiteral("选择 PKCS#12 个人证书"),
        QString(), QStringLiteral("PKCS#12 (*.p12 *.pfx);;All files (*)"));
    if (path.isEmpty()) return;
    bool ok = false;
    QString pwd = QInputDialog::getText(this,
        QStringLiteral("PKCS#12 口令"),
        QStringLiteral("请输入 PKCS#12 文件的口令："),
        QLineEdit::Password, QString(), &ok);
    if (!ok) return;
    auto info = CertificateManager::instance().importPersonal(path, pwd);
    if (info.id.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), CertificateManager::getLastError());
        return;
    }
    QMessageBox::information(this, QStringLiteral("导入成功"),
        QStringLiteral("个人证书已导入：%1 (%2)").arg(info.commonName, info.email));
}

void CertManagerDialog::importContact() {
    QString path = QFileDialog::getOpenFileName(this,
        QStringLiteral("选择联系人公钥 (PEM X.509)"),
        QString(), QStringLiteral("PEM X.509 (*.pem *.crt *.cer);;All files (*)"));
    if (path.isEmpty()) return;
    auto info = CertificateManager::instance().importContact(path);
    if (info.id.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), CertificateManager::getLastError());
        return;
    }
    QMessageBox::information(this, QStringLiteral("导入成功"),
        QStringLiteral("联系人公钥已导入：%1 (%2)").arg(info.commonName, info.email));
}

void CertManagerDialog::removeSelectedPersonal() {
    auto row = m_personalTb->currentRow();
    if (row < 0) return;
    QString id = m_personalTb->item(row, 0)->data(Qt::UserRole).toString();
    if (QMessageBox::question(this, QStringLiteral("确认删除"),
        QStringLiteral("确定删除该个人证书？删除后无法解密已收到的加密邮件。"))
        != QMessageBox::Yes) return;
    CertificateManager::instance().removePersonal(id);
}

void CertManagerDialog::removeSelectedContact() {
    auto row = m_contactsTb->currentRow();
    if (row < 0) return;
    QString id = m_contactsTb->item(row, 0)->data(Qt::UserRole).toString();
    if (QMessageBox::question(this, QStringLiteral("确认删除"),
        QStringLiteral("确定删除该联系人公钥？删除后将无法向该联系人发送 S/MIME 加密邮件。"))
        != QMessageBox::Yes) return;
    CertificateManager::instance().removeContact(id);
}

void CertManagerDialog::exportSelectedPersonal() {
    auto row = m_personalTb->currentRow();
    if (row < 0) {
        QMessageBox::information(this, QStringLiteral("未选择"),
            QStringLiteral("请先在表格中选择一个个人证书。"));
        return;
    }
    QString id = m_personalTb->item(row, 0)->data(Qt::UserRole).toString();

    // 1) 询问"当前 PKCS#12 口令"（解锁本地存储用）
    bool ok = false;
    QString curPwd = QInputDialog::getText(this,
        QStringLiteral("当前 PKCS#12 口令"),
        QStringLiteral("请输入该个人证书的当前 PKCS#12 口令："),
        QLineEdit::Password, QString(), &ok);
    if (!ok || curPwd.isEmpty()) return;

    // 2) 询问"新 PFX 文件口令"
    QString newPwd = QInputDialog::getText(this,
        QStringLiteral("新 PKCS#12 口令"),
        QStringLiteral("请设置新导出的 .p12 文件的口令（务必牢记）："),
        QLineEdit::Password, QString(), &ok);
    if (!ok || newPwd.isEmpty()) return;
    QString newPwd2 = QInputDialog::getText(this,
        QStringLiteral("再次输入新口令"),
        QStringLiteral("请再次输入新口令以确认："),
        QLineEdit::Password, QString(), &ok);
    if (!ok) return;
    if (newPwd != newPwd2) {
        QMessageBox::warning(this, QStringLiteral("口令不一致"),
            QStringLiteral("两次输入的新口令不一致，已取消。"));
        return;
    }

    // 3) 选择导出路径
    QString destPath = QFileDialog::getSaveFileName(this,
        QStringLiteral("导出个人证书为 PKCS#12"),
        QDir::homePath() + "/personal.p12",
        QStringLiteral("PKCS#12 (*.p12 *.pfx)"));
    if (destPath.isEmpty()) return;

    QString err;
    if (!CertificateManager::instance().exportPersonal(id, destPath, curPwd, newPwd, &err)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), err);
        return;
    }
    QMessageBox::information(this, QStringLiteral("导出成功"),
        QStringLiteral("已导出到：\n%1\n\n请妥善保管 .p12 文件和新口令——文件可用于备份/迁移到其他设备。").arg(destPath));
}

void CertManagerDialog::exportSelectedContact() {
    auto row = m_contactsTb->currentRow();
    if (row < 0) {
        QMessageBox::information(this, QStringLiteral("未选择"),
            QStringLiteral("请先在表格中选择一个联系人公钥。"));
        return;
    }
    QString id = m_contactsTb->item(row, 0)->data(Qt::UserRole).toString();
    QString em  = m_contactsTb->item(row, 1)->text();
    QString safe = em.isEmpty() ? id.left(16) : em;
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._@-]")), QStringLiteral("_"));

    QString destPath = QFileDialog::getSaveFileName(this,
        QStringLiteral("导出联系人公钥为 PEM"),
        QDir::homePath() + "/" + safe + ".pem",
        QStringLiteral("PEM X.509 (*.pem *.crt)"));
    if (destPath.isEmpty()) return;

    QString err;
    if (!CertificateManager::instance().exportContact(id, destPath, &err)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), err);
        return;
    }
    QMessageBox::information(this, QStringLiteral("导出成功"),
        QStringLiteral("已导出到：\n%1\n\n也可使用\"复制到剪贴板\"按钮直接发给同事。").arg(destPath));
}