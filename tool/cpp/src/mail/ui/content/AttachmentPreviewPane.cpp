#include "mail/ui/content/AttachmentPreviewPane.h"

#include "app/Theme.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

AttachmentPreviewPane::AttachmentPreviewPane(QWidget* parent) : QWidget(parent) {
    buildUi();
}

void AttachmentPreviewPane::buildUi() {
    setStyleSheet(QStringLiteral("background: %1;").arg(Theme::kBg));
    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(12, 8, 12, 12);
    m_root->setSpacing(8);

    // 顶部工具栏：[← 返回正文]   文件名 …    [打开] [定位]
    auto* topBar = new QHBoxLayout;
    topBar->setSpacing(6);

    m_backBtn = new QPushButton(QStringLiteral("← 返回正文"));
    m_backBtn->setCursor(Qt::PointingHandCursor);
    m_backBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: none; padding: 4px 12px; border-radius: 3px; }"
        "QPushButton:hover { background: %3; }").arg(Theme::kTitleBar, Theme::kText, Theme::kBorderLight));
    connect(m_backBtn, &QPushButton::clicked, this, &AttachmentPreviewPane::onBack);
    topBar->addWidget(m_backBtn);

    topBar->addStretch();

    m_openBtn = new QPushButton(QStringLiteral("打开"));
    m_openBtn->setCursor(Qt::PointingHandCursor);
    m_openBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3; padding: 4px 12px; border-radius: 3px; }"
        "QPushButton:hover { background: %4; }").arg(Theme::kTitleBar, Theme::kText, Theme::kBorder, Theme::kBorderLight));
    connect(m_openBtn, &QPushButton::clicked, this, &AttachmentPreviewPane::onOpen);
    topBar->addWidget(m_openBtn);

    m_revealBtn = new QPushButton(QStringLiteral("在文件夹显示"));
    m_revealBtn->setCursor(Qt::PointingHandCursor);
    m_revealBtn->setStyleSheet(m_openBtn->styleSheet());
    connect(m_revealBtn, &QPushButton::clicked, this, &AttachmentPreviewPane::onReveal);
    topBar->addWidget(m_revealBtn);

    m_root->addLayout(topBar);

    // 图片预览：QScrollArea 包 QLabel（缩放大图）
    m_imageLabel = new QLabel;
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::kSurfaceAlt));
    m_imageLabel->setMinimumHeight(200);
    m_imageLabel->setText(QStringLiteral("(无预览)"));
    m_imageLabel->hide();

    auto* scrollImg = new QScrollArea;
    scrollImg->setWidgetResizable(true);
    scrollImg->setWidget(m_imageLabel);
    scrollImg->setStyleSheet(QStringLiteral(
        "QScrollArea { background: %1; border: 1px solid %2; }").arg(Theme::kSurfaceAlt, Theme::kBorder));
    scrollImg->hide();
    m_root->addWidget(scrollImg, 1);

    // 文本预览：QPlainTextEdit
    m_textEdit = new QPlainTextEdit;
    m_textEdit->setReadOnly(true);
    m_textEdit->setFont(QFont(QStringLiteral("Consolas"), 9));
    m_textEdit->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: %1; color: %2; border: 1px solid %3; }")
        .arg(Theme::kSurfaceAlt, Theme::kText, Theme::kBorder));
    m_textEdit->hide();
    m_root->addWidget(m_textEdit, 1);

    // 不支持类型占位
    m_unsupportedLabel = new QLabel;
    m_unsupportedLabel->setAlignment(Qt::AlignCenter);
    m_unsupportedLabel->setWordWrap(true);
    m_unsupportedLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: %1; color: %2; border: 1px solid %3; "
        "padding: 40px; border-radius: 6px; font-size: 13px; }")
        .arg(Theme::kSurfaceAlt, Theme::kText, Theme::kBorder));
    m_unsupportedLabel->hide();
    m_root->addWidget(m_unsupportedLabel, 1);

    clear();
}

void AttachmentPreviewPane::clear() {
    m_path.clear();
    m_mimeType.clear();
    m_imageLabel->hide();
    m_textEdit->hide();
    m_unsupportedLabel->hide();
    m_openBtn->setEnabled(false);
    m_revealBtn->setEnabled(false);
}

