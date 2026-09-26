#include "mail/AccountDialog.h"
#include "mail/MailAccountManager.h"
#include "mail/MailPoller.h"
#include "mail/ui/CertManagerDialog.h"
#include "app/Theme.h"
#include "core/Settings.h"
#include "core/Logger.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QTabWidget>
#include <QCoreApplication>

AccountDialog::AccountDialog(const QString& mode, const QVariantMap& data, QWidget* parent)
    : QDialog(parent), m_mode(mode) {
    setWindowTitle(mode == "edit" ? "编辑邮箱账号" : "新增邮箱账号");
    setMinimumWidth(520);
    setupUI();
    loadData(data);
}

void AccountDialog::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);

    auto* header = new QLabel(m_mode == "edit" ? "编辑邮箱账号" : "新增邮箱账号");
    header->setStyleSheet(Theme::pageHeader());
    layout->addWidget(header);

    // 主内容：基本信息 tab + 邮件加密 tab
    auto* tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    layout->addWidget(tabs);

    // ── 基本信息 ──
    auto* baseGroup = new QGroupBox("基本信息");
    auto* baseForm = new QFormLayout(baseGroup);
    baseForm->setSpacing(8);

    m_nameEdit = new QLineEdit;
    m_nameEdit->setPlaceholderText("如 工作邮箱 / 个人邮箱（留空则使用邮箱地址）");
    baseForm->addRow("昵称:", m_nameEdit);

    m_emailEdit = new QLineEdit;
    m_emailEdit->setPlaceholderText("完整邮箱地址，如 you@qq.com");
    connect(m_emailEdit, &QLineEdit::textChanged, this, &AccountDialog::onEmailChanged);
    baseForm->addRow("邮箱:", m_emailEdit);

    m_passEdit = new QLineEdit;
    m_passEdit->setEchoMode(QLineEdit::Password);
    m_passEdit->setPlaceholderText("密码 / 授权码（QQ/163 邮箱需在邮箱设置中开启 SMTP/IMAP）");
    baseForm->addRow("密码:", m_passEdit);

    // 显示/隐藏密码按钮
    {
        auto* toggle = new QPushButton("显示");
        toggle->setCheckable(true);
        toggle->setFixedWidth(56);
        toggle->setCursor(Qt::PointingHandCursor);
        connect(toggle, &QPushButton::toggled, this, [this, toggle](bool on) {
            m_passEdit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
            toggle->setText(on ? "隐藏" : "显示");
        });
        auto* wrap = new QWidget(this);
        auto* hl = new QHBoxLayout(wrap);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);
        hl->addWidget(m_passEdit, 1);
        hl->addWidget(toggle);
        // 把 wrap 替换到 baseForm 中密码那一行
        baseForm->removeRow(baseForm->rowCount() - 1);
        baseForm->addRow("密码:", wrap);
    }

    m_defaultCheck = new QCheckBox("设为默认发件账号");
    baseForm->addRow("", m_defaultCheck);

    m_pollIntervalSpin = new QSpinBox;
    m_pollIntervalSpin->setRange(1, 1440);
    m_pollIntervalSpin->setSuffix(" 分钟");
    m_pollIntervalSpin->setToolTip("自动轮询新邮件的全局间隔（对所有邮箱账号生效）");
    baseForm->addRow("轮询间隔:", m_pollIntervalSpin);

    // ── 邮件服务器 ──
    auto* srvGroup = new QGroupBox("邮件服务器");
    auto* srvForm = new QFormLayout(srvGroup);
    srvForm->setSpacing(8);
    srvForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // ── 收件服务器（固定 IMAP，直接配置，不再选择协议） ──
    auto* recvGroup = new QGroupBox(QStringLiteral("收件服务器"));
    {
        auto* f = new QFormLayout(recvGroup);
        f->setSpacing(6);
        f->setContentsMargins(12, 18, 12, 12);
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_imapHostEdit = new QLineEdit;
        m_imapHostEdit->setPlaceholderText("如 imap.qq.com");
        f->addRow("服务器:", m_imapHostEdit);

        m_imapPortSpin = new QSpinBox;
        m_imapPortSpin->setRange(1, 65535);
        m_imapPortSpin->setValue(993);
        f->addRow("端口:", m_imapPortSpin);

        m_imapSslCheck = new QCheckBox("SSL 直连（勾选=993，不勾选=143）");
        m_imapSslCheck->setChecked(true);
        f->addRow("", m_imapSslCheck);
    }
    connect(m_imapSslCheck, &QCheckBox::toggled, this, [this](bool on){
        if (m_imapPortSpin->value() == 993 || m_imapPortSpin->value() == 143
            || m_imapPortSpin->value() == 0) {
            m_imapPortSpin->setValue(on ? 993 : 143);
        }
    });
    srvForm->addRow(recvGroup);

    // ── 发件服务器（SMTP）：始终直接配置 ──
    auto* smtpGroup = new QGroupBox(QStringLiteral("发件服务器（SMTP）"));
    {
        auto* f = new QFormLayout(smtpGroup);
        f->setSpacing(6);
        f->setContentsMargins(12, 18, 12, 12);
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_smtpHostEdit = new QLineEdit;
        m_smtpHostEdit->setPlaceholderText("如 smtp.qq.com");
        f->addRow("服务器:", m_smtpHostEdit);

        m_smtpPortSpin = new QSpinBox;
        m_smtpPortSpin->setRange(1, 65535);
        m_smtpPortSpin->setValue(465);
        f->addRow("端口:", m_smtpPortSpin);

        m_smtpSslCheck = new QCheckBox("SSL 直连（465 勾选；587 端口不勾选走 STARTTLS）");
        m_smtpSslCheck->setChecked(true);
        f->addRow("", m_smtpSslCheck);
    }
    srvForm->addRow(smtpGroup);

    // ── 根据邮箱自动配置 ――
    // 按当前邮箱域名自动填充收件(IMAP)/发件(SMTP)服务器、端口、SSL，有预设则一键配好
    auto* autoBtn = new QPushButton(QStringLiteral("根据邮箱自动配置"));
    autoBtn->setCursor(Qt::PointingHandCursor);
    autoBtn->setToolTip(
        "按当前邮箱域名自动填充收件(IMAP)与发件(SMTP)服务器、端口、SSL；\n"
        "支持的域名：QQ/163/126/Gmail/Outlook/阿里/雅虎/iCloud/139/新浪/搜狐/Foxmail 等。");
    connect(autoBtn, &QPushButton::clicked, this, [this]{
        applyPresetForEmail();
    });
    srvForm->insertRow(0, QStringLiteral("自动配置:"), autoBtn);

    // ── CalDAV 高级 ──
    auto* sep2 = new QFrame;
    sep2->setFrameShape(QFrame::HLine);
    sep2->setFrameShadow(QFrame::Sunken);
    srvForm->addRow(sep2);

    auto* calDavHeader = new QHBoxLayout;
    calDavHeader->setContentsMargins(0, 0, 0, 0);
    calDavHeader->setSpacing(6);
    m_calDavEnableCheck = new QCheckBox("启用 CalDAV（日历 / 联系人同步）");
    m_calDavEnableCheck->setToolTip(
        "CalDAV 用于同步日历与通讯录。\n"
        "常用域名：iCloud = caldav.icloud.com，Outlook = outlook.office365.com");
    calDavHeader->addWidget(m_calDavEnableCheck);
    calDavHeader->addStretch();
    srvForm->addRow(calDavHeader);

    // CalDAV 详细面板
    m_calDavPanel = new QWidget;
    auto* calDavForm = new QFormLayout(m_calDavPanel);
    calDavForm->setContentsMargins(20, 4, 0, 0);
    calDavForm->setSpacing(6);
    calDavForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_calDavHostEdit = new QLineEdit;
    m_calDavHostEdit->setPlaceholderText("如 caldav.icloud.com");
    calDavForm->addRow("CalDAV 服务器:", m_calDavHostEdit);

    m_calDavPortSpin = new QSpinBox;
    m_calDavPortSpin->setRange(1, 65535);
    m_calDavPortSpin->setValue(443);
    calDavForm->addRow("CalDAV 端口:", m_calDavPortSpin);

    m_calDavSslCheck = new QCheckBox("SSL 直连（推荐 443）");
    m_calDavSslCheck->setChecked(true);
    calDavForm->addRow("", m_calDavSslCheck);

    m_calDavUserEdit = new QLineEdit;
    m_calDavUserEdit->setPlaceholderText("默认使用邮箱地址");
    calDavForm->addRow("CalDAV 用户名:", m_calDavUserEdit);

    srvForm->addRow(m_calDavPanel);
    m_calDavPanel->setVisible(false);

    connect(m_calDavEnableCheck, &QCheckBox::toggled,
            this, &AccountDialog::onCalDavToggled);

    // ── 基本信息 tab：收纳原"基本信息"+"邮件服务器"两组 ──
    auto* basicPage = new QWidget;
    {
        auto* bl = new QVBoxLayout(basicPage);
        bl->setContentsMargins(16, 16, 16, 16);
        bl->setSpacing(14);
        bl->addWidget(baseGroup);
        bl->addWidget(srvGroup);
        bl->addStretch();
    }
    tabs->addTab(basicPage, QStringLiteral("基本信息"));

    // ── 邮件加密 tab：原"设置 → 邮件加密"移入 ──
    auto* encPage = new QWidget;
    {
        auto* el = new QVBoxLayout(encPage);
        el->setContentsMargins(16, 16, 16, 16);
        el->setSpacing(14);
        auto* encGroup = new QGroupBox(QStringLiteral("邮件加密"));
        auto* ef = new QFormLayout(encGroup);
        ef->setSpacing(10);
        {
            auto* wrap = new QWidget;
            auto* hl = new QHBoxLayout(wrap);
            hl->setContentsMargins(0, 0, 0, 0);
            hl->setSpacing(8);
            auto* openCertBtn = new QPushButton(QStringLiteral("管理 S/MIME 证书…"));
            openCertBtn->setToolTip(
                QStringLiteral("管理 S/MIME 个人证书与联系人公钥（用于邮件加密）"));
            openCertBtn->setCursor(Qt::PointingHandCursor);
            connect(openCertBtn, &QPushButton::clicked,
                    this, &AccountDialog::openMailCertManager);
            hl->addWidget(openCertBtn);
            auto* tip = new QLabel(QStringLiteral(
                "<span style='color:%1;'>%2<br>%3</span>")
                .arg(Theme::kMuted,
                     QStringLiteral("个人证书（含私钥）：解密别人发给你的 S/MIME 邮件 / 签名你的邮件"),
                     QStringLiteral("联系人公钥：给对方发送加密邮件前需要先导入对方的 .pem/.crt")));
            tip->setWordWrap(true);
            hl->addWidget(tip, 1);
            ef->addRow(QStringLiteral("证书:"), wrap);
        }
        el->addWidget(encGroup);
        el->addStretch();
    }
    tabs->addTab(encPage, QStringLiteral("邮件加密"));

    // ── 按钮 ──
    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btnBox->button(QDialogButtonBox::Ok)->setText(m_mode == "edit" ? "保存" : "添加");
    btnBox->button(QDialogButtonBox::Ok)->setMinimumWidth(80);
    btnBox->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    btnBox->button(QDialogButtonBox::Cancel)->setMinimumWidth(80);
    connect(btnBox, &QDialogButtonBox::accepted, this, &AccountDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, this, &AccountDialog::reject);
    layout->addWidget(btnBox);
    m_btnBox = btnBox;
}

