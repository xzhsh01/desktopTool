#include "wechat/ui/WeChatSidebar.h"
#include "wechat/WeChatAccountManager.h"
#include "app/Theme.h"

#include <QAction>
#include <QHBoxLayout>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

using Account = WeChatAccountManager::Account;

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

    // ── 树形侧边栏（账号 + 文件夹，不展开叶子） ──
    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setExpandsOnDoubleClick(false);          // 单击触发，不靠双击
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setMinimumWidth(180);
    m_tree->setIndentation(18);
    m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_tree->setStyleSheet(QString(
        "QTreeWidget{background:%1;border:1px solid %2;outline:0;color:%3;}"
        "QTreeWidget::item{height:30px;border-radius:6px;margin:1px 0;}"
        "QTreeWidget::item:hover{background:%4;}"
        "QTreeWidget::item:selected{background:%5;color:%6;}"
        "QTreeWidget::branch{background:transparent;}")
        .arg(Theme::kBg, Theme::kBorder, Theme::kText,
             Theme::kSurface, Theme::kBorder, Theme::kAccent));
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
    if (it) {
        m_tree->setCurrentItem(it);
        for (auto* p = it; p; p = p->parent()) m_tree->expandItem(p);
    }
}

void WeChatSidebar::rebuildTree(const QString& selectAccId) {
    if (!m_tree) return;
    const QString prevSel = currentAccountId();
    const QString target = !selectAccId.isEmpty() ? selectAccId : prevSel;

    m_tree->blockSignals(true);
    m_tree->clear();

    for (const auto& a : WeChatAccountManager::instance().accounts()) {
        auto* accItem = makeAccountItem(a.id, a.name);
        m_tree->addTopLevelItem(accItem);

        auto* chatFolder    = makeFolderItem(a.id, "💬 聊天",    NodeChatFolder);
        auto* contactFolder = makeFolderItem(a.id, "👥 联系人", NodeContactFolder);
        accItem->addChild(chatFolder);
        accItem->addChild(contactFolder);

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

QTreeWidgetItem* WeChatSidebar::findAccountItem(const QString& accId) const {
    if (!m_tree || accId.isEmpty()) return nullptr;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* it = m_tree->topLevelItem(i);
        if (it->data(0, Qt::UserRole).toString() == accId) return it;
    }
    return nullptr;
}

void WeChatSidebar::onTreeItemClicked(QTreeWidgetItem* item, int col) {
    if (!item) return;
    const NodeType t = NodeType(item->data(col, NodeTypeRole).toInt());
    const QString accId = item->data(col, Qt::UserRole).toString();

    switch (t) {
    case NodeChatFolder:
        emit chatFolderClicked(accId);
        break;
    case NodeContactFolder:
        emit contactFolderClicked(accId);
        break;
    default:
        // 账号根节点：什么都不做（选中即生效）
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