void AttachmentPreviewPane::show(const QString& filePath, const QString& mimeType) {
    m_path = filePath;
    m_mimeType = mimeType;
    m_openBtn->setEnabled(true);
    m_revealBtn->setEnabled(true);

    QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isReadable()) {
        m_imageLabel->hide();
        m_textEdit->hide();
        m_unsupportedLabel->setText(QStringLiteral(
            "❌ 文件不存在或不可读：\n%1").arg(filePath));
        m_unsupportedLabel->show();
        return;
    }

    const QString mt = mimeType.toLower();

    // 图片
    if (mt.startsWith(QStringLiteral("image/"))) {
        QImageReader reader(filePath);
        reader.setAutoTransform(true);
        QImage img = reader.read();
        if (img.isNull()) {
            renderUnsupported(filePath, mimeType);
            return;
        }
        QPixmap pix = QPixmap::fromImage(img);
        // 上限缩放：宽度 1200，高度 1800（避免超大图卡死 UI）
        const QSize maxSize(1200, 1800);
        if (pix.width() > maxSize.width() || pix.height() > maxSize.height())
            pix = pix.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_imageLabel->setPixmap(pix);
        m_imageLabel->adjustSize();
        m_imageLabel->show();
        m_textEdit->hide();
        m_unsupportedLabel->hide();
        return;
    }

    // 文本类
    if (mt.startsWith(QStringLiteral("text/"))
        || mt == QStringLiteral("application/json")
        || mt == QStringLiteral("application/xml")
        || mt == QStringLiteral("application/javascript")
        || mt.contains(QStringLiteral("xml"))
        || mt.endsWith(QStringLiteral("csv"))
        || mt == QStringLiteral("application/csv")) {
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            renderUnsupported(filePath, mimeType);
            return;
        }
        QByteArray bytes = f.readAll();
        // 自动检测编码（先 UTF-8 BOM/有效，失败回退 Latin1）
        QString text;
        if (bytes.startsWith("\xEF\xBB\xBF")) {
            text = QString::fromUtf8(bytes.mid(3));
        } else {
            text = QString::fromUtf8(bytes);
            if (text.contains(QChar(0xFFFD))) text = QString::fromLatin1(bytes);
        }
        // 大文件截断
        if (text.size() > 1024 * 1024) {
            text = text.left(1024 * 1024);
            text += QStringLiteral("\n\n... (剩余内容已截断)");
        }
        m_textEdit->setPlainText(text);
        m_textEdit->show();
        m_imageLabel->hide();
        m_unsupportedLabel->hide();
        return;
    }

    // 不支持的类型
    renderUnsupported(filePath, mimeType);
}

void AttachmentPreviewPane::renderUnsupported(const QString& filePath, const QString& mimeType) {
    QFileInfo fi(filePath);
    qint64 size = fi.size();
    QString sizeStr;
    if (size < 1024) sizeStr = QStringLiteral("%1 B").arg(size);
    else if (size < 1024*1024) sizeStr = QStringLiteral("%1 KB").arg(QString::number(size/1024.0, 'f', 1));
    else sizeStr = QStringLiteral("%1 MB").arg(QString::number(size/1024.0/1024.0, 'f', 2));

    QString name = fi.fileName();
    QString mt = mimeType.isEmpty() ? QStringLiteral("未知") : mimeType;
    m_unsupportedLabel->setText(QStringLiteral(
        "[附件] %1\n\n"
        "类型：%2\n"
        "大小：%3\n\n"
        "该类型暂不支持内联预览。\n"
        "请使用上方 [打开] / [在文件夹显示] 按钮查看。")
        .arg(name, mt, sizeStr));
    m_unsupportedLabel->show();
    m_imageLabel->hide();
    m_textEdit->hide();
}

void AttachmentPreviewPane::onBack() {
    emit backRequested();
}

void AttachmentPreviewPane::onOpen() {
    if (m_path.isEmpty()) return;
    emit openExternallyRequested(m_path);
}

void AttachmentPreviewPane::onReveal() {
    if (m_path.isEmpty()) return;
    emit revealRequested(m_path);
}