void AccountDialog::onCalDavToggled(bool on) {
    m_calDavPanel->setVisible(on);
    if (on) {
        if (m_calDavUserEdit->text().isEmpty()) {
            m_calDavUserEdit->setText(m_emailEdit->text().trimmed());
        }
    }
}

void AccountDialog::openMailCertManager() {
    // 邮件加密 tab：签发"邮件证书管理"对话框（非模态，可同时打开多个实例）
    auto* dlg = new CertManagerDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    Logger::instance().info("打开邮件证书管理", "settings");
}

void AccountDialog::loadData(const QVariantMap& data) {
    m_nameEdit->setText(data.value("name").toString());
    m_emailEdit->setText(data.value("email").toString());
    m_passEdit->setText(data.value("password").toString());

    // 收件服务器（固定 IMAP）：直接回填收件主机/端口/SSL
    m_imapHostEdit->setText(data.value("imapHost").toString());
    m_imapPortSpin->setValue(data.value("imapPort", 993).toInt());
    m_imapSslCheck->setChecked(data.value("imapSsl", true).toBool());

    // 回填已保存的 POP3 预设，避免编辑账号时丢失
    m_pop3Host = data.value("pop3Host").toString();
    m_pop3Port = data.value("pop3Port", 995).toInt();
    m_pop3Ssl  = data.value("pop3Ssl", true).toBool();

    // 发件
    m_smtpHostEdit->setText(data.value("smtpHost").toString());
    m_smtpPortSpin->setValue(data.value("smtpPort", 465).toInt());
    m_smtpSslCheck->setChecked(data.value("smtpSsl", true).toBool());

    // CalDAV
    bool cal = data.value("calDavEnabled", false).toBool();
    m_calDavEnableCheck->setChecked(cal);
    m_calDavHostEdit->setText(data.value("calDavHost").toString());
    m_calDavPortSpin->setValue(data.value("calDavPort", 443).toInt());
    m_calDavSslCheck->setChecked(data.value("calDavSsl", true).toBool());
    m_calDavUserEdit->setText(data.value("calDavUser").toString());
    m_calDavPanel->setVisible(cal);

    m_defaultCheck->setChecked(data.value("isDefault").toBool());

    // 轮询间隔（全局设置）
    m_pollIntervalSpin->setValue(Settings::instance().get("mail.pollIntervalMin", 5).toInt());
}

