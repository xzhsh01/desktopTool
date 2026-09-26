#include "wechat/ui/WeChatListPanel.h"
#include "app/Theme.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QPainter>
#include <QPixmap>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QCryptographicHash>

namespace {
// 圆形头像（与 WeChatSidebar / WeChatDetailPanel 内同名实现保持一致）
static QPixmap roundAvatar(const QString& text, const QString& key, int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    quint32 h = 0;
    for (const QChar c : key) h = h * 131 + c.unicode();
    const QColor colors[] = {
        QColor("#5B8DEF"), QColor("#27AE60"), QColor("#E67E22"), QColor("#9B59B6"),
        QColor("#16A085"), QColor("#C0392B"), QColor("#2980B9"), QColor("#D4A017"),
    };
    p.setBrush(colors[h % 8]);
    p.setPen(Qt::NoPen);
    p.drawEllipse(0, 0, size, size);

    p.setPen(Qt::white);
    p.setFont(QFont("Microsoft YaHei", size / 3, QFont::Bold));
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter,
               text.isEmpty() ? QStringLiteral("?") : QString(text.at(0).toUpper()));
    return pm;
}

// 单行 chat 列表项 widget：头像 + 名称/预览 两行
static QWidget* makeChatRow(const QString& title,
                            const QString& preview,
                            const QString& key) {
    auto* row = new QWidget;
    row->setStyleSheet("background:transparent;");
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(10, 6, 10, 6);
    lay->setSpacing(10);

    auto* avatar = new QLabel;
    avatar->setPixmap(roundAvatar(title, key, 36));
    avatar->setFixedSize(36, 36);
    lay->addWidget(avatar, 0, Qt::AlignVCenter);

    auto* col = new QVBoxLayout;
    col->setSpacing(2);
    auto* name = new QLabel(title);
    name->setStyleSheet(QString("color:%1; font-size:13px; font-weight:600;")
                            .arg(Theme::kTextBright));
    col->addWidget(name);

    if (!preview.isEmpty()) {
        auto* pv = new QLabel(preview);
        pv->setStyleSheet(QString("color:%1; font-size:11px;").arg(Theme::kMuted));
        pv->setMaximumWidth(220);
        QString cut = preview;
        if (cut.size() > 60) cut = cut.left(60) + "…";
        pv->setText(cut);
        col->addWidget(pv);
    }
    lay->addLayout(col, 1);
    return row;
}

// 单行 contact 列表项 widget：头像 + 名称 + 群聊 tag
static QWidget* makeContactRow(const QString& display,
                               const QString& key,
                               bool isRoom) {
    auto* row = new QWidget;
    row->setStyleSheet("background:transparent;");
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(10, 6, 10, 6);
    lay->setSpacing(10);

    auto* avatar = new QLabel;
    avatar->setPixmap(roundAvatar(display, key, 36));
    avatar->setFixedSize(36, 36);
    lay->addWidget(avatar, 0, Qt::AlignVCenter);

    auto* name = new QLabel(display);
    name->setStyleSheet(QString("color:%1; font-size:13px;").arg(Theme::kText));
    lay->addWidget(name, 1, Qt::AlignVCenter);

    if (isRoom) {
        auto* tag = new QLabel("群聊");
        tag->setStyleSheet(QString("color:%1; font-size:11px; border:1px solid %1;"
                                   "border-radius:3px; padding:0 4px;")
                               .arg(Theme::kCatWeChat));
        lay->addWidget(tag, 0, Qt::AlignVCenter);
    }
    return row;
}
} // namespace

WeChatListPanel::WeChatListPanel(QWidget* parent) : QWidget(parent) {
    buildUi();
    showChatList();
}

