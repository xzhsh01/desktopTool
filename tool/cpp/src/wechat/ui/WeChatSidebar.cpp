#include "wechat/ui/WeChatSidebar.h"
#include "wechat/WeChatAccountManager.h"
#include "app/Theme.h"
#include "core/Logger.h"

#include <QAction>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHBoxLayout>
#include <QMap>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

using Account = WeChatAccountManager::Account;

namespace {
// 列表内容指纹：cache → label 内容指纹；用于判断徽标是否需要刷新
//   注意：仅对"影响徽标呈现的字段"做哈希，忽略无关字段
QString labelFingerprint(const QVariantList& list) {
    if (list.isEmpty()) return QStringLiteral("0");
    QCryptographicHash h(QCryptographicHash::Sha1);
    int unreadSum = 0;
    qint64 latestTs = 0;
    for (const auto& v : list) {
        const auto m = v.toMap();
        unreadSum += m["unread"].toInt();
        qint64 t = m["time"].toLongLong();
        if (t > latestTs) latestTs = t;
    }
    h.addData(QByteArray::number(unreadSum) + "|" +
              QByteArray::number(latestTs) + "|" +
              QByteArray::number(list.size()));
    return QString::fromLatin1(h.result().toHex());
}
} // namespace

WeChatSidebar::WeChatSidebar(QWidget* parent) : QWidget(parent) {
    buildUi();
    rebuildTree();
}

void WeChatSidebar::buildUi() {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);

    // ── 顶部按钮行：[+ 添加账号] [↻ 刷新] ──
    auto* topBtnRow = new QHBoxLayout;
    topBtnRow->setContentsMargins(0, 0, 0, 0);
    topBtnRow->setSpacing(6);
    auto* addBtn = new QPushButton(QStringLiteral("+ 添加微信账号"));
    addBtn->setCursor(Qt::PointingHandCursor);
    addBtn->setStyleSheet(Theme::flatBtnPrimary());
    connect(addBtn, &QPushButton::clicked, this, &WeChatSidebar::onAddClicked);
    topBtnRow->addWidget(addBtn, 1);

    auto* refreshBtn = new QPushButton(QStringLiteral("↻"));
    refreshBtn->setCursor(Qt::PointingHandCursor);
    refreshBtn->setToolTip(QStringLiteral("刷新当前账号"));
    refreshBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: none;"
        " padding: 4px 10px; font-size: 16px; border-radius: 3px; }"
        "QPushButton:hover    { background: %3; }"
        "QPushButton:pressed  { background: %4; }"
        "QPushButton:disabled { background: %1; color: %5; }")
        .arg(Theme::kTitleBar, Theme::kText,
             Theme::kBorderLight, Theme::kBorder, Theme::kDisabled));
    connect(refreshBtn, &QPushButton::clicked, this, &WeChatSidebar::onRefreshClicked);
    topBtnRow->addWidget(refreshBtn);
    lay->addLayout(topBtnRow);

    // ── 树形侧边栏：账号 → 两个分组头（可点击，不再展开叶子节点） ──
    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    // 注意：不能 setRootIsDecorated(false)，否则 Qt 会把 top-level 当作叶子处理，
    // 即使 setExpanded(true) 也不会渲染子节点。
    // 改为用 stylesheet 隐藏 branch（连接线 + 箭头）
    m_tree->setRootIsDecorated(true);
    m_tree->setExpandsOnDoubleClick(false);       // 单击触发
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setMinimumWidth(200);
    m_tree->setIndentation(14);
    m_tree->setUniformRowHeights(true);           // 行数少但仍开启，避免将来扩展时跳变
    m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    // ── 三态视觉：hover / pressed / selected，都给明确反馈 ──
    // 关键：branch 透明避免根的连接线泄露；item 留白 + 圆角模拟"卡片"
    m_tree->setStyleSheet(QString(
        "QTreeWidget{background:%1;border:1px solid %2;outline:0;color:%3;}"
        "QTreeWidget::item{height:32px;border-radius:6px;margin:1px 4px;"
        "padding:0 8px;border:none;}"
        "QTreeWidget::item:hover{background:%4;}"
        "QTreeWidget::item:pressed{background:%5;}"
        "QTreeWidget::item:selected{background:%6;color:%7;font-weight:600;}"
        "QTreeWidget::item:selected:hover{background:%8;}"
        "QTreeWidget::branch{background:transparent;border-image:none;}"
        "QTreeWidget::branch:has-siblings:!adjoins-item{border-image:none;}"
        "QTreeWidget::branch:has-siblings:adjoins-item{border-image:none;}"
        "QTreeWidget::branch:!has-children:!has-siblings:adjoins-item{border-image:none;}"
        "QTreeWidget::branch:has-children:!has-siblings:closed,"
        "QTreeWidget::branch:closed:has-children:has-siblings{border-image:none;}"
        "QTreeWidget::branch:open:has-children:!has-siblings,"
        "QTreeWidget::branch:open:has-children:has-siblings{border-image:none;}")
        .arg(Theme::kBg, Theme::kBorder, Theme::kText,
             Theme::kSurface,         // hover
             Theme::kBorder,          // pressed
             Theme::kAccent,          // selected bg
             Theme::kBg,              // selected fg（深底配浅色）
             Theme::kBorder));        // selected:hover

    connect(m_tree, &QTreeWidget::itemClicked,
            this, &WeChatSidebar::onTreeItemClicked);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &WeChatSidebar::onContextMenu);
    lay->addWidget(m_tree, 1);
}

