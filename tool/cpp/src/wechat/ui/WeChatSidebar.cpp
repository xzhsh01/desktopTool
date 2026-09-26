#include "wechat/ui/WeChatSidebar.h"
#include "wechat/WeChatAccountManager.h"
#include "app/Theme.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

using Account = WeChatAccountManager::Account;

namespace {
// 圆形头像（与原 WeChatWidget 内实现保持一致）
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

    // ── 搜索框 ──
    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText("搜索会话 / 联系人");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedHeight(32);
    m_searchEdit->setStyleSheet(QString(
        "QLineEdit{background:%1;border:1px solid %2;border-radius:4px;"
        "padding:6px 10px;color:%3;}"
        "QLineEdit:focus{border-color:%4;}")
        .arg(Theme::kSurface, Theme::kBorder, Theme::kText, Theme::kCatWeChat));
    connect(m_searchEdit, &QLineEdit::textChanged, this, &WeChatSidebar::onSearchChanged);
    lay->addWidget(m_searchEdit);

    // ── 树形侧边栏 ──
    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setMinimumWidth(180);
    m_tree->setIndentation(18);
    m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_tree->setStyleSheet(QString(
        "QTreeWidget{background:%1;border:1px solid %2;outline:0;color:%3;}"
        "QTreeWidget::item{height:28px;border-radius:6px;margin:1px 0;}"
        "QTreeWidget::item:hover{background:%4;}"
        "QTreeWidget::item:selected{background:%5;color:%6;}"
        "QTreeWidget::branch{background:transparent;}")
        .arg(Theme::kBg, Theme::kBorder, Theme::kText,
             Theme::kSurface, Theme::kBorder, Theme::kAccent));
    connect(m_tree, &QTreeWidget::itemClicked,
            this, &WeChatSidebar::onTreeItemClicked);
    connect(m_tree, &QTreeWidget::itemExpanded,
            this, &WeChatSidebar::onTreeItemExpanded);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &WeChatSidebar::onContextMenu);
    lay->addWidget(m_tree, 1);
}

QString WeChatSidebar::currentAccountId() const {
    auto* it = m_tree ? m_tree->currentItem() : nullptr;
    if (!it) return QString();
    // 顶级 / 子级 / 叶子节点都把 accountId 存在 UserRole
    QTreeWidgetItem* p = it;
    while (p && p->parent()) p = p->parent();
    return p ? p->data(0, Qt::UserRole).toString() : QString();
}

void WeChatSidebar::selectAccount(const QString& accId) {
    if (!m_tree || accId.isEmpty()) return;
    auto* it = findAccountItem(accId);
    if (it) {
        m_tree->setCurrentItem(it);
        for (auto* p = it; p; p = p->parent()) m_tree->expandItem(p);
    }
}

bool WeChatSidebar::hasData(const QString& accId) const {
    return m_sessionsCache.contains(accId);
}

void WeChatSidebar::setSessions(const QString& accId, const QVariantList& list) {
    m_sessionsCache.insert(accId, list);
    updateFolderCount(accId);
    auto* accItem = findAccountItem(accId);
    if (!accItem) return;
    for (int j = 0; j < accItem->childCount(); ++j) {
        auto* folder = accItem->child(j);
        if (folder && NodeType(folder->data(0, NodeTypeRole).toInt()) == NodeChatFolder) {
            if (folder->isExpanded()) fillChatLeaves(folder, accId);
        }
    }
}

void WeChatSidebar::setContacts(const QString& accId, const QVariantList& list) {
    m_contactsCache.insert(accId, list);
    updateFolderCount(accId);
    auto* accItem = findAccountItem(accId);
    if (!accItem) return;
    for (int j = 0; j < accItem->childCount(); ++j) {
        auto* folder = accItem->child(j);
        if (folder && NodeType(folder->data(0, NodeTypeRole).toInt()) == NodeContactFolder) {
            if (folder->isExpanded()) fillContactLeaves(folder, accId);
        }
    }
}

void WeChatSidebar::clearData(const QString& accId) {
    m_sessionsCache.remove(accId);
    m_contactsCache.remove(accId);
    auto* accItem = findAccountItem(accId);
    if (!accItem) return;
    for (int j = 0; j < accItem->childCount(); ++j) {
        auto* folder = accItem->child(j);
        if (folder) clearFolderChildren(folder);
    }
    updateFolderCount(accId);
}

QString WeChatSidebar::searchText() const {
    return m_searchEdit ? m_searchEdit->text().trimmed() : QString();
}

// ── 树形构建 ────────────────────────────────────────────────────────────────