void AccountDialog::onEmailChanged(const QString& text) {
    Q_UNUSED(text);
    // 新增模式：输入邮箱即自动根据账号填充收件(IMAP)/发件(SMTP)/CalDAV 配置
    if (m_mode != "new") return;
    applyPresetForEmail();
}

// 根据"发件账号"邮箱自动配置服务器：按域名匹配预设并填充
// 收件(IMAP)、发件(SMTP) 的主机/端口/SSL，有 CalDAV 预设时一并填充。
void AccountDialog::applyPresetForEmail() {
    QString text = m_emailEdit->text().trimmed();
    if (text.isEmpty()) return;
    QVariantMap preset = MailAccountManager::presetFor(text);
    if (preset.isEmpty()) return;

    // 发件 (SMTP)
    m_smtpHostEdit->setText(preset.value("smtpHost").toString());
    m_smtpPortSpin->setValue(preset.value("smtpPort", 465).toInt());
    m_smtpSslCheck->setChecked(preset.value("smtpSsl", true).toBool());

    // 收件（固定 IMAP）
    m_imapHostEdit->setText(preset.value("imapHost").toString());
    m_imapPortSpin->setValue(preset.value("imapPort", 993).toInt());
    m_imapSslCheck->setChecked(preset.value("imapSsl", true).toBool());

    // POP3 预设：独立保存，数据层随时可切换收件协议而不失配置
    m_pop3Host = preset.value("pop3Host").toString();
    m_pop3Port = preset.value("pop3Port", 995).toInt();
    m_pop3Ssl  = preset.value("pop3Ssl", true).toBool();

    // CalDAV（如果有预设）
    QString calHost = preset.value("calDavHost").toString();
    if (!calHost.isEmpty()) {
        m_calDavEnableCheck->setChecked(true);
        m_calDavHostEdit->setText(calHost);
        m_calDavPortSpin->setValue(preset.value("calDavPort", 443).toInt());
        m_calDavSslCheck->setChecked(preset.value("calDavSsl", true).toBool());
        m_calDavUserEdit->setText(text);
        m_calDavPanel->setVisible(true);
    }
}