QString WeChatSidebar::currentAccountId() const {
    auto* it = m_tree ? m_tree->currentItem() : nullptr;
    if (!it) return QString();
    QTreeWidgetItem* p = it;
    while (p && p->parent()) p = p->parent();
    return p ? p->data(0, Qt::UserRole).toString() : QString();
}

void WeChatSidebar::selectAccount(const QString& accId) {
    if (!m_tree || accId.isEmpty()) return;
    auto* it = findAccountItem(accId);
    if (it) m_tree->setCurrentItem(it);
}

void WeChatSidebar::rebuildTree(const QString& selectAccId) {
    if (!m_tree) return;
    const QString prevSel = currentAccountId();
    const QString target = !selectAccId.isEmpty() ? selectAccId : prevSel;

    m_tree->blockSignals(true);
    m_tree->clear();

    Logger::instance().info(QString("[sidebar] rebuildTree: accounts=%1").arg(WeChatAccountManager::instance().accounts().size()), "wechat");

    for (const auto& a : WeChatAccountManager::instance().accounts()) {
        auto* accItem = makeAccountItem(a.id, a.name);
        m_tree->addTopLevelItem(accItem);

        auto* contactGroup = makeGroupItem(NodeContactGroup);
        contactGroup->setData(0, Qt::UserRole, a.id);
        contactGroup->setText(0, buildContactLabel(m_contactsByAcc.value(a.id)));
        accItem->addChild(contactGroup);

        auto* chatGroup = makeGroupItem(NodeChatGroup);
        chatGroup->setData(0, Qt::UserRole, a.id);
        chatGroup->setText(0, buildChatLabel(m_sessionsByAcc.value(a.id)));
        accItem->addChild(chatGroup);

        // 关键：必须先 addChild 再 setExpanded(true)
        // —— 在没有 child 时调用 setExpanded(true) 会被 Qt 忽略
        accItem->setExpanded(true);

        Logger::instance().info(QString("[sidebar] account '%1': childCount=%2, expanded=%3, topLevel=%4, total=%5")
            .arg(a.name).arg(accItem->childCount()).arg(accItem->isExpanded()?1:0)
            .arg(m_tree->topLevelItemCount()).arg(m_tree->model()->rowCount()), "wechat");
    }
    m_tree->blockSignals(false);

    // 兜底：在 blockSignals 之外再调一次 expand()，确保子节点在样式/选择模型稳定后被绘制
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* it = m_tree->topLevelItem(i);
        if (it && it->childCount() > 0) it->setExpanded(true);
    }

    if (!target.isEmpty()) selectAccount(target);
}

QTreeWidgetItem* WeChatSidebar::makeAccountItem(const QString& accId,
                                                const QString& name) const {
    auto* it = new QTreeWidgetItem;
    it->setData(0, Qt::UserRole, accId);
    it->setData(0, NodeTypeRole, NodeAccountRoot);
    QString label = name.isEmpty() ? accId : name;
    it->setText(0, "📁 " + label);
    QFont f = it->font(0);
    f.setBold(true);
    it->setFont(0, f);
    // 注意：不要在这里 setExpanded(true)！Qt 在没有 child 时不会保持展开状态。
    // 必须在 addChild 之后再 setExpanded(true)，由 rebuildTree 负责调用。
    it->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator); // 不画展开箭头
    // 关键：必须保留 ItemIsSelectable —— Qt 6 在父节点缺少可点击态且隐藏分支指示符时，
    // 子节点有时不会被绘制。点击命中由 onTreeItemClicked 内 NodeType 分发去忽略，
    // 不影响"账号根不可被选中"的语义。
    it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return it;
}

