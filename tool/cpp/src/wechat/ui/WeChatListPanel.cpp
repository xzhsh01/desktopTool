#include "wechat/ui/WeChatListPanel.h"
#include "app/Theme.h"

#include <QDateTime>
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

// 相对时间格式化（参考邮件列表）：今天→HH:mm，昨天→"昨天"，跨年→"yyyy-MM-dd"
static QString formatRelativeTime(qint64 ts) {
    if (ts <= 0) return QString();
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(ts);
    const QDateTime now = QDateTime::currentDateTime();
    const QDate dToday = now.date();
    const QDate dMsg   = dt.date();

    if (dMsg == dToday)        return dt.toString("HH:mm");
    if (dMsg == dToday.addDays(-1))
        return QStringLiteral("昨天");
    if (dMsg.year() == dToday.year())
        return dt.toString("MM-dd");
    return dt.toString("yyyy-MM-dd");
}

// 红色圆角未读徽标 widget：左侧数字 + 圆形背景
static QLabel* makeUnreadBadge(int n) {
    if (n <= 0) return nullptr;
    auto* lbl = new QLabel;
    lbl->setText(n > 99 ? QStringLiteral("99+") : QString::number(n));
    lbl->setFixedSize(22, 16);
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setStyleSheet(
        "background-color:#e53935;color:#ffffff;border-radius:8px;"
        "font-size:10px;font-weight:bold;");
    return lbl;
}

// 单行 chat 列表项 widget：头像 + 名称/预览 两行 + 右侧时间/未读
static QWidget* makeChatRow(const QString& title,
                            const QString& preview,
                            const QString& key,
                            qint64 ts,
                            int unread) {
    auto* row = new QWidget;
    row->setStyleSheet("background:transparent;");
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(10, 6, 10, 6);
    lay->setSpacing(10);

    auto* avatar = new QLabel;
    avatar->setPixmap(roundAvatar(title, key, 36));
    avatar->setFixedSize(36, 36);
    lay->addWidget(avatar, 0, Qt::AlignVCenter);

    // 中央：名称 + 预览（两行）
    auto* col = new QVBoxLayout;
    col->setSpacing(2);

    auto* nameRow = new QHBoxLayout;
    nameRow->setSpacing(6);
    auto* name = new QLabel(title);
    // 未读 > 0 → 强调（高亮 + 加粗），否则柔和灰
    const bool hasUnread = unread > 0;
    name->setStyleSheet(QString("color:%1; font-size:13px; %2")
                            .arg(hasUnread ? Theme::kAccent : Theme::kTextBright,
                                 hasUnread ? "font-weight:700;" : "font-weight:600;"));
    nameRow->addWidget(name, 1);
    col->addLayout(nameRow);

    if (!preview.isEmpty()) {
        auto* pv = new QLabel(preview);
        const QString pvColor = hasUnread ? Theme::kText : Theme::kMuted;
        pv->setStyleSheet(QString("color:%1; font-size:11px;").arg(pvColor));
        pv->setMaximumWidth(220);
        QString cut = preview;
        if (cut.size() > 60) cut = cut.left(60) + "…";
        pv->setText(cut);
        col->addWidget(pv);
    }
    lay->addLayout(col, 1);

    // 右侧：时间（上）+ 未读徽标（下），垂直右对齐
    auto* right = new QVBoxLayout;
    right->setSpacing(4);
    right->setContentsMargins(0, 0, 0, 0);

    const QString timeText = formatRelativeTime(ts);
    if (!timeText.isEmpty()) {
        auto* timeLbl = new QLabel(timeText);
        timeLbl->setStyleSheet(QString("color:%1; font-size:10px;")
                                   .arg(hasUnread ? Theme::kAccent : Theme::kMuted));
        timeLbl->setAlignment(Qt::AlignRight);
        right->addWidget(timeLbl, 0, Qt::AlignRight | Qt::AlignTop);
    }

    if (auto* badge = makeUnreadBadge(unread)) {
        auto* badgeWrap = new QWidget;
        badgeWrap->setFixedSize(22, 16);
        auto* bl = new QHBoxLayout(badgeWrap);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->addWidget(badge, 0, Qt::AlignRight | Qt::AlignVCenter);
        right->addWidget(badgeWrap, 0, Qt::AlignRight | Qt::AlignTop);
    } else {
        // 占位空白（保持行高度一致）
        auto* spacer = new QWidget;
        spacer->setFixedSize(1, 16);
        right->addWidget(spacer, 0, Qt::AlignRight | Qt::AlignTop);
    }
    lay->addLayout(right, 0);

    return row;
}