void WeChatListPanel::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── 标题栏：分类名 + 计数 + 搜索框 ──
    m_titleBar = new QWidget;
    m_titleBar->setStyleSheet(QString("background:%1; border-bottom:1px solid %2;")
                                  .arg(Theme::kSidebar, Theme::kBorder));
    auto* titleLay = new QVBoxLayout(m_titleBar);
    titleLay->setContentsMargins(12, 10, 12, 8);
    titleLay->setSpacing(6);

    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    auto* titleLbl = new QLabel("💬 聊天");
    titleLbl->setObjectName("wlpTitle");
    titleLbl->setStyleSheet(QString("color:%1; font-size:14px; font-weight:600;")
                                .arg(Theme::kTextBright));
    titleRow->addWidget(titleLbl);

    auto* countLbl = new QLabel("0");
    countLbl->setObjectName("wlpCount");
    countLbl->setStyleSheet(QString("color:%1; font-size:12px;").arg(Theme::kMuted));
    titleRow->addWidget(countLbl);
    titleRow->addStretch(1);
    titleLay->addLayout(titleRow);

    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText("过滤当前列表…");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedHeight(30);
    m_searchEdit->setStyleSheet(QString(
        "QLineEdit{background:%1;border:1px solid %2;border-radius:4px;"
        "padding:4px 8px;color:%3;}"
        "QLineEdit:focus{border-color:%4;}")
        .arg(Theme::kSurface, Theme::kBorder, Theme::kText, Theme::kCatWeChat));
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &WeChatListPanel::onSearchChanged);
    titleLay->addWidget(m_searchEdit);

    root->addWidget(m_titleBar);

    // ── 列表 stacked ──
    m_stack = new QStackedWidget;
    m_stack->setStyleSheet(QString("background:%1;").arg(Theme::kBg));

    const QString listStyle = QString(
        "QListWidget{background:%1;border:none;outline:0;color:%2;}"
        "QListWidget::item{height:60px;border:none;}"
        "QListWidget::item:hover{background:%3;}"
        "QListWidget::item:selected{background:%4;color:%5;}"
        "QListWidget::item:selected:!active{background:%4;}")
        .arg(Theme::kBg, Theme::kText,
             Theme::kSurface, Theme::kBorder, Theme::kAccent);

    m_chatList = new QListWidget;
    m_chatList->setStyleSheet(listStyle);
    m_chatList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 单击 / 双击 / Enter 都触发（itemActivated 只响应双击+Enter）
    connect(m_chatList, &QListWidget::itemClicked,
            this, &WeChatListPanel::onChatItemActivated);
    connect(m_chatList, &QListWidget::itemActivated,
            this, &WeChatListPanel::onChatItemActivated);
    connect(m_chatList, &QListWidget::currentRowChanged,
            this, [this](int){ onCurrentRowChanged(); });
    m_stack->addWidget(m_chatList);

    m_contactList = new QListWidget;
    m_contactList->setStyleSheet(listStyle);
    m_contactList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_contactList, &QListWidget::itemClicked,
            this, &WeChatListPanel::onContactItemActivated);
    connect(m_contactList, &QListWidget::itemActivated,
            this, &WeChatListPanel::onContactItemActivated);
    m_stack->addWidget(m_contactList);

    root->addWidget(m_stack, 1);
}

void WeChatListPanel::setSessions(const QString& accId, const QVariantList& list) {
    const QString fp = fingerprint(list);
    if (m_sessionsCache.value(accId).size() == list.size() &&
        m_sessionsFp.value(accId) == fp) {
        // 数据未变：不重建（重复点击 / 后台空转同步都不会触发 list 重建）
        m_sessionsCache.insert(accId, list);   // 仍刷新引用以保持最新
        return;
    }
    m_sessionsCache.insert(accId, list);
    m_sessionsFp.insert(accId, fp);
    if (accId == m_currentAccId && m_stack->currentIndex() == 0) {
        rebuildChatList();
        updateTitle();
    }
}

void WeChatListPanel::setContacts(const QString& accId, const QVariantList& list) {
    const QString fp = fingerprint(list);
    if (m_contactsCache.value(accId).size() == list.size() &&
        m_contactsFp.value(accId) == fp) {
        m_contactsCache.insert(accId, list);
        return;
    }
    m_contactsCache.insert(accId, list);
    m_contactsFp.insert(accId, fp);
    if (accId == m_currentAccId && m_stack->currentIndex() == 1) {
        rebuildContactList();
        updateTitle();
    }
}