QTreeWidgetItem* WeChatSidebar::makeGroupItem(NodeType type) const {
    auto* it = new QTreeWidgetItem;
    it->setData(0, NodeTypeRole, type);
    it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return it;
}

QTreeWidgetItem* WeChatSidebar::findAccountItem(const QString& accId) const {
    if (!m_tree || accId.isEmpty()) return nullptr;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* it = m_tree->topLevelItem(i);
        if (it->data(0, Qt::UserRole).toString() == accId) return it;
    }
    return nullptr;
}

// ── 标签生成（HTML 富文本）─────────────────────────────────────

// "💬 聊天  <span bg=#e53935 color=#fff>12</span>"
QString WeChatSidebar::buildChatLabel(const QVariantList& sessions) const {
    const int n = sessions.size();
    int unreadSum = 0;
    for (const auto& v : sessions) unreadSum += v.toMap()["unread"].toInt();

    QString badge;
    if (unreadSum > 0) {
        badge = QString("<span style='background-color:#e53935;color:#ffffff;"
                        "border-radius:8px;padding:0 6px;margin-left:6px;"
                        "font-size:10px;font-weight:bold;'>%1</span>")
                .arg(formatBadge(unreadSum));
    }

    if (n == 0) {
        return badge.isEmpty()
            ? QStringLiteral("💬 聊天")
            : QStringLiteral("💬 聊天") + badge;
    }
    return QString("💬 聊天 <span style='color:%1;'>(%2)</span>")
            .arg(Theme::kMuted)
            .arg(n) + badge;
}

// "👥 联系人  <span muted>1024 · 12:34</span>"  (count + 最新联系人时间)
QString WeChatSidebar::buildContactLabel(const QVariantList& contacts) const {
    const int n = contacts.size();
    if (n == 0) return QStringLiteral("👥 联系人");

    // 取最新联系人时间戳（contacts 通常没有 time 字段，但若有则显示）
    qint64 latest = 0;
    for (const auto& v : contacts) {
        qint64 t = v.toMap()["updateTime"].toLongLong();
        if (t > latest) latest = t;
    }
    QString extra = QString("<span style='color:%1;'>%2</span>")
                    .arg(Theme::kMuted).arg(n);
    if (latest > 0) {
        extra += QString(" <span style='color:%1;'>· %2</span>")
                 .arg(Theme::kFaint)
                 .arg(formatRelativeTime(latest));
    }
    return QStringLiteral("👥 联系人 ") + extra;
}

QString WeChatSidebar::formatRelativeTime(qint64 ts) {
    if (ts <= 0) return QString();
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(ts);
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 diffSecs = dt.secsTo(now);
    const QDate dToday = now.date();
    const QDate dMsg   = dt.date();

    if (diffSecs < 60)        return QStringLiteral("刚刚");
    if (diffSecs < 3600)      return QString("%1分前").arg(diffSecs / 60);
    if (dMsg == dToday)       return dt.toString("HH:mm");
    if (dMsg == dToday.addDays(-1))
        return QStringLiteral("昨天 ") + dt.toString("HH:mm");
    if (dMsg.year() == dToday.year())
        return dt.toString("MM-dd");
    return dt.toString("yyyy-MM-dd");
}

QString WeChatSidebar::formatBadge(int n) {
    return n > 99 ? QStringLiteral("99+") : QString::number(n);
}

// ── 数据灌入与缓存 ──────────────────────────────────────

void WeChatSidebar::setLeafData(const QString& accId,
                                const QVariantList& sessions,
                                const QVariantList& contacts) {
    m_sessionsByAcc.insert(accId, sessions);
    m_contactsByAcc.insert(accId, contacts);
    refreshGroupLabelsForAccount(accId);
}

void WeChatSidebar::appendContactsBatch(const QString& accId,
                                        const QVariantList& batch) {
    if (batch.isEmpty()) return;
    auto& cache = m_contactsByAcc[accId];
    QSet<QString> existing;
    existing.reserve(cache.size());
    for (const auto& v : cache) {
        const QString wxid = v.toMap()["userName"].toString();
        if (!wxid.isEmpty()) existing.insert(wxid);
    }
    for (const auto& v : batch) {
        const auto m = v.toMap();
        const QString wxid = m["userName"].toString();
        if (wxid.isEmpty() || existing.contains(wxid)) continue;
        cache.append(m);
        existing.insert(wxid);
    }
    scheduleRefresh(accId);
}

