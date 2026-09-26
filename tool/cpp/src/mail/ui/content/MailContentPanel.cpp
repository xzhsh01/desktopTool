#include "mail/ui/content/MailContentPanel.h"

#include "app/Theme.h"
#include "core/Logger.h"
#include "core/Settings.h"
#include "mail/ImapClient.h"
#include "mail/MailAccountManager.h"
#include "mail/crypto/MailEncryptor.h"
#include "mail/crypto/SmimeCrypto.h"
#include "mail/ui/content/FlowLayout.h"
#include "mail/ui/content/MailPreviewBrowser.h"
#include "mail/ui/content/AttachmentPreviewPane.h"

#include <QDateTime>
#include <QObject>
#include <QDesktopServices>
#include <QDir>
#include <QProcess>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QThread>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QLineEdit>
#include <QMimeDatabase>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QTimer>

namespace {

QHash<QString, QByteArray> parseCidImages(const QByteArray& rawBytes) {
    QHash<QString, QByteArray> out;
    if (rawBytes.isEmpty()) return out;
    const QString raw = QString::fromLatin1(rawBytes);
    static const QRegularExpression ctypeRe(
        "^Content-Type:[ \t]*([^\r\n]+)",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    static const QRegularExpression cteRe(
        "^Content-Transfer-Encoding:[ \t]*([^\r\n]+)",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    static const QRegularExpression cidRe(
        "^Content-I[Dd]:[ \\t]*<?([^>\\r\\n\\s]+)>?",
        QRegularExpression::MultilineOption);
    static const QRegularExpression bndRe(
        "boundary=\"?([^\"\\s;]+)\"?",
        QRegularExpression::CaseInsensitiveOption);
    int imgParts = 0;
    QHash<QString, QByteArray> namedImages;
    QList<QByteArray> anonImages;
    const std::function<void(const QString&, const QString&)> walk =
        [&](const QString& headers, const QString& body) {
            const auto cm = ctypeRe.match(headers);
            const QString ctype = cm.hasMatch()
                ? cm.captured(1).trimmed().toLower()
                : QStringLiteral("text/plain");
            if (ctype.contains("multipart/")) {
                const auto bm = bndRe.match(headers);
                if (!bm.hasMatch()) return;
                const QString bnd = bm.captured(1);
                QString b2 = body;
                if (b2.startsWith("--" + bnd)) b2.prepend("\r\n");
                const QStringList parts = b2.split("\r\n--" + bnd, Qt::SkipEmptyParts);
                for (const QString& p : parts) {
                    if (p.startsWith("--")) continue;
                    int ps = p.indexOf("\r\n\r\n");
                    if (ps < 0) ps = p.indexOf("\n\n");
                    if (ps < 0) continue;
                    walk(p.left(ps), p.mid(ps + (p.at(ps) == '\r' ? 4 : 2)));
                }
                return;
            }
            if (!ctype.startsWith("image/")) return;
            ++imgParts;
            const auto im = cidRe.match(headers);
            const auto em = cteRe.match(headers);
            const QString cte = em.hasMatch()
                ? em.captured(1).trimmed().toLower()
                : QStringLiteral("7bit");
            QByteArray data = body.toLatin1();
            while (!data.isEmpty() && (data.endsWith(' ') || data.endsWith('\r')
                                       || data.endsWith('\n') || data.endsWith('\t')))
                data.chop(1);
            if (cte == "base64") data = QByteArray::fromBase64(data);
            if (data.isEmpty()) return;
            if (im.hasMatch()) {
                QString cid = im.captured(1).toLower();
                if (cid.startsWith('<') && cid.endsWith('>'))
                    cid = cid.mid(1, cid.size() - 2);
                namedImages.insert(cid, data);
            } else {
                anonImages.append(data);
            }
        };
    int sep = raw.indexOf("\r\n\r\n");
    if (sep < 0) sep = raw.indexOf("\n\n");
    if (sep < 0) return out;
    walk(raw.left(sep), raw.mid(sep + (raw.at(sep) == '\r' ? 4 : 2)));
    out = namedImages;
    for (int i = 0; i < anonImages.size(); ++i)
        out.insert(QString("__anon_%1__").arg(i), anonImages[i]);
    Logger::instance().info(
        QString("parseCidImages: rawSize=%1 imgParts=%2 namedCid=%3 anonFallback=%4 totalKeys=%5")
            .arg(rawBytes.size())
            .arg(imgParts)
            .arg(namedImages.size())
            .arg(anonImages.size())
            .arg(out.size()),
        "mail");
    return out;
}

QString formatSizeShort(qint64 bytes) {
    if (bytes <= 0) return QString();
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 2);
}

// 简化版 MIME 解析：从已解密的 RFC822 字节中抽取 text/plain 和 text/html
// 不处理 multipart/related（cid 内联图片）；用于"解密后的明文"预览，
// 因为加密后通常只保留纯文本/HTML，不嵌套复杂附件。
static void extractPlainHtmlFromMime(const QByteArray& mimeBytes,
                                     QString* plainOut, QString* htmlOut) {
    if (plainOut) plainOut->clear();
    if (htmlOut) htmlOut->clear();
    QByteArray latin = mimeBytes; // header/transfer-encoding 字节均 ASCII
    QString raw = QString::fromLatin1(latin);
    int split = raw.indexOf(QStringLiteral("\r\n\r\n"));
    int delimLen = 4;
    if (split < 0) { split = raw.indexOf(QStringLiteral("\n\n")); delimLen = 2; }
    if (split < 0) { if (plainOut) *plainOut = raw; return; }
    QString headers = raw.left(split);
    QString body    = raw.mid(split + delimLen);

    // 顶层 Content-Type
    QRegularExpression ctypeRe(QStringLiteral("^Content-Type:\\s*([^;\\r\\n]+)"),
                               QRegularExpression::CaseInsensitiveOption |
                               QRegularExpression::MultilineOption);
    auto cm = ctypeRe.match(headers);
    QString topCtype = cm.hasMatch() ? cm.captured(1).trimmed().toLower() : QString();

    // text/plain 直接
    if (topCtype.startsWith(QStringLiteral("text/plain"))) {
        if (plainOut) *plainOut = body; return;
    }
    if (topCtype.startsWith(QStringLiteral("text/html"))) {
        if (htmlOut) *htmlOut = body; return;
    }

    // multipart/alternative → 逐 part 找 text/plain + text/html
    if (topCtype.startsWith(QStringLiteral("multipart/"))) {
        QRegularExpression bndRe(QStringLiteral("boundary=\"?([^\\s;\\\"]+)\"?"),
                                 QRegularExpression::CaseInsensitiveOption);
        auto bm = bndRe.match(headers);
        if (!bm.hasMatch()) return;
        QString bnd = bm.captured(1);
        QStringList parts = body.split(QRegularExpression(
            QStringLiteral("\\r?\\n--%1(?:\\r?\\n|--)").arg(QRegularExpression::escape(bnd))),
            Qt::KeepEmptyParts);
        for (const QString& p : parts) {
            int pSplit = p.indexOf(QStringLiteral("\r\n\r\n"));
            int pDelim = 4;
            if (pSplit < 0) { pSplit = p.indexOf(QStringLiteral("\n\n")); pDelim = 2; }
            if (pSplit < 0) continue;
            QString ph = p.left(pSplit);
            QString pb = p.mid(pSplit + pDelim);
            auto pm = ctypeRe.match(ph);
            QString pctype = pm.hasMatch() ? pm.captured(1).trimmed().toLower() : QString();
            if (plainOut && plainOut->isEmpty() && pctype.startsWith(QStringLiteral("text/plain")))
                *plainOut = pb;
            else if (htmlOut && htmlOut->isEmpty() && pctype.startsWith(QStringLiteral("text/html")))
                *htmlOut = pb;
            if (!plainOut->isEmpty() && !htmlOut->isEmpty()) break;
        }
    }
}

constexpr int kAddrMaxShow = 3;
constexpr int kAddrMaxLen  = 80;

QFrame* makeSeparator() {
    auto* f = new QFrame;
    f->setFrameShape(QFrame::HLine);
    f->setFrameShadow(QFrame::Plain);
    f->setStyleSheet(QString("color: %1; background: %1;").arg(Theme::kBorder));
    f->setFixedHeight(1);
    return f;
}

QLabel* makeValueLabel(QWidget* parent) {
    auto* l = new QLabel(parent);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    l->setWordWrap(true);
    l->setStyleSheet(QString("color: %1; font-size: 13px;").arg(Theme::kText));
    return l;
}

QString attIcon(const QString& mime) {
    if (mime.startsWith("image/")) return QString::fromUtf8("\xF0\x9F\x93\xB7");
    if (mime.startsWith("video/")) return QString::fromUtf8("\xF0\x9F\x8E\xA5");
    if (mime.startsWith("audio/")) return QString::fromUtf8("\xF0\x9F\x8E\xB5");
    if (mime.contains("pdf"))     return QString::fromUtf8("\xF0\x9F\x93\x84");
    if (mime.contains("zip") || mime.contains("rar") || mime.contains("7z") || mime.contains("tar"))
        return QString::fromUtf8("\xF0\x9F\x97\x84");
    if (mime.contains("text"))    return QString::fromUtf8("\xF0\x9F\x93\x9D");
    return QString::fromUtf8("\xF0\x9F\x93\x8E");
}

QString defaultExtension(const QString& mime, const QString& fname) {
    static const QHash<QString, QString> map = {
        {"application/pdf", ".pdf"},
        {"application/zip", ".zip"},
        {"application/json", ".json"},
        {"application/xml", ".xml"},
        {"text/plain", ".txt"},
    };
    if (fname.contains('.')) return QString();
    return map.value(mime.split(';').first().trimmed().toLower(), QString());
}

} // namespace
MailContentPanel::MailContentPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // "回复"按钮（默认：仅回给发件人）
    m_replyBtn = new QPushButton(QStringLiteral("↩回复"));
    // 按钮样式：背景跟原顶部 toolbar 一致（Theme::kTitleBar）—— 比头部背景（kBg）深一档，
    // 形成"暗色凹陷"层次感（视觉上和原来 toolbar 浮在头部上效果一致）
    const QString headerBtnStyle = QStringLiteral(
        "QPushButton, QToolButton { background: %1; color: %2; border: none;"
        " padding: 6px 16px; font-size: 13px; border-radius: 4px; }"
        "QPushButton:hover, QToolButton:hover    { background: %3; }"
        "QPushButton:pressed, QToolButton:pressed { background: %4; }"
        "QPushButton:disabled, QToolButton:disabled { background: %1; color: %5; }")
        .arg(Theme::kTitleBar, Theme::kText,
             Theme::kBorderLight, Theme::kBorder, Theme::kDisabled);