void AccountDialog::accept() {
    if (m_emailEdit->text().trimmed().isEmpty()) {
        m_emailEdit->setFocus();
        return;
    }
    // 防重复提交：进入提交流程后禁用按钮 + 显示"保存中"，
    // 快速连点确认不会重复执行（重复添加账号 / 重复重启轮询卡死界面）
    if (m_submitting) return;
    m_submitting = true;
    if (m_btnBox) {
        if (auto* okBtn = m_btnBox->button(QDialogButtonBox::Ok)) {
            okBtn->setEnabled(false);
            okBtn->setText("保存中...");
        }
        if (auto* cancelBtn = m_btnBox->button(QDialogButtonBox::Cancel))
            cancelBtn->setEnabled(false);
    }
    setCursor(Qt::WaitCursor);
    QCoreApplication::processEvents();   // 立即重绘按钮/光标，再执行后续保存

    m_result["name"]        = m_nameEdit->text().trimmed();
    m_result["email"]       = m_emailEdit->text().trimmed();
    m_result["password"]    = m_passEdit->text();

    // 收件服务器固定 IMAP：直接保存主机/端口/SSL
    QString recvHost = m_imapHostEdit->text().trimmed();
    int    recvPort  = m_imapPortSpin->value();
    bool   recvSsl   = m_imapSslCheck->isChecked();
    m_result["recvProto"]   = "IMAP";
    m_result["imapHost"]    = recvHost;
    m_result["imapPort"]    = recvPort > 0 ? recvPort : 993;
    m_result["imapSsl"]     = recvSsl;
    // POP3 预设独立保存（自动配置命中则用其值，否则以 IMAP 兜底），便于协议间切换
    m_result["pop3Host"]    = m_pop3Host.isEmpty() ? recvHost : m_pop3Host;
    m_result["pop3Port"]    = m_pop3Port > 0 ? m_pop3Port : recvPort;
    m_result["pop3Ssl"]     = m_pop3Ssl;

    m_result["smtpHost"]    = m_smtpHostEdit->text().trimmed();
    m_result["smtpPort"]    = m_smtpPortSpin->value();
    m_result["smtpSsl"]     = m_smtpSslCheck->isChecked();

    m_result["calDavEnabled"] = m_calDavEnableCheck->isChecked();
    m_result["calDavHost"]    = m_calDavHostEdit->text().trimmed();
    m_result["calDavPort"]    = m_calDavPortSpin->value();
    m_result["calDavSsl"]     = m_calDavSslCheck->isChecked();
    m_result["calDavUser"]    = m_calDavUserEdit->text().trimmed();

    m_result["isDefault"]   = m_defaultCheck->isChecked();

    Logger::instance().info(
        QString("AccountDialog.accept(): mode=%1 email=%2 recv=%3 imapHost='%4' imapPort=%5 smtpHost='%6' smtpPort=%7 passLen=%8 default=%9")
            .arg(m_mode)
            .arg(m_result["email"].toString())
            .arg(m_result["recvProto"].toString())
            .arg(m_result["imapHost"].toString())
            .arg(m_result["imapPort"].toInt())
            .arg(m_result["smtpHost"].toString())
            .arg(m_result["smtpPort"].toInt())
            .arg(m_result["password"].toString().length())
            .arg(m_result["isDefault"].toBool() ? QStringLiteral("Y") : QStringLiteral("N")),
        "mail");

    // 轮询间隔：全局设置，变化时保存并重启轮询
    int pollMin = m_pollIntervalSpin->value();
    if (Settings::instance().get("mail.pollIntervalMin", 5).toInt() != pollMin) {
        Settings::instance().update({{"mail.pollIntervalMin", pollMin}});
        MailPoller::instance().stop();
        MailPoller::instance().start();
    }
    QDialog::accept();
}