// 单行 contact 列表项 widget：头像 + 名称 + 群聊 tag + 可选时间徽标
static QWidget* makeContactRow(const QString& display,
                               const QString& key,
                               bool isRoom,
                               qint64 updateTime = 0) {
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

    // 时间徽标：联系人最近更新时间（若有）
    if (updateTime > 0) {
        auto* timeLbl = new QLabel(formatRelativeTime(updateTime));
        timeLbl->setStyleSheet(QString("color:%1; font-size:10px;")
                                   .arg(Theme::kFaint));
        lay->addWidget(timeLbl, 0, Qt::AlignVCenter);
    }

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
        "QListWidget::item{height:64px;border:none;margin:0 4px;border-radius:6px;}"
        "QListWidget::item:hover{background:%3;}"
        "QListWidget::item:pressed{background:%4;}"
        "QListWidget::item:selected{background:%5;color:%6;font-weight:600;}"
        "QListWidget::item:selected:!active{background:%5;}"
        "QListWidget::item:selected:hover{background:%4;}")
        .arg(Theme::kBg, Theme::kText,
             Theme::kSurface,           // hover
             Theme::kBorder,            // pressed / selected:hover
             Theme::kAccent,            // selected bg
             Theme::kBg);               // selected fg

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
    const int prevSize = m_sessionsCache.value(accId).size();
    const QString prevFp = m_sessionsFp.value(accId);
    if (prevSize == list.size() && !prevFp.isEmpty() && prevFp == fp) {
        // 数据完全一致：不重建（重复点击 / 后台空转同步都不会触发 list 重建）
        m_sessionsCache.insert(accId, list);   // 仍刷新引用以保持最新
        return;
    }
    if (prevSize == list.size() && prevFp.isEmpty()) {
        // partial 模式：fp 已清掉（appendBatch 已插入 UI），但 size 已匹配
        // 信任 appendBatch 的结果，跳过全量 rebuild（避免闪烁）
        m_sessionsCache.insert(accId, list);
        m_sessionsFp.insert(accId, fp);
        updateTitle();
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
    const int prevSize = m_contactsCache.value(accId).size();
    const QString prevFp = m_contactsFp.value(accId);
    if (prevSize == list.size() && !prevFp.isEmpty() && prevFp == fp) {
        m_contactsCache.insert(accId, list);
        return;
    }
    if (prevSize == list.size() && prevFp.isEmpty()) {
        // partial 模式：fp 已清掉，appendBatch 已插入 UI，信任并跳过
        m_contactsCache.insert(accId, list);
        m_contactsFp.insert(accId, fp);
        updateTitle();
        return;
    }
    m_contactsCache.insert(accId, list);
    m_contactsFp.insert(accId, fp);
    if (accId == m_currentAccId && m_stack->currentIndex() == 1) {
        rebuildContactList();
        updateTitle();
    }
}

// ── 流式追加（同步中每隔 N 条回调一次，仅插入新 wxid/talker，不重建） ──

int WeChatListPanel::appendContactsBatch(const QString& accId,
                                        const QVariantList& batch) {
    if (batch.isEmpty()) return 0;
    // 已有 wxid 集合（避免重复插入 / 触发 rebuild）
    auto& cache = m_contactsCache[accId];
    QSet<QString> existing;
    existing.reserve(cache.size());
    for (const auto& v : cache) {
        const QString wxid = v.toMap()["userName"].toString();
        if (!wxid.isEmpty()) existing.insert(wxid);
    }
    QVariantList appended;
    appended.reserve(batch.size());
    for (const auto& v : batch) {
        const auto m = v.toMap();
        const QString wxid = m["userName"].toString();
        if (wxid.isEmpty() || existing.contains(wxid)) continue;
        cache.append(m);
        appended.append(m);
        existing.insert(wxid);
    }
    if (appended.isEmpty()) return 0;
    // 失效 fingerprint（避免 setContacts 后续全量 rebuild 时做 fingerprint 比对浪费时间）
    m_contactsFp.remove(accId);
    // 当前账号 + 联系人页可见 → 立即插入 UI（不重建）
    if (accId == m_currentAccId && m_stack->currentIndex() == 1) {
        const QString filter = searchText();
        if (filter.isEmpty()) {
            // 无过滤词：直接追加（无需 rebuild）
            for (const auto& v : appended) {
                m_contactList->addItem(makeContactItem(v.toMap()));
            }
            updateTitle();
        } else {
            // 有过滤词：cache 已更新，但 UI 不能盲加（可能不匹配）
            // mark dirty 让下次 rebuildContactList 生效
            m_contactListBuiltFp.clear();
        }
    }
    return appended.size();
}