    m_replyBtn->setCursor(Qt::PointingHandCursor);
    m_replyBtn->setStyleSheet(headerBtnStyle);
    m_replyBtn->setEnabled(false);
    m_replyBtn->hide();   // 默认隐藏：仅收件箱(INBOX)显示
    connect(m_replyBtn, &QPushButton::clicked, this, &MailContentPanel::replyRequested);

    // "回复全部"按钮（回给发件人 + 抄送）
    m_replyAllBtn = new QPushButton(QStringLiteral("↩回复全部"));
    m_replyAllBtn->setCursor(Qt::PointingHandCursor);
    m_replyAllBtn->setStyleSheet(headerBtnStyle);
    m_replyAllBtn->setEnabled(false);
    m_replyAllBtn->hide();
    connect(m_replyAllBtn, &QPushButton::clicked, this, &MailContentPanel::replyAllRequested);

    // "→ 转发 ▼" 下拉按钮：3 个 action
    //   1) 直接转发邮件（原邮件内容作为新邮件正文，无附件）— 即原"转发"行为
    //   2) 作为附件转发（原邮件 .eml 作为附件 + 正文直接转发）
    //   3) 原件转发（原邮件 .eml 作为附件 + 正文简短标识）
    m_forwardMenuBtn = new QToolButton;
    m_forwardMenuBtn->setText(QStringLiteral("→转发"));
    m_forwardMenuBtn->setCursor(Qt::PointingHandCursor);
    m_forwardMenuBtn->setStyleSheet(headerBtnStyle);
    m_forwardMenuBtn->setEnabled(false);
    m_forwardMenuBtn->hide();
    m_forwardMenuBtn->setPopupMode(QToolButton::InstantPopup);   // 点击直接弹菜单

    m_forwardMenu = new QMenu(m_forwardMenuBtn);
    QAction* actInline  = m_forwardMenu->addAction(QStringLiteral("直接转发邮件"));
    QAction* actAttach  = m_forwardMenu->addAction(QStringLiteral("作为附件转发"));
    QAction* actOriginal= m_forwardMenu->addAction(QStringLiteral("原件转发"));
    m_forwardMenuBtn->setMenu(m_forwardMenu);
    connect(actInline,   &QAction::triggered, this, &MailContentPanel::forwardRequested);
    connect(actAttach,   &QAction::triggered, this, &MailContentPanel::forwardAsAttachmentRequested);
    connect(actOriginal, &QAction::triggered, this, &MailContentPanel::forwardOriginalRequested);

    // "重新编辑" 按钮：载入写邮件窗口（预填收件人/主题/正文），可修改后重新发送
    m_reeditBtn = new QPushButton(QStringLiteral("✎ 重新编辑"));
    m_reeditBtn->setCursor(Qt::PointingHandCursor);
    m_reeditBtn->setStyleSheet(headerBtnStyle);
    m_reeditBtn->setEnabled(false);
    m_reeditBtn->hide();   // 默认隐藏：仅收件箱外的文件夹显示
    connect(m_reeditBtn, &QPushButton::clicked, this, &MailContentPanel::reeditRequested);

    // "再次发送" 按钮：不打开编辑器，按原收件人/主题/正文原样直接重发
    m_resendBtn = new QPushButton(QStringLiteral("↻ 再次发送"));
    m_resendBtn->setCursor(Qt::PointingHandCursor);
    m_resendBtn->setStyleSheet(headerBtnStyle);
    m_resendBtn->setEnabled(false);
    m_resendBtn->hide();
    connect(m_resendBtn, &QPushButton::clicked, this, &MailContentPanel::resendRequested);

    // "撤销" 按钮：仅已发送(Sent)文件夹显示，尝试撤回已发邮件
    m_revokeBtn = new QPushButton(QStringLiteral("⟲ 撤销"));
    m_revokeBtn->setCursor(Qt::PointingHandCursor);
    m_revokeBtn->setStyleSheet(headerBtnStyle);
    m_revokeBtn->setEnabled(false);
    m_revokeBtn->hide();
    connect(m_revokeBtn, &QPushButton::clicked, this, &MailContentPanel::revokeRequested);

    // "删除" 按钮：非收件箱文件夹/未读视图中显示，直接删除当前选中邮件/草稿
    m_deleteBtn = new QPushButton(QStringLiteral("🗑 删除"));
    m_deleteBtn->setCursor(Qt::PointingHandCursor);
    m_deleteBtn->setStyleSheet(headerBtnStyle);
    m_deleteBtn->setEnabled(false);
    m_deleteBtn->hide();
    connect(m_deleteBtn, &QPushButton::clicked, this, &MailContentPanel::deleteRequested);

    m_headerWidget = new QWidget;
    m_headerWidget->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::kBg));
    m_headerLayout = new QFormLayout(m_headerWidget);
    m_headerLayout->setContentsMargins(12, 10, 12, 10);
    m_headerLayout->setHorizontalSpacing(8);
    m_headerLayout->setVerticalSpacing(4);
    m_headerLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    m_headerLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto makeFieldLabel = [](const QString& text) {
        auto* l = new QLabel(text + QStringLiteral(":"));
        l->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 600;").arg(Theme::kMuted));
        l->setMinimumWidth(56);
        return l;
    };

    m_subjectLabel = new QLabel;
    m_subjectLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_subjectLabel->setWordWrap(true);
    m_subjectLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 16px; font-weight: 600;").arg(Theme::kTextBright));
    auto* subjectCol = new QVBoxLayout;
    subjectCol->setContentsMargins(0, 0, 0, 0);
    subjectCol->setSpacing(6);
    // 顶部一行：靠右放回复/转发
    auto* btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(6);
    btnRow->addWidget(m_replyBtn);
    btnRow->addWidget(m_replyAllBtn);
    btnRow->addWidget(m_forwardMenuBtn);
    btnRow->addWidget(m_reeditBtn);
    btnRow->addWidget(m_resendBtn);
    btnRow->addWidget(m_revokeBtn);
    btnRow->addWidget(m_deleteBtn);
    btnRow->addStretch();   // 按钮靠左，后面的 stretch 把剩余空间推到右侧
    subjectCol->addLayout(btnRow);
    // 下面一行：主题文字
    auto* subjTextRow = new QHBoxLayout;
    subjTextRow->setContentsMargins(0, 0, 0, 0);
    subjTextRow->setSpacing(6);
    auto* subjectKey = new QLabel(QStringLiteral(""));//主题
    subjectKey->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 600;").arg(Theme::kMuted));
    subjTextRow->addWidget(subjectKey, 0, Qt::AlignTop);
    subjTextRow->addWidget(m_subjectLabel, 1);
    subjectCol->addLayout(subjTextRow);
    auto* subjectWrap = new QWidget;
    subjectWrap->setLayout(subjectCol);
    m_headerLayout->addRow(subjectWrap);

    m_fromValueLabel = makeValueLabel(this);
    m_headerLayout->addRow(makeFieldLabel(QStringLiteral("发件人")), m_fromValueLabel);
    m_toValueLabel = makeValueLabel(this);
    m_toExpandBtn = new QToolButton;
    m_toExpandBtn->setCursor(Qt::PointingHandCursor);
    m_toExpandBtn->setStyleSheet(QStringLiteral("color: %1; border: none; background: transparent; padding: 0 4px; font-size: 12px;").arg(Theme::kAccent));
    m_toExpandBtn->hide();
    connect(m_toExpandBtn, &QToolButton::clicked, this, &MailContentPanel::onToExpandToggled);
    auto* toRow = new QHBoxLayout;
    toRow->setContentsMargins(0, 0, 0, 0);
    toRow->setSpacing(0);
    toRow->addWidget(m_toValueLabel, 1);
    toRow->addWidget(m_toExpandBtn, 0, Qt::AlignTop);
    auto* toWrap = new QWidget;
    toWrap->setLayout(toRow);
    m_headerLayout->addRow(makeFieldLabel(QStringLiteral("收件人")), toWrap);

    m_ccFieldLabel = makeFieldLabel(QStringLiteral("抄送"));
    m_ccValueLabel = makeValueLabel(this);
    m_ccExpandBtn = new QToolButton;
    m_ccExpandBtn->setCursor(Qt::PointingHandCursor);
    m_ccExpandBtn->setStyleSheet(m_toExpandBtn->styleSheet());
    m_ccExpandBtn->hide();
    connect(m_ccExpandBtn, &QToolButton::clicked, this, &MailContentPanel::onCcExpandToggled);
    auto* ccRow = new QHBoxLayout;
    ccRow->setContentsMargins(0, 0, 0, 0);
    ccRow->setSpacing(0);
    ccRow->addWidget(m_ccValueLabel, 1);
    ccRow->addWidget(m_ccExpandBtn, 0, Qt::AlignTop);
    m_ccRowWrap = new QWidget;
    m_ccRowWrap->setLayout(ccRow);
    m_headerLayout->addRow(m_ccFieldLabel, m_ccRowWrap);
    m_ccRowWrap->hide();

    m_dateLabel = makeValueLabel(this);
    m_headerLayout->addRow(makeFieldLabel(QStringLiteral("日期")), m_dateLabel);

    // 默认隐藏头部信息（subject/from/to/cc/date）— 未选邮件时只显示欢迎卡片，
    // 选中邮件后由 renderHeader() 调 show() 展开
    m_headerWidget->hide();
    root->addWidget(m_headerWidget);
    root->addWidget(makeSeparator());

    m_attachWidget = new QWidget;
    m_attachWidget->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::kBg));
    auto* attachLay = new QVBoxLayout(m_attachWidget);
    attachLay->setContentsMargins(12, 8, 12, 8);
    attachLay->setSpacing(6);
    m_attachTitleLabel = new QLabel;
    m_attachTitleLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 600;").arg(Theme::kTextBright));
    attachLay->addWidget(m_attachTitleLabel);
    m_attachRowsHost = new QWidget;
    m_attachRowsLayout = new FlowLayout(m_attachRowsHost);
    m_attachRowsLayout->setContentsMargins(0, 0, 0, 0);
    m_attachRowsLayout->setSpacing(4);
    attachLay->addWidget(m_attachRowsHost);
    m_attachWidget->hide();
    root->addWidget(m_attachWidget);
    root->addWidget(makeSeparator());
    m_bodyStack = new QStackedWidget;
    m_bodyStack->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::kBg));

    m_previewView = new MailPreviewBrowser;
    m_previewView->setReadOnly(true);
    m_previewView->setOpenLinks(false);
    m_previewView->setPlaceholderText(QStringLiteral("选择左侧邮件查看内容"));
    m_previewView->setStyleSheet(QStringLiteral("QTextBrowser { background: #ffffff; color: #1e1e1e; border: none; padding: 12px; }"));
    // 正文阅读区采用"白纸"样式：底层背景白色、正文深色文字，便于阅读
    //（QTextDocument 默认以 viewport 调色板 Base 色为底层，透明/无背景 body 露出它）。
    m_previewView->document()->setDefaultStyleSheet(
        QStringLiteral("body { background-color: #ffffff; color: #1e1e1e; }"));
    QPalette previewPal = m_previewView->palette();
    previewPal.setColor(QPalette::Base, QColor("#ffffff"));
    previewPal.setColor(QPalette::Text, QColor("#1e1e1e"));
    m_previewView->setPalette(previewPal);
    connect(m_previewView, &QTextBrowser::anchorClicked, this, &MailContentPanel::onAnchorClicked);
    m_bodyStack->addWidget(m_previewView);
    // 首次构造后立即渲染默认占位卡片（参照截图样式：图标 + 标题 + 副标题），
    // 覆盖 Qt 默认空白状态。showMessage() 调用 setHtml(实际内容) 会自动替换此占位。
    m_previewView->setHtml(buildWelcomeHtml());

    m_loadingWidget = new QWidget;
    m_loadingWidget->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::kBg));
    auto* loadLay = new QVBoxLayout(m_loadingWidget);
    loadLay->setContentsMargins(24, 32, 24, 24);
    loadLay->setSpacing(10);
    loadLay->addStretch();
    m_loadingLabel = new QLabel(QStringLiteral("进行中，正在拉取正文…"));
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    m_loadingLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;").arg(Theme::kAccent));
    loadLay->addWidget(m_loadingLabel);
    m_loadingBar = new QProgressBar;
    m_loadingBar->setRange(0, 0);
    m_loadingBar->setTextVisible(true);
    m_loadingBar->setFormat(QStringLiteral("0 B"));
    m_loadingBar->setStyleSheet(
        QStringLiteral("QProgressBar { background: %1; border: 1px solid %2; border-radius: 3px; height: 6px; text-align: center; color: %3; font-size: 11px; } QProgressBar::chunk { background: %4; border-radius: 3px; }")
            .arg(Theme::kSurface, Theme::kBorder, Theme::kMuted, Theme::kAccent));
    loadLay->addWidget(m_loadingBar);
    auto* loadHint = new QLabel(QStringLiteral("(远程拉取中，请耐心等待)"));
    loadHint->setAlignment(Qt::AlignCenter);
    loadHint->setStyleSheet(Theme::mutedText());
    loadLay->addWidget(loadHint);
    loadLay->addStretch();
    m_bodyStack->addWidget(m_loadingWidget);

    m_rawView = new QPlainTextEdit;
    m_rawView->setReadOnly(true);
    m_rawView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_rawView->setFont(QFont(QStringLiteral("Consolas"), 9));
    m_rawView->setPlaceholderText(QStringLiteral("拉取原始 MIME 原文…"));
    m_rawView->setStyleSheet(QStringLiteral("QPlainTextEdit { background: %1; color: %2; border: none; padding: 12px; }").arg(Theme::kBg, Theme::kText));
    m_bodyStack->addWidget(m_rawView);

    // ── 附件内联预览面板（page 3）─────────────────────────────────
    m_attachPreview = new AttachmentPreviewPane;
    connect(m_attachPreview, &AttachmentPreviewPane::backRequested, this, [this]{
        // 返回正文预览
        m_attachPreviewKey.clear();
        m_bodyStack->setCurrentIndex(0);
    });
    connect(m_attachPreview, &AttachmentPreviewPane::openExternallyRequested,
            this, [this](const QString& p){ QDesktopServices::openUrl(QUrl::fromLocalFile(p)); });
    connect(m_attachPreview, &AttachmentPreviewPane::revealRequested,
            this, [this](const QString& p){
#ifdef Q_OS_WIN
                QProcess::startDetached(QStringLiteral("explorer.exe"),
                    { QStringLiteral("/select,"), QDir::toNativeSeparators(p) });
#else
                QFileInfo fi(p);
                QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
#endif
            });
    m_bodyStack->addWidget(m_attachPreview);

    root->addWidget(m_bodyStack, 1);

    // ── 轻量 toast（右下角提示条，下载成功/失败等瞬时反馈，自动消失）──
    m_toast = new QLabel(this);
    m_toast->setObjectName(QStringLiteral("contentToast"));
    m_toast->setContentsMargins(14, 8, 14, 8);
    m_toast->setAlignment(Qt::AlignCenter);
    m_toast->setWordWrap(false);
    m_toast->setStyleSheet(QStringLiteral(
        "QLabel#contentToast { color: #ffffff; font-size: 12px; border-radius: 6px;"
        "  background: rgba(24,26,32,0.95); border: 1px solid #3a3d46; padding: 8px 16px; }"));
    m_toast->hide();
    m_toastOpacity = new QGraphicsOpacityEffect(m_toast);
    m_toastOpacity->setOpacity(1.0);
    m_toast->setGraphicsEffect(m_toastOpacity);
    m_toastTimer = new QTimer(this);
    m_toastTimer->setSingleShot(true);
    m_toastTimer->setInterval(2200);
    connect(m_toastTimer, &QTimer::timeout, this, &MailContentPanel::hideToastNow);
}
void MailContentPanel::setActionsEnabled(bool on) {
    if (m_replyBtn)       m_replyBtn->setEnabled(on);
    if (m_replyAllBtn)    m_replyAllBtn->setEnabled(on);
    if (m_forwardMenuBtn) m_forwardMenuBtn->setEnabled(on);
}

