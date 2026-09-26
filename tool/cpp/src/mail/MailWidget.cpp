#include "mail/MailWidget.h"
#include "mail/MailAccountManager.h"
#include "mail/MailStore.h"
#include "mail/TemplateStore.h"
#include "mail/MailPoller.h"
#include "mail/AccountDialog.h"
#include "mail/SmtpClient.h"
#include "mail/ScheduledQueue.h"
#include "mail/ui/MarkdownBridge.h"
#include "mail/ImapClient.h"
#include "mail/crypto/MailEncryptor.h"
#include "mail/ui/CertManagerDialog.h"
#include "mail/ui/folders/MailFolderPanel.h"
#include "mail/ui/list/MailListPanel.h"
#include "mail/ui/content/MailContentPanel.h"
#include "mail/ui/content/FlowLayout.h"
#include "app/Theme.h"
#include "core/Settings.h"
#include "core/Logger.h"

#include <memory>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QTabBar>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTextDocument>
#include <QPushButton>
#include <QToolButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAction>
#include <QActionGroup>
#include <QFontComboBox>
#include <QComboBox>
#include <QTime>
#include <QDateTimeEdit>
#include <QCheckBox>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QApplication>
#include <QMap>
#include <QMenu>
#include <QSignalMapper>
#include <QFormLayout>
#include <QDateTime>
#include <QSet>
#include <QMimeData>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QThread>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMimeDatabase>
#include <QClipboard>
#include <QUrl>
#include <QUuid>
#include <QColorDialog>
#include <QTextList>
#include <algorithm>

