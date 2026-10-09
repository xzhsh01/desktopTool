// AttachDetailDialog.cpp — 视频/文件附件详情对话框实现
#include "AttachDetailDialog.h"
#include "app/Theme.h"

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QClipboard>
#include <QGuiApplication>
#include <QPushButton>

AttachDetailDialog::AttachDetailDialog(const QVariantMap& msg, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("附件详情"));
    setMinimumWidth(420);
    setStyleSheet(QStringLiteral("background:%1; color:%2;")
                  .arg(Theme::kBg, Theme::kText));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    // 标题
    auto* title = new QLabel(QStringLiteral("附件元数据"));
    title->setStyleSheet(QStringLiteral("color:%1; font-size:15px; font-weight:600;")
                         .arg(Theme::kTextBright));
    root->addWidget(title);

    // 表单：逐字段展示 msg 中有值的 key
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setSpacing(6);

    // 展示顺序：先已知关键字段，再其余字段
    static const QStringList orderedKeys = {
        "attachMd5", "attachUrl", "attachAesKey", "attachSize",
        "attachExt", "attachFileName", "attachType", "attachSubType",
        "talker", "createTime", "msgId"
    };

    auto addRow = [&](const QString& key, const QString& val) {
        if (val.isEmpty()) return;
        auto* label = new QLabel(key);
        label->setStyleSheet(QStringLiteral("color:%1; font-size:12px;")
                             .arg(Theme::kMuted));
        auto* edit = new QLineEdit(val);
        edit->setReadOnly(true);
        edit->setStyleSheet(QStringLiteral(
            "QLineEdit{ background:%1; color:%2; border:1px solid %3;"
            " border-radius:4px; padding:4px 8px; font-size:12px; }")
            .arg(Theme::kSurface, Theme::kText, Theme::kBorder));
        form->addRow(label, edit);
    };

    // 先按有序列表展示
    QSet<QString> shown;
    for (const auto& key : orderedKeys) {
        if (msg.contains(key)) {
            addRow(key, msg.value(key).toString());
            shown.insert(key);
        }
    }
    // 再展示其余字段
    for (auto it = msg.begin(); it != msg.end(); ++it) {
        if (shown.contains(it.key())) continue;
        addRow(it.key(), it.value().toString());
    }

    root->addLayout(form);

    // 按钮：复制全部 + 关闭
    auto* btnBox = new QHBoxLayout;
    btnBox->addStretch();

    auto* copyBtn = new QPushButton(QStringLiteral("复制全部"));
    copyBtn->setStyleSheet(QStringLiteral(
        "QPushButton{ background:%1; color:%2; border:none; border-radius:4px;"
        " padding:6px 16px; font-size:12px; }"
        "QPushButton:hover{ background:%3; }")
        .arg(Theme::kSurface, Theme::kText, Theme::kBorderLight));
    connect(copyBtn, &QPushButton::clicked, this, [msg]() {
        QString text;
        for (auto it = msg.begin(); it != msg.end(); ++it) {
            text += QStringLiteral("%1: %2\n").arg(it.key(), it.value().toString());
        }
        QGuiApplication::clipboard()->setText(text.trimmed());
    });
    btnBox->addWidget(copyBtn);

    auto* closeBtn = new QPushButton(QStringLiteral("关闭"));
    closeBtn->setStyleSheet(QStringLiteral(
        "QPushButton{ background:%1; color:%2; border:none; border-radius:4px;"
        " padding:6px 16px; font-size:12px; }"
        "QPushButton:hover{ background:%3; }")
        .arg(Theme::kAccent, QStringLiteral("#000000"), QStringLiteral("#39b0e6")));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnBox->addWidget(closeBtn);

    root->addLayout(btnBox);
}