void MailContentPanel::setReplyForwardActionsVisible(bool on) {
    // 回复/回复全部/转发：仅在收件箱(INBOX)显示；off 时隐藏并禁用
    if (m_replyBtn)       { m_replyBtn->setVisible(on);       m_replyBtn->setEnabled(on); }
    if (m_replyAllBtn)    { m_replyAllBtn->setVisible(on);    m_replyAllBtn->setEnabled(on); }
    if (m_forwardMenuBtn) { m_forwardMenuBtn->setVisible(on); m_forwardMenuBtn->setEnabled(on); }
}

void MailContentPanel::setReSendActionsVisible(bool on) {
    // 可见性与启用态一起控制：on=true 才可见且可用（非收件箱选中时）；
    // off 时隐藏并禁用（草稿文件夹里这两个按钮始终隐藏，交给编辑器打开草稿）
    if (m_reeditBtn) { m_reeditBtn->setVisible(on); m_reeditBtn->setEnabled(on); }
    if (m_resendBtn) { m_resendBtn->setVisible(on); m_resendBtn->setEnabled(on); }
}

void MailContentPanel::setRevokeActionVisible(bool on) {
    if (m_revokeBtn) { m_revokeBtn->setVisible(on); m_revokeBtn->setEnabled(on); }
}

void MailContentPanel::setDeleteActionVisible(bool on) {
    if (m_deleteBtn) { m_deleteBtn->setVisible(on); m_deleteBtn->setEnabled(on); }
}

void MailContentPanel::clearPreview() {
    m_currentMsgId.clear();
    if (m_previewView) m_previewView->setHtml(buildWelcomeHtml());
    if (m_attachWidget) m_attachWidget->hide();
    if (m_headerWidget) {
        m_subjectLabel->clear();
        m_fromValueLabel->clear();
        m_toValueLabel->clear();
        m_dateLabel->clear();
        m_ccValueLabel->clear();
        m_headerWidget->hide();   // 切走邮件后隐藏头部，回到欢迎卡片视图
    }
    if (m_ccRowWrap) m_ccRowWrap->hide();
    for (auto* w : m_attRows) { w->deleteLater(); }
    m_attRows.clear();
    if (m_attachPreview) m_attachPreview->clear();
    m_attachPreviewKey.clear();
    if (m_bodyStack) m_bodyStack->setCurrentIndex(0);
    m_showingRaw = false;
}

void MailContentPanel::refresh() { showMessage(m_currentMsgId); }

void MailContentPanel::refreshRawIfCurrent(const QString& msgId) {
    if (m_bodyStack && m_showingRaw && m_currentMsgId == msgId)
        showRawView(msgId);
}

void MailContentPanel::throttledRefresh(qint64& lastMs) {
    if (!m_previewView || m_showingRaw) return;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastMs >= 100) { lastMs = now; refresh(); }
}

bool MailContentPanel::isAttachmentDownloading(const QString& key) const {
    return m_downloadingAtts.contains(key);
}

void MailContentPanel::setAttachmentDownloading(const QString& key, bool on) {
    if (on) m_downloadingAtts.insert(key);
    else    m_downloadingAtts.remove(key);
    // 仅在切到"非下载中"时刷新；切到"下载中"时调用方通常紧跟 setAttachmentProgress，
    // 由 progress 路径统一刷新行，避免重复 setStyleSheet 抖动。
    if (!on) refreshAttachmentRow(key);
}

void MailContentPanel::markAttachmentDownloaded(const QString& key, bool ok) {
    if (!ok) {
        m_attProgress.remove(key);
    }
    refreshAttachmentRow(key);

    // 下载结束提示（成功/失败）：仅对当前可见邮件瞬时弹出，避免后台/切走时打扰
    if (key.section(QLatin1Char('|'), 0, 0) != m_currentMsgId) return;
    QString name;
    int idx = key.section(QLatin1Char('|'), 1).toInt();
    if (auto* m = MailStore::instance().message(m_currentMsgId)) {
        if (idx >= 0 && idx < m->attachments.size()) name = m->attachments[idx].name;
    }
    if (name.isEmpty()) name = key;
    showToast(ok ? QStringLiteral("✓ 附件已下载：%1").arg(name)
                 : QStringLiteral("✕ 下载失败：%1").arg(name), ok);
}

// ── 轻量 toast ────────────────────────────────────────────
void MailContentPanel::showToast(const QString& text, bool ok) {
    if (!m_toast) return;
    m_toast->setText(text);
    // 成功用绿、失败用红的描边色块
    const QString cl = ok ? Theme::kAccent : QStringLiteral("#ef5350");
    m_toast->setStyleSheet(QStringLiteral(
        "QLabel#contentToast { color: #ffffff; font-size: 12px; border-radius: 6px;"
        "  background: rgba(24,26,32,0.96); border: 2px solid %1; padding: 6px 14px; }")
        .arg(cl));
    m_toast->adjustSize();
    repositionToast();
    if (m_toast->isVisible()) {
        // 已在显示 → 直接延迟隐藏（内容已更新）
        m_toastTimer->start();
        return;
    }
    if (m_toastAnim) { m_toastAnim->stop(); m_toastAnim->deleteLater(); m_toastAnim = nullptr; }
    m_toastAnim = new QPropertyAnimation(m_toastOpacity, "opacity", this);
    m_toastAnim->setDuration(200);
    m_toastAnim->setStartValue(0.0);
    m_toastAnim->setEndValue(1.0);
    m_toastAnim->start();
    m_toast->show();
    m_toast->raise();
    m_toastTimer->start();
}