namespace {
// 从原始 MIME 字节里解析 Message-ID 头（角括号包裹），去掉外层尖括号。
// 用于 ENVELOPE 没返回 message-id 时回填。
QString extractMessageIdFromRaw(const QByteArray& raw) {
    if (raw.isEmpty()) return QString();
    static const QRegularExpression re(
        QString("^Message-ID:[ \\t]*<([^>\\r\\n]+)>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    auto m = re.match(QString::fromLatin1(raw));
    if (!m.hasMatch()) {
        // 兼容无尖括号的裸地址形式（极少）
        static const QRegularExpression re2(
            QString("^Message-ID:[ \\t]*([^<\\s][^\\r\\n]*)"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
        m = re2.match(QString::fromLatin1(raw));
        if (!m.hasMatch()) return QString();
        return m.captured(1).trimmed();
    }
    return m.captured(1).trimmed();
}

// ImapClient::Folder 列表 ↔ QVariantList（账号 JSON 持久化用）
QVariantList foldersToVariantList(const QList<ImapClient::Folder>& folders) {
    QVariantList out;
    for (const auto& f : folders) {
        QVariantMap m;
        m["name"]      = f.name;
        m["delimiter"] = f.delimiter;
        m["flags"]     = f.flags;
        out.append(m);
    }
    return out;
}

QList<ImapClient::Folder> foldersFromVariantList(const QVariantList& list) {
    QList<ImapClient::Folder> out;
    for (const auto& v : list) {
        const QVariantMap m = v.toMap();
        ImapClient::Folder f;
        f.name      = m.value("name").toString();
        f.delimiter = m.value("delimiter").toString();
        f.flags     = m.value("flags").toString();
        if (!f.name.isEmpty()) out.append(f);
    }
    return out;
}
} // namespace

MailWidget::~MailWidget() {
    // 兜底：带未关闭编辑器 tab 退出时，EditorPage 是纯数据 struct（非 QObject），
    // 不会随对象树回收，这里补齐释放；panel 等 QWidget 仍由对象树负责
    qDeleteAll(m_editorPages);
    m_editorPages.clear();
}

MailWidget::MailWidget(QWidget* parent) : QWidget(parent) {
    setObjectName("MailWidget");
    // 全局扁平化按钮 QSS（覆盖未单独 setStyleSheet 的按钮）：
    // 无圆角、无渐变、纯色填充 + hover/pressed 颜色阶梯
    setStyleSheet(
        "QPushButton {"
        "  background: #252830; color: #c8c8c8; border: none;"
        "  padding: 5px 12px; font-size: 13px;"
        "}"
        "QPushButton:hover    { background: #3a3d46; }"
        "QPushButton:pressed  { background: #4a4d56; }"
        "QPushButton:disabled { background: #1e2128; color: #555555; }"
        "QPushButton:default  { background: #4fc3f7; color: #0d1116; font-weight: 600; }"
        "QPushButton:default:hover    { background: #6ed1fb; }"
        "QPushButton:default:pressed  { background: #3ba9dd; }"
        "QPushButton:menu-indicator { image: none; width: 0; }"
    );
    setupUI();
    connectPanels();
    // 清理孤儿邮件（accountId 对应账号已被删除的缓存，如自测遗留），
    // 避免脏数据长期残留在 messages.json
    {
        QStringList validIds;
        for (const auto& a : MailAccountManager::instance().accounts())
            validIds.append(a.id);
        MailStore::instance().removeOrphanMessages(validIds);
    }
    refreshAccounts();
    connect(&MailAccountManager::instance(), &MailAccountManager::accountsChanged,
            this, &MailWidget::refreshAccounts);
    connect(&MailStore::instance(), &MailStore::messagesChanged,
            this, [this]{ scheduleRefreshList(true); });   // 防抖：正文逐封回写时合并高频触发
    // rawSource 更新专用信号:不刷新整个列表,只在预览区显示新原件
    connect(&MailStore::instance(), &MailStore::rawSourceUpdated,
            this, [this](const QString& id){
                if (m_contentPanel) m_contentPanel->refreshRawIfCurrent(id);
            });
    // 正文到达专用信号:不刷新整个列表,只在预览区把这封邮件的正文渲染出来
    connect(&MailStore::instance(), &MailStore::bodyUpdated,
            this, [this](const QString& id){
                if (selectedMessageId() == id) refreshPreview();
            });
    // DSN 请求（rawSource 写入后检测到 Disposition-Notification-To）：弹窗询问用户
    connect(&MailStore::instance(), &MailStore::readReceiptRequested,
            this, &MailWidget::onReadReceiptRequested);
    connect(&MailStore::instance(), &MailStore::draftsChanged,
            this, [this]{ if (currentFolder() == "Drafts") refreshMessageList(); });
    connect(&MailPoller::instance(), &MailPoller::notify,
            this, [this](const QString& name, const QString& subj, const QString&){
                if (m_statusLabel)
                    m_statusLabel->setText(QString("新邮件: %1 - %2").arg(name, subj));
            });

    // 监听顶层窗口激活事件：从其他窗口切回自动刷新当前账号
    if (auto* tlw = window()) {
        tlw->installEventFilter(this);
    }
    MailPoller::instance().start();
}

bool MailWidget::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == window() && ev->type() == QEvent::WindowActivate) {
        // 窗口被激活时，自动刷新当前选中的账号（避免重复则在 worker 已运行时跳过）
        QString accId = currentAccountId();
        if (!accId.isEmpty()) {
            Logger::instance().info(
                QString("窗口激活 → 自动刷新当前账号 %1").arg(accId), "mail");
            MailPoller::instance().pollAccount(accId);
        }
    }
    // 编辑器正文 QTextEdit 接受拖拽：图片 → 自动 inline 插入；非图片 → 加入附件列表
    if (ev->type() == QEvent::DragEnter || ev->type() == QEvent::Drop) {
        EditorPage* p = nullptr;
        for (auto* ep : m_editorPages) {
            if (ep->bodyEdit == obj || ep->mdEdit == obj) { p = ep; break; }
        }
        if (p) {
            auto* mime = (ev->type() == QEvent::DragEnter)
                ? static_cast<QDragEnterEvent*>(ev)->mimeData()
                : static_cast<QDropEvent*>(ev)->mimeData();
            if (mime && mime->hasUrls()) {
                if (ev->type() == QEvent::DragEnter) {
                    static_cast<QDragEnterEvent*>(ev)->acceptProposedAction();
                    return true;
                }
                // Drop
                auto* drop = static_cast<QDropEvent*>(ev);
                drop->acceptProposedAction();
                for (const QUrl& u : mime->urls()) {
                    const QString local = u.toLocalFile();
                    if (local.isEmpty() || !QFile::exists(local)) continue;
                    QFileInfo fi(local);
                    QString mimeType = QMimeDatabase().mimeTypeForFile(local).name();
                    // 图片：inline 插入
                    if (mimeType.startsWith(QStringLiteral("image/"))) {
                        EditorAttachment a;
                        a.filePath    = local;
                        a.displayName = fi.fileName();
                        a.sizeBytes   = fi.size();
                        a.mimeType    = mimeType;
                        a.contentId   = QUuid::createUuid().toString(QUuid::WithoutBraces);
                        a.insertedInline = true;
                        p->attachments.append(a);
                        if (p->markdownMode && p->mdEdit) {
                            // Markdown 模式：插入 ![alt](path)（收件方收到源码 + cid 转换在 SmtpClient 处理）
                            p->mdEdit->insertPlainText(
                                QStringLiteral("\n![%1](%2)\n")
                                    .arg(a.displayName, local));
                        } else if (p->bodyEdit) {
                            p->bodyEdit->insertHtml(
                                QStringLiteral("<img src=\"cid:%1\" alt=\"%2\" />")
                                    .arg(a.contentId, a.displayName.toHtmlEscaped()));
                        }
                        Logger::instance().info(
                            QStringLiteral("拖拽插入正文: %1").arg(local), "mail");
                    } else {
                        // 非图片：加入附件列表
                        bool exists = false;
                        for (const auto& a : p->attachments) if (a.filePath == local) { exists = true; break; }
                        if (!exists) {
                            EditorAttachment a;
                            a.filePath    = local;
                            a.displayName = fi.fileName();
                            a.sizeBytes   = fi.size();
                            a.mimeType    = mimeType;
                            p->attachments.append(a);
                            Logger::instance().info(
                                QStringLiteral("拖拽加入附件: %1").arg(local), "mail");
                        }
                    }
                }
                refreshAttachmentTable(p);
                return true;
            }
        }
    }
    // 附件表格拖拽重排：Qt InternalMove 只重排 UI 不重排数据；Drop 后我们重新
    // 按表格行序同步 p->attachments。
    if (ev->type() == QEvent::Drop && qobject_cast<QTableWidget*>(obj)) {
        auto* tw = static_cast<QTableWidget*>(obj);
        if (tw->dragDropMode() != QAbstractItemView::InternalMove) {
            return QWidget::eventFilter(obj, ev);
        }
        // 找对应 EditorPage
        EditorPage* owner = nullptr;
        for (auto* ep : m_editorPages) {
            if (ep->attachTable == tw) { owner = ep; break; }
        }
        if (!owner) return QWidget::eventFilter(obj, ev);

        // 记录当前顺序（按 UserRole 推断"原始索引"）
        QList<int> beforeOrder;
        for (int r = 0; r < tw->rowCount(); ++r) {
            auto* it = tw->item(r, 0);
            if (!it) continue;
            beforeOrder << it->data(Qt::UserRole).toInt();
        }
        // 让 QTableWidget 先完成默认的拖拽处理
        QWidget::eventFilter(obj, ev);
        // 然后按当前表格行顺序重排 p->attachments
        QList<int> afterOrder;
        for (int r = 0; r < tw->rowCount(); ++r) {
            auto* it = tw->item(r, 0);
            if (!it) continue;
            afterOrder << it->data(Qt::UserRole).toInt();
        }
        // 顺序确实变了才同步
        if (afterOrder != beforeOrder && afterOrder.size() == owner->attachments.size()) {
            QList<EditorAttachment> synced;
            synced.reserve(owner->attachments.size());
            for (int idx : afterOrder) {
                if (idx >= 0 && idx < owner->attachments.size())
                    synced.append(owner->attachments[idx]);
            }
            if (synced.size() == owner->attachments.size()) {
                owner->attachments = synced;
                Logger::instance().info(
                    QStringLiteral("附件拖拽重排: %1 项").arg(synced.size()), "mail");
                // 重新刷新表格（让 UserRole 反映新的"原始索引"）
                refreshAttachmentTable(owner);
            }
        }
        return true;
    }
    return QWidget::eventFilter(obj, ev);
}

void MailWidget::setupUI() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── 主堆叠：page 0 空账号引导；page 1 正常工作区 ──
    m_mainStack = new QStackedWidget(this);
    root->addWidget(m_mainStack, 1);

    // ── 空账号引导页 ──
    m_emptyPage = new QWidget;
    auto* emptyL = new QVBoxLayout(m_emptyPage);
    emptyL->setContentsMargins(0, 0, 0, 0);
    emptyL->setAlignment(Qt::AlignCenter);
    auto* emptyCard = new QFrame;
    emptyCard->setObjectName("mailEmptyCard");
    emptyCard->setFrameShape(QFrame::StyledPanel);
    emptyCard->setMinimumWidth(420);
    emptyCard->setMaximumWidth(560);
    emptyCard->setStyleSheet(
        "#mailEmptyCard { background: #2a2a2a; border: 1px solid #3a3a3a; border-radius: 12px; }");
    auto* cardL = new QVBoxLayout(emptyCard);
    cardL->setContentsMargins(40, 36, 40, 36);
    cardL->setSpacing(14);
    cardL->setAlignment(Qt::AlignHCenter);
    auto* bigIcon = new QLabel("\xE2\x9C\x89");  // ✉
    bigIcon->setAlignment(Qt::AlignCenter);
    bigIcon->setStyleSheet("font-size: 64px; color: #4fc3f7;");
    cardL->addWidget(bigIcon);
    auto* titleLbl = new QLabel("尚未配置邮箱账号");
    titleLbl->setAlignment(Qt::AlignCenter);
    titleLbl->setStyleSheet("font-size: 20px; font-weight: bold; color: #e0e0e0;");
    cardL->addWidget(titleLbl);
    auto* subLbl = new QLabel(
        "配置邮箱后可在此接收、发送、回复、转发邮件，并支持多账号与新邮件提醒。\n"
        "支持 QQ / 163 / Gmail / Outlook 等常见邮箱，输入域名自动识别服务器。");
    subLbl->setAlignment(Qt::AlignCenter);
    subLbl->setWordWrap(true);
    subLbl->setStyleSheet("color: #9e9e9e; font-size: 13px; line-height: 1.6;");
    cardL->addWidget(subLbl);
    auto* addBtn = new QPushButton("+  新增邮箱账号");
    addBtn->setCursor(Qt::PointingHandCursor);
    addBtn->setMinimumHeight(40);
    addBtn->setMinimumWidth(180);
    addBtn->setStyleSheet(Theme::flatBtnPrimary() +
        " QPushButton { padding: 8px 20px; font-size: 14px; }");
    connect(addBtn, &QPushButton::clicked, this, &MailWidget::onEmptyStateAddClicked);
    cardL->addWidget(addBtn, 0, Qt::AlignHCenter);
    emptyL->addWidget(emptyCard, 0, Qt::AlignCenter);
    m_mainStack->addWidget(m_emptyPage);

    // ── 工作页（包含三栏 splitter + 底部状态栏）──
    // 账号操作（添加/编辑/删除/设为默认/刷新）统一通过右键文件夹树弹出
    m_workPage = new QWidget;
    auto* workRoot = new QVBoxLayout(m_workPage);
    workRoot->setContentsMargins(0, 0, 0, 0);      // tab 行与三栏内容全部零边距贴边
    workRoot->setSpacing(10);

    // 顶部 tab 行：tab 0 = 邮件浏览（不可关闭）；tab 1..N = 编辑器（每封邮件一个，带「×」）
    m_filterTabs = new QTabBar;
    m_filterTabs->setDrawBase(false);
    m_filterTabs->setDocumentMode(true);
    m_filterTabs->setExpanding(false);
    m_filterTabs->setElideMode(Qt::ElideRight);       // tab 标题过长省略号
    m_filterTabs->setUsesScrollButtons(true);         // tab 多时出滚动箭头
    m_filterTabs->setTabsClosable(true);
    connect(m_filterTabs, &QTabBar::tabCloseRequested,
            this, &MailWidget::onEditorTabCloseRequested);
    // 单个 tab 文字 = 当前选中文件夹的友好名；默认显示"收件箱"（启动后 INBOX 默认选中）
    m_filterTabs->addTab("\xe6\x94\xb6\xe4\xbb\xb6\xe7\xae\xb1");   // 收件箱
    m_filterTabs->setTabButton(0, QTabBar::RightSide, nullptr);    // 浏览页隐藏「×」
    m_filterTabs->setStyleSheet(
        QString("QTabBar::tab { background: %1; color: %2; padding: 4px 10px;"
                "  border: 1px solid %3; border-bottom: none; border-top-left-radius: 4px;"
                "  border-top-right-radius: 4px; font-size: 12px; }"
                "QTabBar::tab:selected { background: %4; color: %5;"
                "  border: 1px solid %5; border-bottom: none; }"
                "QTabBar::tab:hover { background: %6; border-color: %7; }")
        .arg(Theme::kSurfaceAlt, Theme::kMuted, Theme::kBorderLight,
             Theme::kBg, Theme::kAccent, Theme::kSurface, Theme::kBorderHover));
    connect(m_filterTabs, &QTabBar::currentChanged, this, &MailWidget::onFilterTabChanged);
    // tab 行在 splitter 之上，左/中/右三栏同处于该 tab 下
    workRoot->addWidget(m_filterTabs);

    // ── 三栏：邮件文件夹 | 邮件列表 | 邮件内容 ──
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    m_folderPanel  = new MailFolderPanel;
    m_listPanel    = new MailListPanel;
    m_contentPanel = new MailContentPanel;
    m_contentPanel->setMinimumWidth(380);

    splitter->addWidget(m_folderPanel);
    splitter->addWidget(m_listPanel);
    splitter->addWidget(m_contentPanel);
    // 三栏比例 左:中:右 = 1:1:3；左栏（文件夹树）stretch=0 不随窗口拉伸，
    // 初始宽度压到最小值（相当于手动拖到最左），空间让给列表/内容栏
    splitter->setStretchFactor(0,0); splitter->setStretchFactor(1,1); splitter->setStretchFactor(2,3);
    QTimer::singleShot(0, this, [this, splitter] {
        const int minW = m_folderPanel->minimumSizeHint().width();
        const int rest = qMax(splitter->width() - minW, 0);
        splitter->setSizes({minW, rest / 4, rest - rest / 4});
    });

    // 顶层内容堆叠：page 0 = 三栏浏览（splitter），page 1 = 编辑器 stack（多 tab）
    m_editorStack = new QStackedWidget;
    m_contentStack = new QStackedWidget;
    m_contentStack->addWidget(splitter);
    m_contentStack->addWidget(m_editorStack);
    workRoot->addWidget(m_contentStack, 1);

    // 底部状态条已去除（用户不再需要常驻状态提示；m_statusLabel 保留为孤儿对象以兼容既有 setText 调用）
    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet(Theme::mutedText());

    m_mainStack->addWidget(m_workPage);
}

// 三个面板的信号 → 本类槽/业务逻辑
void MailWidget::connectPanels() {
    // ── 邮件文件夹（左栏）──
    connect(m_folderPanel, &MailFolderPanel::selectionChanged,
            this, &MailWidget::onFolderChanged);
    connect(m_folderPanel, &MailFolderPanel::searchChanged,
            this, [this]{ scheduleRefreshList(); });
    // "写邮件"按钮（搜索框上方）→ 新建编辑器
    connect(m_folderPanel, &MailFolderPanel::newMailRequested,
            this, &MailWidget::onNewMailClicked);
    // "刷新"图标按钮（写邮件按钮右侧）→ 全量刷新当前账号
    connect(m_folderPanel, &MailFolderPanel::refreshRequested,
            this, &MailWidget::onRefreshClicked);
    connect(m_folderPanel, &MailFolderPanel::addAccountRequested,
            this, &MailWidget::onNewAccountClicked);
    connect(m_folderPanel, &MailFolderPanel::editAccountRequested,
            this, &MailWidget::editAccount);
    connect(m_folderPanel, &MailFolderPanel::deleteAccountRequested,
            this, &MailWidget::deleteAccount);
    connect(m_folderPanel, &MailFolderPanel::setDefaultRequested,
            this, &MailWidget::setDefaultAccount);
    // 右键"刷新"：文件夹行 → 仅同步该文件夹（弹窗显示进度）
    connect(m_folderPanel, &MailFolderPanel::refreshFolderRequested,
            this, [this](const QString& accId, const QString& folderKey){
                showSyncTip(QString("%1【正在获取邮件总数 ...】")
                                .arg(MailFolderPanel::displayNameForKey(folderKey)));
                m_folderSyncTotal = 0;   // 单文件夹模式：完成即撤提示
                m_folderSyncDone  = 0;
                fetchFolderHeaders(accId, folderKey);
            });
    // 右键"刷新"：账号行 → 全量刷新（先选中该账号，同步文件夹树与邮件列表）
    connect(m_folderPanel, &MailFolderPanel::refreshAccountRequested,
            this, [this](const QString& accId){
                selectAccount(accId);
                onRefreshClicked();
            });

    // ── 邮件列表（中栏）──
    // "写邮件"按钮已迁移到 MailFolderPanel（搜索框上方）
    connect(m_listPanel, &MailListPanel::deleteRequested,
            this, &MailWidget::onDeleteMailClicked);
    connect(m_listPanel, &MailListPanel::syncRequested,
            this, &MailWidget::onRefreshClicked);
    connect(m_listPanel, &MailListPanel::selectionChanged,
            this, &MailWidget::onMessageItemSelectionChanged);
    connect(m_listPanel, &MailListPanel::itemDoubleClicked,
            this, [this](const QString& key){
                // 双击草稿 → 打开编辑器
                if (currentFolder() != "Drafts") return;
                if (key.startsWith("draft:")) openDraftInEditor(key.mid(6));
            });
    connect(m_listPanel, &MailListPanel::contextMenuRequested,
            this, &MailWidget::onMessageContextMenu);
    connect(m_listPanel, &MailListPanel::rowClicked, this, [this]{
        // 列表点选时若当前在编辑器页 → 切回预览页（保留防御）
        if (m_contentStack && m_contentStack->currentIndex() == 1)
            showPreviewPage();
        // 左键点击预览 → 若该邮件未读则后台改为已读（本地 + IMAP 同步）；
        // 右键 setCurrentRow 不触发 rowClicked，因此右键选中不会标已读。
        if (currentFolder() == "Drafts") return;
        QString mid = selectedMessageId();
        if (mid.isEmpty()) return;
        auto* m = MailStore::instance().message(mid);
        if (m && !m->read) setMessageRead(mid, true);
    });

    // ── 邮件内容（右栏）──
    connect(m_contentPanel, &MailContentPanel::replyRequested,
            this, &MailWidget::onReplyClicked);
    connect(m_contentPanel, &MailContentPanel::replyAllRequested,
            this, &MailWidget::onReplyAllClicked);
    connect(m_contentPanel, &MailContentPanel::forwardRequested,
            this, &MailWidget::onForwardClicked);
    connect(m_contentPanel, &MailContentPanel::forwardAsAttachmentRequested,
            this, &MailWidget::onForwardAsAttachmentClicked);
    connect(m_contentPanel, &MailContentPanel::forwardOriginalRequested,
            this, &MailWidget::onForwardOriginalClicked);
    connect(m_contentPanel, &MailContentPanel::reeditRequested,
            this, &MailWidget::onReeditClicked);
    connect(m_contentPanel, &MailContentPanel::resendRequested,
            this, &MailWidget::onResendClicked);
    connect(m_contentPanel, &MailContentPanel::revokeRequested,
            this, &MailWidget::onRevokeClicked);
    connect(m_contentPanel, &MailContentPanel::deleteRequested,
            this, &MailWidget::onDeleteMailClicked);
    connect(m_contentPanel, &MailContentPanel::attachmentClicked,
            this, &MailWidget::onAttachmentClicked);
    connect(m_contentPanel, &MailContentPanel::attachmentOpenRequested,
            this, &MailWidget::onAttachmentOpenRequested);
    connect(m_contentPanel, &MailContentPanel::attachmentRevealRequested,
            this, &MailWidget::onAttachmentRevealRequested);
    connect(m_contentPanel, &MailContentPanel::bodyLoadRequested,
            this, [this](const QString& msgId){
                if (auto* m = MailStore::instance().message(msgId))
                    requestBodyLoad(m);
            });
}

void MailWidget::refreshAccounts() {
    // 账号列表直接由文件夹树呈现（顶级 = 账号），无需下拉框
    // 先注入上次同步持久化的文件夹列表：启动零等待显示本机已加载的全部文件夹，
    // 随后的 IMAP LIST 完成后再覆盖刷新
    const auto& accounts = MailAccountManager::instance().accounts();
    for (const auto& a : accounts) {
        if (!m_folderPanel->hasRemoteFolders(a.id) && !a.cachedFolders.isEmpty())
            m_folderPanel->setRemoteFolders(a.id, foldersFromVariantList(a.cachedFolders));
    }
    refreshFolders();
    updateEmptyState();
    // 为未缓存文件夹列表的账号后台拉取真实文件夹
    for (const auto& a : accounts) {
        if (!m_folderPanel->hasRemoteFolders(a.id)) loadRemoteFolders(a.id);
    }
}

void MailWidget::loadRemoteFolders(const QString& accountId) {
    auto* acc = MailAccountManager::instance().getById(accountId);
    if (!acc) return;
    if (acc->recvProto == "SMTP" || acc->imapHost.isEmpty() || acc->password.isEmpty()) return;
    if (m_foldersLoading.contains(accountId)) return;
    m_foldersLoading.insert(accountId);
    ImapClient::Config cfg;
    cfg.host     = acc->imapHost;
    cfg.port     = acc->imapPort;
    cfg.ssl      = acc->imapSsl;
    cfg.username = acc->email;
    cfg.password = acc->password;
    cfg.timeoutSec = 15;

    QString accId = accountId;
    QThread* t = QThread::create([this, cfg, accId]() {
        QList<ImapClient::Folder> folders;
        QString err;
        bool ok = ImapClient::listFolders(cfg, &folders, &err);
        // 回主线程更新缓存并刷新文件夹树
        QMetaObject::invokeMethod(this, [this, ok, folders, err, accId]() {
            m_foldersLoading.remove(accId);
            if (ok) {
                m_folderPanel->setRemoteFolders(accId, folders);
                // 持久化到账号 JSON：下次启动无需等 IMAP LIST 即可显示全部文件夹
                MailAccountManager::instance().setCachedFolders(
                    accId, foldersToVariantList(folders));
                Logger::instance().info(
                    QString("文件夹列表已更新: acc=%1 共 %2 个").arg(accId).arg(folders.size()),
                    "mail");
                refreshFolders();
            } else {
                Logger::instance().warn(
                    QString("IMAP LIST 失败: acc=%1: %2").arg(accId, err), "mail");
            }
            // 刷新流程：所有账号文件夹同步完成后，再拉邮件
            if (m_pendingFolderSync > 0) {
                --m_pendingFolderSync;
                if (m_pendingFolderSync == 0) syncMailAfterFolders();
            }
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void MailWidget::updateEmptyState() {
    if (!m_mainStack) return;
    bool hasAccounts = !MailAccountManager::instance().accounts().isEmpty();
    m_mainStack->setCurrentIndex(hasAccounts ? 1 : 0);
}

void MailWidget::onEmptyStateAddClicked() {
    AccountDialog dlg("new", {}, this);
    if (dlg.exec() != QDialog::Accepted) return;
    MailAccountManager::instance().add(dlg.result());
    // 添加后 accountsChanged 信号会自动触发 refreshAccounts → updateEmptyState
    m_statusLabel->setText("账号已添加");
}

void MailWidget::onManageCertsClicked() {
    auto* dlg = new CertManagerDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MailWidget::refreshFolders() {
    if (m_folderPanel) m_folderPanel->rebuild();
}

QString MailWidget::currentAccountId() const {
    return m_folderPanel ? m_folderPanel->currentAccountId() : QString();
}
QString MailWidget::currentFolder() const {
    return m_folderPanel ? m_folderPanel->currentFolder() : QString();
}
QString MailWidget::selectedMessageId() const {
    return m_listPanel ? m_listPanel->selectedMessageId() : QString();
}

void MailWidget::refreshMessageList() {
    if (!m_listPanel) return;
    const QString folder = currentFolder();
    const QString keyword = m_folderPanel ? m_folderPanel->searchText() : QString();
    m_listPanel->refreshMessages(currentAccountId(), folder, keyword);
}

// 高频触发源（messagesChanged / 搜索输入）的防抖入口：
// 150ms 内连续触发合并为一次列表重建；withFolders 时到期连带刷新文件夹树
void MailWidget::scheduleRefreshList(bool withFolders) {
    if (!m_listDebounce) {
        m_listDebounce = new QTimer(this);
        m_listDebounce->setSingleShot(true);
        m_listDebounce->setInterval(150);
        connect(m_listDebounce, &QTimer::timeout, this, [this]{
            refreshMessageList();
            if (m_debounceFolders) {
                m_debounceFolders = false;
                refreshFolders();
            }
        });
    }
    if (withFolders) m_debounceFolders = true;
    m_listDebounce->start();
}

// ── PART3+：基于三个面板（folder/list/content）的 MailWidget 协作逻辑 ──
// 此处保留：账号管理（add/edit/delete/setDefault）、IMAP 同步
//（loadRemoteFolders/fetchFolderHeaders）、编辑器多 tab、草稿读写、邮件
//发送/删除、正文按需拉取、附件下载、状态栏日志、批量操作等。
// 旧实现中直接操作 m_folderTree/m_msgTable/m_previewStack/m_previewView/
//m_rawView/m_searchEdit/m_replyBtn/m_forwardBtn 的逻辑全部迁移到
// 三个面板内部（MailFolderPanel / MailListPanel / MailContentPanel），
// 本类仅消费它们发出的信号并通过公开接口回写。

void MailWidget::onFolderChanged() {
    if (!m_folderPanel) return;

    QString accId  = currentAccountId();
    QString folder = currentFolder();

    // 顶级节点（账号）点击：刷新列表 + 后台拉新邮件
    if (folder.isEmpty()) {
        if (!accId.isEmpty()) {
            Logger::instance().info(
                QString("切换账号 → 自动刷新 %1").arg(accId), "mail");
            MailPoller::instance().pollAccount(accId);
        }
        return;
    }

    // 子级节点 = 文件夹：刷新列表 + 按需触发 IMAP 拉取
    refreshMessageList();
    refreshPreview();

    if (!accId.isEmpty()) {
        if (folder == "INBOX") {
            // INBOX：后台新邮件提醒 + 全量列表（ENVELOPE 批量）
            Logger::instance().info(
                QString("切换文件夹 → 自动刷新 %1").arg(accId), "mail");
            MailPoller::instance().pollAccount(accId);
            fetchFolderHeaders(accId, folder);
        } else if (folder != "Drafts") {
            // Sent 及 IMAP 真实文件夹：拉取该文件夹的邮件头
            Logger::instance().info(
                QString("切换真实文件夹 → 拉取 %1 [%2]").arg(accId, folder), "mail");
            fetchFolderHeaders(accId, folder);
        }
    }
}

void MailWidget::fetchFolderHeaders(const QString& accountId,
                                    const QString& folder) {
    auto* acc = MailAccountManager::instance().getById(accountId);
    if (!acc) return;
    if (acc->imapHost.isEmpty() || acc->password.isEmpty()) return;
    QString key = accountId + "|" + folder;
    if (m_headersLoading.contains(key)) return;
    m_headersLoading.insert(key);

    // 本地固定 key → IMAP 服务器真实文件夹名：
    // "INBOX" 所有服务器通用；"Sent" 在 139 等服务器真实名是"已发送"
    // （modified UTF-7），SELECT "Sent" 会失败，需从 LIST 缓存解析真实名。
    QString storageKey = folder;   // MailStore 落盘 key（与 UI currentFolder 一致）
    QString imapName   = folder;   // IMAP SELECT 用的服务器名
    if (folder == "Sent") {
        const auto remote = m_folderPanel
            ? m_folderPanel->remoteFolders(accountId)
            : QList<ImapClient::Folder>{};
        for (const auto& rf : remote) {
            QString low = ImapClient::decodeFolderName(rf.name).toLower();
            if (rf.flags.contains("\\Sent", Qt::CaseInsensitive) ||
                low.contains("sent") || low.contains("已发送")) {
                imapName = rf.name;
                break;
            }
        }
    }

    ImapClient::Config cfg;
    cfg.host     = acc->imapHost;
    cfg.port     = acc->imapPort;
    cfg.ssl      = acc->imapSsl;
    cfg.username = acc->email;
    cfg.password = acc->password;
    cfg.timeoutSec = 60;   // 文件夹下可能多封邮件，批量 header 需要更长超时

    QString accId = accountId;
    QString fold  = imapName;
    if (m_statusLabel) m_statusLabel->setText(QString("正在拉取 %1 ...").arg(folder));
    QThread* t = QThread::create([this, cfg, accId, fold, storageKey, key]() {
        QList<ImapClient::FetchedMessage> fetched;
        QString err;
        bool ok = ImapClient::fetchHeaders(cfg, fold, 50, &fetched, &err);
        QMetaObject::invokeMethod(this, [this, ok, fetched, err, accId, storageKey, key]() {
            m_headersLoading.remove(key);
            if (!ok) {
                Logger::instance().warn(
                    QString("拉取文件夹失败: %1 [%2]: %3").arg(accId, storageKey, err), "mail");
                if (m_statusLabel) m_statusLabel->setText(QString("拉取失败: %1").arg(err));
                return;
            }
            QList<MailStore::Message> toUpsert;
            for (const auto& f : fetched) {
                MailStore::Message m;
                m.accountId = accId;
                m.folder    = storageKey;
                m.messageId = f.messageId;
                m.imapUid   = f.imapUid;
                m.from      = f.from;
                m.to        = f.to;
                m.cc        = f.cc;
                m.subject   = f.subject;
                m.date      = f.date;
                m.read      = f.seen;
                toUpsert.append(m);
            }
            if (!toUpsert.isEmpty()) MailStore::instance().upsertMessages(toUpsert);
            Logger::instance().info(
                QString("文件夹拉取完成: [%1] %2 封").arg(storageKey).arg(toUpsert.size()), "mail");
            if (m_statusLabel)
                m_statusLabel->setText(QString("%1: %2 封").arg(storageKey).arg(toUpsert.size()));
            refreshMessageList();
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void MailWidget::onFilterTabChanged(int idx) {
    // tab 0 = 邮件浏览（三栏）；tab 1..N = 编辑器（每封邮件一个）
    if (m_contentStack) m_contentStack->setCurrentIndex(idx == 0 ? 0 : 1);
    if (idx == 0) {
        refreshMessageList();
    } else if (m_editorStack && m_filterTabs) {
        if (auto* p = findEditorPage(m_filterTabs->tabData(idx).toString()))
            m_editorStack->setCurrentWidget(p->panel);
    }
}

void MailWidget::onMessageItemSelectionChanged() {
    if (!m_listPanel) return;
    bool hasSel = m_listPanel->hasSelection();
    QString folder = currentFolder();
    bool isMail  = !folder.isEmpty() && folder != "Drafts" && hasSel;
    // 列表选中变化 → 启用/禁用删除按钮（由 panel 内部维护）+ 触发预览
    if (m_contentPanel) m_contentPanel->setActionsEnabled(isMail);
    // "回复/回复全部/转发" 三个按钮：仅收件箱(INBOX)预览时显示；
    // 已发送/草稿/自定义文件夹/未读视图一律隐藏（只保留重发相关按钮）
    bool showReplyForward = hasSel && folder == "INBOX";
    if (m_contentPanel) m_contentPanel->setReplyForwardActionsVisible(showReplyForward);
    // "重新编辑"/"再次发送" 按钮：收件箱(INBOX)隐藏，其余文件夹（已发送/草稿/自定义/未读）且选中时显示
    bool showReSend = hasSel && !folder.isEmpty() && folder != "INBOX";
    if (m_contentPanel) m_contentPanel->setReSendActionsVisible(showReSend);
    // "撤销" 按钮：仅已发送(Sent)文件夹且选中时显示
    bool showRevoke = hasSel && folder == "Sent";
    if (m_contentPanel) m_contentPanel->setRevokeActionVisible(showRevoke);
    // "删除" 按钮：非收件箱文件夹及未读视图选中时显示（收件箱内隐藏，用列表勾选删除）
    bool showDelete = hasSel && !folder.isEmpty() && folder != "INBOX";
    if (m_contentPanel) m_contentPanel->setDeleteActionVisible(showDelete);
    if (m_listPanel) m_listPanel->setDeleteEnabled(hasSel);
    refreshPreview();
}

// 右键邮件列表行：在预览区显示 + 弹出"勾选/取消勾选"菜单
void MailWidget::onMessageContextMenu(const QPoint& pos) {
    if (!m_listPanel) return;
    int row = m_listPanel->rowAt(pos);
    if (row < 0) return;
    if (m_listPanel->isGroupRow(row)) return;     // 分组行

    // 先切回预览页，再选中行，保证随后的 refreshPreview 能取到刚选中的邮件
    showPreviewPage();
    m_listPanel->setCurrentRow(row);

    QString folder = currentFolder();
    QString key    = m_listPanel->keyAt(row);

    // 草稿：右键直接打开编辑器
    if (folder == "Drafts" && key.startsWith("draft:")) {
        openDraftInEditor(key.mid(6));
        return;
    }

    // 普通邮件：刷新预览（右键点击不再标记已读；已读状态仅在左键点击预览时更新）
    refreshPreview();
    QString id = selectedMessageId();
    if (id.isEmpty()) return;

    // 菜单项：勾选 / 取消勾选
    bool checked = m_listPanel->isChecked(row);
    QMenu menu;

    QAction* toggleAct = menu.addAction(
        checked ? QStringLiteral("\u2715  取消勾选") : QStringLiteral("\u2713  勾选"));

    // 在 table 视口全局坐标处弹出
    if (auto* tbl = m_listPanel->findChild<QTableWidget*>()) {
        QPoint gpos = tbl->viewport()->mapToGlobal(pos);
        QAction* chosen = menu.exec(gpos);
        if (chosen == toggleAct)
            m_listPanel->setChecked(row, !checked);
        // 附件项已通过 connect 自行处理
    }
}

void MailWidget::refreshPreview() {
    if (!m_contentPanel) return;
    // 草稿箱：列表键为 draft:<id>，直接展示草稿内容
    QString draftId = m_listPanel ? m_listPanel->selectedDraftId() : QString();
    if (!draftId.isEmpty()) { m_contentPanel->showDraft(draftId); return; }
    QString id = selectedMessageId();
    if (id.isEmpty()) { m_contentPanel->clearPreview(); return; }
    m_contentPanel->showMessage(id);
}

// 邮件已读/未读本地标记 + 后台同步服务器（已读 → markSeen；未读 → markUnseen）
void MailWidget::setMessageRead(const QString& msgId, bool read) {
    auto* m = MailStore::instance().message(msgId);
    if (!m) return;
    MailStore::instance().markRead(m->id, read);   // 本地立即生效（驱动列表未读态）
    const QString uid   = m->imapUid;
    const QString mFol  = m->folder;
    const QString accId = m->accountId;
    if (uid.isEmpty()) return;
    auto* acc = MailAccountManager::instance().getById(accId);
    if (!acc) return;
    ImapClient::Config cfg;
    cfg.host = acc->imapHost; cfg.port = acc->imapPort; cfg.ssl = acc->imapSsl;
    cfg.username = acc->email; cfg.password = acc->password;
    cfg.timeoutSec = 15;
    // 后台线程同步标志（与 UI 无耦合，直接起线程即可）
    QThread* t = QThread::create([cfg, mFol, uid, read]() {
        QString err;
        bool ok = read ? ImapClient::markSeen(cfg, mFol, uid, &err)
                       : ImapClient::markUnseen(cfg, mFol, uid, &err);
        if (!ok) {
            Logger::instance().warn(
                QString("IMAP 标记%1失败: uid=%2: %3")
                    .arg(read ? QStringLiteral("已读") : QStringLiteral("未读"))
                    .arg(uid).arg(err), "mail");
        }
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

// 单封按需拉正文（本地无 body/htmlBody/rawSource 时触发；防重复）
void MailWidget::requestBodyLoad(const MailStore::Message* m) {
    if (!m) return;
    if (!m->body.isEmpty() || !m->htmlBody.isEmpty() || !m->rawSource.isEmpty())
        return;   // 已有正文，无需重拉
    if (m->imapUid.isEmpty()) return;
    if (m_loadingBodyIds.contains(m->id)) return;
    m_loadingBodyIds.insert(m->id);

    auto* acc = MailAccountManager::instance().getById(m->accountId);
    if (!acc) { m_loadingBodyIds.remove(m->id); return; }
    ImapClient::Config cfg;
    cfg.host = acc->imapHost; cfg.port = acc->imapPort; cfg.ssl = acc->imapSsl;
    cfg.username = acc->email; cfg.password = acc->password;
    cfg.timeoutSec = 15;

    QString msgId = m->id;
    QString folder = m->folder;
    QString uid    = m->imapUid;

    // 进入"加载中"页：进度条 0 B，提示正在拉取
    if (m_contentPanel) m_contentPanel->setBodyProgress(msgId, 0, 0);

    // 节流进度回调（后台线程触发，节流后跨线程到 UI 线程）
    // 节流状态用 shared_ptr 共享：std::function 会被拷贝多份，需共享同一份状态；
    // 生命周期随最后一个拷贝自动结束，无需手动 delete
    auto lastEmitMs  = std::make_shared<qint64>(0);
    auto lastEmitExp = std::make_shared<qint64>(0);
    ImapClient::FetchProgressCb onProgress =
        [this, msgId, lastEmitMs, lastEmitExp](qint64 recv, qint64 exp) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            qint64 lastExp = *lastEmitExp;
            // 节流：80ms 内且 exp 未变 → 跳过；首帧/exp 变化/末尾 → 必发
            bool tail = exp > 0 && recv >= exp;
            if (!tail && lastExp == exp && (now - *lastEmitMs) < 80) return;
            *lastEmitMs = now;
            *lastEmitExp = exp;
            QMetaObject::invokeMethod(this, [this, msgId, recv, exp]{
                if (m_contentPanel) m_contentPanel->setBodyProgress(msgId, recv, exp);
            }, Qt::QueuedConnection);
        };

    QThread* t = QThread::create([this, cfg, msgId, folder, uid, onProgress]() {
        QString body, html; QByteArray raw; QString err;
        QList<MailStore::Attachment> atts;
        bool ok = false;
        // 预览加速快路径：只拉正文、附件仅取元数据（带附件邮件大幅提速）。
        // 复杂邮件（内嵌图/转发/签名）返回 false → 自动回退 fetchBody 整封拉。
        if (ImapClient::fetchBodyFast(cfg, folder, uid, &body, &html, &atts,
                                      &err, onProgress)) {
            ok = true;
        } else {
            err.clear();
            ok = ImapClient::fetchBody(cfg, folder, uid, &body, &html, &raw,
                                       &atts, &err, onProgress);
        }
        if (ok) {
            // MailStore 驻留主线程且非线程安全；把两次写入 + 预览刷新全部
            // 排到主线程上同步执行，保证 rawSource 写入后再 refreshPreview，
            // 避免预览时拿不到 cid: 图片数据。
            QMetaObject::invokeMethod(this, [this, msgId, body, html, raw, atts]() {
                Logger::instance().info(
                    QString("正文拉取返回: msgId=%1 bodyLen=%2 htmlLen=%3 rawLen=%4 atts=%5")
                        .arg(msgId).arg(body.size()).arg(html.size()).arg(raw.size()).arg(atts.size()),
                    "mail");
                MailStore::instance().updateBody(msgId, body, html, atts);
                if (!raw.isEmpty())
                    MailStore::instance().updateRawSource(msgId, raw);
                m_loadingBodyIds.remove(msgId);
                if (m_contentPanel) m_contentPanel->clearBodyProgress(msgId);
                if (selectedMessageId() == msgId) refreshPreview();
            }, Qt::QueuedConnection);
        } else {
            Logger::instance().warn(
                QString("正文按需拉取失败: uid=%1: %2").arg(uid).arg(err), "mail");
            QMetaObject::invokeMethod(this, [this, msgId]{
                m_loadingBodyIds.remove(msgId);
                if (m_contentPanel) m_contentPanel->clearBodyProgress(msgId);
                // 关键：失败必须退出"拉取中"加载页，否则进度条停留 100%。先标记失败，
                // 再刷新预览 → renderBody 检测到标记后显示错误提示；标记消费后清除，
                // 下次选中该邮件可重新拉取。
                if (m_contentPanel) m_contentPanel->setBodyLoadError(msgId);
                if (selectedMessageId() == msgId) refreshPreview();
            }, Qt::QueuedConnection);
        }
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

// 附件下载（已下载则直接打开；未下载走后台 fetchPart → 写盘 → 打开）
void MailWidget::onAttachmentClicked(const QString& msgId, int idx,
                                     const QString& name) {
    auto* m = MailStore::instance().message(msgId);
    if (!m || idx < 0 || idx >= m->attachments.size()) return;
    const auto& a = m->attachments[idx];

    // 已下载 → 内联预览（图片/文本等）；非可预览类型则在面板上显示"打开"按钮
    QString savedPath = MailContentPanel::attachmentSavedPath(msgId, a);
    if (QFileInfo(savedPath).exists()) {
        if (m_contentPanel) m_contentPanel->showAttachmentPreview(msgId, idx, savedPath, a.mimeType);
        return;
    }
    // 下载中 → 忽略重复点击
    QString key = msgId + "|" + QString::number(idx);
    if (m_contentPanel && m_contentPanel->isAttachmentDownloading(key)) return;

    startAttachmentDownload(msgId, idx, name);
}

void MailWidget::startAttachmentDownload(const QString& msgId, int idx,
                                         const QString& name) {
    auto* m = MailStore::instance().message(msgId);
    if (!m || idx < 0 || idx >= m->attachments.size()) return;
    const auto& a = m->attachments[idx];
    const QString key = msgId + "|" + QString::number(idx);

    // ── 本地加速：该封原件(rawSource)已被预览拉取缓存到内存 → 附件本地切取，免联网 ──
    // fetchBody 拉全文时已把整封（含附件内容）写入 rawSource，点附件再重新 IMAP 下载
    // 是重复开销；直接按 section 从原件切出并解码，速度快一个量级。
    if (!m->rawSource.isEmpty() && !a.section.isEmpty()) {
        const QByteArray localRaw = m->rawSource;   // 拷贝到后台线程，避免触碰非线程安全的 MailStore
        const QString section = a.section;
        const QString enc = a.encoding;
        const QString savedPath = MailContentPanel::attachmentSavedPath(msgId, a);
        const QString mId = msgId;
        const QString attName = a.name.isEmpty()
            ? QString("attachment_%1.bin").arg(a.section) : a.name;
        if (m_contentPanel) m_contentPanel->setAttachmentDownloading(key, true);

        QThread* t = QThread::create([this, mId, idx, name, section, enc, savedPath,
                                         key, attName, localRaw]() {
            QByteArray data;
            const bool ok = ImapClient::extractAttachmentFromRaw(localRaw, section, enc, &data);
            QMetaObject::invokeMethod(this, [this, mId, idx, name, savedPath, key, attName,
                                             data, ok]() {
                if (ok && !data.isEmpty()) {
                    Logger::instance().info(
                        QString("附件本地直取成功: %1 (%2 字节)").arg(attName).arg(data.size()), "mail");
                    finishAttachmentDownload(savedPath, data, key, attName, mId, true);
                } else {
                    // 本地提取缺失/失败（section 错位等极端情况）→ 回退联网下载
                    if (m_contentPanel) {
                        m_contentPanel->setAttachmentDownloading(key, false);
                        m_contentPanel->clearAttachmentProgress(key);
                    }
                    downloadAttachmentOverNetwork(mId, idx, name);
                }
            }, Qt::QueuedConnection);
        });
        connect(t, &QThread::finished, t, &QObject::deleteLater);
        t->start();
        return;
    }

    // ── 原件未缓存：走网络路径（后台 fetchPart，独立 IMAP 连接） ──
    downloadAttachmentOverNetwork(msgId, idx, name);
}

void MailWidget::downloadAttachmentOverNetwork(const QString& msgId, int idx,
                                               const QString& /*name*/) {
    auto* m = MailStore::instance().message(msgId);
    if (!m || idx < 0 || idx >= m->attachments.size()) return;
    const auto& a = m->attachments[idx];

    auto* acc = MailAccountManager::instance().getById(m->accountId);
    if (!acc) return;

    ImapClient::Config cfg;
    cfg.host = acc->imapHost; cfg.port = acc->imapPort; cfg.ssl = acc->imapSsl;
    cfg.username = acc->email; cfg.password = acc->password;
    cfg.timeoutSec = 60;

    QString savedPath = MailContentPanel::attachmentSavedPath(msgId, a);
    QDir().mkpath(QFileInfo(savedPath).absolutePath());

    QString key = msgId + "|" + QString::number(idx);
    QString attName = a.name.isEmpty()
        ? QString("attachment_%1.bin").arg(a.section) : a.name;
    QString mId = msgId;
    QString folder = m->folder;
    QString uid    = m->imapUid;
    QString section = a.section;   // IMAP BODY[] 编号（如 "2" / "1.2"）
    QString encoding = a.encoding; // "base64" / "quoted-printable" / "7bit" …

    if (m_contentPanel) m_contentPanel->setAttachmentDownloading(key, true);

    // 不捕获 MailStore::Message* m：该指针可能在后台线程运行期间因主线程
    // 更新存储而失效（悬空捕获）；所需字段已在上方拷贝为局部值
    QThread* t = QThread::create([this, cfg, mId, folder, uid, section, encoding,
                                     savedPath, key, attName]() {
        QString err;
        QByteArray data;
        bool ok = ImapClient::fetchPart(cfg, folder, uid, section, encoding,
                                        &data, &err,
        [this, key, mId](qint64 recv, qint64 exp){
            QMetaObject::invokeMethod(this, [this, key, mId, recv, exp]{
                if (m_contentPanel) {
                    m_contentPanel->setAttachmentDownloading(key, true);
                    m_contentPanel->setAttachmentProgress(key, recv, exp);
                }
            }, Qt::QueuedConnection);
        });
        QMetaObject::invokeMethod(this, [this, ok, err, savedPath, data, key, attName, mId]() {
            if (!ok) {
                Logger::instance().warn(
                    QString("附件下载失败: msg=%1 name=%2: %3").arg(mId, attName).arg(err), "mail");
                // 失败态由 finishAttachmentDownload 统一标记 + toast，避免重复提示
            }
            finishAttachmentDownload(savedPath, data, key, attName, mId, ok);
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

// 附件落盘 + 更新下载行状态 + 切换内联预览（主线程收尾统一入口，网络/本地共用）
void MailWidget::finishAttachmentDownload(const QString& savedPath, const QByteArray& data,
                                          const QString& key, const QString& attName,
                                          const QString& mId, bool ok) {
    if (m_contentPanel) {
        m_contentPanel->setAttachmentDownloading(key, false);
        m_contentPanel->clearAttachmentProgress(key);
    }
    if (!ok || data.isEmpty()) {
        if (m_contentPanel) m_contentPanel->markAttachmentDownloaded(key, false);
        return;
    }
    QFile f(savedPath);
    if (!f.open(QIODevice::WriteOnly)) {
        Logger::instance().warn(
            QString("附件写盘失败: %1: %2").arg(savedPath, f.errorString()), "mail");
        if (m_contentPanel) m_contentPanel->markAttachmentDownloaded(key, false);
        QMessageBox::warning(this, "下载失败", attName + ": 写盘失败 " + f.errorString());
        return;
    }
    f.write(data);
    f.close();
    Logger::instance().info(
        QString("附件下载完成: %1 (%2 字节)").arg(attName).arg(data.size()), "mail");
    // 标记行已下载（显示"打开"和"定位"图标）
    if (m_contentPanel) m_contentPanel->markAttachmentDownloaded(key, true);
    // 切换到内联预览面板（图片/文本直接显示；PDF/Office 显示"打开"按钮）
    int idx = key.section(QLatin1Char('|'), 1).toInt();
    auto* mm = MailStore::instance().message(mId);
    if (m_contentPanel && mm && idx >= 0 && idx < mm->attachments.size()) {
        m_contentPanel->showAttachmentPreview(mId, idx, savedPath,
            mm->attachments[idx].mimeType);
    }
    // 刷新预览
    refreshPreview();
}

// 已下载附件的"打开"：直接用系统默认应用打开本地文件
void MailWidget::onAttachmentOpenRequested(const QString& msgId, int idx,
                                           const QString& name,
                                           const QString& savedPath) {
    Q_UNUSED(msgId); Q_UNUSED(idx); Q_UNUSED(name);
    if (savedPath.isEmpty() || !QFileInfo(savedPath).exists()) {
        QMessageBox::warning(this, "打开失败",
            QString("本地文件不存在或已被删除:\n%1").arg(savedPath));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(savedPath))) {
        QMessageBox::warning(this, "打开失败",
            QString("无法打开文件:\n%1").arg(savedPath));
    }
}

// 已下载附件的"在资源管理器中显示"：Windows 上用 explorer /select，
// 其他平台退化为打开所在目录。
void MailWidget::onAttachmentRevealRequested(const QString& msgId, int idx,
                                             const QString& name,
                                             const QString& savedPath) {
    Q_UNUSED(msgId); Q_UNUSED(idx); Q_UNUSED(name);
    if (savedPath.isEmpty() || !QFileInfo(savedPath).exists()) {
        QMessageBox::warning(this, "定位失败",
            QString("本地文件不存在或已被删除:\n%1").arg(savedPath));
        return;
    }
#ifdef Q_OS_WIN
    // explorer /select,"<path>"  打开所在目录并选中目标文件
    QString native = QDir::toNativeSeparators(savedPath);
    bool ok = QProcess::startDetached("explorer.exe",
                                      QStringList() << QString("/select,\"%1\"").arg(native));
    if (ok) return;
#endif
    // 退化：仅打开所在目录
    QFileInfo fi(savedPath);
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
}

// ── 编辑器多 tab 管理 ─────────────────────────────────────────────────────────
MailWidget::EditorPage* MailWidget::createEditorPage(const QString& key,
                                                     const QString& tabTitle) {
    auto* p = new EditorPage;
    p->key = key;

    p->panel = new QWidget;
    auto* eL = new QVBoxLayout(p->panel); eL->setContentsMargins(0,0,0,0); eL->setSpacing(6);
    // 顶部"新邮件 / 回复 / 转发" contextLabel 行已移除：保留对象以兼容 setText 调用，但不加入布局
    p->contextLabel = new QLabel;
    p->contextLabel->setStyleSheet(Theme::mutedText());
    auto* eForm = new QFormLayout; eForm->setSpacing(6);

    // 发件人行：fromEdit + 右侧"抄送"/"密送"切换按钮（点击切换下方输入框可见）
    auto* fromRow = new QHBoxLayout;
    fromRow->setSpacing(6);
    p->fromEdit = new QLineEdit; p->fromEdit->setReadOnly(true);
    fromRow->addWidget(p->fromEdit, 1);
    auto* ccToggleBtn = new QToolButton; ccToggleBtn->setText(QStringLiteral("抄送"));
    ccToggleBtn->setCheckable(true);
    ccToggleBtn->setCursor(Qt::PointingHandCursor);
    ccToggleBtn->setToolTip(QStringLiteral("点击显示/隐藏抄送输入框"));
    ccToggleBtn->setStyleSheet(QStringLiteral(
        "QToolButton { padding: 2px 10px; border: 1px solid %1; border-radius: 3px; color: %2; }"
        "QToolButton:hover { background: %3; }"
        "QToolButton:checked { background: %4; color: %5; border-color: %4; }")
        .arg(Theme::kBorder, Theme::kText, Theme::kSurface,
             Theme::kAccent, QStringLiteral("#0d1116")));
    fromRow->addWidget(ccToggleBtn);
    auto* bccToggleBtn = new QToolButton; bccToggleBtn->setText(QStringLiteral("密送"));
    bccToggleBtn->setCheckable(true);
    bccToggleBtn->setCursor(Qt::PointingHandCursor);
    bccToggleBtn->setToolTip(QStringLiteral("点击显示/隐藏密送输入框"));
    bccToggleBtn->setStyleSheet(ccToggleBtn->styleSheet());
    fromRow->addWidget(bccToggleBtn);
    eForm->addRow(QStringLiteral("发件人:"), fromRow);

    p->toEdit = new QLineEdit;
    p->toEdit->setPlaceholderText("收件人，多个用逗号/分号分隔");
    eForm->addRow("收件人:", p->toEdit);

    // 抄送/密送：默认隐藏；点击 fromRow 上的切换按钮显示对应行
    p->ccEdit  = new QLineEdit; p->ccEdit->setPlaceholderText("抄送（可选，多个用逗号/分号分隔）");
    p->bccEdit = new QLineEdit; p->bccEdit->setPlaceholderText("密送（可选，多个用逗号/分号分隔）");
    // 用独立 widget 作为行容器（默认隐藏），让 QFormLayout 标签/控件整体隐显
    p->ccRowHost  = new QWidget; {
         auto* h = new QHBoxLayout(p->ccRowHost);
        h->setContentsMargins(0,0,0,0); h->addWidget(p->ccEdit); 
    }
    p->bccRowHost = new QWidget; {
         auto* h = new QHBoxLayout(p->bccRowHost);
        h->setContentsMargins(0,0,0,0); h->addWidget(p->bccEdit); 
    }
    // addRow 前记录行索引，用于后面同时隐藏"抄送:"标签 + ccRowHost 整行
    int ccRowIdx  = eForm->rowCount();
    eForm->addRow(QStringLiteral("抄送:"), p->ccRowHost);
    int bccRowIdx = eForm->rowCount();
    eForm->addRow(QStringLiteral("密送:"), p->bccRowHost);
    // 整行隐藏（含 label + field），避免只藏容器时 QFormLayout 标签仍占行
    auto hideFormRow = [eForm](int row, bool on){
        if (auto* labelItem = eForm->itemAt(row, QFormLayout::LabelRole))
            if (auto* w = labelItem->widget()) w->setVisible(on);
        if (auto* fieldItem = eForm->itemAt(row, QFormLayout::FieldRole))
            if (auto* w = fieldItem->widget()) w->setVisible(on);
    };
    hideFormRow(ccRowIdx,  false);
    hideFormRow(bccRowIdx, false);
    connect(ccToggleBtn, &QToolButton::toggled, this, [hideFormRow, ccRowIdx](bool on){
        hideFormRow(ccRowIdx, on);
    });
    connect(bccToggleBtn, &QToolButton::toggled, this, [hideFormRow, bccRowIdx](bool on){
        hideFormRow(bccRowIdx, on);
    });

    p->subjectEdit = new QLineEdit;
    p->subjectEdit->setPlaceholderText("主题");
    eForm->addRow("主题:", p->subjectEdit);

    // ── 选项行：紧急 + 已读回执 + 加密 + 定时发送（全部默认关闭，状态字段由 EditorPage 持有） ──
    auto* optRow = new QHBoxLayout;
    p->urgentChk = new QCheckBox(QStringLiteral("紧急"));
    p->urgentChk->setToolTip(QStringLiteral("发送时附加 X-Priority: 1 / Importance: High 头"));
    p->urgentChk->setCursor(Qt::PointingHandCursor);
    p->readReceiptChk = new QCheckBox(QStringLiteral("已读回执"));
    p->readReceiptChk->setToolTip(QStringLiteral("请求收件方在打开邮件时发送回执（Disposition-Notification-To）"));
    p->readReceiptChk->setCursor(Qt::PointingHandCursor);
    p->plainTextChk = new QCheckBox(QStringLiteral("纯文本"));
    p->plainTextChk->setToolTip(QStringLiteral("以纯文本发送正文（不发送 HTML/富文本格式）"));
    p->plainTextChk->setCursor(Qt::PointingHandCursor);

    // 加密下拉
    p->encryptCombo = new QComboBox;
    p->encryptCombo->addItem(QStringLiteral("不加密"), "none");
    p->encryptCombo->addItem(QStringLiteral("自动 (S/MIME→口令)"), "auto");
    p->encryptCombo->addItem(QStringLiteral("强制 S/MIME"), "smime");
    p->encryptCombo->addItem(QStringLiteral("口令加密"), "password");
    p->encryptCombo->setToolTip(QStringLiteral(
        "邮件加密：S/MIME 用收件人 X.509 公钥；口令加密用共享口令\n"
        "切到\"口令加密\"会自动弹窗输入口令"));
    p->encryptCombo->setCursor(Qt::PointingHandCursor);
    p->encryptCombo->setMinimumWidth(150);
    {
        QString m = Settings::instance().mailDefaultEncryptMode();
        int idx = p->encryptCombo->findData(m);
        if (idx >= 0) p->encryptCombo->setCurrentIndex(idx);
    }

    // 选项行顺序：已读回执 → 纯文本 → 定时发送 → 紧急 → 加密
    p->scheduleBtn = new QPushButton(QStringLiteral("⏱ 定时发送"));
    p->scheduleBtn->setCursor(Qt::PointingHandCursor);
    p->scheduleBtn->setToolTip(QStringLiteral("选择定时发送时间（到点自动发送）"));
    connect(p->scheduleBtn, &QPushButton::clicked, this, [p, this]{
        if (p) pickScheduleTime(p);
    });

    optRow->addWidget(p->readReceiptChk);   // 已读回执
    optRow->addWidget(p->plainTextChk);     // 纯文本
    // 定时发送已由底部"定时发送"复选框 + 发送按钮承载，选项行不再重复显示 scheduleBtn
    optRow->addWidget(p->urgentChk);        // 紧急
    optRow->addSpacing(8);
    optRow->addWidget(new QLabel(QStringLiteral("加密方式:")));
    optRow->addWidget(p->encryptCombo);
    optRow->addStretch();
    // 底部对齐：checkbox / label / combobox 高度不同，统一在垂直方向底部齐平
    optRow->setAlignment(Qt::AlignBottom);
    // 选项行改放到 eBtn（发送按钮行）之上：通过下方 eL->addLayout(optRow) 接管
    connect(p->urgentChk, &QCheckBox::toggled, this, [p](bool on){ p->urgent = on; });
    connect(p->readReceiptChk, &QCheckBox::toggled, this, [p](bool on){ p->readReceipt = on; });
    connect(p->plainTextChk, &QCheckBox::toggled, this, [p](bool on){ p->plainText = on; });
    connect(p->encryptCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, p](int idx){
        QString v = p->encryptCombo->itemData(idx).toString();
        p->encryptMode = v;
        // 切到口令 → 弹窗输入口令
        if (v == "password" && p->encryptPassword.isEmpty()) {
            bool ok = false;
            QString pwd = QInputDialog::getText(this, QStringLiteral("口令加密"),
                QStringLiteral("请输入共享口令（收件方需要同一口令解密）："),
                QLineEdit::Password, QString(), &ok);
            if (ok && !pwd.isEmpty()) p->encryptPassword = pwd;
            else if (!ok) p->encryptCombo->setCurrentIndex(0);
        }
    });
    // scheduleBtn / scheduleChkLabel 已由底部"定时发送"复选框 + 发送按钮承载（保留字段不再显示）

    eL->addLayout(eForm);

    // ── 富文本正文：工具栏 + QTextEdit ──────────────────────────────
    // 工具栏白底、深文字，与正文（白色）融为一体，构成统一的白色编辑面板
    auto* bodyToolHost = new QFrame;
    bodyToolHost->setObjectName(QStringLiteral("mailBodyToolbar"));
    bodyToolHost->setStyleSheet(QStringLiteral(
        "#mailBodyToolbar { background: white; border: none; border-bottom: 1px solid #d0d7de; }"));
    auto* bodyBar = new QHBoxLayout(bodyToolHost);
    bodyBar->setContentsMargins(6, 4, 6, 2);
    bodyBar->setSpacing(2);

    // 字体 / 字号（白底深字，与工具栏融为一个整体）
    const QString lightCombo = QStringLiteral(
        "QFontComboBox, QComboBox { background: white; color: #1f2328; border: 1px solid #d0d7de; border-radius: 3px; padding: 2px 4px; }"
        "QFontComboBox::drop-down, QComboBox::drop-down { border: none; }"
        "QFontComboBox QAbstractItemView, QComboBox QAbstractItemView { background: white; color: #1f2328; selection-background-color: %1; selection-color: white; }")
        .arg(Theme::kAccent);
    auto* fontBox = new QFontComboBox;
    fontBox->setMinimumWidth(140);
    fontBox->setStyleSheet(lightCombo);
    bodyBar->addWidget(fontBox);
    auto* sizeBox = new QComboBox;
    sizeBox->setEditable(true);
    for (int s : {9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 36}) sizeBox->addItem(QString::number(s));
    sizeBox->setCurrentText("11");
    sizeBox->setMinimumWidth(56);
    sizeBox->setStyleSheet(lightCombo);
    bodyBar->addWidget(sizeBox);

    auto mkBtn = [this](const QString& tip, const QString& text = QString()) {
        auto* b = new QToolButton;
        b->setText(text.isEmpty() ? tip : text);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setAutoRaise(true);
        return b;
    };
    auto* boldBtn   = mkBtn("粗体 (Ctrl+B)", "B");
    auto* italBtn   = mkBtn("斜体 (Ctrl+I)", "I");
    auto* undrBtn   = mkBtn("下划线 (Ctrl+U)", "U");
    auto* strikBtn  = mkBtn("删除线", "S");
    auto* colorBtn  = mkBtn("文字颜色");
    auto* alignLBtn = mkBtn("左对齐");
    auto* alignCBtn = mkBtn("居中");
    auto* alignRBtn = mkBtn("右对齐");
    auto* listOBtn  = mkBtn("有序列表");
    auto* listUBtn  = mkBtn("无序列表");
    auto* indentPBtn= mkBtn("增加缩进");
    auto* indentNBtn= mkBtn("减少缩进");
    auto* quoteBtn  = mkBtn("引用");
    auto* hrBtn     = mkBtn("水平线");
    auto* clearBtn  = mkBtn("清除格式");
    auto* undoBtn   = mkBtn("撤销 (Ctrl+Z)");
    auto* redoBtn   = mkBtn("重做 (Ctrl+Y)");
    auto* imgBtn    = mkBtn("插入图片");
    auto* linkBtn   = mkBtn("超链接");
    auto* codeBtn   = mkBtn("行内代码");
    auto* codeBlockBtn = mkBtn("代码块");
    auto* tableBtn  = mkBtn("插入表格");
    auto* emojiBtn  = mkBtn("表情");
    // ── 精简分组：高频按钮留在主工具栏，对齐/列表/插入合并到下拉，"更多"放冷门 ──
    auto applyBarBtnStyle = [](QToolButton* b){
        b->setStyleSheet(QStringLiteral(
            "QToolButton { padding: 2px 8px; color: #1f2328; border: 1px solid transparent; border-radius: 3px; }"
            "QToolButton:hover { background: #eef1f4; border: 1px solid #d0d7de; }"
            "QToolButton:pressed { background: #e1e6eb; }"));
    };
    auto addSep = [&](){
        auto* sep = new QFrame; sep->setFrameShape(QFrame::VLine); sep->setFrameShadow(QFrame::Sunken);
        sep->setStyleSheet(QStringLiteral("color: #d0d7de;"));
        sep->setFixedHeight(18);
        bodyBar->addWidget(sep);
    };
    // 构造下拉按钮（带 ▾，与主按钮风格一致，白底深字）
    auto mkDropBtn = [&](const QString& label, const QString& tip){
        auto* b = new QToolButton;
        b->setText(label);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setPopupMode(QToolButton::InstantPopup);
        b->setStyleSheet(QStringLiteral(
            "QToolButton { padding: 2px 10px; color: #1f2328; border: 1px solid #d0d7de; border-radius: 3px; }"
            "QToolButton:hover { background: #eef1f4; }"));
        return b;
    };
    // 给下拉菜单添加可点击项：直接转发到对应隐形按钮的 click()
    auto addAct = [](QMenu* menu, QToolButton* b){
        menu->addAction(b->text(), [b]{ b->click(); });
    };

    // 组1：文字格式（高频：粗体 / 斜体 / 下划线 / 删除线 / 文字颜色）
    for (auto* b : {boldBtn, italBtn, undrBtn, strikBtn, colorBtn}) applyBarBtnStyle(b);
    bodyBar->addWidget(boldBtn); bodyBar->addWidget(italBtn);
    bodyBar->addWidget(undrBtn); bodyBar->addWidget(strikBtn);
    bodyBar->addWidget(colorBtn);
    addSep();

    // "对齐 ▾" 下拉（左/中/右）
    auto* alignDropBtn = mkDropBtn(QStringLiteral("对齐 ▾"), QStringLiteral("段落对齐方式"));
    auto* alignMenu = new QMenu(alignDropBtn);
    addAct(alignMenu, alignLBtn);
    addAct(alignMenu, alignCBtn);
    addAct(alignMenu, alignRBtn);
    alignDropBtn->setMenu(alignMenu);
    bodyBar->addWidget(alignDropBtn);

    // "列表 ▾" 下拉（有序 / 无序 / 缩进+/- / 引用）
    auto* listDropBtn = mkDropBtn(QStringLiteral("列表 ▾"), QStringLiteral("列表、缩进、引用"));
    auto* listMenu = new QMenu(listDropBtn);
    addAct(listMenu, listOBtn);
    addAct(listMenu, listUBtn);
    listMenu->addSeparator();
    addAct(listMenu, indentPBtn);
    addAct(listMenu, indentNBtn);
    listMenu->addSeparator();
    addAct(listMenu, quoteBtn);
    listDropBtn->setMenu(listMenu);
    bodyBar->addWidget(listDropBtn);
    addSep();

    // 撤销 / 重做（高频）
    for (auto* b : {undoBtn, redoBtn}) applyBarBtnStyle(b);
    bodyBar->addWidget(undoBtn); bodyBar->addWidget(redoBtn);
    addSep();

    // "插入 ▾" 下拉（图片 / 链接 / 表格 / 表情）
    auto* insertDropBtn = mkDropBtn(QStringLiteral("插入 ▾"), QStringLiteral("插入图片、链接、表格、表情"));
    auto* insertMenu = new QMenu(insertDropBtn);
    addAct(insertMenu, imgBtn);
    addAct(insertMenu, linkBtn);
    addAct(insertMenu, tableBtn);
    insertMenu->addSeparator();
    addAct(insertMenu, emojiBtn);
    insertDropBtn->setMenu(insertMenu);
    bodyBar->addWidget(insertDropBtn);

    // "更多 ▾" 下拉（冷门：行内代码 / 代码块 / 水平线 / 清除格式）
    for (auto* b : {codeBtn, codeBlockBtn, hrBtn, clearBtn}) applyBarBtnStyle(b);
    auto* moreMenuBtn = mkDropBtn(QStringLiteral("更多 ▾"), QStringLiteral("行内代码、代码块、水平线、清除格式"));
    auto* moreMenu = new QMenu(moreMenuBtn);
    addAct(moreMenu, codeBtn);
    addAct(moreMenu, codeBlockBtn);
    moreMenu->addSeparator();
    addAct(moreMenu, hrBtn);
    moreMenu->addSeparator();
    addAct(moreMenu, clearBtn);
    moreMenuBtn->setMenu(moreMenu);
    bodyBar->addWidget(moreMenuBtn);
    bodyBar->addStretch();

    // 富文本编辑器
    p->bodyEdit = new QTextEdit;
    p->bodyEdit->setAcceptRichText(true);
    p->bodyEdit->setPlaceholderText(QStringLiteral("正文（支持富文本：拖入图片直接插入，工具栏调整格式）"));
    p->bodyEdit->setMinimumHeight(220);
    // 富文本输入框：背景设为白色（正文邮件内容惯用白底），配深色文字保证可读；去掉自身边框以融入编辑卡片
    p->bodyEdit->setFrameShape(QAbstractScrollArea::NoFrame);
    p->bodyEdit->setStyleSheet(QStringLiteral(
        "QTextEdit { border: none; background: white; color: #1f2328; selection-background-color: %1; selection-color: #ffffff; }")
        .arg(Theme::kAccent));

    // Markdown 源码编辑器（Markdown 模式下使用）
    p->mdEdit = new QPlainTextEdit;
    p->mdEdit->setPlaceholderText(QStringLiteral(
        "Markdown 正文（GitHub 风格）：\n\n"
        "# 标题\n\n"
        "**加粗** *斜体* ~~删除线~~ `行内代码`\n\n"
        "- 无序列表\n"
        "1. 有序列表\n\n"
        "> 引用\n\n"
        "[链接](https://example.com)\n\n"
        "```cpp\n代码块\n```\n\n"
        "| 列1 | 列2 |\n"
        "|----|----|\n"
        "| A   | B   |"));
    p->mdEdit->setFont(QFont(QStringLiteral("Consolas"), 10));
    // 融入编辑卡片：与富文本一样去边框、背景透明
    p->mdEdit->setFrameShape(QAbstractScrollArea::NoFrame);
    p->mdEdit->setStyleSheet(QStringLiteral("QPlainTextEdit { border: none; background: transparent; }"));
    p->mdEdit->setMinimumHeight(220);
    // Markdown 模式下也接受拖拽：图片 → ![](path)，文件 → 记录 path 在末尾
    p->mdEdit->setAcceptDrops(true);
    p->mdEdit->installEventFilter(this);

    // 工具栏交互
    auto applyFontFamily = [fontBox, p](const QString& fam){
        if (!p->bodyEdit) return;
        QTextCursor c = p->bodyEdit->textCursor();
        QTextCharFormat f = c.charFormat();
        f.setFontFamilies({fam});
        c.mergeCharFormat(f);
        p->bodyEdit->setTextCursor(c);
    };
    auto applyFontSize = [sizeBox, p](const QString& s){
        bool ok=false; int pt = s.toInt(&ok); if (!ok || pt<=0) return;
        QTextCursor c = p->bodyEdit->textCursor();
        QTextCharFormat f = c.charFormat();
        f.setFontPointSize(pt);
        c.mergeCharFormat(f);
        p->bodyEdit->setTextCursor(c);
    };
    connect(fontBox, &QFontComboBox::currentFontChanged, this, [applyFontFamily](const QFont& f){
        applyFontFamily(f.family());
    });
    connect(sizeBox, &QComboBox::currentTextChanged, this, applyFontSize);
    connect(boldBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        p->bodyEdit->setFontWeight(p->bodyEdit->fontWeight() >= QFont::Bold ? QFont::Normal : QFont::Bold);
    });
    connect(italBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        p->bodyEdit->setFontItalic(!p->bodyEdit->fontItalic());
    });
    connect(undrBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        p->bodyEdit->setFontUnderline(!p->bodyEdit->fontUnderline());
    });
    connect(strikBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        QTextCursor c = p->bodyEdit->textCursor();
        QTextCharFormat f; f.setFontStrikeOut(!c.charFormat().fontStrikeOut());
        c.mergeCharFormat(f);
        p->bodyEdit->setTextCursor(c);
    });
    connect(colorBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        QColor c = QColorDialog::getColor(p->bodyEdit->textColor(), p->bodyEdit, "选择文字颜色");
        if (!c.isValid()) return;
        p->bodyEdit->setTextColor(c);
    });
    connect(alignLBtn, &QToolButton::clicked, this, [p]{ if (p->bodyEdit) p->bodyEdit->setAlignment(Qt::AlignLeft); });
    connect(alignCBtn, &QToolButton::clicked, this, [p]{ if (p->bodyEdit) p->bodyEdit->setAlignment(Qt::AlignCenter); });
    connect(alignRBtn, &QToolButton::clicked, this, [p]{ if (p->bodyEdit) p->bodyEdit->setAlignment(Qt::AlignRight); });
    connect(listOBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        QTextCursor c = p->bodyEdit->textCursor();
        QTextListFormat lf; lf.setStyle(QTextListFormat::ListDecimal);
        c.createList(lf);
        p->bodyEdit->setTextCursor(c);
    });
    connect(listUBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        QTextCursor c = p->bodyEdit->textCursor();
        QTextListFormat lf; lf.setStyle(QTextListFormat::ListDisc);
        c.createList(lf);
        p->bodyEdit->setTextCursor(c);
    });
    connect(indentPBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        QTextCursor c = p->bodyEdit->textCursor();
        QTextBlockFormat bf = c.blockFormat();
        bf.setIndent(bf.indent() + 1);
        c.mergeBlockFormat(bf);
        p->bodyEdit->setTextCursor(c);
    });
    connect(indentNBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        QTextCursor c = p->bodyEdit->textCursor();
        QTextBlockFormat bf = c.blockFormat();
        bf.setIndent(qMax(0, bf.indent() - 1));
        c.mergeBlockFormat(bf);
        p->bodyEdit->setTextCursor(c);
    });
    connect(quoteBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        QTextCursor c = p->bodyEdit->textCursor();
        QTextBlockFormat bf = c.blockFormat();
        bf.setLeftMargin(24); bf.setIndent(1);
        c.mergeBlockFormat(bf);
        p->bodyEdit->setTextCursor(c);
    });
    connect(hrBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        p->bodyEdit->insertHtml(QStringLiteral("<hr/>"));
    });
    connect(clearBtn, &QToolButton::clicked, this, [p]{
        if (!p->bodyEdit) return;
        QTextCursor c = p->bodyEdit->textCursor();
        c.setCharFormat(QTextCharFormat());
        c.setBlockFormat(QTextBlockFormat());
        p->bodyEdit->setTextCursor(c);
    });
    connect(undoBtn, &QToolButton::clicked, this, [p]{ if (p->bodyEdit) p->bodyEdit->undo(); });
    connect(redoBtn, &QToolButton::clicked, this, [p]{ if (p->bodyEdit) p->bodyEdit->redo(); });
    connect(imgBtn, &QToolButton::clicked, this, [this, p]{
        if (!p) return;
        onInsertInlineImageClicked(p);
    });
    connect(linkBtn, &QToolButton::clicked, this, [p]{
        if (!p || !p->bodyEdit) return;
        bool ok = false;
        QString url = QInputDialog::getText(p->bodyEdit, QStringLiteral("插入超链接"),
                                            QStringLiteral("URL（链接地址）："),
                                            QLineEdit::Normal, QStringLiteral("https://"), &ok);
        if (!ok || url.trimmed().isEmpty()) return;
        QString text = QInputDialog::getText(p->bodyEdit, QStringLiteral("插入超链接"),
                                            QStringLiteral("显示文本："),
                                            QLineEdit::Normal, url, &ok);
        if (!ok) text = url;
        QString safeText = text.toHtmlEscaped();
        QString safeUrl  = url.toHtmlEscaped();
        p->bodyEdit->insertHtml(QStringLiteral("<a href=\"%1\">%2</a>").arg(safeUrl, safeText));
    });
    connect(codeBtn, &QToolButton::clicked, this, [p]{
        if (!p || !p->bodyEdit) return;
        QTextCursor c = p->bodyEdit->textCursor();
        // 用等宽字体 + 浅灰背景 + 行内代码样式
        QTextCharFormat f;
        f.setFontFamilies({QStringLiteral("Consolas"), QStringLiteral("Menlo"), QStringLiteral("monospace")});
        f.setBackground(QColor(0x33, 0x36, 0x40));
        f.setForeground(QColor(0xe6, 0xe6, 0xe6));
        // 把光标处文本包在 <code> 里（用 insertHtml 也行；这里用 mergeCharFormat 也可，
        // 但更通用的是插一个 <code> 包裹）
        QString sel = c.selectedText();
        if (sel.isEmpty()) sel = QStringLiteral("code");
        c.insertHtml(QStringLiteral("<code>%1</code>").arg(sel.toHtmlEscaped()));
    });
    connect(codeBlockBtn, &QToolButton::clicked, this, [p]{
        if (!p || !p->bodyEdit) return;
        QTextCursor c = p->bodyEdit->textCursor();
        QString sel = c.selectedText();
        if (sel.isEmpty()) sel = QStringLiteral("// code");
        p->bodyEdit->insertHtml(QStringLiteral(
            "<pre style=\"background:#1e2128; color:#e6e6e6; "
            "font-family: Consolas, Menlo, monospace; padding: 8px; "
            "border-radius: 4px; border: 1px solid #2a2d36;\">"
            "%1</pre>").arg(sel.toHtmlEscaped()));
    });
    connect(tableBtn, &QToolButton::clicked, this, [p]{
        if (!p || !p->bodyEdit) return;
        bool okR = false, okC = false;
        int rows = QInputDialog::getInt(p->bodyEdit, QStringLiteral("插入表格"),
                                        QStringLiteral("行数："), 3, 1, 20, 1, &okR);
        if (!okR) return;
        int cols = QInputDialog::getInt(p->bodyEdit, QStringLiteral("插入表格"),
                                        QStringLiteral("列数："), 3, 1, 10, 1, &okC);
        if (!okC) return;
        QString html = QStringLiteral(
            "<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" "
            "style=\"border-collapse: collapse; border-color: #4a4d56;\">");
        for (int r = 0; r < rows; ++r) {
            html += QStringLiteral("<tr>");
            for (int c2 = 0; c2 < cols; ++c2)
                html += QStringLiteral("<td>&nbsp;</td>");
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table><br/>");
        p->bodyEdit->insertHtml(html);
    });
    connect(emojiBtn, &QToolButton::clicked, this, [p]{
        if (!p || !p->bodyEdit) return;
        // 简化版表情面板：常用 emoji 列表，点选直接插入
        static const QStringList emojis = {
            QStringLiteral("😀"), QStringLiteral("😃"), QStringLiteral("😄"), QStringLiteral("😁"),
            QStringLiteral("😆"), QStringLiteral("😅"), QStringLiteral("😂"), QStringLiteral("🤣"),
            QStringLiteral("😊"), QStringLiteral("😇"), QStringLiteral("🙂"), QStringLiteral("🙃"),
            QStringLiteral("😉"), QStringLiteral("😌"), QStringLiteral("😍"), QStringLiteral("🥰"),
            QStringLiteral("😘"), QStringLiteral("😗"), QStringLiteral("😙"), QStringLiteral("😚"),
            QStringLiteral("👍"), QStringLiteral("👎"), QStringLiteral("👏"), QStringLiteral("🙌"),
            QStringLiteral("🙏"), QStringLiteral("💪"), QStringLiteral("🤝"), QStringLiteral("✌️"),
            QStringLiteral("✅"), QStringLiteral("❌"), QStringLiteral("⭐"), QStringLiteral("🌟"),
            QStringLiteral("💡"), QStringLiteral("📌"), QStringLiteral("📎"), QStringLiteral("🔗"),
            QStringLiteral("📧"), QStringLiteral("📨"), QStringLiteral("📩"), QStringLiteral("✉️"),
            QStringLiteral("📝"), QStringLiteral("📋"), QStringLiteral("📊"), QStringLiteral("📈"),
            QStringLiteral("🎉"), QStringLiteral("🎊"), QStringLiteral("🎁"), QStringLiteral("🎂"),
            QStringLiteral("❤️"), QStringLiteral("💔"), QStringLiteral("💖"), QStringLiteral("💯"),
            QStringLiteral("🔥"), QStringLiteral("✨"), QStringLiteral("🌈"), QStringLiteral("☀️"),
        };
        QMenu menu;
        for (int i = 0; i < emojis.size(); ++i) {
            const QString e = emojis[i];
            QAction* act = menu.addAction(e);
            // 直接用 lambda 插入（避免 QSignalMapper::map 重载歧义）
            connect(act, &QAction::triggered, p->bodyEdit, [p, e]{
                if (p && p->bodyEdit) p->bodyEdit->insertPlainText(e);
            });
        }
        menu.exec(QCursor::pos());
    });

    // 附件工具栏（📎 添加附件）紧贴主题下方，置于富文本工具栏之上、与之 0 间距
    auto* attachBar = new QHBoxLayout;
    attachBar->setSpacing(6);

    auto* addAttBtn = new QToolButton;
    addAttBtn->setText(QStringLiteral("📎 添加附件"));
    addAttBtn->setCursor(Qt::PointingHandCursor);
    addAttBtn->setStyleSheet(QStringLiteral(
        "QToolButton { padding: 2px 10px; border: 1px solid %1; border-radius: 3px; }"
        "QToolButton:hover { background: %2; }").arg(Theme::kBorder, Theme::kSurface));
    addAttBtn->setToolTip(QStringLiteral("从磁盘选择文件附加到邮件"));
    connect(addAttBtn, &QToolButton::clicked, this, [this, p]{
        onAddAttachmentClicked();
        refreshAttachmentTable(p);
    });
    attachBar->addWidget(addAttBtn);

    // 模板按钮（下拉菜单）：紧跟"添加附件"之后
    p->templateBtn = new QToolButton;
    p->templateBtn->setText(QStringLiteral("模板 ▾"));
    p->templateBtn->setCursor(Qt::PointingHandCursor);
    p->templateBtn->setPopupMode(QToolButton::InstantPopup);
    p->templateBtn->setStyleSheet(QStringLiteral(
        "QToolButton { background: %1; color: %2; border: 1px solid %3; padding: 4px 8px; border-radius: 3px; }"
        "QToolButton:hover { background: %4; }"
        "QToolButton::menu-indicator { image: none; }")
        .arg(Theme::kTitleBar, Theme::kText, Theme::kBorder, Theme::kBorderLight));
    p->templateBtn->setToolTip(QStringLiteral(
        "从预设模板快速填充主题与正文。\n内置：续约 / 跟进 / 投诉 / 邀请 / 感谢 / 请假 / 汇报 / 道歉。\n"
        "右键或菜单底部可保存当前邮件为模板，或删除已保存模板。"));
    auto* tplMenu = new QMenu(p->templateBtn);
    p->templateBtn->setMenu(tplMenu);
    // 填充菜单（每次弹出时构建）
    connect(p->templateBtn, &QToolButton::triggered, this, [this, p]{
        if (p->templateBtn && p->templateBtn->menu()) {
            buildTemplateMenu(p->templateBtn->menu(), p);
            p->templateBtn->showMenu();
        }
    });
    // 先做一次 build 以便菜单首次就能显示
    buildTemplateMenu(tplMenu, p);
    attachBar->addWidget(p->templateBtn);
    attachBar->addStretch();

    // 富文本 / Markdown 切换的容器（Markdown UI 按钮已移除，但保留 bodyStack 以兼容旧草稿）
    p->bodyStack = new QStackedWidget;
    p->bodyStack->addWidget(p->bodyEdit);   // index 0: 富文本
    p->bodyStack->addWidget(p->mdEdit);     // index 1: Markdown 源码
    // 附件工具栏 + 富文本工具栏 + 正文输入框"融为一体"：
    // 包进一个圆角边框的编辑卡片（内部 spacing=0），背景连续、无外部分隔线
    auto* editCard = new QFrame;
    editCard->setObjectName(QStringLiteral("mailEditCard"));
    editCard->setStyleSheet(QStringLiteral(
        "#mailEditCard { background: %1; border: 1px solid %2; border-radius: 8px; }")
        .arg(Theme::kSurfaceAlt, Theme::kBorder));
    auto* editHost = new QVBoxLayout(editCard);
    editHost->setContentsMargins(6, 4, 6, 6);
    editHost->setSpacing(0);
    editHost->addLayout(attachBar);         // 1. 附件工具栏

    // 1.5 平铺附件展示：已添加附件以 chip 横向铺开、宽度不足自动换行，每项带 ✕ 删除按钮
    p->attListHost = new QWidget;
    p->attListHost->setVisible(false);              // 无附件时隐藏
    p->attListLayout = new FlowLayout(p->attListHost, 0, 6, 6);
    p->attListLayout->setContentsMargins(0, 0, 0, 0);
    editHost->addWidget(p->attListHost);

    // 附件区与富文本工具栏之间的一条分隔线：附件工具栏/平铺列表保持卡片样式，用一条线区分下方
    auto* attachSep = new QFrame;
    attachSep->setFrameShape(QFrame::HLine);
    attachSep->setStyleSheet(QStringLiteral("color: %1; max-height: 1px; border: none; border-top: 1px solid %1;").arg(Theme::kBorderLight));
    editHost->addWidget(attachSep);

    editHost->addWidget(bodyToolHost);      // 2. 富文本工具栏（白底，与正文融为一体）
    editHost->addWidget(p->bodyStack, 1);   // 3. 正文编辑区（与富文本工具栏 0 间距）
    eL->addWidget(editCard, 1);

    // 选项行（紧急 / 已读回执 / 加密）放在发送按钮行上方：
    // 富文本工具栏 + bodyStack 已就绪 → 此处插入 → 接着 eBtn
    eL->addLayout(optRow);

    // 接受外部拖入（图片直接 inline，文件直接附件）
    p->bodyEdit->setAcceptDrops(true);
    p->bodyEdit->installEventFilter(this);

    // 附件列表置于主体下方（eL index 1，紧贴 eForm 之后）：添加附件时显示，无附件时隐藏
    p->attachTable = new QTableWidget(0, 4);
    p->attachTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    p->attachTable->setSelectionMode(QAbstractItemView::SingleSelection);
    p->attachTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    p->attachTable->verticalHeader()->setVisible(false);
    p->attachTable->setMaximumHeight(110);   // 防止占满垂直空间
    p->attachTable->horizontalHeader()->setStretchLastSection(false);
    p->attachTable->setColumnWidth(0, 300);
    p->attachTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    QStringList heads = { QStringLiteral("文件名"), QStringLiteral("大小"), QStringLiteral("类型"), QStringLiteral("状态") };
    p->attachTable->setHorizontalHeaderLabels(heads);
    p->attachTable->setContextMenuPolicy(Qt::CustomContextMenu);
    p->attachTable->setStyleSheet(QStringLiteral(
        "QTableWidget { background: %1; gridline-color: %2; color: %3; }"
        "QTableWidget::item:selected { background: %4; color: %5; }")
        .arg(Theme::kSurfaceAlt, Theme::kBorder, Theme::kText, Theme::kAccent, QStringLiteral("#0d1116")));
    connect(p->attachTable, &QTableWidget::customContextMenuRequested,
            this, [this, p](const QPoint& pos){ onAttachmentTableContextMenu(p, pos); });
    connect(p->attachTable, &QTableWidget::cellDoubleClicked,
            this, [this, p](int row, int){ onAttachmentRowDoubleClicked(p, row); });
    // 插入主题（eForm）下方，index=1：eForm → attachTable → editCard → optRow → eBtn
    eL->insertWidget(1, p->attachTable);
    // 无附件时隐藏
    p->attachTable->setVisible(false);

    auto* eBtn = new QHBoxLayout;
    eBtn->setSpacing(8);

    // 定时发送复选框：勾选 = 定时发送（提示设置时间）；未勾选 = 立即发送
    p->scheduleChk = new QCheckBox(QStringLiteral("定时发送"));
    p->scheduleChk->setCursor(Qt::PointingHandCursor);
    p->scheduleChk->setToolTip(QStringLiteral(
        "勾选后先设置发送时间，到点自动发送\n"
        "不勾选 → 发送按钮为\"立即发送\"，点击直接发送"));
    eBtn->addWidget(p->scheduleChk);

    p->sendBtn = new QPushButton(QStringLiteral("立即发送"));
    p->sendBtn->setCursor(Qt::PointingHandCursor);
    p->sendBtn->setStyleSheet(Theme::flatBtnPrimary());
    p->sendBtn->setMinimumWidth(96);

    // 勾选/取消勾选：勾选时提示并设置发送时间；取消勾选即撤销定时
    connect(p->scheduleChk, &QCheckBox::toggled, this, [this, p](bool checked){
        if (!p) return;
        if (checked) {
            // 已有定时则复用，否则提示设置发送时间
            if (!p->scheduledAt.isValid()) {
                if (!pickScheduleTime(p)) {
                    QSignalBlocker blk(p->scheduleChk);
                    p->scheduleChk->setChecked(false);   // 未设时间 → 自动撤勾选退回"立即发送"
                }
            }
        } else {
            p->scheduledAt = QDateTime();                // 取消勾选即取消定时
            if (m_statusLabel) m_statusLabel->setText(QStringLiteral("已取消定时发送，将立即发送。"));
        }
        refreshSendBtnText(p);
    });

    // 发送按钮：勾选定时走定时流程；未勾选直接立即发送
    connect(p->sendBtn, &QPushButton::clicked, this, [this, p]{
        if (!p) return;
        if (!p->scheduleChk->isChecked()) {
            refreshSendBtnText(p);
            onSendClicked();
            return;
        }
        // 已勾选定时但未设时间 → 先提示设置
        if (!p->scheduledAt.isValid()) {
            if (!pickScheduleTime(p)) return;
            refreshSendBtnText(p);
            onSendClicked();
            return;
        }
        // 已有定时 → 让用户确认/重选/取消定时
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QStringLiteral("定时发送"));
        box.setText(QStringLiteral("当前已设置定时发送（%1）。\n\n"
                                   "• \"立即发送\" → 取消定时，马上发送\n"
                                   "• \"重新选择\" → 更改发送时间\n"
                                   "• \"取消定时\" → 撤销定时，保留为未发送草稿\n"
                                   "• 关闭窗口 → 保持当前定时")
                          .arg(p->scheduledAt.toString("yyyy-MM-dd HH:mm:ss")));
        QPushButton* sendNowBtn  = box.addButton(QStringLiteral("立即发送"), QMessageBox::AcceptRole);
        QPushButton* redoBtn     = box.addButton(QStringLiteral("重新选择"), QMessageBox::ActionRole);
        QPushButton* clearTmrBtn = box.addButton(QStringLiteral("取消定时"), QMessageBox::DestructiveRole);
        QPushButton* keepBtn     = box.addButton(QStringLiteral("保持定时"), QMessageBox::RejectRole);
        box.setDefaultButton(keepBtn);
        box.exec();
        QAbstractButton* clicked = box.clickedButton();
        if (clicked == keepBtn) return;
        if (clicked == clearTmrBtn) {
            p->scheduledAt = QDateTime();
            { QSignalBlocker blk(p->scheduleChk); p->scheduleChk->setChecked(false); }
            if (m_statusLabel) m_statusLabel->setText(QStringLiteral("已取消定时发送，邮件保留为未发送草稿。"));
            refreshSendBtnText(p);
            return;
        }
        if (clicked == redoBtn) {
            p->scheduledAt = QDateTime();
            if (!pickScheduleTime(p)) return;
        }
        // 立即发送：把时间清空（让 onSendClicked 走 SMTP 即时分支）
        if (clicked == sendNowBtn) p->scheduledAt = QDateTime();
        refreshSendBtnText(p);
        onSendClicked();
    });
    eBtn->addWidget(p->sendBtn);
    auto* saveDraftBtn = new QPushButton("保存草稿");
    saveDraftBtn->setCursor(Qt::PointingHandCursor);
    saveDraftBtn->setStyleSheet(Theme::flatBtn());
    connect(saveDraftBtn, &QPushButton::clicked, this, &MailWidget::onSaveDraftClicked);
    eBtn->addWidget(saveDraftBtn);
    auto* discardBtn = new QPushButton("放弃");
    discardBtn->setCursor(Qt::PointingHandCursor);
    discardBtn->setStyleSheet(Theme::flatBtnDanger());
    connect(discardBtn, &QPushButton::clicked, this, &MailWidget::onDiscardDraftClicked);
    eBtn->addWidget(discardBtn);
    eBtn->addStretch();
    eL->addLayout(eBtn);

    // 注册：编辑器 stack 页 + 顶部 tab（tabData 存 key 供反查）
    if (m_editorStack) m_editorStack->addWidget(p->panel);
    if (m_filterTabs) {
        int idx = m_filterTabs->addTab(tabTitle);
        m_filterTabs->setTabData(idx, key);
    }
    m_editorPages.append(p);
    return p;
}

MailWidget::EditorPage* MailWidget::findEditorPage(const QString& key) const {
    for (auto* p : m_editorPages)
        if (p->key == key) return p;
    return nullptr;
}

MailWidget::EditorPage* MailWidget::currentEditorPage() const {
    if (!m_filterTabs || m_filterTabs->currentIndex() <= 0) return nullptr;
    return findEditorPage(m_filterTabs->tabData(m_filterTabs->currentIndex()).toString());
}

int MailWidget::editorTabIndex(const EditorPage* p) const {
    if (!m_filterTabs || !p) return -1;
    for (int i = 1; i < m_filterTabs->count(); ++i)
        if (m_filterTabs->tabData(i).toString() == p->key) return i;
    return -1;
}

void MailWidget::activateEditorTab(EditorPage* p) {
    int idx = editorTabIndex(p);
    if (idx < 0) return;
    if (m_filterTabs->currentIndex() != idx)
        m_filterTabs->setCurrentIndex(idx);   // 触发 onFilterTabChanged → 切 stack 页
    else if (m_editorStack)
        m_editorStack->setCurrentWidget(p->panel);
}

void MailWidget::destroyEditorPage(EditorPage* p) {
    if (!p) return;
    int idx = editorTabIndex(p);
    if (idx > 0) m_filterTabs->removeTab(idx);   // 若是当前 tab，QTabBar 自动回落并触发切换
    if (m_editorStack) m_editorStack->removeWidget(p->panel);
    p->panel->deleteLater();
    m_editorPages.removeOne(p);
    delete p;
}

void MailWidget::onEditorTabCloseRequested(int index) {
    closeEditorTab(index);
}

void MailWidget::closeEditorTab(int index) {
    if (!m_filterTabs || index <= 0) return;   // tab 0 = 浏览页，不可关闭
    auto* p = findEditorPage(m_filterTabs->tabData(index).toString());
    if (!p) { m_filterTabs->removeTab(index); return; }

    // 有未保存内容 → 询问（保存草稿 / 直接关闭 / 取消）
    QString bodyText = p->markdownMode
        ? (p->mdEdit ? p->mdEdit->toPlainText() : QString())
        : (p->bodyEdit ? p->bodyEdit->toPlainText() : QString());
    bool dirty = !p->toEdit->text().trimmed().isEmpty()
              || !p->subjectEdit->text().trimmed().isEmpty()
              || !bodyText.trimmed().isEmpty()
              || !p->attachments.isEmpty();
    if (dirty) {
        auto ret = QMessageBox::question(this, "关闭编辑器",
            "当前邮件尚未发送，是否保存为草稿？",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (ret == QMessageBox::Cancel) return;
        if (ret == QMessageBox::Save) saveDraftForPage(p);
    }
    destroyEditorPage(p);
}

bool MailWidget::saveDraftForPage(EditorPage* p) {
    if (!p) return false;
    QString accId = currentAccountId();
    if (accId.isEmpty()) { QMessageBox::warning(this, "提示", "请先选择账号"); return false; }
    MailStore::Draft d;
    d.id = p->draftId;
    d.accountId = accId;
    auto splitAddrs = [](const QString& s){
        QStringList out;
        for (const QString& t : s.split(QRegularExpression("[,;,\xef\xbc\x8c\xef\xbc\x9b\\s]+"), Qt::SkipEmptyParts))
            out << t;
        return out;
    };
    d.to = splitAddrs(p->toEdit->text());
    d.cc = splitAddrs(p->ccEdit->text());
    d.subject = p->subjectEdit->text();
    if (p->markdownMode) {
        // Markdown 模式：保存源码 + 渲染后的 HTML
        QString md = p->mdEdit ? p->mdEdit->toPlainText() : QString();
        d.body    = md;
        d.htmlBody = mail::markdown::toHtmlFragment(md);
        d.markdownMode = true;  // 草稿恢复时识别
    } else {
        d.body    = p->bodyEdit->toPlainText();
        d.htmlBody = p->bodyEdit->toHtml();
        d.markdownMode = false;
    }
    // 附件：保存 path + displayName 两个并列列表
    for (const auto& a : p->attachments) {
        d.attachmentPaths.append(a.filePath);
        d.attachmentDisplayNames.append(a.displayName);
    }
    d.forwardOfId = p->forwardOf;
    d.updatedAt = QDateTime::currentDateTime();
    auto saved = MailStore::instance().upsertDraft(d);
    p->draftId = saved.id;
    if (m_statusLabel)
        m_statusLabel->setText(QString("\xe2\x9c\x93 草稿已保存 (%1)").arg(saved.updatedAt.toString("HH:mm:ss")));
    return true;
}

void MailWidget::showPreviewPage() {
    if (m_filterTabs && m_filterTabs->currentIndex() != 0)
        m_filterTabs->setCurrentIndex(0);
    if (m_contentStack) m_contentStack->setCurrentIndex(0);
}

void MailWidget::fillEditorForNew(EditorPage* p, const QString& accountId) {
    p->contextLabel->setText("新邮件");
    auto* acc = MailAccountManager::instance().getById(accountId);
    if (acc) p->fromEdit->setText(acc->email);
}

void MailWidget::fillEditorForReply(EditorPage* p, const QString& messageId) {
    auto* m = MailStore::instance().message(messageId);
    if (!m) return;
    auto* acc = MailAccountManager::instance().getById(currentAccountId());
    if (acc) p->fromEdit->setText(acc->email);
    p->toEdit->setText(m->from);
    QString subj = m->subject;
    if (!subj.startsWith("Re:", Qt::CaseInsensitive)) subj = "Re: " + subj;
    p->subjectEdit->setText(subj);
    QString quote = "\n\n-------- \xe5\x8e\x9f\xe5\xa7\x8b\xe9\x82\xae\xe4\xbb\xb6 --------\n"
        + QString("发件人: %1\n").arg(m->from)
        + QString("日期: %1\n").arg(m->date.toString("yyyy-MM-dd HH:mm:ss"))
        + QString("%1\n\n").arg(m->subject)
        + m->body;
    p->bodyEdit->setPlainText(quote);
    p->contextLabel->setText(QString("回复: %1").arg(m->subject));
    if (!m->messageId.isEmpty()) p->references = QStringList(m->messageId);
    p->forwardOf = m->id;
}

void MailWidget::fillEditorForReplyAll(EditorPage* p, const QString& messageId) {
    auto* m = MailStore::instance().message(messageId);
    if (!m) return;
    auto* acc = MailAccountManager::instance().getById(currentAccountId());
    if (acc) p->fromEdit->setText(acc->email);
    // 收件人 = 发件人 + 原 to + 原 cc；去重并排除自己（避免自回）
    QStringList rcpts;
    auto addOnce = [&](const QString& s){
        QString t = s.trimmed(); if (t.isEmpty()) return;
        if (acc && t.compare(acc->email, Qt::CaseInsensitive) == 0) return;
        if (!rcpts.contains(t, Qt::CaseInsensitive)) rcpts << t;
    };
    addOnce(m->from);
    for (const auto& s : m->to)  addOnce(s);
    for (const auto& s : m->cc)  addOnce(s);
    p->toEdit->setText(rcpts.join(", "));
    QString subj = m->subject;
    if (!subj.startsWith("Re:", Qt::CaseInsensitive)) subj = "Re: " + subj;
    p->subjectEdit->setText(subj);
    QString quote = "\n\n-------- \xe5\x8e\x9f\xe5\xa7\x8b\xe9\x82\xae\xe4\xbb\xb6 --------\n"
        + QString("发件人: %1\n").arg(m->from)
        + QString("日期: %1\n").arg(m->date.toString("yyyy-MM-dd HH:mm:ss"))
        + QString("%1\n\n").arg(m->subject)
        + m->body;
    p->bodyEdit->setPlainText(quote);
    p->contextLabel->setText(QString("回复全部: %1").arg(m->subject));
    if (!m->messageId.isEmpty()) p->references = QStringList(m->messageId);
    p->forwardOf = m->id;
}

void MailWidget::fillEditorForForward(EditorPage* p, const QString& messageId) {
    auto* m = MailStore::instance().message(messageId);
    if (!m) return;
    auto* acc = MailAccountManager::instance().getById(currentAccountId());
    if (acc) p->fromEdit->setText(acc->email);
    QString subj = m->subject;
    if (!subj.startsWith("Fwd:", Qt::CaseInsensitive) &&
        !subj.startsWith("Fw:",  Qt::CaseInsensitive)) subj = "Fwd: " + subj;
    p->subjectEdit->setText(subj);
    QString body = "\n\n-------- \xe8\xbd\xac\xe5\x8f\x91\xe9\x82\xae\xe4\xbb\xb6 --------\n"
        + QString("发件人: %1\n").arg(m->from)
        + QString("日期: %1\n").arg(m->date.toString("yyyy-MM-dd HH:mm:ss"))
        + QString("%1\n").arg(m->subject);
    if (!m->to.isEmpty()) body += QString("收件人: %1\n").arg(m->to.join(", "));
    if (!m->cc.isEmpty()) body += QString("抄送: %1\n").arg(m->cc.join(", "));
    body += "\n" + m->body;
    p->bodyEdit->setPlainText(body);
    p->contextLabel->setText(QString("转发: %1").arg(m->subject));
    if (!m->messageId.isEmpty()) p->references = QStringList(m->messageId);
    p->forwardOf = m->id;
}

// 通用：把原邮件原始 MIME 写到临时目录并加入编辑器附件，返回写入的本地路径
static QString exportMessageRawToEml(const MailStore::Message* m) {
    if (!m) return {};
    if (m->rawSource.isEmpty()) return {};
    QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    QString sub = QString("mail_fwd_%1").arg(QCoreApplication::applicationPid());
    QDir().mkpath(base + "/" + sub);
    QString safeSubj = m->subject;
    safeSubj.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    if (safeSubj.isEmpty()) safeSubj = QStringLiteral("无主题");
    QString fname = QString("%1/%2_%3.eml")
        .arg(base, sub, safeSubj.left(60));
    QFile f(fname);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(m->rawSource);
    f.close();
    return f.fileName();
}

// "作为附件转发"：原邮件 .eml 作为附件 + 正文直接转发邮件（原邮件内容引文）
void MailWidget::fillEditorForForwardAsAttachment(EditorPage* p, const QString& messageId) {
    auto* m = MailStore::instance().message(messageId);
    if (!m) return;
    auto* acc = MailAccountManager::instance().getById(currentAccountId());
    if (acc) p->fromEdit->setText(acc->email);
    QString subj = m->subject;
    if (!subj.startsWith("Fwd:", Qt::CaseInsensitive) &&
        !subj.startsWith("Fw:",  Qt::CaseInsensitive)) subj = "Fwd: " + subj;
    p->subjectEdit->setText(subj);
    // 正文 = "直接转发邮件"的引文（与默认转发相同）
    QString body = "\n\n-------- \xe8\xbd\xac\xe5\x8f\x91\xe9\x82\xae\xe4\xbb\xb6 --------\n"
        + QString("发件人: %1\n").arg(m->from)
        + QString("日期: %1\n").arg(m->date.toString("yyyy-MM-dd HH:mm:ss"))
        + QString("%1\n").arg(m->subject);
    if (!m->to.isEmpty()) body += QString("收件人: %1\n").arg(m->to.join(", "));
    if (!m->cc.isEmpty()) body += QString("抄送: %1\n").arg(m->cc.join(", "));
    body += "\n" + m->body;
    p->bodyEdit->setPlainText(body);
    // 附件：原邮件原始 MIME → .eml
    QString emlPath = exportMessageRawToEml(m);
    if (!emlPath.isEmpty()) {
        // 避免重复添加
        bool exists = false;
        for (const auto& a : p->attachments) if (a.filePath == emlPath) { exists = true; break; }
        if (!exists) {
            EditorAttachment ea;
            ea.filePath    = emlPath;
            ea.displayName = QFileInfo(emlPath).fileName();
            ea.sizeBytes   = QFileInfo(emlPath).size();
            ea.mimeType    = QStringLiteral("message/rfc822");
            p->attachments.append(ea);
            refreshAttachmentTable(p);
        }
    }
    p->contextLabel->setText(QString("作为附件转发: %1").arg(m->subject));
    if (!m->messageId.isEmpty()) p->references = QStringList(m->messageId);
    p->forwardOf = m->id;
}

// "原件转发"：原邮件 .eml 作为附件 + 正文简短"转发邮件"标识
void MailWidget::fillEditorForForwardOriginal(EditorPage* p, const QString& messageId) {
    auto* m = MailStore::instance().message(messageId);
    if (!m) return;
    auto* acc = MailAccountManager::instance().getById(currentAccountId());
    if (acc) p->fromEdit->setText(acc->email);
    QString subj = m->subject;
    if (!subj.startsWith("Fwd:", Qt::CaseInsensitive) &&
        !subj.startsWith("Fw:",  Qt::CaseInsensitive)) subj = "Fwd: " + subj;
    p->subjectEdit->setText(subj);
    // 正文：简短标识
    p->bodyEdit->setPlainText(QStringLiteral("\n\n-------- \xe8\xbd\xac\xe5\x8f\x91\xe9\x82\xae\xe4\xbb\xb6 --------\n"));
    // 附件：原邮件 .eml
    QString emlPath = exportMessageRawToEml(m);
    if (!emlPath.isEmpty()) {
        bool exists = false;
        for (const auto& a : p->attachments) if (a.filePath == emlPath) { exists = true; break; }
        if (!exists) {
            EditorAttachment ea;
            ea.filePath    = emlPath;
            ea.displayName = QFileInfo(emlPath).fileName();
            ea.sizeBytes   = QFileInfo(emlPath).size();
            ea.mimeType    = QStringLiteral("message/rfc822");
            p->attachments.append(ea);
            refreshAttachmentTable(p);
        }
    }
    p->contextLabel->setText(QString("原件转发: %1").arg(m->subject));
    if (!m->messageId.isEmpty()) p->references = QStringList(m->messageId);
    p->forwardOf = m->id;
}

void MailWidget::fillEditorFromDraft(EditorPage* p, const QString& draftId) {
    auto* d = MailStore::instance().draft(draftId);
    if (!d) return;
    p->draftId = d->id;
    p->forwardOf = d->forwardOfId;
    auto* acc = MailAccountManager::instance().getById(d->accountId);
    if (acc) p->fromEdit->setText(acc->email);
    p->toEdit->setText(d->to.join(", "));
    p->ccEdit->setText(d->cc.join(", "));
    p->subjectEdit->setText(d->subject);
    // 还原 Markdown 模式 / 富文本模式（Markdown UI 已移除，但底层 stack 仍保留以兼容旧草稿）
    if (d->markdownMode) {
        p->markdownMode = true;
        if (p->mdEdit) p->mdEdit->setPlainText(d->body);  // body 字段存的就是 Markdown 源码
        if (p->bodyStack)   p->bodyStack->setCurrentIndex(1);
    } else if (!d->htmlBody.isEmpty()) {
        p->bodyEdit->setHtml(d->htmlBody);
    } else {
        p->bodyEdit->setPlainText(d->body);
    }
    // 还原附件列表（仅 path + displayName；cid/inline 状态未持久化）
    p->attachments.clear();
    for (int i = 0; i < d->attachmentPaths.size(); ++i) {
        EditorAttachment a;
        a.filePath    = d->attachmentPaths[i];
        a.displayName = i < d->attachmentDisplayNames.size()
                            ? d->attachmentDisplayNames[i]
                            : QFileInfo(d->attachmentPaths[i]).fileName();
        QFileInfo fi(a.filePath);
        a.sizeBytes = fi.exists() ? fi.size() : 0;
        a.mimeType  = QMimeDatabase().mimeTypeForFile(a.filePath).name();
        p->attachments.append(a);
    }
    refreshAttachmentTable(p);
    p->contextLabel->setText(QString("草稿 (更新时间 %1)")
        .arg(d->updatedAt.toString("yyyy-MM-dd HH:mm:ss")));
}

// "重新编辑"：把原邮件内容载入写邮件窗口（预填收件人/抄送/主题/正文），
// 保留原措辞（不加 Re:/Fwd: 前缀），供用户修改后重新发送。
void MailWidget::fillEditorForEdit(EditorPage* p, const QString& messageId) {
    auto* m = MailStore::instance().message(messageId);
    if (!m) return;
    auto* acc = MailAccountManager::instance().getById(currentAccountId());
    if (acc) p->fromEdit->setText(acc->email);
    // 收件人预填：原文收件人 → 收件人；原文抄送 → 抄送（如有）
    p->toEdit->setText(m->to.join(", "));
    if (!m->cc.isEmpty()) p->ccEdit->setText(m->cc.join(", "));
    p->subjectEdit->setText(m->subject);
    if (!m->htmlBody.isEmpty()) {
        p->bodyEdit->setHtml(m->htmlBody);
    } else {
        p->bodyEdit->setPlainText(m->body);
    }
    p->contextLabel->setText(QString("重新编辑: %1").arg(m->subject));
    p->references = QStringList(m->messageId);
    p->forwardOf = m->id;
}

void MailWidget::onReeditClicked() {
    QString id = selectedMessageId();
    if (id.isEmpty()) return;
    if (id.startsWith("draft:")) { openDraftInEditor(id.mid(6)); return; } // 草稿直接打开编辑器
    if (id.isEmpty()) return;
    QString key = "reedit:" + id;
    if (auto* p = findEditorPage(key)) { activateEditorTab(p); return; }
    auto* m = MailStore::instance().message(id);
    QString title = m ? QString("重新编辑: %1").arg(m->subject)
                      : QStringLiteral("重新编辑");
    auto* p = createEditorPage(key, title);
    fillEditorForEdit(p, id);
    activateEditorTab(p);
}

// "再次发送"/草稿发送：按原收件人/抄送/主题/正文发一封邮件。
// - 普通邮件：从中栏选中邮件的缓存数据直接重发；
// - 草稿箱：把所选草稿的收件人/抄送/主题/正文/附件真正发送出去。
void MailWidget::onResendClicked() {
    // 草稿箱：使用 draft: 选中 → 取草稿数据发送
    QString draftId = m_listPanel ? m_listPanel->selectedDraftId() : QString();
    if (!draftId.isEmpty()) {
        resendDraft(draftId);
        return;
    }
    // 非草稿：原邮件重发
    QString id = selectedMessageId();
    if (id.isEmpty()) return;
    auto* m = MailStore::instance().message(id);
    if (!m) return;
    QString accId = currentAccountId();
    auto* acc = MailAccountManager::instance().getById(accId);
    if (!acc) { QMessageBox::warning(this, "提示", "请先选择发件账号"); return; }
    if (m->to.isEmpty()) { QMessageBox::warning(this, "提示", "原邮件没有收件人，无法重发"); return; }
    if (acc->smtpHost.isEmpty() || acc->password.isEmpty()) {
        QMessageBox::warning(this, "提示", "当前账号 SMTP 配置不完整"); return;
    }
    auto ret = QMessageBox::question(this, "再次发送",
        QString("按原收件人/主题/正文向 %1 重新发送一封相同的邮件？")
            .arg(m->to.join(", ")),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (ret != QMessageBox::Yes) return;

    SmtpClient::Params params;
    params.host = acc->smtpHost; params.port = acc->smtpPort; params.ssl = acc->smtpSsl;
    params.username = acc->email; params.password = acc->password;
    params.fromName = acc->displayName.isEmpty() ? acc->name : acc->displayName;
    params.fromEmail = acc->email;
    params.to = m->to; params.cc = m->cc;
    params.subject = m->subject;
    params.body    = m->body;
    params.htmlBody = m->htmlBody;
    params.inReplyTo = m->messageId;
    params.references = QStringList(m->messageId);

    if (m_statusLabel) m_statusLabel->setText(QString("正在再次发送至 %1...").arg(m->to.join(", ")));

    const QString senderEmail = acc->email;
    const QString senderAccId = acc->id;
    QStringList   toList      = m->to;
    QStringList   ccList      = m->cc;
    QString       subj        = m->subject;
    QString       body        = m->body;
    QString       htmlBody    = m->htmlBody;

    resendMailAsync(params, senderEmail, senderAccId, toList, ccList, subj, body, htmlBody);
}

// 草稿箱：把所选草稿作为新邮件真正发送（含附件），成功后从草稿箱移除
void MailWidget::resendDraft(const QString& draftId) {
    auto* d = MailStore::instance().draft(draftId);
    if (!d) return;
    auto* acc = MailAccountManager::instance().getById(d->accountId);
    if (!acc) { QMessageBox::warning(this, "提示", "请先选择发件账号"); return; }
    if (d->to.isEmpty()) { QMessageBox::warning(this, "提示", "草稿没有收件人，无法发送"); return; }
    if (acc->smtpHost.isEmpty() || acc->password.isEmpty()) {
        QMessageBox::warning(this, "提示", "当前账号 SMTP 配置不完整"); return;
    }
    auto ret = QMessageBox::question(this, "发送草稿",
        QString("确定将草稿「%1」发送给 %2？\n发送成功后该草稿将从草稿箱移除。")
            .arg(d->subject, d->to.join(", ")),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (ret != QMessageBox::Yes) return;

    SmtpClient::Params params;
    params.host = acc->smtpHost; params.port = acc->smtpPort; params.ssl = acc->smtpSsl;
    params.username = acc->email; params.password = acc->password;
    params.fromName = acc->displayName.isEmpty() ? acc->name : acc->displayName;
    params.fromEmail = acc->email;
    params.to = d->to; params.cc = d->cc; params.bcc = d->bcc;
    params.subject = d->subject;
    params.body    = d->body;
    params.htmlBody = d->htmlBody;
    // 附件
    for (int i = 0; i < d->attachmentPaths.size(); ++i) {
        QString name = i < d->attachmentDisplayNames.size()
                       ? d->attachmentDisplayNames[i] : QString();
        SmtpClient::Attachment a;
        a.filePath = d->attachmentPaths[i];
        a.fileName = name.isEmpty() ? QFileInfo(d->attachmentPaths[i]).fileName() : name;
        params.attachments.append(a);
    }

    if (m_statusLabel) m_statusLabel->setText(QString("正在发送草稿至 %1...").arg(d->to.join(", ")));

    const QString senderEmail = acc->email;
    const QString senderAccId = acc->id;
    QStringList   toList      = d->to;
    QStringList   ccList      = d->cc;
    QString       subj        = d->subject;
    QString       body        = d->body;
    QString       htmlBody    = d->htmlBody;

    resendMailAsync(params, senderEmail, senderAccId, toList, ccList, subj, body, htmlBody, draftId);
}

// 后台发送并存入"已发送"；成功回调通过 QMetaObject 回主线程更新 UI。
// 传值拷贝，所有指针均已解引用为值。
void MailWidget::resendMailAsync(const SmtpClient::Params& params,
                                 const QString& senderEmail, const QString& senderAccId,
                                 QStringList toList, QStringList ccList,
                                 QString subj, QString body, QString htmlBody,
                                 QString draftIdToRemove) {
    QThread* t = QThread::create([this, params, senderEmail, senderAccId, toList, ccList, subj, body, htmlBody, draftIdToRemove]() {
        QString err;
        bool ok = SmtpClient::send(params, &err);
        QMetaObject::invokeMethod(this, [this, ok, err, senderEmail, senderAccId, toList, ccList, subj, body, htmlBody, draftIdToRemove] {
            if (ok) {
                if (m_statusLabel) m_statusLabel->setText("\xe2\x9c\x93 已发送");
                // 若来自草稿：发送成功后从草稿箱移除
                if (!draftIdToRemove.isEmpty())
                    MailStore::instance().removeDraft(draftIdToRemove);
                // 存一封到已发送
                MailStore::Message sent;
                sent.accountId = senderAccId; sent.folder = "Sent";
                sent.from = senderEmail; sent.to = toList; sent.cc = ccList;
                sent.subject = subj;
                sent.body = body; sent.htmlBody = htmlBody;
                sent.date = QDateTime::currentDateTime(); sent.read = true;
                MailStore::instance().upsertMessages({sent});
            } else {
                Logger::instance().error("再次发送失败: " + err, "mail");
                QMessageBox::warning(this, "发送失败", err);
            }
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

// "撤销"：撤回已发送邮件。IMAP 侧将 UID 标记 \Deleted 并 EXPUNGE（应用服务器支持），
// 本地同时移除该邮件的缓存记录。
void MailWidget::onRevokeClicked() {
    QString id = selectedMessageId();
    if (id.isEmpty()) return;
    auto* m = MailStore::instance().message(id);
    if (!m) return;

    auto ret = QMessageBox::question(this, "撤回邮件",
        QString("确定要撤回邮件「%1」吗？\n将尝试从已发送文件夹删除/撤回这封邮件（取决于服务器支持）。")
            .arg(m->subject),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (ret != QMessageBox::Yes) return;

    QString uid    = m->imapUid;
    QString folder = m->folder;
    QString accId  = m->accountId;
    QString subj   = m->subject;
    MailStore::instance().removeMessage(id);

    if (m_statusLabel) m_statusLabel->setText("正在撤回邮件...");

    if (uid.isEmpty()) {
        if (m_statusLabel) m_statusLabel->setText("该邮件无 IMAP UID，仅作本地移除");
        return;
    }

    auto* acc = MailAccountManager::instance().getById(accId);
    if (!acc) return;
    ImapClient::Config cfg;
    cfg.host     = acc->imapHost;
    cfg.port     = acc->imapPort;
    cfg.ssl      = acc->imapSsl;
    cfg.username = acc->email;
    cfg.password = acc->password;
    cfg.timeoutSec = 15;
    QThread* t = QThread::create([cfg, folder, uid, subj]() {
        QString err;
        bool ok = ImapClient::markDeleted(cfg, folder, uid, &err);
        Logger::instance().info(
            QString("撤销(已发送) %1: %2").arg(subj, ok ? "成功" : "失败: " + err), "mail");
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void MailWidget::onNewAccountClicked() {
    AccountDialog dlg("new", {}, this);
    if (dlg.exec() == QDialog::Accepted) {
        MailAccountManager::instance().add(dlg.result());
        if (m_statusLabel) m_statusLabel->setText("账号已添加");
    }
}

void MailWidget::editAccount(const QString& id) {
    if (id.isEmpty()) return;
    auto* acc = MailAccountManager::instance().getById(id);
    if (!acc) return;
    QVariantMap data;
    data["name"]=acc->name; data["email"]=acc->email;
    data["password"]=acc->password; data["smtpHost"]=acc->smtpHost; data["smtpPort"]=acc->smtpPort;
    data["smtpSsl"]=acc->smtpSsl; data["recvProto"]=acc->recvProto;
    data["imapHost"]=acc->imapHost; data["imapPort"]=acc->imapPort; data["imapSsl"]=acc->imapSsl;
    data["calDavEnabled"]=acc->calDavEnabled; data["calDavHost"]=acc->calDavHost;
    data["calDavPort"]=acc->calDavPort; data["calDavSsl"]=acc->calDavSsl; data["calDavUser"]=acc->calDavUser;
    data["isDefault"]=acc->isDefault;
    AccountDialog dlg("edit", data, this);
    if (dlg.exec() == QDialog::Accepted) {
        MailAccountManager::instance().update(id, dlg.result());
        if (m_statusLabel) m_statusLabel->setText("账号已更新");
    }
}

void MailWidget::deleteAccount(const QString& id) {
    if (id.isEmpty()) return;
    auto* acc = MailAccountManager::instance().getById(id);
    if (!acc) return;
    if (QMessageBox::question(this, "删除账号",
            QString("确定要删除账号 \"%1 <%2>\" 吗？\n草稿和缓存的邮件不会被删除。").arg(acc->name, acc->email))
        != QMessageBox::Yes) return;
    MailAccountManager::instance().remove(id);
}

void MailWidget::setDefaultAccount(const QString& id) {
    if (id.isEmpty()) return;
    MailAccountManager::instance().setDefault(id);
    if (m_statusLabel) m_statusLabel->setText("已设为默认账号");
}

void MailWidget::selectAccount(const QString& id) {
    if (m_folderPanel) m_folderPanel->selectAccount(id);
}

void MailWidget::onNewMailClicked() {
    QString id = currentAccountId();
    if (id.isEmpty()) { QMessageBox::warning(this, "提示", "请先新增邮箱账号"); return; }
    QString key = QString("new:%1").arg(++m_editorSeq);
    auto* p = createEditorPage(key, QStringLiteral("写邮件"));
    fillEditorForNew(p, id);
    activateEditorTab(p);
}

void MailWidget::onReplyClicked() {
    QString id = selectedMessageId();
    if (id.isEmpty()) return;
    QString key = "reply:" + id;
    if (auto* p = findEditorPage(key)) { activateEditorTab(p); return; }
    auto* m = MailStore::instance().message(id);
    QString title = m ? QString("回复: %1").arg(m->subject)
                      : QStringLiteral("回复");
    auto* p = createEditorPage(key, title);
    fillEditorForReply(p, id);
    activateEditorTab(p);
}

void MailWidget::onForwardClicked() {
    QString id = selectedMessageId();
    if (id.isEmpty()) return;
    QString key = "fwd:" + id;
    if (auto* p = findEditorPage(key)) { activateEditorTab(p); return; }
    auto* m = MailStore::instance().message(id);
    QString title = m ? QString("转发: %1").arg(m->subject)
                      : QStringLiteral("转发");
    auto* p = createEditorPage(key, title);
    fillEditorForForward(p, id);
    activateEditorTab(p);
}

void MailWidget::onReplyAllClicked() {
    QString id = selectedMessageId();
    if (id.isEmpty()) return;
    QString key = "replyAll:" + id;
    if (auto* p = findEditorPage(key)) { activateEditorTab(p); return; }
    auto* m = MailStore::instance().message(id);
    QString title = m ? QString("回复全部: %1").arg(m->subject)
                      : QStringLiteral("回复全部");
    auto* p = createEditorPage(key, title);
    fillEditorForReplyAll(p, id);
    activateEditorTab(p);
}

void MailWidget::onForwardAsAttachmentClicked() {
    QString id = selectedMessageId();
    if (id.isEmpty()) return;
    QString key = "fwdAtt:" + id;
    if (auto* p = findEditorPage(key)) { activateEditorTab(p); return; }
    auto* m = MailStore::instance().message(id);
    QString title = m ? QString("作为附件转发: %1").arg(m->subject)
                      : QStringLiteral("作为附件转发");
    auto* p = createEditorPage(key, title);
    fillEditorForForwardAsAttachment(p, id);
    activateEditorTab(p);
}

void MailWidget::onForwardOriginalClicked() {
    QString id = selectedMessageId();
    if (id.isEmpty()) return;
    QString key = "fwdOrig:" + id;
    if (auto* p = findEditorPage(key)) { activateEditorTab(p); return; }
    auto* m = MailStore::instance().message(id);
    QString title = m ? QString("原件转发: %1").arg(m->subject)
                      : QStringLiteral("原件转发");
    auto* p = createEditorPage(key, title);
    fillEditorForForwardOriginal(p, id);
    activateEditorTab(p);
}

void MailWidget::openDraftInEditor(const QString& draftId) {
    QString key = "draft:" + draftId;
    if (auto* p = findEditorPage(key)) { activateEditorTab(p); return; }
    auto* d = MailStore::instance().draft(draftId);
    QString title = d ? QString("草稿: %1").arg(d->subject.isEmpty() ? "(无主题)" : d->subject)
                      : QStringLiteral("草稿");
    auto* p = createEditorPage(key, title);
    fillEditorFromDraft(p, draftId);
    activateEditorTab(p);
}

void MailWidget::onDeleteMailClicked() {
    if (!m_listPanel) return;

    // 优先删除复选框勾选的邮件/草稿（批量）；无勾选则删除当前选中
    QStringList checkedMailIds, checkedDraftIds;
    m_listPanel->collectChecked(checkedMailIds, checkedDraftIds);

    // 删除确认：批量（勾选）或单个（当前选中）
    if (!checkedMailIds.isEmpty() || !checkedDraftIds.isEmpty()) {
        auto ret = QMessageBox::question(this, "确认删除",
            QString("确定要删除所选 %1 封邮件、%2 封草稿吗？\n删除后不可恢复。")
                .arg(checkedMailIds.size()).arg(checkedDraftIds.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    } else {
        QString confirmText;
        if (currentFolder() == "Drafts") {
            const QString key  = m_listPanel->selectedKey();
            auto* dd = key.startsWith("draft:")
                       ? MailStore::instance().draft(key.mid(6)) : nullptr;
            confirmText = dd
                ? QString("确定要删除草稿「%1」吗？删除后不可恢复。").arg(dd->subject)
                : QString();
        } else {
            QString id = selectedMessageId();
            if (id.isEmpty()) return;
            auto* mm = MailStore::instance().message(id);
            confirmText = mm
                ? QString("确定要删除邮件「%1」吗？删除后不可恢复。").arg(mm->subject)
                : QString();
        }
        if (confirmText.isEmpty()) return;
        auto ret = QMessageBox::question(this, "确认删除", confirmText,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    }

    if (!checkedMailIds.isEmpty() || !checkedDraftIds.isEmpty()) {
        for (const QString& id : checkedDraftIds)
            MailStore::instance().removeDraft(id);
        // 按 账号→文件夹 分组收集 IMAP 删除参数：跨账号勾选时各用各的凭据，
        // 同账号同文件夹合并为一次连接批量删除（UID STORE 多 uid + EXPUNGE）
        QMap<QString, QMap<QString, QStringList>> imapDels;
        for (const QString& id : checkedMailIds) {
            auto* m = MailStore::instance().message(id);
            if (!m) continue;
            if (!m->imapUid.isEmpty())
                imapDels[m->accountId][m->folder].append(m->imapUid);
            MailStore::instance().removeMessage(id);
        }
        if (!imapDels.isEmpty()) {
            // 主线程组装好每账号的 cfg（账号对象仅主线程可访问），后台线程只碰值拷贝
            struct AccDel { ImapClient::Config cfg; QMap<QString, QStringList> folders; };
            QList<AccDel> dels;
            for (auto it = imapDels.constBegin(); it != imapDels.constEnd(); ++it) {
                auto* acc = MailAccountManager::instance().getById(it.key());
                if (!acc) continue;
                AccDel ad;
                ad.cfg.host     = acc->imapHost;
                ad.cfg.port     = acc->imapPort;
                ad.cfg.ssl      = acc->imapSsl;
                ad.cfg.username = acc->email;
                ad.cfg.password = acc->password;
                ad.cfg.timeoutSec = 15;
                ad.folders = it.value();
                dels.append(ad);
            }
            QThread* t = QThread::create([dels]() {
                QStringList failInfo;
                for (const auto& ad : dels) {
                    for (auto fit = ad.folders.constBegin();
                         fit != ad.folders.constEnd(); ++fit) {
                        QString err;
                        if (ImapClient::markDeletedBatch(ad.cfg, fit.key(), fit.value(), &err)) {
                            Logger::instance().info(
                                QString("IMAP 批量删除成功: %1 x%2 封")
                                    .arg(fit.key()).arg(fit.value().size()), "mail");
                        } else {
                            failInfo << QString("%1: %2").arg(fit.key(), err);
                            Logger::instance().warn(
                                QString("IMAP 批量删除失败: %1 x%2 封: %3")
                                    .arg(fit.key()).arg(fit.value().size()).arg(err), "mail");
                        }
                    }
                }
                if (!failInfo.isEmpty()) {
                    // 服务器删除失败 → 下次同步邮件会"复活"，必须告知用户
                    QString msg = QString("部分邮件服务器删除失败（同步后可能重新出现）：\n%1")
                                      .arg(failInfo.join("\n"));
                    QMetaObject::invokeMethod(qApp, [msg]() {
                        QMessageBox::warning(nullptr, QStringLiteral("删除提示"), msg);
                    }, Qt::QueuedConnection);
                }
            });
            connect(t, &QThread::finished, t, &QObject::deleteLater);
            t->start();
        }
        if (m_statusLabel)
            m_statusLabel->setText(QString("已删除 %1 封邮件、%2 封草稿")
                                       .arg(checkedMailIds.size()).arg(checkedDraftIds.size()));
        return;
    }

    if (currentFolder() == "Drafts") {
        QString key = m_listPanel->selectedKey();
        if (key.startsWith("draft:")) {
            MailStore::instance().removeDraft(key.mid(6));
            if (m_statusLabel) m_statusLabel->setText("草稿已删除");
        }
    } else {
        QString id = selectedMessageId();
        if (id.isEmpty()) return;
        auto* m = MailStore::instance().message(id);
        if (!m) return;
        QString uid    = m->imapUid;
        QString folder = m->folder;
        QString accId  = m->accountId;
        MailStore::instance().removeMessage(id);
        if (m_statusLabel) m_statusLabel->setText("邮件已删除");
        if (!uid.isEmpty()) {
            auto* acc = MailAccountManager::instance().getById(accId);
            if (acc) {
                ImapClient::Config cfg;
                cfg.host     = acc->imapHost;
                cfg.port     = acc->imapPort;
                cfg.ssl      = acc->imapSsl;
                cfg.username = acc->email;
                cfg.password = acc->password;
                cfg.timeoutSec = 15;
                QThread* t = QThread::create([cfg, folder, uid]() {
                    QString err;
                    if (ImapClient::markDeleted(cfg, folder, uid, &err)) {
                        Logger::instance().info(
                            QString("IMAP 删除成功: uid=%1").arg(uid), "mail");
                    } else {
                        Logger::instance().warn(
                            QString("IMAP 删除失败: uid=%1: %2").arg(uid).arg(err), "mail");
                        QString msg = QString("服务器删除失败（同步后该邮件可能重新出现）：\n%1").arg(err);
                        QMetaObject::invokeMethod(qApp, [msg]() {
                            QMessageBox::warning(nullptr, QStringLiteral("删除提示"), msg);
                        }, Qt::QueuedConnection);
                    }
                });
                connect(t, &QThread::finished, t, &QObject::deleteLater);
                t->start();
            }
        }
    }
}

void MailWidget::onAddAttachmentClicked() {
    auto* p = currentEditorPage();
    if (!p) return;
    QStringList files = QFileDialog::getOpenFileNames(this, "选择附件");
    if (files.isEmpty()) return;

    // 去重（同路径不重复添加）
    QSet<QString> existing;
    for (const auto& a : p->attachments) existing.insert(a.filePath);
    int added = 0;
    for (const QString& f : files) {
        if (existing.contains(f)) continue;
        QFileInfo fi(f);
        EditorAttachment a;
        a.filePath    = f;
        a.displayName = fi.fileName();
        a.sizeBytes   = fi.exists() ? fi.size() : 0;
        a.mimeType    = QMimeDatabase().mimeTypeForFile(f).name();
        p->attachments.append(a);
        ++added;
    }
    refreshAttachmentTable(p);
    Logger::instance().info(
        QString("添加附件: 新增 %1, 总数 %2").arg(added).arg(p->attachments.size()), "mail");
}

void MailWidget::onSendClicked() {
    auto* p = currentEditorPage();
    if (!p) return;
    // 发件账号：优先按"发件人(From)"邮箱自动匹配账号，避免仅依赖当前选中账号
    QString fromEmail = p->fromEdit->text().trimmed();
    QString accId = currentAccountId();
    MailAccountManager::Account* acc = nullptr;
    const auto& allAccs = MailAccountManager::instance().accounts();
    for (const auto& a : allAccs) {
        if (!fromEmail.isEmpty() && a.email.compare(fromEmail, Qt::CaseInsensitive) == 0) {
            acc = MailAccountManager::instance().getById(a.id);
            break;
        }
    }
    if (!acc)
        acc = MailAccountManager::instance().getById(accId);
    if (!acc) { QMessageBox::warning(this, "提示", "请先选择发件账号"); return; }
    QString toText = p->toEdit->text().trimmed();
    if (toText.isEmpty()) { QMessageBox::warning(this, "提示", "请填写收件人"); return; }
    if (acc->smtpHost.isEmpty() || acc->password.isEmpty()) {
        QMessageBox::warning(this, "提示",
            QStringLiteral("账号 %1 的 SMTP 配置不完整").arg(acc->email)); return;
    }
    QStringList to, cc;
    auto splitAddrs = [](const QString& s, QStringList& out){
        for (const QString& t : s.split(QRegularExpression("[,;,\xef\xbc\x8c\xef\xbc\x9b\\s]+"), Qt::SkipEmptyParts))
            out << t;
    };
    splitAddrs(toText, to);
    splitAddrs(p->ccEdit->text().trimmed(), cc);

    SmtpClient::Params params;
    params.host = acc->smtpHost; params.port = acc->smtpPort; params.ssl = acc->smtpSsl;
    params.username = acc->email; params.password = acc->password;
    params.fromName = acc->displayName.isEmpty() ? acc->name : acc->displayName;
    params.fromEmail = acc->email;
    params.to = to; params.cc = cc;
    params.subject = p->subjectEdit->text();
    if (p->markdownMode) {
        // Markdown 模式：源码 → HTML 转换后作为 htmlBody，源码本身作为 body fallback
        QString md = p->mdEdit ? p->mdEdit->toPlainText() : QString();
        params.body    = md;
        params.htmlBody = mail::markdown::toHtmlFragment(md);
    } else {
        params.body    = p->bodyEdit->toPlainText();   // 始终生成纯文本 fallback
        params.htmlBody = p->plainText ? QString()
                                       : p->bodyEdit->toHtml();  // "纯文本"勾选则只发纯文本，不发送 HTML
    }
    params.inReplyTo = p->references.value(0);
    params.references = p->references;
    params.priority     = p->urgent ? 1 : 0;
    params.readReceipt  = p->readReceipt;

    // 加密策略
    if (!p->encryptMode.isEmpty() && p->encryptMode != "none") {
        if (p->encryptMode == "auto")        params.encryption.mode = MailEncryptor::Auto;
        else if (p->encryptMode == "smime")  params.encryption.mode = MailEncryptor::Smime;
        else if (p->encryptMode == "password") params.encryption.mode = MailEncryptor::Password;
        params.encryption.password = p->encryptPassword;
    }

    // 拆分附件：已 inline 插入正文的走 inlineImages；其余走 attachments
    for (const auto& a : p->attachments) {
        SmtpClient::Attachment att;
        att.filePath  = a.filePath;
        att.fileName  = a.displayName;
        if (a.insertedInline && !a.contentId.isEmpty()) {
            att.contentId = a.contentId;
            params.inlineImages.append(att);
        } else {
            params.attachments.append(att);
        }
    }

    // ── 定时发送分支：到点之前不入 SMTP 队列，先落盘 ScheduledQueue ──
    if (p->scheduledAt.isValid() && p->scheduledAt > QDateTime::currentDateTime()) {
        ScheduledQueue::Item si;
        si.accountId   = accId;
        si.to          = to;
        si.cc          = cc;
        si.subject     = params.subject;
        si.body        = params.body;
        si.htmlBody    = params.htmlBody;
        si.urgent      = p->urgent;
        si.readReceipt = p->readReceipt;
        si.inReplyTo   = params.inReplyTo;
        si.references  = params.references;
        for (const auto& a : params.attachments) {
            si.attachments.append({a.filePath, a.fileName});
        }
        for (const auto& im : params.inlineImages) {
            si.inlineImages.append({im.filePath, im.fileName, im.contentId});
        }
        si.fireAt      = p->scheduledAt;
        QString id = ScheduledQueue::instance().schedule(si);
        if (id.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("定时发送失败"),
                QStringLiteral("无法写入定时队列磁盘文件，请检查 AppData 目录权限。"));
            p->sendBtn->setEnabled(true);
            refreshSendBtnText(p);
            return;
        }
        if (m_statusLabel) m_statusLabel->setText(
            QStringLiteral("[T] 已加入定时队列: %1 (%2)")
                .arg(id.left(8), p->scheduledAt.toString("yyyy-MM-dd HH:mm:ss")));
        if (!p->draftId.isEmpty())
            MailStore::instance().removeDraft(p->draftId);
        destroyEditorPage(p);
        showPreviewPage();
        return;
    }

    p->sendBtn->setEnabled(false);
    p->sendBtn->setText(QStringLiteral("发送中..."));
    if (m_statusLabel) m_statusLabel->setText(QString("正在发送至 %1...").arg(to.join(", ")));
    QCoreApplication::processEvents();

    QString err;
    bool ok = SmtpClient::send(params, &err);

    if (ok) {
        if (m_statusLabel) m_statusLabel->setText("\xe2\x9c\x93 已发送");
        if (!p->draftId.isEmpty())
            MailStore::instance().removeDraft(p->draftId);
        MailStore::Message sent;
        sent.accountId = acc->id; sent.folder = "Sent";
        sent.from = acc->email; sent.to = to; sent.cc = cc;
        sent.subject = params.subject;
        sent.body     = params.body;
        sent.htmlBody = params.htmlBody;
        sent.date = QDateTime::currentDateTime(); sent.read = true;
        MailStore::instance().upsertMessages({sent});
        destroyEditorPage(p);
        showPreviewPage();
    } else {
        p->sendBtn->setEnabled(true);
        refreshSendBtnText(p);
        if (m_statusLabel) m_statusLabel->setText("\xe2\x9c\x97 发送失败");
        Logger::instance().error("邮件发送失败: " + err, "mail");
        QMessageBox::warning(this, "发送失败", err);
    }
}

void MailWidget::onSaveDraftClicked() {
    auto* p = currentEditorPage();
    if (!p) return;
    if (!saveDraftForPage(p)) return;
    QString subj = p->subjectEdit->text().trimmed();
    int idx = editorTabIndex(p);
    if (idx > 0 && m_filterTabs)
        m_filterTabs->setTabText(idx,
            QString("草稿: %1").arg(subj.isEmpty() ? "(无主题)" : subj));
}

// ─────────────────────────────────────────────────────────────────────────────
// 附件表格相关辅助方法
// ─────────────────────────────────────────────────────────────────────────────

// 把字节数格式化成人类可读字符串
static QString formatSizeShort(qint64 bytes) {
    if (bytes < 0) return QString();
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    double kb = bytes / 1024.0;
    if (kb < 1024) return QStringLiteral("%1 KB").arg(QString::number(kb, 'f', 1));
    double mb = kb / 1024.0;
    if (mb < 1024) return QStringLiteral("%1 MB").arg(QString::number(mb, 'f', 1));
    return QStringLiteral("%1 GB").arg(QString::number(mb / 1024.0, 'f', 2));
}

// 重新构建附件表格 + chip 列表（按 p->attachments 当前内容）。状态列显示"已插入正文"或"—"
// 表格 attachTable 已 hide（用户不可见），仅供旧 moveUp/Down/重命名/复制等函数通过 setCurrentCell/currentRow 引用
// 真正展示给用户的是下方 attListHost（FlowLayout chip）
void MailWidget::refreshAttachmentTable(EditorPage* p) {
    if (!p) return;
    p->attachTable->setRowCount(p->attachments.size());
    for (int i = 0; i < p->attachments.size(); ++i) {
        const auto& a = p->attachments[i];
        auto* nameIt = new QTableWidgetItem(a.displayName);
        nameIt->setData(Qt::UserRole, i);
        nameIt->setToolTip(a.filePath);
        auto* sizeIt = new QTableWidgetItem(formatSizeShort(a.sizeBytes));
        sizeIt->setTextAlignment(Qt::AlignCenter);
        auto* mimeIt = new QTableWidgetItem(a.mimeType);
        mimeIt->setTextAlignment(Qt::AlignCenter);
        QString status = a.insertedInline ? QStringLiteral("已插入正文") : QStringLiteral("—");
        if (a.insertedInline) status = QStringLiteral("\xe2\x9c\x93 ") + status;
        auto* statIt = new QTableWidgetItem(status);
        statIt->setTextAlignment(Qt::AlignCenter);
        p->attachTable->setItem(i, 0, nameIt);
        p->attachTable->setItem(i, 1, sizeIt);
        p->attachTable->setItem(i, 2, mimeIt);
        p->attachTable->setItem(i, 3, statIt);
    }
    if (p->attachTable->rowCount() > 0 && p->attachTable->currentRow() < 0)
        p->attachTable->setCurrentCell(0, 0);

    // 表格始终隐藏（仅保留作右键/排序等旧逻辑的引用），真正展示用 attListHost 平铺 chips
    p->attachTable->setVisible(false);
    // 同步刷新平铺附件展示
    refreshAttachmentList(p);
}

// 平铺附件展示：重建 attListHost 内的 chips。
// 每项显示「📎 文件名 + 大小」，带 ✕ 删除按钮；无附件时隐藏整个区域。
void MailWidget::refreshAttachmentList(EditorPage* p) {
    if (!p || !p->attListLayout) return;
    // 清空旧的 chips（先移出布局再释放）
    while (QLayoutItem* it = p->attListLayout->takeAt(0)) {
        if (QWidget* w = it->widget()) {
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete it;
    }
    if (p->attachments.isEmpty()) {
        p->attListHost->setVisible(false);
        return;
    }
    p->attListHost->setVisible(true);
    for (int i = 0; i < p->attachments.size(); ++i) {
        const auto& a = p->attachments[i];
        auto* chip = new QFrame(p->attListHost);
        chip->setObjectName(QStringLiteral("mailAttChip"));
        chip->setStyleSheet(QStringLiteral(
            "#mailAttChip { background: %1; border: 1px solid %2; border-radius: 4px; }")
            .arg(Theme::kSurface, Theme::kBorder));
        auto* hl = new QHBoxLayout(chip);
        hl->setContentsMargins(6, 2, 4, 2);
        hl->setSpacing(4);

        auto* iconLbl = new QLabel(QStringLiteral("📎"), chip);
        iconLbl->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        hl->addWidget(iconLbl);

        auto* nameLbl = new QLabel(a.displayName, chip);
        nameLbl->setStyleSheet(QStringLiteral("background: transparent; border: none; color: %1;").arg(Theme::kText));
        nameLbl->setToolTip(a.filePath);
        hl->addWidget(nameLbl);

        auto* sizeLbl = new QLabel(formatSizeShort(a.sizeBytes), chip);
        sizeLbl->setStyleSheet(QStringLiteral("background: transparent; border: none; color: %1;").arg(Theme::kMuted));
        hl->addWidget(sizeLbl);

        auto* delBtn = new QToolButton(chip);
        delBtn->setText(QStringLiteral("✕"));
        delBtn->setCursor(Qt::PointingHandCursor);
        delBtn->setFixedSize(18, 18);
        delBtn->setStyleSheet(QStringLiteral(
            "QToolButton { background: transparent; color: %1; border: none; border-radius: 3px; font-size: 12px; }"
            "QToolButton:hover { background: %2; color: #ffffff; }")
            .arg(Theme::kMuted, Theme::kAccent));
        delBtn->setToolTip(QStringLiteral("删除该附件"));
        connect(delBtn, &QToolButton::clicked, this, [this, p, i]{
            removeAttachmentFromList(p, i);
        });
        hl->addWidget(delBtn);

        p->attListLayout->addWidget(chip);
        chip->updateGeometry();
    }
}

// 附件表格右键菜单：预览 / 打开 / 在文件管理器显示 / 重命名 / 复制 / 移动 / 删除 / 上移 / 下移
void MailWidget::onAttachmentTableContextMenu(EditorPage* p, const QPoint& pos) {
    if (!p || !p->attachTable) return;
    int row = p->attachTable->rowAt(pos.y());
    if (row < 0 || row >= p->attachments.size()) return;
    p->attachTable->setCurrentCell(row, 0);

    auto* menu = new QMenu(p->attachTable);
    auto* previewAct  = menu->addAction(QStringLiteral("预览"));
    auto* openAct     = menu->addAction(QStringLiteral("打开"));
    auto* revealAct   = menu->addAction(QStringLiteral("在文件管理器中显示"));
    menu->addSeparator();
    auto* renameAct   = menu->addAction(QStringLiteral("重命名（仅列表显示）"));
    auto* copyAct     = menu->addAction(QStringLiteral("复制（仅列表项）"));
    auto* moveUpAct   = menu->addAction(QStringLiteral("上移"));
    auto* moveDownAct = menu->addAction(QStringLiteral("下移"));
    // 排序子菜单：按文件名 / 大小 / 类型 / 状态
    auto* sortMenu = menu->addMenu(QStringLiteral("排序"));
    auto* sortByNameAct    = sortMenu->addAction(QStringLiteral("按文件名（A→Z）"));
    auto* sortBySizeAct    = sortMenu->addAction(QStringLiteral("按大小（小→大）"));
    auto* sortByTypeAct    = sortMenu->addAction(QStringLiteral("按类型"));
    auto* sortByInlineAct  = sortMenu->addAction(QStringLiteral("已插入正文图片优先"));
    auto* sortByOriginalAct= sortMenu->addAction(QStringLiteral("恢复原始顺序"));
    menu->addSeparator();
    auto* inlineAct   = menu->addAction(QStringLiteral("插入正文（图片，cid:）"));
    auto* removeAct   = menu->addAction(QStringLiteral("删除"));

    const auto& a = p->attachments[row];
    bool isImage = a.mimeType.startsWith(QStringLiteral("image/"));
    inlineAct->setEnabled(isImage && !a.insertedInline);

    connect(previewAct, &QAction::triggered, this, [this, p, row]{ previewAttachment(p, row); });
    connect(openAct,    &QAction::triggered, this, [this, p, row]{
        const auto& a = p->attachments[row];
        QDesktopServices::openUrl(QUrl::fromLocalFile(a.filePath));
    });
    connect(revealAct,  &QAction::triggered, this, [this, p, row]{
        const auto& a = p->attachments[row];
        // Windows 下用 explorer /select；其它平台用 xdg-open 目录
#ifdef Q_OS_WIN
        QProcess::startDetached(QStringLiteral("explorer.exe"),
            { QStringLiteral("/select,"), QDir::toNativeSeparators(a.filePath) });
#else
        QFileInfo fi(a.filePath);
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
#endif
    });
    connect(renameAct,  &QAction::triggered, this, [this, p, row]{ renameAttachmentInList(p, row); });
    connect(copyAct,    &QAction::triggered, this, [this, p, row]{ duplicateAttachmentInList(p, row); });
    connect(moveUpAct,  &QAction::triggered, this, [this, p, row]{
        if (row <= 0) return;
        p->attachments.swapItemsAt(row, row - 1);
        refreshAttachmentTable(p);
        p->attachTable->setCurrentCell(row - 1, 0);
    });
    connect(moveDownAct,&QAction::triggered, this, [this, p, row]{
        if (row >= p->attachments.size() - 1) return;
        p->attachments.swapItemsAt(row, row + 1);
        refreshAttachmentTable(p);
        p->attachTable->setCurrentCell(row + 1, 0);
    });
    connect(inlineAct,   &QAction::triggered, this, [this, p, row]{ insertAttachmentInline(p, row); });
    connect(removeAct,  &QAction::triggered, this, [this, p, row]{ removeAttachmentFromList(p, row); });

    // 排序 handlers
    connect(sortByNameAct, &QAction::triggered, this, [this, p]{
        std::sort(p->attachments.begin(), p->attachments.end(),
                  [](const EditorAttachment& a, const EditorAttachment& b){
                      return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
                  });
        refreshAttachmentTable(p);
    });
    connect(sortBySizeAct, &QAction::triggered, this, [this, p]{
        std::sort(p->attachments.begin(), p->attachments.end(),
                  [](const EditorAttachment& a, const EditorAttachment& b){
                      return a.sizeBytes < b.sizeBytes;
                  });
        refreshAttachmentTable(p);
    });
    connect(sortByTypeAct, &QAction::triggered, this, [this, p]{
        std::sort(p->attachments.begin(), p->attachments.end(),
                  [](const EditorAttachment& a, const EditorAttachment& b){
                      return a.mimeType.compare(b.mimeType) < 0;
                  });
        refreshAttachmentTable(p);
    });
    connect(sortByInlineAct, &QAction::triggered, this, [this, p]{
        std::sort(p->attachments.begin(), p->attachments.end(),
                  [](const EditorAttachment& a, const EditorAttachment& b){
                      // 已插入正文的图片优先
                      return (a.insertedInline && !b.insertedInline)
                          || (a.insertedInline == b.insertedInline
                              && a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0);
                  });
        refreshAttachmentTable(p);
    });
    connect(sortByOriginalAct, &QAction::triggered, this, [this, p]{
        // 恢复拖入时的顺序：按 filePath 在首次出现位置排序（路径排序的稳定排序保持相对顺序）
        // 简化实现：用 stable_sort 按 filePath 字典序，实际等价于先拖入优先
        std::stable_sort(p->attachments.begin(), p->attachments.end(),
                  [](const EditorAttachment& a, const EditorAttachment& b){
                      return a.filePath.compare(b.filePath) < 0;
                  });
        refreshAttachmentTable(p);
    });

    menu->exec(p->attachTable->viewport()->mapToGlobal(pos));
    delete menu;
}

// 双击：图片弹预览窗口；其他用系统默认应用打开
void MailWidget::onAttachmentRowDoubleClicked(EditorPage* p, int row) {
    if (!p || row < 0 || row >= p->attachments.size()) return;
    previewAttachment(p, row);
}

// 预览附件：图片用 QLabel 弹窗；其他用系统默认应用
void MailWidget::previewAttachment(EditorPage* p, int row) {
    if (!p || row < 0 || row >= p->attachments.size()) return;
    const auto& a = p->attachments[row];
    if (!QFile::exists(a.filePath)) {
        QMessageBox::warning(this, QStringLiteral("预览失败"),
            QStringLiteral("文件不存在: %1").arg(a.filePath));
        return;
    }
    if (a.mimeType.startsWith(QStringLiteral("image/"))) {
        auto* dlg = new QDialog(this);
        dlg->setWindowTitle(a.displayName);
        dlg->resize(800, 600);
        auto* lay = new QVBoxLayout(dlg);
        auto* lab = new QLabel;
        QPixmap pm(a.filePath);
        if (pm.isNull()) {
            delete dlg;
            QMessageBox::warning(this, QStringLiteral("预览失败"),
                QStringLiteral("无法加载图片: %1").arg(a.filePath));
            return;
        }
        lab->setPixmap(pm.scaled(780, 580, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        lay->addWidget(lab);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    } else {
        QDesktopServices::openUrl(QUrl::fromLocalFile(a.filePath));
    }
}

// 重命名（仅列表显示）：弹 QInputDialog，不动磁盘
void MailWidget::renameAttachmentInList(EditorPage* p, int row) {
    if (!p || row < 0 || row >= p->attachments.size()) return;
    auto& a = p->attachments[row];
    bool ok = false;
    QString newName = QInputDialog::getText(this, QStringLiteral("重命名（仅列表显示）"),
        QStringLiteral("新的显示名（磁盘文件不会改动）："), QLineEdit::Normal, a.displayName, &ok);
    if (!ok || newName.trimmed().isEmpty()) return;
    a.displayName = newName.trimmed();
    refreshAttachmentTable(p);
    Logger::instance().info(QStringLiteral("附件列表项重命名: %1 -> %2").arg(a.filePath, a.displayName), "mail");
}

// 复制（仅列表项）：同路径再加一条，displayName 加 " (副本)"
void MailWidget::duplicateAttachmentInList(EditorPage* p, int row) {
    if (!p || row < 0 || row >= p->attachments.size()) return;
    EditorAttachment copy = p->attachments[row];
    copy.displayName = copy.displayName + QStringLiteral(" (副本)");
    copy.contentId.clear();
    copy.insertedInline = false;
    p->attachments.insert(row + 1, copy);
    refreshAttachmentTable(p);
    Logger::instance().info(QStringLiteral("复制列表项: %1").arg(copy.filePath), "mail");
}

// 删除（列表项 + 同步正文里的 cid 引用）
void MailWidget::removeAttachmentFromList(EditorPage* p, int row) {
    if (!p || row < 0 || row >= p->attachments.size()) return;
    EditorAttachment a = p->attachments.takeAt(row);
    // 若该附件已被 inline 插入，从正文里把对应 cid 图片换成"已移除"占位
    if (!a.contentId.isEmpty() && p->bodyEdit) {
        QTextDocument* doc = p->bodyEdit->document();
        QString html = doc->toHtml();
        QString pat  = QStringLiteral("src=\"cid:%1\"").arg(a.contentId);
        QString repl = QStringLiteral("alt=\"[已移除:%1]\"")
            .arg(a.displayName.toHtmlEscaped());
        html.replace(pat, repl);
        doc->setHtml(html);
    }
    refreshAttachmentTable(p);
    Logger::instance().info(QStringLiteral("删除列表项: %1").arg(a.filePath), "mail");
}

// "插入正文(图片)" 按钮 / 右键菜单：把选中图片附件以 <img src="cid:xxx"> 插入正文
void MailWidget::insertAttachmentInline(EditorPage* p, int row) {
    if (!p || row < 0 || row >= p->attachments.size()) return;
    auto& a = p->attachments[row];
    if (!a.mimeType.startsWith(QStringLiteral("image/"))) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("仅图片附件可插入正文（cid: 引用）。"));
        return;
    }
    if (a.insertedInline) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("该附件已插入正文。"));
        return;
    }
    if (a.contentId.isEmpty()) {
        a.contentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    QString html = QStringLiteral("<img src=\"cid:%1\" alt=\"%2\" />")
        .arg(a.contentId, a.displayName.toHtmlEscaped());
    p->bodyEdit->insertHtml(html);
    a.insertedInline = true;
    refreshAttachmentTable(p);
    Logger::instance().info(QStringLiteral("插入正文(cid): %1 cid=%2")
        .arg(a.filePath, a.contentId), "mail");
}

// 工具栏"插入图片"按钮：从文件选图，inline 插入同时加入 attachments
void MailWidget::onInsertInlineImageClicked(EditorPage* p) {
    if (!p) return;
    QString f = QFileDialog::getOpenFileName(this, QStringLiteral("选择图片"),
        QString(), QStringLiteral("图片 (*.png *.jpg *.jpeg *.gif *.bmp *.webp *.svg)"));
    if (f.isEmpty()) return;
    QFileInfo fi(f);
    EditorAttachment a;
    a.filePath    = f;
    a.displayName = fi.fileName();
    a.sizeBytes   = fi.size();
    a.mimeType    = QMimeDatabase().mimeTypeForFile(f).name();
    a.contentId   = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p->attachments.append(a);
    p->bodyEdit->insertHtml(QStringLiteral("<img src=\"cid:%1\" alt=\"%2\" />")
        .arg(a.contentId, a.displayName.toHtmlEscaped()));
    a.insertedInline = true;
    p->attachments.last().insertedInline = true;
    refreshAttachmentTable(p);
}

// "插入正文(图片)" 工具栏按钮：从图片附件列表中选一张插入正文（不再依赖附件表格）
void MailWidget::onInsertAttachmentInlineClicked(EditorPage* p) {
    if (!p) return;
    // 收集未插入的图片附件
    struct ImgItem { int idx; QString name; };
    QList<ImgItem> imgs;
    for (int i = 0; i < p->attachments.size(); ++i) {
        const auto& a = p->attachments[i];
        if (a.mimeType.startsWith(QStringLiteral("image/"))) imgs.append({i, a.displayName});
    }
    if (imgs.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("当前没有图片附件，请先添加图片文件后再插入正文。"));
        return;
    }
    if (imgs.size() == 1) { insertAttachmentInline(p, imgs.first().idx); return; }
    QStringList names;
    for (const auto& it : imgs) names << it.name;
    bool ok = false;
    QString pick = QInputDialog::getItem(this, QStringLiteral("选择插入正文的图片"),
                                        QStringLiteral("多张图片附件，选择要插入正文的那一张："),
                                        names, 0, false, &ok);
    if (!ok || pick.isEmpty()) return;
    int idx = names.indexOf(pick);
    if (idx < 0) return;
    insertAttachmentInline(p, imgs[idx].idx);
}

// ─────────────────────────────────────────────────────────────────────────────
// 定时发送：弹时间选择 → 写入 ScheduledQueue（发送按钮入口）
// ─────────────────────────────────────────────────────────────────────────────

bool MailWidget::pickScheduleTime(EditorPage* p) {
    if (!p) return false;
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("选择定时发送时间"));
    QVBoxLayout* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel(QStringLiteral("邮件将在指定时间发送（选择当前/过去时间即立即发送）：")));
    QDateTimeEdit* dt = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(300), &dlg);
    dt->setCalendarPopup(true);
    dt->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    dt->setMinimumDateTime(QDateTime::currentDateTime().addSecs(-3600)); // 允许选过去时间（=立即发）
    v->addWidget(dt);
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return false;
    p->scheduledAt = dt->dateTime();
    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("[T] 定时发送: %1")
            .arg(p->scheduledAt.toString("yyyy-MM-dd HH:mm:ss")));
    }
    refreshSendBtnText(p);
    return true;
}

// 根据"定时发送"复选框 + scheduledAt 同步发送按钮文案：
// 勾选并已设时间 → 显示时间；勾选未设时间 → "定时发送"；未勾选 → "立即发送"
void MailWidget::refreshSendBtnText(EditorPage* p) {
    if (!p || !p->sendBtn) return;
    bool chk = p->scheduleChk != nullptr && p->scheduleChk->isChecked();
    // 已有有效定时但未勾选（如草稿还原）→ 自动勾选
    if (!chk && p->scheduleChk && p->scheduledAt.isValid()
        && p->scheduledAt > QDateTime::currentDateTime()) {
        QSignalBlocker blk(p->scheduleChk);
        p->scheduleChk->setChecked(true);
        chk = true;
    }
    if (chk && p->scheduledAt.isValid() && p->scheduledAt > QDateTime::currentDateTime()) {
        p->sendBtn->setText(QStringLiteral("定时 %1")
            .arg(p->scheduledAt.toString("MM-dd HH:mm")));
        p->sendBtn->setToolTip(QStringLiteral("已定时在 %1 发送。\n点击可立即发送 / 重新选择 / 取消定时。")
            .arg(p->scheduledAt.toString("yyyy-MM-dd HH:mm:ss")));
    } else if (chk) {
        p->sendBtn->setText(QStringLiteral("定时发送"));
        p->sendBtn->setToolTip(QStringLiteral("已勾选定时发送，点击将先提示设置发送时间。"));
    } else {
        p->sendBtn->setText(QStringLiteral("立即发送"));
        p->sendBtn->setToolTip(QStringLiteral("点击直接发送（走 SMTP）。"));
    }
}

void MailWidget::onDiscardDraftClicked() {
    if (!m_filterTabs) return;
    int idx = m_filterTabs->currentIndex();
    if (idx > 0) closeEditorTab(idx);
}

// ─────────────────────────────────────────────────────────────────────────────
// 已读回执（DSN）接收方：用户点击查看带 Disposition-Notification-To 头的邮件时，
// MailStore::readReceiptRequested 触发 → 按全局策略决定动作（弹窗 / 自动发 / 忽略）→ 同意后用当前账号发 multipart/report。
// ─────────────────────────────────────────────────────────────────────────────
void MailWidget::onReadReceiptRequested(const QString& msgId, const QString& rto) {
    auto* m = MailStore::instance().message(msgId);
    if (!m) return;
    auto* acc = MailAccountManager::instance().getById(currentAccountId());
    if (!acc || acc->smtpHost.isEmpty() || acc->password.isEmpty()) {
        // 没有可用账号发回执 → 仅提示，不强行发
        Logger::instance().warn(
            QStringLiteral("DSN: 当前无可用账号发送回执 (msgId=%1 rto=%2)")
                .arg(msgId, rto), "mail");
        return;
    }

    // 全局策略
    const QString policy = Settings::instance().readReceiptPolicy(); // always / ask / never
    if (policy == "never") {
        Logger::instance().info(
            QStringLiteral("DSN: 全局策略=never，忽略回执请求 msgId=%1 rto=%2").arg(msgId, rto), "mail");
        return;
    }
    if (policy != "always") {
        // 弹窗询问（ask / 其它 兜底）
        QString fromText = QStringLiteral("%1 &lt;%2&gt;")
            .arg(m->from.isEmpty() ? QStringLiteral("?") : m->from)
            .arg(rto);
        QString info = QStringLiteral(
            "<p><b>发件人请求已读回执</b></p>"
            "<p>主题: %1<br>发件人: %2</p>"
            "<p>是否向回执地址 <code>%3</code> 发送 \"已读\" 通知？</p>"
            "<p><small>（可在 设置 → 邮件 中修改全局策略）</small></p>")
            .arg(m->subject.toHtmlEscaped(),
                 fromText,
                 rto.toHtmlEscaped());
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QStringLiteral("已读回执"));
        box.setText(QStringLiteral("发件人请求已读回执，是否发送？"));
        box.setInformativeText(info);
        box.setTextFormat(Qt::RichText);
        QPushButton* yesBtn = box.addButton(QStringLiteral("发送回执"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("忽略"), QMessageBox::RejectRole);
        box.setDefaultButton(yesBtn);
        box.exec();
        if (box.clickedButton() != yesBtn) return;
    } else {
        Logger::instance().info(
            QStringLiteral("DSN: 全局策略=always，自动发送回执 msgId=%1 rto=%2").arg(msgId, rto), "mail");
    }

    // 构造 DSN 邮件
    SmtpClient::Params params;
    params.host      = acc->smtpHost;
    params.port      = acc->smtpPort;
    params.ssl       = acc->smtpSsl;
    params.username  = acc->email;
    params.password  = acc->password;
    params.fromName  = acc->displayName.isEmpty() ? acc->name : acc->displayName;
    params.fromEmail = acc->email;
    params.to        = QStringList{rto};
    params.subject   = QStringLiteral("Read receipt: %1").arg(m->subject);
    params.body      = QStringLiteral("Your message has been read.");
    params.inReplyTo = m->messageId;   // 关联原邮件
    params.isDispositionNotification = true;
    params.dsnOriginalFrom       = m->from;
    params.dsnOriginalSubject    = m->subject;
    params.dsnOriginalMessageId  = m->messageId;

    QString err;
    bool ok = SmtpClient::send(params, &err);
    if (ok) {
        if (m_statusLabel) m_statusLabel->setText(
            QStringLiteral("\xe2\x9c\x93 已发送已读回执 -> %1").arg(rto));
        Logger::instance().info(
            QStringLiteral("DSN: 已发送已读回执 msgId=%1 rto=%2").arg(msgId, rto), "mail");
    } else {
        QMessageBox::warning(this, QStringLiteral("回执发送失败"), err);
        Logger::instance().error(
            QStringLiteral("DSN: 回执发送失败 msgId=%1 err=%2").arg(msgId, err), "mail");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 模板相关
// ─────────────────────────────────────────────────────────────────────────────

void MailWidget::buildTemplateMenu(QMenu* menu, EditorPage* p) {
    if (!menu || !p) return;
    menu->clear();
    auto& store = mail::TemplateStore::instance();

    // 分类组织：{ 分类名 → [模板] }
    QHash<QString, QList<mail::Template>> byCat;
    QStringList catOrder;
    for (const auto& t : store.listAll()) {
        if (!byCat.contains(t.category)) catOrder << t.category;
        byCat[t.category].append(t);
    }

    if (catOrder.isEmpty()) {
        auto* noTpl = menu->addAction(QStringLiteral("(暂无模板)"));
        noTpl->setEnabled(false);
    } else {
        for (const QString& cat : catOrder) {
            QMenu* sub = menu->addMenu(cat);
            for (const auto& t : byCat[cat]) {
                QString label = t.name;
                if (!t.isBuiltin) label += QStringLiteral(" (用户)");
                QAction* act = sub->addAction(label);
                act->setToolTip(t.description);
                QString tplId = t.id;
                connect(act, &QAction::triggered, this, [this, p, tplId]{
                    auto* tp = mail::TemplateStore::instance().findById(tplId);
                    if (!tp) return;
                    applyTemplateToPage(p, *tp);
                });
                if (!t.isBuiltin) {
                    // 用户模板右键区域留空（保留二级菜单入口）
                }
            }
        }
        menu->addSeparator();
        // 用户模板管理（仅在有用户模板时显示"删除最近一个"，否则灰色）
        auto userList = store.listUser();
        if (!userList.isEmpty()) {
            auto* delMenu = menu->addMenu(QStringLiteral("删除用户模板"));
            for (const auto& t : userList) {
                QString label = t.name + QStringLiteral("  (") + t.updatedAt.toString(QStringLiteral("MM-dd HH:mm")) + QStringLiteral(")");
                QAction* a = delMenu->addAction(label);
                QString id = t.id;
                connect(a, &QAction::triggered, this, [this, p, id]{
                    removeUserTemplateById(id, p);
                });
            }
        }
    }

    menu->addSeparator();
    auto* saveAct = menu->addAction(QStringLiteral("保存 把当前邮件另存为模板…"));
    connect(saveAct, &QAction::triggered, this, [this, p]{
        saveCurrentPageAsTemplate(p);
    });
    auto* resetAct = menu->addAction(QStringLiteral("刷新 重新载入内置模板"));
    connect(resetAct, &QAction::triggered, this, [this]{
        mail::TemplateStore::instance().reload();
        QMessageBox::information(this, QStringLiteral("模板"),
            QStringLiteral("已重新载入内置模板。\n如内置模板定义更新，下次启动会自动生效。"));
    });
}

void MailWidget::applyTemplateToPage(EditorPage* p, const mail::Template& tpl) {
    if (!p) return;
    QString rcpt = p->toEdit ? p->toEdit->text() : QString();
    QString sender;
    if (p->fromEdit) {
        QString fullFrom = p->fromEdit->text();
        int lt = fullFrom.indexOf(QLatin1Char('<'));
        int gt = fullFrom.lastIndexOf(QLatin1Char('>'));
        if (lt >= 0 && gt > lt) sender = fullFrom.mid(lt + 1, gt - lt - 1);
        else sender = fullFrom;
    }
    QHash<QString,QString> extras;
    extras[QStringLiteral("name")] = QStringLiteral("");  // 让 Settings 兜底

    QString subj = mail::TemplateStore::applyPlaceholders(tpl.subject, rcpt, sender, extras);
    QString body = mail::TemplateStore::applyPlaceholders(tpl.body, rcpt, sender, extras);

    if (p->subjectEdit) p->subjectEdit->setText(subj);
    // 如果用户已在编辑器输入了内容，给个确认
    QString existing = p->markdownMode
        ? (p->mdEdit ? p->mdEdit->toPlainText() : QString())
        : (p->bodyEdit ? p->bodyEdit->toPlainText() : QString());
    if (!existing.trimmed().isEmpty()) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QStringLiteral("应用模板"));
        box.setText(QStringLiteral("当前正文已有内容，是否覆盖？"));
        box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box.setButtonText(QMessageBox::Yes, QStringLiteral("覆盖"));
        box.setButtonText(QMessageBox::No,  QStringLiteral("取消"));
        if (box.exec() != QMessageBox::Yes) return;
    }
    if (p->markdownMode) {
        if (p->mdEdit) p->mdEdit->setPlainText(body);
    } else {
        // 模板正文是 Markdown 源码 → 渲染为 HTML
        QString html = mail::markdown::toHtmlFragment(body);
        if (p->bodyEdit) p->bodyEdit->setHtml(html);
    }
    // 焦点移到正文开头，让用户接着写
    if (p->bodyEdit) {
        QTextCursor c = p->bodyEdit->textCursor();
        c.movePosition(QTextCursor::Start);
        p->bodyEdit->setTextCursor(c);
        p->bodyEdit->setFocus();
    } else if (p->mdEdit) {
        QTextCursor c = p->mdEdit->textCursor();
        c.movePosition(QTextCursor::Start);
        p->mdEdit->setTextCursor(c);
        p->mdEdit->setFocus();
    }

    Logger::instance().info(
        QStringLiteral("应用模板: %1 (id=%2)").arg(tpl.name, tpl.id), "mail");
    if (m_statusLabel) m_statusLabel->setText(
        QStringLiteral("✓ 已应用模板: %1").arg(tpl.name));
}

void MailWidget::saveCurrentPageAsTemplate(EditorPage* p) {
    if (!p) return;
    QString body = p->markdownMode
        ? (p->mdEdit ? p->mdEdit->toPlainText() : QString())
        : (p->bodyEdit ? p->bodyEdit->toPlainText() : QString());
    if (p->subjectEdit->text().trimmed().isEmpty() && body.trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("保存模板"),
            QStringLiteral("主题与正文都为空，无法保存模板。"));
        return;
    }
    bool ok = false;
    QString name = QInputDialog::getText(this, QStringLiteral("保存为模板"),
        QStringLiteral("模板名称："), QLineEdit::Normal,
        p->subjectEdit->text().left(40), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    QString cat = QInputDialog::getText(this, QStringLiteral("保存为模板"),
        QStringLiteral("分类（如 业务/投诉/自定义）："),
        QLineEdit::Normal, QStringLiteral("自定义"), &ok);
    if (!ok) cat = QStringLiteral("自定义");

    mail::Template t;
    t.name = name.trimmed();
    t.category = cat.trimmed().isEmpty() ? QStringLiteral("自定义") : cat.trimmed();
    t.subject = p->subjectEdit->text();
    t.body    = body;  // 存 Markdown 源码（与编辑器保持一致）
    QString newId = mail::TemplateStore::instance().addUserTemplate(t);
    if (!newId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("保存模板"),
            QStringLiteral("已保存模板：%1\n下次可在「模板 ▾」菜单中选择。").arg(t.name));
    }
}

void MailWidget::removeUserTemplateById(const QString& id, EditorPage* p) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("删除模板"));
    box.setText(QStringLiteral("确定删除这个用户模板？\n此操作无法撤销。"));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setButtonText(QMessageBox::Yes, QStringLiteral("删除"));
    box.setButtonText(QMessageBox::No,  QStringLiteral("取消"));
    if (box.exec() != QMessageBox::Yes) return;
    if (mail::TemplateStore::instance().removeUserTemplate(id)) {
        // 重建菜单
        if (p && p->templateBtn && p->templateBtn->menu()) {
            buildTemplateMenu(p->templateBtn->menu(), p);
        }
        QMessageBox::information(this, QStringLiteral("删除模板"),
            QStringLiteral("已删除。"));
    }
}

void MailWidget::onRefreshClicked() {
    const auto& accounts = MailAccountManager::instance().accounts();
    QStringList syncable;   // 可同步文件夹的账号
    for (const auto& a : accounts) {
        if (a.recvProto != "SMTP" && !a.imapHost.isEmpty() && !a.password.isEmpty()) {
            syncable.append(a.id);
        }
    }
    if (syncable.isEmpty()) {
        Logger::instance().warn("onRefreshClicked: 无可刷新账号", "mail");
        if (m_statusLabel) m_statusLabel->setText("请先添加邮箱账号");
        return;
    }
    Logger::instance().info(
        QString("onRefreshClicked: 先同步文件夹(%1 个账号) → 再同步邮件").arg(syncable.size()),
        "mail");
    if (m_statusLabel)
        m_statusLabel->setText(QString("正在同步文件夹 (%1 个账号)...").arg(syncable.size()));

    // 阶段 1：强制重拉所有账号的文件夹列表（可能新增/删除了文件夹）。
    // 不预先清空内存缓存：重拉期间文件夹树继续显示旧列表，LIST 完成后覆盖
    m_pendingFolderSync = syncable.size();
    for (const QString& id : syncable) {
        loadRemoteFolders(id);
    }
    // 兜底：账号中途被删等导致计数无法归零时，5 秒后强制进入阶段 2
    QTimer::singleShot(5000, this, [this]() {
        if (m_pendingFolderSync > 0) {
            Logger::instance().warn(
                QString("文件夹同步超时兜底: 剩余 %1 个账号未完成, 直接同步邮件")
                    .arg(m_pendingFolderSync),
                "mail");
            m_pendingFolderSync = 0;
            syncMailAfterFolders();
        }
    });
}

void MailWidget::onFullSyncClicked() {
    // 与 onRefreshClicked 行为一致（工具栏「全量同步」入口）：先同步文件夹，再同步邮件
    onRefreshClicked();
}

void MailWidget::syncMailAfterFolders() {
    Logger::instance().info("文件夹同步完成 → 开始同步邮件", "mail");
    if (m_statusLabel) m_statusLabel->setText("正在同步邮件...");
    MailPoller::instance().pollNow();
    QString curAcc  = currentAccountId();
    QString curFold = currentFolder();
    if (!curAcc.isEmpty() && !curFold.isEmpty() && curFold != "Drafts")
        fetchFolderHeaders(curAcc, curFold);
}

void MailWidget::showSyncTip(const QString& text) {
    if (m_statusLabel) m_statusLabel->setText(text);
}

void MailWidget::closeSyncTip(const QString& summary) {
    if (m_statusLabel) m_statusLabel->setText(summary);
}