// 计算列表指纹：数量 + 各条目关键字段拼接 → 整型 hash
// （每次 O(N) 但只对比 hash，不遍历/不创建 widget）
QString WeChatListPanel::fingerprint(const QVariantList& list) {
    if (list.isEmpty()) return QStringLiteral("0");
    QCryptographicHash h(QCryptographicHash::Sha1);
    for (const auto& v : list) {
        const auto m = v.toMap();
        // 通用：拼接所有字段的 type+key+value，schema 不敏感
        QMap<QString, QVariant> sorted(m);
        QString s;
        for (auto it = sorted.constBegin(); it != sorted.constEnd(); ++it) {
            s += it.key() + "=" + it.value().toString() + ";";
        }
        h.addData(s.toUtf8());
    }
    return QString::fromLatin1(h.result().toHex());
}

void WeChatListPanel::clearData(const QString& accId) {
    m_sessionsCache.remove(accId);
    m_contactsCache.remove(accId);
    if (accId == m_currentAccId) {
        m_chatList->clear();
        m_contactList->clear();
        updateTitle();
    }
}

void WeChatListPanel::showChatList() {
    if (!m_stack) return;
    if (m_stack->currentIndex() == 0) return;  // 已在当前页：跳过（重复点击不重建）
    m_stack->setCurrentIndex(0);
    updateTitle();
    rebuildChatList();
}

void WeChatListPanel::showContactList() {
    if (!m_stack) return;
    if (m_stack->currentIndex() == 1) return;  // 已在当前页：跳过
    m_stack->setCurrentIndex(1);
    updateTitle();
    rebuildContactList();
}

bool WeChatListPanel::isShowingChatList() const {
    return m_stack && m_stack->currentIndex() == 0;
}

void WeChatListPanel::setCurrentAccId(const QString& accId) {
    if (m_currentAccId == accId) return;       // 账号未变：不重建（避免重复点击触发昂贵 list 重建）
    m_currentAccId = accId;
    // 清掉旧 fp：账号变了，必须重新 rebuild 一次（rebuild 内部的 fp 短路不能挡这次）
    m_chatListBuiltFp.clear();
    m_contactListBuiltFp.clear();
    if (m_stack->currentIndex() == 0) rebuildChatList();
    else                                rebuildContactList();
    updateTitle();
}

QString WeChatListPanel::searchText() const {
    return m_searchEdit ? m_searchEdit->text().trimmed() : QString();
}

void WeChatListPanel::rebuildChatList() {
    if (!m_chatList) return;
    const QString filter = searchText();

    // 缓存未就绪 → 显示"加载中…"（已显示就跳过，避免重复 addItem）
    if (!m_sessionsCache.contains(m_currentAccId)) {
        if (m_chatListBuiltFp == "__loading__") return;
        m_chatListBuiltFp = "__loading__";
        m_chatList->clear();
        auto* it = new QListWidgetItem("加载中…");
        it->setFlags(Qt::NoItemFlags);
        it->setForeground(QColor(Theme::kMuted));
        m_chatList->addItem(it);
        return;
    }

    // 指纹短路：来回切 sidebar chat/contact 时不重建同一份数据
    // （watcher 频繁写但数据未必变；账号切换会清 fp 强制重建）
    const auto& sessions = m_sessionsCache.value(m_currentAccId);
    const QString fp = fingerprint(sessions);
    if (fp == m_chatListBuiltFp && m_chatList->count() == sessions.size()) {
        return;   // 数据未变 → 跳过 clear + 重建（关键卡顿优化点）
    }
    m_chatListBuiltFp = fp;

    m_chatList->clear();
    for (const auto& v : sessions) {
        const auto s = v.toMap();
        const QString title = s["title"].toString();
        if (!filter.isEmpty() &&
            !title.contains(filter, Qt::CaseInsensitive)) continue;
        m_chatList->addItem(makeChatItem(s));
    }
    if (m_chatList->count() == 0) {
        auto* it = new QListWidgetItem(filter.isEmpty() ? "  暂无会话" : "  无匹配项");
        it->setFlags(Qt::NoItemFlags);
        it->setForeground(QColor(Theme::kMuted));
        m_chatList->addItem(it);
    }
}