void MailContentPanel::repositionToast() {
    if (!m_toast) return;
    const int pad = 16;
    int x = width() - m_toast->width() - pad;
    int y = height() - m_toast->height() - pad;
    m_toast->move(qMax(pad, x), qMax(pad, y));
}

void MailContentPanel::hideToastNow() {
    if (!m_toast) return;
    if (m_toastAnim) { m_toastAnim->stop(); m_toastAnim->deleteLater(); m_toastAnim = nullptr; }
    m_toastOpacity->setOpacity(1.0);
    m_toast->hide();
}

void MailContentPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    repositionToast();
}

void MailContentPanel::setAttachmentProgress(const QString& key,
                                             qint64 received, qint64 expected) {
    qint64 oldExp = m_attProgress.value(key).second;
    m_attProgress[key] = QPair<qint64, qint64>(received, expected > 0 ? expected : oldExp);
    const QString msgId = key.section(QLatin1Char('|'), 0, 0);
    if (msgId != m_currentMsgId) return;
    auto* row = m_attRows.value(key, nullptr);
    if (!row) return;
    if (auto* bar = row->findChild<QProgressBar*>(QStringLiteral("attBar"))) {
        qint64 exp = m_attProgress[key].second;
        if (exp > 0) { bar->setRange(0, int(exp)); bar->setValue(int(received)); }
        else         { bar->setRange(0, 0); }
    }
    if (auto* info = row->findChild<QLabel*>(QStringLiteral("attInfo"))) {
        qint64 exp = m_attProgress[key].second;
        QString recvTxt  = QLocale().formattedDataSize(received, 1, QLocale::DataSizeIecFormat);
        QString totalTxt = exp > 0
            ? QLocale().formattedDataSize(exp, 1, QLocale::DataSizeIecFormat)
            : QStringLiteral("?");
        int pct = exp > 0 ? int(received * 100 / exp) : -1;
        info->setText(pct >= 0
            ? QStringLiteral("%1 / %2  ·  %3%").arg(recvTxt, totalTxt).arg(pct)
            : QStringLiteral("%1 / %2").arg(recvTxt, totalTxt));
    }
}

void MailContentPanel::clearAttachmentProgress(const QString& key) {
    m_attProgress.remove(key);
    if (m_currentMsgId == key.section(QLatin1Char('|'), 0, 0)) refreshAttachmentRow(key);
}

void MailContentPanel::setBodyProgress(const QString& msgId,
                                       qint64 received, qint64 expected) {
    m_bodyProgress[msgId] = QPair<qint64, qint64>(received, expected);
    if (msgId != m_currentMsgId || m_showingRaw) return;
    if (m_loadingBar) {
        if (expected > 0) {
            m_loadingBar->setRange(0, int(expected));
            m_loadingBar->setValue(int(received));
            m_loadingBar->setFormat(QStringLiteral("%1 / %2").arg(
                QLocale().formattedDataSize(received, 1, QLocale::DataSizeIecFormat),
                QLocale().formattedDataSize(expected, 1, QLocale::DataSizeIecFormat)));
        } else {
            m_loadingBar->setRange(0, 0);
            m_loadingBar->setFormat(QLocale().formattedDataSize(received, 1, QLocale::DataSizeIecFormat));
        }
    }
    if (m_loadingLabel && expected > 0) {
        int pct = int(received * 100 / expected);
        m_loadingLabel->setText(QStringLiteral("进行中，正在拉取正文…  %1%").arg(pct));
    }
}

void MailContentPanel::clearBodyProgress(const QString& msgId) {
    m_bodyProgress.remove(msgId);
}

void MailContentPanel::setBodyLoadError(const QString& msgId) {
    m_bodyLoadFailMsgId = msgId;
}

// 清理附件文件名：去除 Windows 文件系统非法字符与控制字符，
// 处理上游解码失败产生的 `?`（U+003F 等）以及末尾不可点/空格，
// 保证写盘时不会触发 "文件名、目录名或卷标语法不正确"。
static QString sanitizeFileName(const QString& raw, const QString& section) {
    QString name = raw;
    if (name.isEmpty()) name = QStringLiteral("attachment_%1.bin").arg(section);
    // 1. 替换 Windows 路径非法字符
    static const QRegularExpression illegalRe(QStringLiteral("[\\\\/:*?\"<>|]"));
    name.replace(illegalRe, QStringLiteral("_"));
    // 2. 删除所有控制字符（含 \r \n \t 与 0x00-0x1F / 0x7F）
    QString cleaned;
    cleaned.reserve(name.size());
    for (QChar c : name) {
        ushort u = c.unicode();
        if (u >= 0x20 && u != 0x7F) cleaned.append(c);
    }
    name = cleaned;
    // 3. 去除前导/尾随空格、点（Windows 不允许）
    while (!name.isEmpty() && (name.endsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char('.'))))
        name.chop(1);
    while (!name.isEmpty() && (name.startsWith(QLatin1Char(' ')) || name.startsWith(QLatin1Char('.'))))
        name.remove(0, 1);
    // 4. 防路径穿越
    while (name.contains(QStringLiteral(".."))) name.replace(QStringLiteral(".."), QStringLiteral("_"));
    // 5. 空字符串兜底
    if (name.isEmpty()) name = QStringLiteral("attachment_%1.bin").arg(section);
    // 6. 长度限制（Windows MAX_PATH = 260，留出目录开销）
    if (name.size() > 200) name = name.left(200);
    return name;
}

QString MailContentPanel::attachmentSavedPath(const QString& msgId,
                                              const MailStore::Attachment& a) {
    QString dir = Settings::instance().downloadDir();
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QString name = sanitizeFileName(a.name, a.section);
    return QDir(dir).filePath(QStringLiteral("mail_attachments/%1/%2").arg(msgId, name));
}
void MailContentPanel::onToExpandToggled() {
    if (m_currentMsgId.isEmpty()) return;
    QString key = m_currentMsgId + QStringLiteral("|to");
    if (m_expandState.contains(key)) m_expandState.remove(key);
    else m_expandState.insert(key);
    auto* m = MailStore::instance().message(m_currentMsgId);
    if (m) renderHeader(m);
}

void MailContentPanel::onCcExpandToggled() {
    if (m_currentMsgId.isEmpty()) return;
    QString key = m_currentMsgId + QStringLiteral("|cc");
    if (m_expandState.contains(key)) m_expandState.remove(key);
    else m_expandState.insert(key);
    auto* m = MailStore::instance().message(m_currentMsgId);
    if (m) renderHeader(m);
}

void MailContentPanel::setAddressText(QLabel* valLabel, QToolButton* btn,
                                      const QStringList& list, const QString& stateKey) {
    if (list.isEmpty()) { valLabel->clear(); btn->hide(); return; }
    const bool tooMany = list.size() > kAddrMaxShow;
    const bool tooLong = list.join(QStringLiteral(", ")).size() > kAddrMaxLen;
    const bool collapsed = (tooMany || tooLong) && !m_expandState.contains(stateKey);
    if (!collapsed) {
        valLabel->setText(list.join(QStringLiteral(", ")));
        btn->setText(QStringLiteral("▲ 收起"));
        btn->show();
    } else {
        valLabel->setText(list.mid(0, kAddrMaxShow).join(QStringLiteral(", "))
                          + QStringLiteral(" 等 %1 人").arg(list.size()));
        btn->setText(QStringLiteral("▼ 展开全部"));
        btn->show();
    }
}