void WeChatSidebar::appendSessionsBatch(const QString& accId,
                                        const QVariantList& batch) {
    if (batch.isEmpty()) return;
    auto& cache = m_sessionsByAcc[accId];
    QSet<QString> existing;
    existing.reserve(cache.size());
    for (const auto& v : cache) {
        const QString t = v.toMap()["talker"].toString();
        if (!t.isEmpty()) existing.insert(t);
    }
    for (const auto& v : batch) {
        const auto m = v.toMap();
        const QString t = m["talker"].toString();
        if (t.isEmpty() || existing.contains(t)) continue;
        cache.append(m);
        existing.insert(t);
    }
    scheduleRefresh(accId);
}

void WeChatSidebar::scheduleRefresh(const QString& accId, int delayMs) {
    auto* t = m_rebuildTimerByAcc.value(accId, nullptr);
    if (!t) {
        t = new QTimer(this);
        t->setSingleShot(true);
        t->setInterval(delayMs);
        connect(t, &QTimer::timeout, this, [this, accId]() {
            refreshGroupLabelsForAccount(accId);
        });
        m_rebuildTimerByAcc.insert(accId, t);
    } else {
        t->setInterval(delayMs);
    }
    t->start();
}

void WeChatSidebar::clearLeafData(const QString& accId) {
    m_sessionsByAcc.remove(accId);
    m_contactsByAcc.remove(accId);
    m_contactsLabelFp.remove(accId);
    m_sessionsLabelFp.remove(accId);
    refreshGroupLabelsForAccount(accId);
}

void WeChatSidebar::refreshGroupLabelsForAccount(const QString& accId) {
    auto* accItem = findAccountItem(accId);
    if (!accItem) return;
    if (accItem->childCount() < 2) return;
    auto* contactGroup = accItem->child(0);
    auto* chatGroup    = accItem->child(1);

    // 联系人分组
    const auto contacts = m_contactsByAcc.value(accId);
    const QString cFp = labelFingerprint(contacts);
    if (cFp != m_contactsLabelFp.value(accId)) {
        m_contactsLabelFp.insert(accId, cFp);
        m_tree->blockSignals(true);
        contactGroup->setText(0, buildContactLabel(contacts));
        m_tree->blockSignals(false);
    }

    // 聊天分组
    const auto sessions = m_sessionsByAcc.value(accId);
    const QString sFp = labelFingerprint(sessions);
    if (sFp != m_sessionsLabelFp.value(accId)) {
        m_sessionsLabelFp.insert(accId, sFp);
        m_tree->blockSignals(true);
        chatGroup->setText(0, buildChatLabel(sessions));
        m_tree->blockSignals(false);
    }
}

void WeChatSidebar::onTreeItemClicked(QTreeWidgetItem* item, int col) {
    if (!item) return;
    const NodeType t = NodeType(item->data(col, NodeTypeRole).toInt());
    const QString accId = item->data(col, Qt::UserRole).toString();

    switch (t) {
    case NodeChatGroup:
        if (!accId.isEmpty()) emit chatGroupClicked(accId);
        break;
    case NodeContactGroup:
        if (!accId.isEmpty()) emit contactGroupClicked(accId);
        break;
    case NodeAccountRoot:
    default:
        break;
    }
}

void WeChatSidebar::onContextMenu(const QPoint& pos) {
    if (!m_tree) return;
    auto* item = m_tree->itemAt(pos);

    auto* menu = new QMenu(this);
    menu->setStyleSheet(QString(
        "QMenu{background:%1;border:1px solid %2;border-radius:6px;padding:4px;}"
        "QMenu::item{padding:6px 18px;color:%3;border-radius:4px;}"
        "QMenu::item:selected{background:%4;}"
        "QMenu::separator{height:1px;background:%2;margin:4px 8px;}")
        .arg(Theme::kSurface, Theme::kBorder, Theme::kText, Theme::kBorder));

    if (item && NodeType(item->data(0, NodeTypeRole).toInt()) == NodeAccountRoot) {
        const QString accId = item->data(0, Qt::UserRole).toString();
        auto* editAct = menu->addAction("编辑账号…");
        connect(editAct, &QAction::triggered, this,
                [this, accId]{ emit editAccountRequested(accId); });
        auto* delAct = menu->addAction("删除账号");
        connect(delAct, &QAction::triggered, this,
                [this, accId]{ emit deleteAccountRequested(accId); });
        menu->addSeparator();
    }
    auto* addAct = menu->addAction("添加微信账号…");
    connect(addAct, &QAction::triggered, this, &WeChatSidebar::addAccountRequested);

    menu->exec(m_tree->viewport()->mapToGlobal(pos));
    delete menu;
}

void WeChatSidebar::onAddClicked()   { emit addAccountRequested(); }
void WeChatSidebar::onRefreshClicked() { emit refreshRequested(); }