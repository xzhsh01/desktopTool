#pragma once

#include <QDialog>
#include <QVariantMap>

class QLineEdit;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QFormLayout;
class QComboBox;
class QStackedWidget;
class QLabel;
class QGroupBox;
class QDialogButtonBox;

/**
 * AccountDialog: 邮箱账号新增/编辑对话框
 *
 * - 邮箱变化时自动按域名（QQ/163/Gmail/Outlook 等）填充服务器预设
 * - 返回 QVariantMap（字段同 MailAccountManager::Account）
 */
class AccountDialog : public QDialog {
    Q_OBJECT

public:
    // mode: "new" 或 "edit"
    explicit AccountDialog(const QString& mode, const QVariantMap& data = {}, QWidget* parent = nullptr);
    QVariantMap result() const { return m_result; }

private slots:
    void onEmailChanged(const QString& text);
    void onCalDavToggled(bool on);
    void accept() override;

private:
    void setupUI();
    void loadData(const QVariantMap& data);
    void applyPresetForEmail();   // 根据当前邮箱自动配置收件(IMAP)/发件(SMTP)/CalDAV
    void openMailCertManager();   // 邮件加密 tab：打开 S/MIME 证书管理对话框

    QString  m_mode;
    QVariantMap m_result;

    QLineEdit* m_nameEdit       = nullptr;
    QLineEdit* m_emailEdit      = nullptr;
    QLineEdit* m_passEdit       = nullptr;
    QCheckBox* m_defaultCheck   = nullptr;
    QSpinBox*  m_pollIntervalSpin = nullptr;  // 轮询间隔（全局设置）

    // 收件服务器（界面仍固定 IMAP 直接配置；自动配置时同时填充并保存 POP3 预设，数据层支持切换）
    QLineEdit* m_imapHostEdit   = nullptr;
    QSpinBox*  m_imapPortSpin   = nullptr;
    QCheckBox* m_imapSslCheck   = nullptr;

    // 自动配置得到的 POP3 预设（独立于 IMAP 持久化，供 accept 写入）
    QString m_pop3Host;
    int     m_pop3Port = 995;
    bool    m_pop3Ssl  = true;

    // 发件服务器 (SMTP)
    QLineEdit* m_smtpHostEdit   = nullptr;
    QSpinBox*  m_smtpPortSpin   = nullptr;
    QCheckBox* m_smtpSslCheck   = nullptr;

    // CalDAV（高级 / 可选）
    QCheckBox*  m_calDavEnableCheck = nullptr;
    QPushButton* m_calDavToggleBtn   = nullptr;
    QWidget*    m_calDavPanel       = nullptr;
    QLineEdit*  m_calDavHostEdit    = nullptr;
    QSpinBox*   m_calDavPortSpin    = nullptr;
    QCheckBox*  m_calDavSslCheck    = nullptr;
    QLineEdit*  m_calDavUserEdit    = nullptr;

    QDialogButtonBox* m_btnBox      = nullptr;   // 底部按钮条（提交时禁用防重复点击）
    bool              m_submitting  = false;     // 已进入提交流程（accept 防重入）
};