void MailContentPanel::renderHeader(const MailStore::Message* m) {
    if (m_headerWidget) m_headerWidget->show();   // 选中邮件时展开头部
    m_subjectLabel->setText(m->subject.isEmpty()
        ? QStringLiteral("(无主题)") : m->subject);
    m_fromValueLabel->setText(m->from);
    m_dateLabel->setText(m->date.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    setAddressText(m_toValueLabel, m_toExpandBtn, m->to, m->id + QStringLiteral("|to"));
    if (m->cc.isEmpty()) {
        m_ccRowWrap->hide();
        m_ccFieldLabel->hide();
    } else {
        m_ccRowWrap->show();
        m_ccFieldLabel->show();
        setAddressText(m_ccValueLabel, m_ccExpandBtn, m->cc, m->id + QStringLiteral("|cc"));
    }
}
// 可点击的附件行：row 自身接收 mousePressEvent，触发 attachmentClicked。
// 子按钮（打开 / 定位到文件夹）拦截事件后独立发信号，不触发 attachmentClicked。
class AttachmentRow : public QWidget {
public:
    AttachmentRow(const QString& msgId, int idx, const QString& name,
                  MailContentPanel* panel, QWidget* parent = nullptr)
        : QWidget(parent), m_panel(panel), m_msgId(msgId),
          m_idx(idx), m_name(name) {
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);
    }
    void mousePressEvent(QMouseEvent* e) override {
        // 子控件（如打开 / 定位按钮）已 accept，则不重复触发行点击
        if (e->button() == Qt::LeftButton) {
            emit m_panel->attachmentClicked(m_msgId, m_idx, m_name);
            e->accept();
            return;
        }
        if (e->button() == Qt::RightButton) {
            // 右键菜单：用系统默认应用打开 / 在文件夹显示
            QMenu menu;
            QAction* openAct = menu.addAction(QStringLiteral("用系统应用打开"));
            QAction* revealAct = menu.addAction(QStringLiteral("在文件夹显示"));
            QAction* chosen = menu.exec(e->globalPos());
            if (chosen == openAct) {
                // 走和点击附件同样的逻辑（已下载直接开，未下载先下载再开）
                emit m_panel->attachmentClicked(m_msgId, m_idx, m_name);
            } else if (chosen == revealAct) {
                // 在文件夹显示：即便未下载也允许（用邮件 id + idx 触发 reveal 信号）
                // 简化：从 MailStore 找 savedPath
                auto* m = MailStore::instance().message(m_msgId);
                if (m && m_idx >= 0 && m_idx < m->attachments.size()) {
                    QString path = MailContentPanel::attachmentSavedPath(m_msgId, m->attachments[m_idx]);
                    if (QFileInfo(path).exists()) {
#ifdef Q_OS_WIN
                        QProcess::startDetached(QStringLiteral("explorer.exe"),
                            { QStringLiteral("/select,"), QDir::toNativeSeparators(path) });
#else
                        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
                    } else {
                        QMessageBox::information(m_panel, QStringLiteral("提示"),
                            QStringLiteral("附件尚未下载，无法在文件夹显示。\n请先点击附件行以下载。"));
                    }
                }
            }
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }
private:
    MailContentPanel* m_panel = nullptr;
    QString m_msgId;
    int     m_idx = 0;
    QString m_name;
};

// 文件类型徽章：根据 mime / 扩展名返回带字母的小色块，模仿 Outlook 风格。
// 返回的 QLabel 由调用方纳入布局，所有权随之转移。
static QWidget* makeFileTypeBadge(const QString& mimeType, const QString& fname) {
    QString ext = QFileInfo(fname).suffix().toLower();
    QString mime = mimeType.split(QLatin1Char(';')).first().trimmed().toLower();

    QString letter = QStringLiteral("?");
    QString bg     = QStringLiteral("#5A6B7A"); // 默认灰

    auto set = [&](const QString& l, const QString& c){ letter = l; bg = c; };

    if (mime.contains(QStringLiteral("spreadsheetml")) ||
        mime.contains(QStringLiteral("excel")) ||
        mime.contains(QStringLiteral("spreadsheet")) ||
        ext == QStringLiteral("xls") || ext == QStringLiteral("xlsx") ||
        ext == QStringLiteral("csv") || ext == QStringLiteral("xlsm")) {
        set(QStringLiteral("X"), QStringLiteral("#1F7244"));  // Excel 绿
    } else if (mime.contains(QStringLiteral("wordprocessingml")) ||
               mime.contains(QStringLiteral("word")) ||
               mime.contains(QStringLiteral("msword")) ||
               ext == QStringLiteral("doc") || ext == QStringLiteral("docx") ||
               ext == QStringLiteral("rtf") || ext == QStringLiteral("wps")) {
        set(QStringLiteral("W"), QStringLiteral("#2B579A"));  // Word 蓝
    } else if (mime.contains(QStringLiteral("pdf")) || ext == QStringLiteral("pdf")) {
        set(QStringLiteral("P"), QStringLiteral("#B30B00"));  // PDF 红
    } else if (mime.contains(QStringLiteral("zip")) || mime.contains(QStringLiteral("compressed")) ||
               mime.contains(QStringLiteral("rar")) || mime.contains(QStringLiteral("7z")) ||
               mime.contains(QStringLiteral("tar")) || mime.contains(QStringLiteral("gzip")) ||
               ext == QStringLiteral("zip") || ext == QStringLiteral("rar") ||
               ext == QStringLiteral("7z") || ext == QStringLiteral("tar") ||
               ext == QStringLiteral("gz")  || ext == QStringLiteral("bz2")) {
        set(QStringLiteral("Z"), QStringLiteral("#A66100"));  // 压缩包 棕
    } else if (mime.startsWith(QStringLiteral("image/")) ||
               ext == QStringLiteral("png") || ext == QStringLiteral("jpg") ||
               ext == QStringLiteral("jpeg") || ext == QStringLiteral("gif") ||
               ext == QStringLiteral("bmp") || ext == QStringLiteral("webp") ||
               ext == QStringLiteral("svg")) {
        set(QStringLiteral("I"), QStringLiteral("#7E3CFF"));  // 图片 紫
    } else if (mime.startsWith(QStringLiteral("audio/")) ||
               ext == QStringLiteral("mp3") || ext == QStringLiteral("wav") ||
               ext == QStringLiteral("flac") || ext == QStringLiteral("m4a")) {
        set(QStringLiteral("A"), QStringLiteral("#A020A0"));  // 音频 品红
    } else if (mime.startsWith(QStringLiteral("video/")) ||
               ext == QStringLiteral("mp4") || ext == QStringLiteral("mov") ||
               ext == QStringLiteral("avi") || ext == QStringLiteral("mkv")) {
        set(QStringLiteral("V"), QStringLiteral("#D04020"));  // 视频 橙红
    } else if (mime.startsWith(QStringLiteral("text/")) ||
               mime.contains(QStringLiteral("json")) || mime.contains(QStringLiteral("xml")) ||
               mime.contains(QStringLiteral("html")) || mime.contains(QStringLiteral("javascript")) ||
               ext == QStringLiteral("txt") || ext == QStringLiteral("log") || ext == QStringLiteral("md") ||
               ext == QStringLiteral("json") || ext == QStringLiteral("xml") || ext == QStringLiteral("htm") ||
               ext == QStringLiteral("html") || ext == QStringLiteral("js") || ext == QStringLiteral("css")) {
        set(QStringLiteral("T"), QStringLiteral("#4A5C6E"));  // 文本 深灰
    } else if (ext == QStringLiteral("ppt") || ext == QStringLiteral("pptx") || ext == QStringLiteral("key")) {
        set(QStringLiteral("P"), QStringLiteral("#C43E1C"));  // PPT 橙
    }

    auto* badge = new QLabel(letter);
    badge->setObjectName(QStringLiteral("attBadge"));
    badge->setFixedSize(26, 26);
    badge->setAlignment(Qt::AlignCenter);
    badge->setStyleSheet(QStringLiteral(
        "QLabel { background: %1; color: white; border-radius: 4px; "
        "font-weight: 600; font-family: 'Segoe UI', 'Microsoft YaHei', sans-serif; font-size: 13px; }")
        .arg(bg));
    return badge;
}

static QWidget* buildAttachmentRow(const QString& msgId, int idx,
                                  const MailStore::Attachment& a,
                                  MailContentPanel* panel) {
    QString fname = sanitizeFileName(a.name, a.section);
    QString ext = defaultExtension(a.mimeType, fname);
    if (!ext.isEmpty() && !fname.endsWith(ext)) fname += ext;
    // 扩展名拼接后若以 . 或 空格结尾，再次清理（防 Windows 末位点）
    fname = sanitizeFileName(fname, a.section);

    auto* row = new AttachmentRow(msgId, idx, fname, panel);
    row->setObjectName(QStringLiteral("attRow_%1_%2").arg(msgId).arg(idx));
    row->setStyleSheet(QStringLiteral("background: %1; border: 1px solid %2; border-radius: 4px;").arg(Theme::kSidebar, Theme::kBorder));

    // 整体：单行 = [徽章] [附件:] [文件名 ellipsis] [...] [大小] [···菜单]
    // 外层 QVBoxLayout：topRow(主行) + bar(进度条，下载中显示，隐藏时折叠)
    auto* outerLayout = new QVBoxLayout(row);
    outerLayout->setContentsMargins(8, 6, 8, 6);
    outerLayout->setSpacing(2);

    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(8);

    // 1. 文件类型徽章（左侧）
    topRow->addWidget(makeFileTypeBadge(a.mimeType, fname));


    // 3. 文件名（占主要宽度，溢出自动 ellipsis）
    auto* name = new QLabel(fname);
    name->setObjectName(QStringLiteral("attName"));
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    name->setMinimumWidth(0);
    name->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;").arg(Theme::kTextBright));
    topRow->addWidget(name, 1);

    // 4. 文件大小
    QString sizeStr = formatSizeShort(a.size);
    auto* sizeLabel = new QLabel(sizeStr);
    sizeLabel->setObjectName(QStringLiteral("attSize"));
    sizeLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(Theme::kMuted));
    if (sizeStr.isEmpty()) sizeLabel->hide();
    topRow->addWidget(sizeLabel);

    // 5. "···" 操作菜单按钮（InstantPopup 弹出"打开" / "在文件夹中显示"）
    auto* menuBtn = new QToolButton(row);
    menuBtn->setObjectName(QStringLiteral("attMenuBtn"));
    menuBtn->setText(QStringLiteral("···"));
    menuBtn->setToolTip(QStringLiteral("更多操作"));
    menuBtn->setCursor(Qt::PointingHandCursor);
    menuBtn->setAutoRaise(true);
    menuBtn->setFixedSize(22, 22);
    menuBtn->setPopupMode(QToolButton::InstantPopup);
    menuBtn->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; font-size: 14px; font-weight: 700; border: none; background: transparent; } "
        "QToolButton:hover { color: %2; } "
        "QToolButton::menu-indicator { image: none; }")
        .arg(Theme::kMuted, Theme::kAccent));

    auto* menu = new QMenu(menuBtn);
    menu->setStyleSheet(QStringLiteral(
        "QMenu { background: %1; color: %2; border: 1px solid %3; padding: 4px; } "
        "QMenu::item { padding: 4px 16px; border-radius: 3px; } "
        "QMenu::item:selected { background: %4; color: white; } "
        "QMenu::item:disabled { color: %5; }")
        .arg(Theme::kSurface, Theme::kTextBright, Theme::kBorder, Theme::kAccent, Theme::kFaint));
    auto* downloadAct = menu->addAction(QStringLiteral("下载"));       // 下载
    auto* openAct     = menu->addAction(QStringLiteral("打开"));       // 打开
    auto* saveAsAct   = menu->addAction(QStringLiteral("另存为")); // 另存为
    auto* revealAct   = menu->addAction(QStringLiteral("在文件夹中显示")); // 在文件夹中显示
    // 给动作绑 objectName，便于 refreshAttachmentRow 按名称定位
    downloadAct->setObjectName(QStringLiteral("attActDownload"));
    openAct->setObjectName(QStringLiteral("attActOpen"));
    saveAsAct->setObjectName(QStringLiteral("attActSaveAs"));
    revealAct->setObjectName(QStringLiteral("attActReveal"));
    menuBtn->setMenu(menu);
    topRow->addWidget(menuBtn);

    outerLayout->addLayout(topRow);

    // 6. 进度条：仅下载中可见（隐藏时折叠该行），避免"100% 满" / "空灰条"冗余
    auto* bar = new QProgressBar;
    bar->setObjectName(QStringLiteral("attBar"));
    bar->setRange(0, 1);
    bar->setValue(0);
    bar->setTextVisible(false);
    bar->setFixedHeight(3);
    bar->setStyleSheet(QStringLiteral(
        "QProgressBar { background: %1; border: none; border-radius: 1px; } "
        "QProgressBar::chunk { background: %2; border-radius: 1px; }")
        .arg(Theme::kSurface, Theme::kMuted));
    bar->hide();
    outerLayout->addWidget(bar);

    // 菜单动作 → 透传 panel 信号（带 savedPath）
    auto savedPathFor = [msgId, idx]() -> QString {
        return MailContentPanel::attachmentSavedPath(msgId,
            [&]() -> MailStore::Attachment {
                if (auto* mm = MailStore::instance().message(msgId)) {
                    if (idx >= 0 && idx < mm->attachments.size())
                        return mm->attachments[idx];
                }
                return {};
            }());
    };
    QObject::connect(downloadAct, &QAction::triggered, row, [panel, msgId, idx, fname]{
        emit panel->attachmentDownloadRequested(msgId, idx, fname);
    });
    QObject::connect(openAct,   &QAction::triggered, row, [panel, msgId, idx, fname, savedPathFor]{
        emit panel->attachmentOpenRequested(msgId, idx, fname, savedPathFor());
    });
    QObject::connect(saveAsAct, &QAction::triggered, row, [panel, msgId, idx, fname, savedPathFor]{
        emit panel->attachmentSaveAsRequested(msgId, idx, fname, savedPathFor());
    });
    QObject::connect(revealAct, &QAction::triggered, row, [panel, msgId, idx, fname, savedPathFor]{
        emit panel->attachmentRevealRequested(msgId, idx, fname, savedPathFor());
    });

    return row;
}
void MailContentPanel::refreshAttachmentRow(const QString& key) {
    if (m_currentMsgId != key.section(QLatin1Char('|'), 0, 0)) return;
    auto* row = m_attRows.value(key, nullptr);
    if (!row) return;
    bool okIdx = false;
    int idx = key.section(QLatin1Char('|'), 1).toInt(&okIdx);
    if (!okIdx) return;
    auto* m = MailStore::instance().message(m_currentMsgId);
    if (!m || idx < 0 || idx >= m->attachments.size()) return;
    const auto& a = m->attachments[idx];

    auto* sizeLabel = row->findChild<QLabel*>(QStringLiteral("attSize"));
    auto* bar       = row->findChild<QProgressBar*>(QStringLiteral("attBar"));
    auto* menuBtn   = row->findChild<QToolButton*>(QStringLiteral("attMenuBtn"));
    if (!bar) return;

    QString savedPath = attachmentSavedPath(m->id, a);
    // 仅在非下载中状态才依据磁盘文件判断"已下载"；
    // 下载中时优先显示进度态，避免 fetchPart 返回后、写盘完成前瞬间被误判为未下载。
    bool inProgress = m_downloadingAtts.contains(key);
    bool downloaded = !inProgress && QFileInfo(savedPath).exists();

    auto setBarChunkColor = [](QProgressBar* bar, const QString& color){
        bar->setStyleSheet(QStringLiteral(
            "QProgressBar { background: %1; border: none; border-radius: 1px; } "
            "QProgressBar::chunk { background: %2; border-radius: 1px; }")
            .arg(Theme::kSurface, color));
    };

    // 尺寸常驻：刷新一次即可（不会因状态变化）
    QString sizeTxt = formatSizeShort(a.size);
    if (sizeLabel) {
        if (sizeTxt.isEmpty()) sizeLabel->hide();
        else { sizeLabel->show(); sizeLabel->setText(sizeTxt); }
    }

    // 菜单动作：根据状态切换 enabled：
    //   下载       —— 始终允许（已下载也可触发重新下载，覆盖本地文件）
    //   打开       —— 仅已下载时启用
    //   另存为     —— 仅已下载时启用（需要源文件可复制）
    //   在文件夹中显示 —— 仅已下载时启用
    if (menuBtn && menuBtn->menu()) {
        auto setByName = [&](const QString& name, bool en){
            if (auto* a = menuBtn->menu()->findChild<QAction*>(name)) a->setEnabled(en);
        };
        setByName(QStringLiteral("attActDownload"), true);
        setByName(QStringLiteral("attActOpen"),     downloaded);
        setByName(QStringLiteral("attActSaveAs"),   downloaded);
        setByName(QStringLiteral("attActReveal"),   downloaded);
    }

    if (inProgress) {
        // 下载中：进度条可见，蓝色 chunk
        qint64 recv = m_attProgress.value(key).first;
        qint64 exp  = m_attProgress.value(key).second;
        if (exp > 0) {
            bar->setRange(0, int(exp));
            bar->setValue(int(recv));
        } else {
            bar->setRange(0, 0);
        }
        setBarChunkColor(bar, Theme::kAccent);
        bar->show();
    } else {
        // 未下载 / 已下载：进度条折叠，避免冗余视觉
        bar->hide();
    }
}
void MailContentPanel::rebuildAttachmentRows(const MailStore::Message* m) {
    for (auto* w : m_attRows) { w->deleteLater(); }
    m_attRows.clear();
    for (int i = 0; i < m->attachments.size(); ++i) {
        const auto& a = m->attachments[i];
        QString key = attachmentKey(i);
        QWidget* row = buildAttachmentRow(m->id, i, a, this);
        m_attachRowsLayout->addWidget(row);
        m_attRows.insert(key, row);
    }
}

