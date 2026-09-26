#include "wechat/ui/WeChatListPanel.h"
#include "app/Theme.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QStackedWidget>
#include <QVBoxLayout>

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
    connect(m_chatList, &QListWidget::itemActivated,
            this, &WeChatListPanel::onChatItemActivated);
    connect(m_chatList, &QListWidget::currentRowChanged,
            this, [this](int){ onCurrentRowChanged(); });
    m_stack->addWidget(m_chatList);

    m_contactList = new QListWidget;
    m_contactList->setStyleSheet(listStyle);
    m_contactList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_contactList, &QListWidget::itemActivated,
            this, &WeChatListPanel::onContactItemActivated);
    m_stack->addWidget(m_contactList);

    root->addWidget(m_stack, 1);
}

void WeChatListPanel::setSessions(const QString& accId, const QVariantList& list) {
    m_sessionsCache.insert(accId, list);
    if (accId == m_currentAccId && m_stack->currentIndex() == 0) {
        rebuildChatList();
        updateTitle();
    }
}

void WeChatListPanel::setContacts(const QString& accId, const QVariantList& list) {
    m_contactsCache.insert(accId, list);
    if (accId == m_currentAccId && m_stack->currentIndex() == 1) {
        rebuildContactList();
        updateTitle();
    }
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
    m_stack->setCurrentIndex(0);
    updateTitle();
    rebuildChatList();
}

void WeChatListPanel::showContactList() {
    if (!m_stack) return;
    m_stack->setCurrentIndex(1);
    updateTitle();
    rebuildContactList();
}

bool WeChatListPanel::isShowingChatList() const {
    return m_stack && m_stack->currentIndex() == 0;
}

void WeChatListPanel::setCurrentAccId(const QString& accId) {
    m_currentAccId = accId;
    if (m_stack->currentIndex() == 0) rebuildChatList();
    else                                rebuildContactList();
    updateTitle();
}

QString WeChatListPanel::searchText() const {
    return m_searchEdit ? m_searchEdit->text().trimmed() : QString();
}

void WeChatListPanel::rebuildChatList() {
    if (!m_chatList) return;
    m_chatList->clear();
    const QString filter = searchText();

    if (!m_sessionsCache.contains(m_currentAccId)) {
        auto* it = new QListWidgetItem("加载中…");
        it->setFlags(Qt::NoItemFlags);
        it->setForeground(QColor(Theme::kMuted));
        m_chatList->addItem(it);
        return;
    }

    const auto sessions = m_sessionsCache.value(m_currentAccId);
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
    m_contactList->clear();
    const QString filter = searchText();

    if (!m_contactsCache.contains(m_currentAccId)) {
        auto* it = new QListWidgetItem("加载中…");
        it->setFlags(Qt::NoItemFlags);
        it->setForeground(QColor(Theme::kMuted));
        m_contactList->addItem(it);
        return;
    }

    const auto contacts = m_contactsCache.value(m_currentAccId);
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