void WeChatSidebar::rebuildTree(const QString& selectAccId) {
    if (!m_tree) return;
    const QString prevSel = currentAccountId();
    const QString target = !selectAccId.isEmpty() ? selectAccId : prevSel;

    m_tree->blockSignals(true);
    m_tree->clear();

    for (const auto& a : WeChatAccountManager::instance().accounts()) {
        auto* accItem = makeAccountItem(a.id, a.name);
        m_tree->addTopLevelItem(accItem);

        auto* chatFolder = makeFolderItem(a.id, "💬 聊天", NodeChatFolder);
        auto* contactFolder = makeFolderItem(a.id, "👥 联系人", NodeContactFolder);
        accItem->addChild(chatFolder);
        accItem->addChild(contactFolder);

        updateFolderCount(a.id);
        accItem->setExpanded(true);   // 默认展开账号
    }
    m_tree->blockSignals(false);

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
    return it;
}

QTreeWidgetItem* WeChatSidebar::makeFolderItem(const QString& accId,
                                                const QString& title,
                                                NodeType type) const {
    auto* it = new QTreeWidgetItem;
    it->setData(0, Qt::UserRole, accId);
    it->setData(0, NodeTypeRole, type);
    it->setText(0, title);
    return it;
}

void WeChatSidebar::updateFolderCount(const QString& accId) {
    auto* accItem = findAccountItem(accId);
    if (!accItem) return;
    const int chat = m_sessionsCache.value(accId).size();
    const int contact = m_contactsCache.value(accId).size();
    for (int j = 0; j < accItem->childCount(); ++j) {
        auto* folder = accItem->child(j);
        const NodeType t = NodeType(folder->data(0, NodeTypeRole).toInt());
        if (t == NodeChatFolder)
            folder->setText(0, QString("💬 聊天 (%1)").arg(chat));
        else if (t == NodeContactFolder)
            folder->setText(0, QString("👥 联系人 (%1)").arg(contact));
    }
}

void WeChatSidebar::clearFolderChildren(QTreeWidgetItem* folder) {
    if (!folder) return;
    // 移除时连同子节点挂载的 QWidget（itemWidget）一起释放
    for (int i = 0; i < folder->childCount(); ++i) {
        QWidget* w = m_tree->itemWidget(folder->child(i), 0);
        if (w) w->deleteLater();
    }
    folder->takeChildren();   // 不递归 delete，itemWidget 已显式 deleteLater
}

void WeChatSidebar::fillChatLeaves(QTreeWidgetItem* folder, const QString& accId) {
    if (!folder) return;
    clearFolderChildren(folder);

    if (!m_sessionsCache.contains(accId)) {
        emit loadSessionsRequested(accId);
        return;
    }

    const auto sessions = m_sessionsCache.value(accId);
    const QString filter = searchText();

    for (const auto& v : sessions) {
        const auto s = v.toMap();
        const QString title = s["title"].toString();
        if (!filter.isEmpty() && !title.contains(filter, Qt::CaseInsensitive)) continue;

        auto* leaf = new QTreeWidgetItem;
        leaf->setData(0, Qt::UserRole, s["talker"].toString());   // talker
        leaf->setData(0, Qt::UserRole + 1, accId);                 // accId
        leaf->setData(0, NodeTypeRole, NodeChatLeaf);
        leaf->setSizeHint(0, QSize(260, 56));

        auto* row = new QWidget;
        row->setStyleSheet("background:transparent;");
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(8, 4, 8, 4);
        rowLay->setSpacing(10);

        auto* avatar = new QLabel;
        avatar->setPixmap(roundAvatar(title, s["talker"].toString(), 36));
        rowLay->addWidget(avatar);

        auto* col = new QVBoxLayout;
        col->setSpacing(2);
        auto* name = new QLabel(title);
        name->setStyleSheet(QString("color:%1; font-size:13px;").arg(Theme::kTextBright));
        col->addWidget(name);
        const QString preview = s["lastMsg"].toString();
        if (!preview.isEmpty()) {
            auto* pv = new QLabel(preview);
            pv->setStyleSheet(QString("color:%1; font-size:11px;").arg(Theme::kMuted));
            pv->setMaximumWidth(180);
            pv->setText(QString(preview).left(60));
            col->addWidget(pv);
        }
        rowLay->addLayout(col, 1);

        m_tree->setItemWidget(leaf, 0, row);
        folder->addChild(leaf);
    }

    if (folder->childCount() == 0) {
        auto* empty = new QTreeWidgetItem(folder);
        empty->setDisabled(true);
        empty->setForeground(0, QColor(Theme::kMuted));
        empty->setText(0, "  暂无会话");
    }
}