void MailContentPanel::renderAttachments(const MailStore::Message* m) {
    if (m->attachments.isEmpty()) {
        for (auto* w : m_attRows) { w->deleteLater(); }
        m_attRows.clear();
        m_attachWidget->hide();
        return;
    }
    // 计算附件总大小；显示 "📎 N个附件 | TOTAL"
    qint64 totalBytes = 0;
    for (const auto& a : m->attachments) totalBytes += a.size;
    QString totalTxt = formatSizeShort(totalBytes);
    QString head = QString::fromUtf8("\xF0\x9F\x93\x8E ") + QStringLiteral("%1个附件").arg(m->attachments.size());
    if (!totalTxt.isEmpty()) head += QStringLiteral("  |  %1").arg(totalTxt);
    m_attachTitleLabel->setText(head);
    rebuildAttachmentRows(m);
    for (int i = 0; i < m->attachments.size(); ++i)
        refreshAttachmentRow(attachmentKey(i));
    m_attachWidget->show();
}
void MailContentPanel::renderBody(const MailStore::Message* m) {
    m_showingRaw = false;
    if (!m) { clearPreview(); return; }

    // ── S/MIME 签名验证（multipart/signed） ──
    // 注意：签名验证放加密检测前。
    // 流程：multipart/signed → 拆出原 MIME + 签名 part → CMS_verify → 显示签名状态条 + 渲染原 MIME
    // 如果同时加密：rawSource 顶层是 application/pkcs7-mime（enveloped），不是 signed，
    //    会被跳过；解密后再渲染。
    QString signedBanner;   // 用于显示在正文上方的签名状态条
    QByteArray signedOriginalMime;  // 验证后展示用的原 MIME bytes（可能是 multipart/mixed、text/plain 等）
    if (!m->rawSource.isEmpty() && SmimeCrypto::isSigned(m->rawSource)) {
        // 信任根 PEM 暂时用空（不验证书链，只验签名哈希 + 提取签名者信息）
        QString trustPem;   // TODO: 后续可读取 Settings 中的 trusted CAs 配置
        QString err;
        SmimeCrypto::VerifyResult vr = SmimeCrypto::verify(m->rawSource, trustPem, &err);
        if (vr.ok) {
            QString who = vr.signerSubject.isEmpty()
                ? (vr.signerEmail.isEmpty() ? QStringLiteral("(未知签名者)") : vr.signerEmail)
                : vr.signerEmail.isEmpty()
                    ? vr.signerSubject
                    : QStringLiteral("%1 <%2>").arg(vr.signerSubject, vr.signerEmail);
            signedBanner = QStringLiteral(
                "<div style='background:#0a3d0a; color:#aaffaa; padding:8px 12px; "
                "border-left:4px solid #4caf50; margin-bottom:12px; font-size:13px;'>"
                "<b>✓ 数字签名验证通过</b>（算法: %1）<br>"
                "签名者: %2<br>"
                "<small>此邮件内容未被篡改，且确实由签名者的私钥签发。</small></div>"
            ).arg(vr.digestAlg, who.toHtmlEscaped());
        } else {
            // 验证失败：可能是真签名但本地无 CA 根证书，也可能真的被篡改
            // 我们尝试用更宽松的方式重验（只验哈希不管证书链）
            bool noCertsOnly = vr.errorReason.contains(QStringLiteral("certificate verify"));
            QString bg = noCertsOnly ? "#3d3d0a" : "#3d0a0a";
            QString fg = noCertsOnly ? "#ffd97a" : "#ffaaaa";
            QString br = noCertsOnly ? "#e8b800" : "#d33";
            QString expl = noCertsOnly
                ? QStringLiteral("签名哈希正确，但无法验证证书链（缺少信任根）")
                : QStringLiteral("签名无效或邮件内容被篡改");
            signedBanner = QStringLiteral(
                "<div style='background:%1; color:%2; padding:8px 12px; "
                "border-left:4px solid %3; margin-bottom:12px; font-size:13px;'>"
                "<b>⚠ 签名验证未通过</b><br>"
                "原因: %4<br>"
                "详情: %5<br>"
                "<small>%6</small></div>"
            ).arg(bg, fg, br,
                  expl.toHtmlEscaped(),
                  vr.errorReason.toHtmlEscaped(),
                  QStringLiteral("请勿轻信此邮件内容。"));
        }
        // 把 multipart/signed 中的"原 MIME part"拆出来，渲染其内容
        // 第一个 part 是 message/rfc822 容器装的原邮件
        int hdrEnd2 = m->rawSource.indexOf("\r\n\r\n");
        if (hdrEnd2 < 0) hdrEnd2 = m->rawSource.indexOf("\n\n");
        if (hdrEnd2 > 0) {
            QByteArray headers2 = m->rawSource.left(hdrEnd2);
            QByteArray body2    = m->rawSource.mid(hdrEnd2 + 4);
            QRegularExpression bndRe2("boundary=\"?([^\"\\s;]+)\"?",
                                      QRegularExpression::CaseInsensitiveOption);
            auto bm2 = bndRe2.match(QString::fromLatin1(headers2));
            if (bm2.hasMatch()) {
                QString bnd2 = bm2.captured(1);
                QString bodyStr = QString::fromLatin1(body2);
                int firstEnd = bodyStr.indexOf(QStringLiteral("--%1").arg(bnd2), 1);
                if (firstEnd > 0) {
                    QString firstPart = bodyStr.left(firstEnd);
                    int phEnd = firstPart.indexOf(QStringLiteral("\r\n\r\n"));
                    if (phEnd > 0) {
                        // 原 MIME 完整文本：headers + body
                        signedOriginalMime = firstPart.mid(phEnd + 4).toUtf8();
                    }
                }
            }
        }
    }

    // ── 加密邮件检测（S/MIME 或 口令信封） ──
    // rawSource 是已拉取的完整 RFC822；若顶层是加密信封，弹窗输入口令 → 解密 → 渲染
    if (!m->rawSource.isEmpty() && signedOriginalMime.isEmpty()) {
        QString rawText = QString::fromUtf8(m->rawSource);
        int hdrEnd = rawText.indexOf(QStringLiteral("\r\n\r\n"));
        if (hdrEnd < 0) hdrEnd = rawText.indexOf(QStringLiteral("\n\n"));
        if (hdrEnd > 0) {
            QString headers = rawText.left(hdrEnd);
            QString topCtype;
            static const QRegularExpression ctypeReTop(
                QStringLiteral("^Content-Type:\\s*([^;\\r\\n]+)"),
                QRegularExpression::CaseInsensitiveOption |
                QRegularExpression::MultilineOption);
            auto cm = ctypeReTop.match(headers);
            if (cm.hasMatch()) topCtype = cm.captured(1).trimmed();

            QString kind = MailEncryptor::detectKind(topCtype, headers, m->rawSource);
            if (!kind.isEmpty()) {
                QString hint;
                if (kind == "smime") {
                    auto meta = SmimeCrypto::peekMeta(m->rawSource);
                    hint = QStringLiteral(
                        "S/MIME 加密邮件（收件人证书：%1）\n\n请输入 PKCS#12 个人证书口令以解密：")
                        .arg(meta.recipientIssuer.isEmpty()
                             ? QStringLiteral("未知") : meta.recipientIssuer);
                } else {
                    hint = QStringLiteral(
                        "口令加密邮件\n\n请输入共享口令以解密：");
                }
                bool ok = false;
                QString pwd = QInputDialog::getText(this,
                    QStringLiteral("解密邮件"),
                    hint,
                    QLineEdit::Password,
                    QString(), &ok);
                if (ok && !pwd.isEmpty()) {
                    QString err;
                    QByteArray plain = MailEncryptor::unwrap(m->rawSource, pwd, &err);
                    if (!plain.isEmpty()) {
                        QString plainText, htmlText;
                        extractPlainHtmlFromMime(plain, &plainText, &htmlText);
                        if (!htmlText.isEmpty()) {
                            if (!htmlText.contains(QStringLiteral("<body"), Qt::CaseInsensitive))
                                htmlText = QStringLiteral("<html><body style=\"background-color: transparent; color:#1e1e1e; font-size:14px;\">")
                                           + htmlText + QStringLiteral("</body></html>");
                            m_previewView->setHtml(htmlText);
                        } else if (!plainText.isEmpty()) {
                            m_previewView->setHtml(
                                QStringLiteral("<html><body style=\"background-color: transparent;\"><pre style=\"white-space: pre-wrap; word-wrap: break-word; color:#1e1e1e; font-family: 'Microsoft YaHei', sans-serif; font-size:14px; line-height:1.6;\">")
                                + plainText.toHtmlEscaped() + QStringLiteral("</pre></body></html>"));
                        } else {
                            m_previewView->setHtml(QStringLiteral(
                                "<html><body style=\"background-color: transparent; color:#1e1e1e; padding:24px;\">"
                                "<p>已解密，但未找到可显示的正文。</p></body></html>"));
                        }
                        m_bodyStack->setCurrentIndex(0);
                        Logger::instance().info(
                            QString("MailContentPanel: 解密成功 msgId=%1 kind=%2").arg(m->id, kind), "mail");
                        return;
                    } else {
                        QMessageBox::warning(this, QStringLiteral("解密失败"), err);
                    }
                }
                // 用户取消或失败 → 显示提示
                m_previewView->setHtml(QStringLiteral(
                    "<html><body style=\"background-color: transparent; color:#555555; padding:24px;\">"
                    "<p>此邮件已加密（%1），未解密。</p>"
                    "<p style=\"font-size:12px;\">点击此邮件重新尝试解密，或在收件箱右键选择\"重新解密\"。</p>"
                    "</body></html>").arg(kind));
                m_bodyStack->setCurrentIndex(0);
                return;
            }
        }
    }

    // 正常渲染（明文）
    if (m) {
        Logger::instance().info(
            QString("renderBody: msgId=%1 htmlLen=%2 bodyLen=%3 rawSize=%4 att=%5")
                .arg(m->id)
                .arg(m->htmlBody.size())
                .arg(m->body.size())
                .arg(m->rawSource.size())
                .arg(m->attachments.size()),
            "mail");
    }
    if (!m || m->htmlBody.isEmpty()) {
        // 顶级不是 multipart/related：cid 通常不存在，直接按 plain 显示或触发按需拉取
        m_previewView->setCidImages({});
        if (m && m->body.isEmpty()) {
            if (m_bodyLoadFailMsgId == m->id) {
                // 最近一次按需拉取失败（超时/断网）：显示错误提示而非停留"拉取中 100%"
                m_bodyLoadFailMsgId.clear();
                const QString tip = QStringLiteral(
                    "<html><body style=\"background-color: transparent; color:#c62828; padding:24px;\">"
                    "<p>正文拉取失败。</p>"
                    "<p style=\"font-size:12px; color:#777777;\">连接服务器超时或网络中断，请检查网络后重新选择该邮件重试。</p>"
                    "</body></html>");
                m_previewView->setHtml(tip);
                m_bodyStack->setCurrentIndex(0);
                Logger::instance().warn(
                    QString("renderBody: msgId=%1 正文拉取失败，已显示错误提示").arg(m->id), "mail");
                return;
            }
            if (m->rawSource.isEmpty()) {
            if (m_loadingBar) {
                auto bp = m_bodyProgress.value(m->id);
                qint64 recv = bp.first, exp = bp.second;
                if (exp > 0) {
                    m_loadingBar->setRange(0, int(exp));
                    m_loadingBar->setValue(int(recv));
                    m_loadingBar->setFormat(QStringLiteral("%1 / %2").arg(QLocale().formattedDataSize(recv, 1, QLocale::DataSizeIecFormat), QLocale().formattedDataSize(exp, 1, QLocale::DataSizeIecFormat)));
                    m_loadingLabel->setText(QStringLiteral("进行中，正在拉取正文…  %1%").arg(int(recv * 100 / exp)));
                } else if (recv > 0) {
                    m_loadingBar->setRange(0, 0);
                    m_loadingBar->setFormat(QLocale().formattedDataSize(recv, 1, QLocale::DataSizeIecFormat));
                    m_loadingLabel->setText(QStringLiteral("进行中，正在拉取正文…"));
                } else {
                    m_loadingBar->setRange(0, 0);
                    m_loadingBar->setFormat(QStringLiteral("0 B"));
                    m_loadingLabel->setText(QStringLiteral("进行中，正在拉取正文…"));
                }
            }
                m_bodyStack->setCurrentIndex(1);
                Logger::instance().info(
                    QString("renderBody: msgId=%1 html+body+raw 全空，触发按需拉取").arg(m->id),
                    "mail");
                emit bodyLoadRequested(m->id);
                return;
            }
            // fetchBody 已完成但解析不出 plain/html（rawSource 已写入，但 body/htmlBody 都为空）
            // 不再回到 loading 分支，否则会卡在 100% 进度条；显示明确提示
            Logger::instance().warn(
                QString("renderBody: msgId=%1 fetchBody 已完成但解析失败 rawSize=%2 (html+body 双空) — raw dump 已写入 app.log，请搜索 'extractBodyParts: 解析失败'")
                    .arg(m->id).arg(m->rawSource.size()),
                "mail");
            const QString tip = QStringLiteral(
                "<html><body style=\"background-color: transparent; color:#555555; font-size:13px; padding:24px;\">"
                "<p>该邮件未能解析出可读的正文。</p>"
                "<p style=\"font-size:12px; color:#777777;\">rawSource 已拉取（%1 B），但未找到 text/plain / text/html 部分，或部分解码失败。</p>"
                "<p style=\"font-size:12px; color:#777777;\">详细信息已写入 app.log，请搜索 “extractBodyParts: 解析失败”。</p>"
                "</body></html>").arg(m->rawSource.size());
            m_previewView->setHtml(tip);
            m_bodyStack->setCurrentIndex(0);
            return;
        }
        auto esc = [](const QString& s){ return s.toHtmlEscaped(); };
        const QString& bodyText = m ? m->body : QString();
        QString pre = QStringLiteral("<pre style=\"white-space: pre-wrap; word-wrap: break-word; color:#1e1e1e; font-family: 'Microsoft YaHei', sans-serif; font-size: 14px; line-height: 1.6;\">") + esc(bodyText) + QStringLiteral("</pre>");
        m_previewView->setHtml(QStringLiteral("<html><body style=\"background-color: transparent; color: #1e1e1e;\">") + signedBanner + pre + QStringLiteral("</body></html>"));
        m_bodyStack->setCurrentIndex(0);
        return;
    }
    // 有 htmlBody：解析 rawSource 中的 cid 内联图片，替换为 data:base64
    // signed 模式下 cid 图片要从原 MIME part 里取，不是 multipart/signed 顶层
    const QHash<QString, QByteArray> cidImages = parseCidImages(
        signedOriginalMime.isEmpty() ? m->rawSource : signedOriginalMime);
    m_previewView->setCidImages(cidImages);
    QString html = m->htmlBody;
    const int htmlLenBefore = html.size();
    int replacedCid = 0;
    if (!cidImages.isEmpty()) {
        static const QRegularExpression cidUrlRe(
            QStringLiteral("src=([\"'])cid:([^\\\"'\\s>]+)\\1"),
            QRegularExpression::CaseInsensitiveOption);
        int from = 0;
        int anonIdx = 0;
        while (from < html.size()) {
            const auto m2 = cidUrlRe.match(html, from);
            if (!m2.hasMatch()) break;
            QString key = m2.captured(2).toLower();
            if (key.startsWith('<') && key.endsWith('>'))
                key = key.mid(1, key.size() - 2);
            QByteArray bytes = cidImages.value(key);
            if (bytes.isEmpty()) {
                const QString anonKey = QString("__anon_%1__").arg(anonIdx);
                bytes = cidImages.value(anonKey);
                if (!bytes.isEmpty()) ++anonIdx;
            }
            if (!bytes.isEmpty()) {
                const auto head = bytes.left(16);
                QString mime = "image/png";
                if (head.startsWith("\x89PNG\r\n\x1a\n"))        mime = "image/png";
                else if (head.startsWith("\xff\xd8\xff"))         mime = "image/jpeg";
                else if (head.startsWith("GIF87a") || head.startsWith("GIF89a")) mime = "image/gif";
                else if (head.size() >= 12 && head.startsWith("RIFF") && head.mid(8, 4) == "WEBP") mime = "image/webp";
                else if (head.startsWith("BM"))                   mime = "image/bmp";
                const QString repl = QStringLiteral("src=%1data:%2;base64,%3%1")
                                          .arg(m2.captured(1), mime, QString::fromLatin1(bytes.toBase64()));
                html.replace(m2.capturedStart(), m2.capturedLength(), repl);
                ++replacedCid;
                from = m2.capturedStart() + repl.size();
            } else {
                from = m2.capturedStart() + m2.capturedLength();
            }
        }
    }
    if (!html.contains(QStringLiteral("<body"), Qt::CaseInsensitive)) {
        // 没 body：自动包装。background-color: transparent 让 widget 深色背景透出
        html = QStringLiteral("<html><body style=\"background-color: transparent; color:#1e1e1e; font-size:14px;\">") + html + QStringLiteral("</body></html>");
    } else {
        // 已有 <body>：用正则确保 background-color: transparent（不覆盖用户显式设的背景）
        static const QRegularExpression bodyTagRe(
            QStringLiteral("<body\\b([^>]*)>"),
            QRegularExpression::CaseInsensitiveOption);
        auto bm = bodyTagRe.match(html);
        if (bm.hasMatch() && !bm.captured(1).contains(QStringLiteral("background"), Qt::CaseInsensitive)) {
            const QString attrs = bm.captured(1);
            QString newTag;
            if (attrs.contains(QStringLiteral("style="), Qt::CaseInsensitive)) {
                // 已有 style：在末尾追加 background-color: transparent;
                QString a = attrs;
                static const QRegularExpression styleRe(
                    QStringLiteral("style\\s*=\\s*\"([^\"]*)\""),
                    QRegularExpression::CaseInsensitiveOption);
                auto sm = styleRe.match(a);
                if (sm.hasMatch()) {
                    QString s = sm.captured(1).trimmed();
                    if (!s.isEmpty() && !s.endsWith(';')) s.append(';');
                    s.append(QStringLiteral("background-color: transparent;"));
                    a.replace(sm.capturedStart(), sm.capturedLength(),
                              QStringLiteral("style=\"%1\"").arg(s));
                    newTag = QStringLiteral("<body") + a + QStringLiteral(">");
                }
            } else {
                // 无 style 属性：插入一个
                newTag = QStringLiteral("<body style=\"background-color: transparent;\"") + attrs + QStringLiteral(">");
            }
            if (!newTag.isEmpty()) {
                html.replace(bm.capturedStart(), bm.capturedLength(), newTag);
            }
        }
        if (!html.contains(QStringLiteral("<html"), Qt::CaseInsensitive))
            html = QStringLiteral("<html>") + html + QStringLiteral("</html>");
    }
    m_previewView->setHtml(signedBanner + html);
    m_bodyStack->setCurrentIndex(0);
    Logger::instance().info(
        QString("renderBody: msgId=%1 htmlLen=%2→%3 cidKeys=%4 replaced=%5")
            .arg(m->id).arg(htmlLenBefore).arg(html.size())
            .arg(cidImages.size()).arg(replacedCid),
        "mail");
}