int WeChatListPanel::appendSessionsBatch(const QString& accId,
                                        const QVariantList& batch) {
    if (batch.isEmpty()) return 0;
    auto& cache = m_sessionsCache[accId];
    QSet<QString> existing;
    existing.reserve(cache.size());
    for (const auto& v : cache) {
        const QString t = v.toMap()["talker"].toString();
        if (!t.isEmpty()) existing.insert(t);
    }
    QVariantList appended;
    appended.reserve(batch.size());
    for (const auto& v : batch) {
        const auto m = v.toMap();
        const QString t = m["talker"].toString();
        if (t.isEmpty() || existing.contains(t)) continue;
        cache.append(m);
        appended.append(m);
        existing.insert(t);
    }
    if (appended.isEmpty()) return 0;
    m_sessionsFp.remove(accId);
    if (accId == m_currentAccId && m_stack->currentIndex() == 0) {
        const QString filter = searchText();
        if (filter.isEmpty()) {
            for (const auto& v : appended) {
                m_chatList->addItem(makeChatItem(v.toMap()));
            }
            updateTitle();
        } else {
            m_chatListBuiltFp.clear();
        }
    }
    return appended.size();
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

// ── 反向同步：sidebar 叶子点击时定位列表中的对应行 ──

bool WeChatListPanel::selectChatByTalker(const QString& talker) {
    if (talker.isEmpty() || !m_chatList) return false;
    // 遍历当前可见列表（不重建！）
    for (int i = 0; i < m_chatList->count(); ++i) {
        auto* it = m_chatList->item(i);
        if (it && it->data(Qt::UserRole).toString() == talker) {
            m_chatList->setCurrentItem(it);
            m_chatList->scrollToItem(it, QAbstractItemView::PositionAtCenter);
            return true;
        }
    }
    // 不在当前列表（可能过滤掉了）：清空过滤让该项出现
    if (m_searchEdit) m_searchEdit->clear();
    for (int i = 0; i < m_chatList->count(); ++i) {
        auto* it = m_chatList->item(i);
        if (it && it->data(Qt::UserRole).toString() == talker) {
            m_chatList->setCurrentItem(it);
            m_chatList->scrollToItem(it, QAbstractItemView::PositionAtCenter);
            return true;
        }
    }
    return false;
}

bool WeChatListPanel::selectContactByWxid(const QString& wxid) {
    if (wxid.isEmpty() || !m_contactList) return false;
    for (int i = 0; i < m_contactList->count(); ++i) {
        auto* it = m_contactList->item(i);
        if (it && it->data(Qt::UserRole).toString() == wxid) {
            m_contactList->setCurrentItem(it);
            m_contactList->scrollToItem(it, QAbstractItemView::PositionAtCenter);
            return true;
        }
    }
    if (m_searchEdit) m_searchEdit->clear();
    for (int i = 0; i < m_contactList->count(); ++i) {
        auto* it = m_contactList->item(i);
        if (it && it->data(Qt::UserRole).toString() == wxid) {
            m_contactList->setCurrentItem(it);
            m_contactList->scrollToItem(it, QAbstractItemView::PositionAtCenter);
            return true;
        }
    }
    return false;
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
    const qint64  ts     = s["time"].toLongLong();
    const int     unread = s["unread"].toInt();
    auto* it = new QListWidgetItem;
    it->setData(Qt::UserRole, talker);
    it->setData(Qt::UserRole + 1, title);
    it->setSizeHint(QSize(0, 64));
    m_chatList->addItem(it);
    m_chatList->setItemWidget(it, makeChatRow(title, prev, talker, ts, unread));
    return it;
}

QListWidgetItem* WeChatListPanel::makeContactItem(const QVariantMap& c) {
    const QString wxid    = c["userName"].toString();
    const QString display = c["display"].toString();
    const bool    isRoom  = c["isRoom"].toBool();
    const qint64  updTime = c["updateTime"].toLongLong();
    auto* it = new QListWidgetItem;
    it->setData(Qt::UserRole, wxid);
    it->setData(Qt::UserRole + 1, display);
    it->setSizeHint(QSize(0, 52));
    m_contactList->addItem(it);
    m_contactList->setItemWidget(it, makeContactRow(display, wxid, isRoom, updTime));
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