void WeChatSidebar::fillContactLeaves(QTreeWidgetItem* folder, const QString& accId) {
    if (!folder) return;
    clearFolderChildren(folder);

    if (!m_contactsCache.contains(accId)) {
        emit loadContactsRequested(accId);
        return;
    }

    const auto contacts = m_contactsCache.value(accId);
    const QString filter = searchText();

    for (const auto& v : contacts) {
        const auto c = v.toMap();
        const QString display = c["display"].toString();
        if (!filter.isEmpty() && !display.contains(filter, Qt::CaseInsensitive)) continue;

        auto* leaf = new QTreeWidgetItem;
        leaf->setData(0, Qt::UserRole, c["userName"].toString());   // wxid
        leaf->setData(0, Qt::UserRole + 1, accId);                  // accId
        leaf->setData(0, NodeTypeRole, NodeContactLeaf);
        leaf->setSizeHint(0, QSize(260, 48));

        auto* row = new QWidget;
        row->setStyleSheet("background:transparent;");
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(8, 4, 8, 4);
        rowLay->setSpacing(10);

        auto* avatar = new QLabel;
        avatar->setPixmap(roundAvatar(display, c["userName"].toString(), 36));
        rowLay->addWidget(avatar);

        auto* name = new QLabel(display);
        name->setStyleSheet(QString("color:%1; font-size:13px;").arg(Theme::kText));
        rowLay->addWidget(name, 1);

        if (c["isRoom"].toBool()) {
            auto* tag = new QLabel("群聊");
            tag->setStyleSheet(QString("color:%1; font-size:11px; border:1px solid %1;"
                                       "border-radius:3px; padding:0 4px;")
                                   .arg(Theme::kCatWeChat));
            rowLay->addWidget(tag);
        }
        m_tree->setItemWidget(leaf, 0, row);
        folder->addChild(leaf);
    }

    if (folder->childCount() == 0) {
        auto* empty = new QTreeWidgetItem(folder);
        empty->setDisabled(true);
        empty->setForeground(0, QColor(Theme::kMuted));
        empty->setText(0, "  暂无联系人");
    }
}

QTreeWidgetItem* WeChatSidebar::findAccountItem(const QString& accId) const {
    if (!m_tree || accId.isEmpty()) return nullptr;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* it = m_tree->topLevelItem(i);
        if (it->data(0, Qt::UserRole).toString() == accId) return it;
    }
    return nullptr;
}

// ── 树节点交互 ──────────────────────────────────────────────────────────────

void WeChatSidebar::onTreeItemClicked(QTreeWidgetItem* item, int col) {
    if (!item) return;
    const NodeType t = NodeType(item->data(col, NodeTypeRole).toInt());

    switch (t) {
    case NodeChatLeaf: {
        const QString talker = item->data(col, Qt::UserRole).toString();
        const QString accId  = item->data(col, Qt::UserRole + 1).toString();
        emit openChatRequested(accId, talker);
        break;
    }
    case NodeContactLeaf: {
        const QString wxid = item->data(col, Qt::UserRole).toString();
        const QString accId = item->data(col, Qt::UserRole + 1).toString();
        emit showContactRequested(accId, wxid);
        break;
    }
    case NodeChatFolder: {
        const QString accId = item->data(col, Qt::UserRole).toString();
        fillChatLeaves(item, accId);
        break;
    }
    case NodeContactFolder: {
        const QString accId = item->data(col, Qt::UserRole).toString();
        fillContactLeaves(item, accId);
        break;
    }
    default: break;
    }
}

void WeChatSidebar::onTreeItemExpanded(QTreeWidgetItem* item) {
    if (!item) return;
    const NodeType t = NodeType(item->data(0, NodeTypeRole).toInt());
    const QString accId = item->data(0, Qt::UserRole).toString();
    if (t == NodeChatFolder)        fillChatLeaves(item, accId);
    else if (t == NodeContactFolder) fillContactLeaves(item, accId);
}

void WeChatSidebar::onSearchChanged(const QString&) {
    if (!m_tree) return;
    // 清掉已展开分类下的内容，让下次展开重新填充（按 filter 过滤）
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* accItem = m_tree->topLevelItem(i);
        for (int j = 0; j < accItem->childCount(); ++j) {
            auto* folder = accItem->child(j);
            if (folder->childCount() > 0) clearFolderChildren(folder);
        }
    }
    // 自动展开当前账号的两个分类
    auto* curAcc = findAccountItem(currentAccountId());
    if (curAcc) {
        for (int j = 0; j < curAcc->childCount(); ++j) curAcc->child(j)->setExpanded(true);
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