void MailContentPanel::showMessage(const QString& msgId) {
    m_currentMsgId = msgId;
    if (msgId.isEmpty()) { clearPreview(); return; }
    auto* m = MailStore::instance().message(msgId);
    if (!m) { clearPreview(); return; }
    renderHeader(m);
    renderAttachments(m);
    renderBody(m);
}

// 草稿预览：复用 m_previewView 显示正文 html（富文本优先，没有再退到纯文本）；
// 邮件头部字段用草稿字段填充（发件人栏空，日期 = updatedAt，收件人 = toList）；
// 不渲染附件（草稿编辑态附件由编辑器里的 chip 区域负责）
void MailContentPanel::showDraft(const QString& draftId) {
    m_currentMsgId = QStringLiteral("draft:") + draftId;   // 用前缀区分邮件 vs 草稿
    if (draftId.isEmpty()) { clearPreview(); return; }
    auto drafts = MailStore::instance().drafts();
    const MailStore::Draft* d = nullptr;
    for (const auto& x : drafts) if (x.id == draftId) { d = &x; break; }
    if (!d) { clearPreview(); return; }

    // 头部：复用现有字段，让草稿预览跟邮件预览风格统一
    if (m_headerWidget) m_headerWidget->show();
    m_subjectLabel->setText(d->subject.isEmpty()
        ? QStringLiteral("(无主题草稿)") : d->subject);
    m_fromValueLabel->setText(QStringLiteral("(草稿，未发送)"));
    m_dateLabel->setText(d->updatedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    QStringList toJoined = d->to;
    setAddressText(m_toValueLabel, m_toExpandBtn, toJoined, m_currentMsgId + QStringLiteral("|to"));
    // 抄送/密送：草稿只有 to，所以隐藏 cc 行
    if (m_ccRowWrap)   m_ccRowWrap->hide();
    if (m_ccFieldLabel) m_ccFieldLabel->hide();

    // 附件：草稿里的附件是源文件路径列表，预览区不渲染（编辑器自带 chip）
    if (m_attachWidget) m_attachWidget->hide();
    for (auto* w : m_attRows) { w->deleteLater(); }
    m_attRows.clear();

    // 正文：富文本优先，回退纯文本
    m_showingRaw = false;
    QString html;
    if (!d->htmlBody.isEmpty()) {
        html = d->htmlBody;
    } else {
        QString esc = d->body.toHtmlEscaped();
        esc.replace(QStringLiteral("\n"), QStringLiteral("<br/>"));
        html = QStringLiteral("<div style=\"white-space:normal;\">%1</div>").arg(esc);
    }
    if (m_previewView) m_previewView->setHtml(html);
    if (m_bodyStack)   m_bodyStack->setCurrentIndex(0);
}

// 切换到附件内联预览面板（MailWidget 在附件下载成功后调用）
void MailContentPanel::showAttachmentPreview(const QString& msgId, int idx,
                                             const QString& savedPath, const QString& mimeType) {
    Q_UNUSED(idx);
    if (!m_attachPreview || !m_bodyStack) return;
    m_attachPreviewKey = msgId + QLatin1Char('|') + QString::number(idx);
    m_attachPreview->show(savedPath, mimeType);
    m_bodyStack->setCurrentWidget(m_attachPreview);
}

void MailContentPanel::showRawView(const QString& msgId) {
    if (!m_rawView || !m_bodyStack) return;
    m_currentMsgId = msgId;
    if (msgId.isEmpty()) { m_rawView->clear(); return; }
    auto* m = MailStore::instance().message(msgId);
    if (!m) { m_rawView->clear(); return; }

    m_showingRaw = true;

    if (!m->rawSource.isEmpty()) {
        QString rawText = QString::fromUtf8(m->rawSource);
        if (rawText.size() > 200 * 1024) {
            rawText = rawText.left(200 * 1024);
            rawText += QStringLiteral("\n\n... (剩余 ") +
                QString::number((m->rawSource.size() - 200 * 1024) / 1024) +
                QStringLiteral(" KB 已截断，仅显示前 200 KB)\n");
        }
        m_rawView->setPlainText(rawText);
        m_bodyStack->setCurrentIndex(2);
        return;
    }

    if (m->imapUid.isEmpty()) {
        m_rawView->setPlainText(QStringLiteral("(该邮件无 imapUid，无法拉取原件)"));
        m_bodyStack->setCurrentIndex(2);
        return;
    }
    auto* acc = MailAccountManager::instance().getById(m->accountId);
    if (!acc) { m_rawView->setPlainText(QStringLiteral("(账号不存在)")); m_bodyStack->setCurrentIndex(2); return; }

    QString uid = m->imapUid, folder = m->folder;
    ImapClient::Config cfg;
    cfg.host     = acc->imapHost;
    cfg.port     = acc->imapPort;
    cfg.ssl      = acc->imapSsl;
    cfg.username = acc->email;
    cfg.password = acc->password;
    cfg.timeoutSec = 30;

    m_rawView->setPlainText(QStringLiteral("(正在拉取原始 MIME 原文…)"));
    m_bodyStack->setCurrentIndex(2);
    QThread* t = QThread::create([this, cfg, folder, uid, msgId]() {
        QByteArray raw; QString err;
        if (ImapClient::fetchRawSource(cfg, folder, uid, &raw, &err)) {
            // MailStore 驻留主线程且非线程安全：回主线程写入后再刷新预览，
            // 避免与主线程的存储读写产生数据竞争
            QMetaObject::invokeMethod(this, [this, msgId, raw]() {
                MailStore::instance().updateRawSource(msgId, raw);
                showRawView(msgId);
            }, Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(this, [this, err]{ m_rawView->setPlainText(QStringLiteral("(原件拉取失败: ") + err + QStringLiteral(")")); }, Qt::QueuedConnection);
        }
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void MailContentPanel::onAnchorClicked(const QUrl& url) {
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto")) {
        QDesktopServices::openUrl(url);
    }
}

QString MailContentPanel::buildWelcomeHtml() const {
    // 默认占位卡片：图标 + 标题 + 副标题，参照 MailWidget emptyCard 风格
    // 用 table 居中 + 圆角背景模拟卡片（QTextBrowser 对 div/card 支持有限）
    return QStringLiteral(
        "<html><body style=\"background-color: transparent; color: %1;\">"
        "<table width=\"100%\" height=\"100%\" border=\"0\" cellpadding=\"0\" cellspacing=\"0\">"
        "<tr><td align=\"center\" valign=\"middle\">"
        "<div style=\"display:inline-block; min-width:380px; max-width:520px; "
                   "background-color:#2a2a2a; border:1px solid #3a3a3a; border-radius:12px; "
                   "padding:36px 40px; text-align:center;\">"
        "<div style=\"font-size:64px; color:%2; line-height:1.0; margin-bottom:14px;\">[Inbox]</div>"
        "<div style=\"font-size:20px; font-weight:bold; color:%3; margin-bottom:14px;\">选择邮件查看内容</div>"
        "<div style=\"font-size:13px; color:%4; line-height:1.6;\">"
        "从左侧邮件列表选择一封邮件，正文会显示在这里。<br>"
        "支持 HTML 富文本、内嵌图片 (cid:)、附件预览与原始 MIME 原文查看。"
        "</div>"
        "</div>"
        "</td></tr>"
        "</table>"
        "</body></html>"
    ).arg(Theme::kText, Theme::kAccent, Theme::kTextBright, Theme::kFaint);
}