void WeChatListPanel::rebuildContactList() {
    if (!m_contactList) return;
    const QString filter = searchText();

    // 缓存未就绪 → 显示"加载中…"（已显示就跳过，避免重复 addItem）
    if (!m_contactsCache.contains(m_currentAccId)) {
        if (m_contactListBuiltFp == "__loading__") return;
        m_contactListBuiltFp = "__loading__";
        m_contactList->clear();
        auto* it = new QListWidgetItem("加载中…");
        it->setFlags(Qt::NoItemFlags);
        it->setForeground(QColor(Theme::kMuted));
        m_contactList->addItem(it);
        return;
    }

    // 指纹短路：来回切 sidebar chat/contact 时不重建同一份数据
    const auto& contacts = m_contactsCache.value(m_currentAccId);
    const QString fp = fingerprint(contacts);
    if (fp == m_contactListBuiltFp && m_contactList->count() == contacts.size()) {
        return;   // 数据未变 → 跳过 clear + 重建（关键卡顿优化点）
    }
    m_contactListBuiltFp = fp;

    m_contactList->clear();
    for (const auto& v : contacts) {
        const auto c = v.toMap();
        const QString display = c["display"].toString();
        if (!filter.isEmpty() &&
            !display.contains(filter, Qt::CaseInsensitive)) continue;
        m_contactList->addItem(makeContactItem(c));
    }
    if (m_contactList->count() == 0) {
        auto* it = new QListWidgetItem(filter.isEmpty() ? "  暂无联系人" : "  无匹配项");
        it->setFlags(Qt::NoItemFlags);
        it->setForeground(QColor(Theme::kMuted));
        m_contactList->addItem(it);
    }
}

void WeChatListPanel::updateTitle() {
    if (!m_titleBar) return;
    auto* titleLbl = m_titleBar->findChild<QLabel*>("wlpTitle");
    auto* countLbl = m_titleBar->findChild<QLabel*>("wlpCount");
    if (!titleLbl || !countLbl) return;

    const bool chatPage = m_stack->currentIndex() == 0;
    int n = 0;
    if (chatPage) {
        titleLbl->setText("💬 聊天");
        n = m_sessionsCache.value(m_currentAccId).size();
    } else {
        titleLbl->setText("👥 联系人");
        n = m_contactsCache.value(m_currentAccId).size();
    }
    countLbl->setText(QString::number(n));
}

QListWidgetItem* WeChatListPanel::makeChatItem(const QVariantMap& s) {
    const QString talker = s["talker"].toString();
    const QString title  = s["title"].toString();
    const QString prev   = s["lastMsg"].toString();
    auto* it = new QListWidgetItem;
    it->setData(Qt::UserRole, talker);
    it->setData(Qt::UserRole + 1, title);
    it->setSizeHint(QSize(0, 60));
    m_chatList->addItem(it);
    m_chatList->setItemWidget(it, makeChatRow(title, prev, talker));
    return it;
}

QListWidgetItem* WeChatListPanel::makeContactItem(const QVariantMap& c) {
    const QString wxid    = c["userName"].toString();
    const QString display = c["display"].toString();
    const bool    isRoom  = c["isRoom"].toBool();
    auto* it = new QListWidgetItem;
    it->setData(Qt::UserRole, wxid);
    it->setData(Qt::UserRole + 1, display);
    it->setSizeHint(QSize(0, 48));
    m_contactList->addItem(it);
    m_contactList->setItemWidget(it, makeContactRow(display, wxid, isRoom));
    return it;
}

void WeChatListPanel::onChatItemActivated(QListWidgetItem* item) {
    if (!item) return;
    const QString talker = item->data(Qt::UserRole).toString();
    if (talker.isEmpty()) return;     // 占位项
    m_currentTalker = talker;
    emit chatItemClicked(m_currentAccId, talker);
}

void WeChatListPanel::onContactItemActivated(QListWidgetItem* item) {
    if (!item) return;
    const QString wxid = item->data(Qt::UserRole).toString();
    if (wxid.isEmpty()) return;
    emit contactItemClicked(m_currentAccId, wxid);
}

void WeChatListPanel::onCurrentRowChanged() {
    // 占位项 / 无意义 item 不允许停留为 current
    if (m_stack->currentIndex() == 0 && m_chatList->currentItem()) {
        const QString t = m_chatList->currentItem()->data(Qt::UserRole).toString();
        if (t.isEmpty()) m_chatList->setCurrentRow(-1);
    }
}

void WeChatListPanel::onSearchChanged(const QString&) {
    if (m_stack->currentIndex() == 0) rebuildChatList();
    else                                rebuildContactList();
    updateTitle();
}