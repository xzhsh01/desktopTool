#include "mail/ui/folders/MailFolderPanel.h"

#include "mail/MailAccountManager.h"
#include "mail/MailStore.h"

#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/Theme.h"
#include <functional>

namespace {
// 真实文件夹 → 友好显示名（解码 UTF-7，映射常见英文名为中文）
QString folderDisplayName(const ImapClient::Folder& f) {
    QString dec = ImapClient::decodeFolderName(f.name);
    if (!f.delimiter.isEmpty() && dec.contains(f.delimiter))
        dec = dec.section(f.delimiter, -1);   // 层级名取最后一段
    QString low = dec.toLower();
    if (low == "inbox")                                return "收件箱";
    if (low.contains("sent"))                          return "已发送";
    if (low.contains("draft"))                         return "草稿箱";
    if (low.contains("trash") || low.contains("deleted")) return "已删除";
    if (low.contains("junk") || low.contains("spam"))  return "垃圾邮件";
    if (low.contains("archive"))                       return "归档";
    return dec;
}
} // namespace

QString MailFolderPanel::displayNameForKey(const QString& key) {
    // INBOX/Sent → 中文；服务器名 → 解码 modified UTF-7（取层级末段）
    if (key == "INBOX") return QString::fromUtf8("\346\224\266\344\273\266\347\256\261");
    if (key == "Sent")  return QString::fromUtf8("\345\267\262\345\217\221\351\200\201");
    QString dec = ImapClient::decodeFolderName(key);
    if (dec.contains('/')) dec = dec.section('/', -1);
    return dec.isEmpty() ? key : dec;
}

MailFolderPanel::MailFolderPanel(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);

    // 顶部按钮行：[+ 写邮件 (stretch=1)] [↻ 刷新]
    auto* topBtnRow = new QHBoxLayout;
    topBtnRow->setContentsMargins(0, 0, 0, 0);
    topBtnRow->setSpacing(6);
    m_newMailBtn = new QPushButton(QStringLiteral("+ 写邮件"));
    m_newMailBtn->setCursor(Qt::PointingHandCursor);
    m_newMailBtn->setStyleSheet(Theme::flatBtnPrimary());
    connect(m_newMailBtn, &QPushButton::clicked, this, &MailFolderPanel::newMailRequested);
    topBtnRow->addWidget(m_newMailBtn, 1);
    m_refreshBtn = new QPushButton(QStringLiteral("↻"));
    m_refreshBtn->setCursor(Qt::PointingHandCursor);
    m_refreshBtn->setToolTip(QStringLiteral("刷新当前账号"));
    // 图标按钮：上下 padding 收紧、与写邮件按钮同高
    m_refreshBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: none;"
        " padding: 4px 10px; font-size: 16px; border-radius: 3px; }"
        "QPushButton:hover    { background: %3; }"
        "QPushButton:pressed  { background: %4; }"
        "QPushButton:disabled { background: %1; color: %5; }")
        .arg(Theme::kTitleBar, Theme::kText,
             Theme::kBorderLight, Theme::kBorder, Theme::kDisabled));
    connect(m_refreshBtn, &QPushButton::clicked, this, &MailFolderPanel::refreshRequested);
    topBtnRow->addWidget(m_refreshBtn);
    lay->addLayout(topBtnRow);

    // 搜索框（全局过滤：主题/发件人/正文），固定高度与中/右栏首行按钮对齐
    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText("搜索主题 / 发件人 / 正文...");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedHeight(32);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MailFolderPanel::searchChanged);
    lay->addWidget(m_searchEdit);

    m_tree = new QTreeWidget;
    m_tree->setMinimumWidth(180);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setIndentation(14);
    // 全局 QSS 未覆盖 QTreeWidget，补深色样式（系统默认样式在深色模式下黑字黑底不可读）
    m_tree->setStyleSheet(
        "QTreeWidget { background: #1e2128; alternate-background-color: #1e2128;"
        "  color: #c8c8c8; border: 1px solid #2a2d36; }"
        "QTreeWidget::item { padding: 4px 2px; }"
        "QTreeWidget::item:hover { background: #252830; }"
        "QTreeWidget::item:selected { background: #2a2d36; color: #4fc3f7; }");
    connect(m_tree, &QTreeWidget::itemSelectionChanged,
            this, &MailFolderPanel::selectionChanged);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &MailFolderPanel::onContextMenu);
    lay->addWidget(m_tree, 1);
}

QString MailFolderPanel::currentAccountId() const {
    auto* it = m_tree ? m_tree->currentItem() : nullptr;
    if (!it) return QString();
    return it->data(0, Qt::UserRole).toString();   // 顶级/子级节点均携带 accountId
}

QString MailFolderPanel::currentFolder() const {
    auto* it = m_tree ? m_tree->currentItem() : nullptr;
    if (!it) return QString();
    if (it->parent() == nullptr) return QString();   // 顶级节点（账号）不携带 folderKey
    return it->data(0, Qt::UserRole + 1).toString();
}

QString MailFolderPanel::searchText() const {
    return m_searchEdit ? m_searchEdit->text().trimmed() : QString();
}

void MailFolderPanel::selectAccount(const QString& id) {
    if (!m_tree || id.isEmpty()) return;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* it = m_tree->topLevelItem(i);
        if (it->data(0, Qt::UserRole).toString() == id) {
            m_tree->setCurrentItem(it);   // 触发 selectionChanged → 刷新邮件列表
            return;
        }
    }
}

void MailFolderPanel::setRemoteFolders(const QString& accountId,
                                       const QList<ImapClient::Folder>& folders) {
    m_remoteFolders.insert(accountId, folders);
}

void MailFolderPanel::removeRemoteFolders(const QString& accountId) {
    m_remoteFolders.remove(accountId);
}

bool MailFolderPanel::hasRemoteFolders(const QString& accountId) const {
    return m_remoteFolders.contains(accountId);
}

QList<ImapClient::Folder> MailFolderPanel::remoteFolders(const QString& accountId) const {
    return m_remoteFolders.value(accountId);
}

void MailFolderPanel::rebuild() {
    if (!m_tree) return;
    QString prevAcc  = currentAccountId();
    QString prevFold = currentFolder();
    m_tree->blockSignals(true);
    m_tree->clear();

    const auto& accounts = MailAccountManager::instance().accounts();
    if (accounts.isEmpty()) {
        auto* empty = new QTreeWidgetItem(m_tree, { "(无账号)" });
        empty->setDisabled(true);
        m_tree->blockSignals(false);
        return;
    }

    // 给一个账号构建顶级 + 子级（文件夹）
    auto buildForAccount = [&](const MailAccountManager::Account& a) -> QTreeWidgetItem* {
        QString title = a.isDefault
            ? QString("\xe2\x98\x85 %1").arg(a.name.isEmpty() ? a.email : a.name)
            : (a.name.isEmpty() ? a.email : a.name);
        auto* accItem = new QTreeWidgetItem(m_tree, { title });
        accItem->setData(0, Qt::UserRole, a.id);          // UserRole = accountId
        accItem->setData(0, Qt::UserRole + 1, "");        // UserRole+1 = folderKey (空表示顶级)
        QFont f = accItem->font(0);
        f.setBold(true);
        accItem->setFont(0, f);
        accItem->setToolTip(0, a.email);

        int unreadInbox = MailStore::instance().unreadCount(a.id, "INBOX");

        // 同名文件夹去重：服务器中文文件夹（如"已发送"）与本地固定节点
        // 显示名相同时只保留先添加的本地节点（key 为 Sent/Drafts 等，
        // 邮件同步与缓存均按本地 key 落盘）
        QSet<QString> seenNames;
        // 显示名规则：仅当未读 > 0 才显示数字，0 时不显示后缀（避免空括号噪音）
        auto makeLabel = [&](const QString& name, int unread) {
            return unread > 0 ? QString("%1 (%2)").arg(name).arg(unread) : name;
        };
        auto addFolder = [&](const QString& name, const QString& key, int unread) {
            if (seenNames.contains(name)) return;         // 已有同名文件夹 → 跳过
            seenNames.insert(name);
            auto* it = new QTreeWidgetItem(accItem, { makeLabel(name, unread) });
            it->setData(0, Qt::UserRole, a.id);             // 父账号 id
            it->setData(0, Qt::UserRole + 1, key);          // 文件夹 key
            if (unread > 0) {
                QFont ff = it->font(0); ff.setBold(true); it->setFont(0, ff);
            }
        };
        addFolder("\xe2\x9c\x89 收件箱", "INBOX", unreadInbox);            // ✉ 只显示未读
        addFolder("\xe2\x9c\x89 已发送", "Sent",
                  MailStore::instance().unreadCount(a.id, "Sent"));        // ✉
        addFolder("\xe2\x9c\x8f 草稿箱", "Drafts", 0);                     // ✏ 不显示数字

        // 追加 IMAP LIST 返回的真实文件夹（多级树：按 delimiter 拆分路径逐层挂接；
        // 中间路径若无对应可选文件夹则作为纯容器节点，folderKey 为空、点击不加载邮件）
        const auto remote = m_remoteFolders.value(a.id);
        QHash<QString, QTreeWidgetItem*> nodeOf;   // 原始全路径 → 树节点

        // 本地固定 key → 现有本地节点（避免 ensurePath 重建同名容器）
        auto findLocalByKey = [&](const QString& key) -> QTreeWidgetItem* {
            for (int i = 0; i < accItem->childCount(); ++i) {
                auto* c = accItem->child(i);
                if (c->data(0, Qt::UserRole + 1).toString() == key) return c;
            }
            return nullptr;
        };

        // 递归确保 rawPath 对应的节点链存在（容器节点先以解码段名占位）
        // 关键：中间路径解码后若等于本地固定 key（INBOX/Sent），复用本地节点，
        // 避免「INBOX/Notes」这种层级化文件夹被自动建出独立的"INBOX"容器，
        // 与上方手动的"✉ 收件箱"并排出现。
        std::function<QTreeWidgetItem*(const QString&, const QString&)> ensurePath;
        ensurePath = [&](const QString& rawPath, const QString& delim) -> QTreeWidgetItem* {
            if (auto* n = nodeOf.value(rawPath)) return n;
            QTreeWidgetItem* parent = accItem;
            QString leafRaw = rawPath;
            if (!delim.isEmpty()) {
                const int cut = rawPath.lastIndexOf(delim);
                if (cut > 0) {
                    parent  = ensurePath(rawPath.left(cut), delim);
                    leafRaw = rawPath.mid(cut + delim.size());
                }
            }
            // 路径段命中本地固定 key → 复用现有本地节点（不新建容器）
            QString dec = ImapClient::decodeFolderName(leafRaw);
            if (dec.compare("INBOX", Qt::CaseInsensitive) == 0) {
                if (auto* local = findLocalByKey("INBOX")) {
                    nodeOf.insert(rawPath, local);
                    return local;
                }
            } else if (dec.compare("Sent", Qt::CaseInsensitive) == 0) {
                if (auto* local = findLocalByKey("Sent")) {
                    nodeOf.insert(rawPath, local);
                    return local;
                }
            }
            auto* n = new QTreeWidgetItem(parent, { dec });
            n->setData(0, Qt::UserRole, a.id);
            n->setData(0, Qt::UserRole + 1, QString());   // 默认纯容器：无 folderKey
            nodeOf.insert(rawPath, n);
            return n;
        };

        for (const auto& rf : remote) {
            if (rf.flags.contains("\\Noselect", Qt::CaseInsensitive)) continue;
            QString disp = folderDisplayName(rf);   // 解码 + 取末段 + 常见名中文化
            QString low  = ImapClient::decodeFolderName(rf.name).toLower();
            // 英文变体（sent items / sent messages / drafts…）与中文
            // 变体（已发送 / 草稿…）都归并到上面的本地节点
            if (low == "inbox" || disp == "收件箱") continue;
            if (low.contains("sent") || low.contains("draft")
                || disp == "已发送" || disp == "草稿箱") continue;
            // 只要服务器 LIST 返回就显示（本地无缓存也显示，邮件待同步）。
            // 不再按本地缓存过滤：否则未同步过的文件夹永远不出现，
            // 用户无法选中它，刷新流程也就永远不会为它拉取邮件（死锁）。
            auto* n = ensurePath(rf.name, rf.delimiter);
            n->setData(0, Qt::UserRole + 1, rf.name);   // 真实可选文件夹：写入 key
            int unread = MailStore::instance().unreadCount(a.id, rf.name);
            n->setText(0, makeLabel("\xe2\x9c\x89 " + disp, unread));          // ✉
            if (unread > 0) {
                QFont ff = n->font(0); ff.setBold(true); n->setFont(0, ff);
            }
        }
        // 纯容器节点：未读数聚合子孙合计，便于折叠状态下定位新邮件
        std::function<int(QTreeWidgetItem*)> aggregateUnread;
        aggregateUnread = [&](QTreeWidgetItem* p) -> int {
            int sum = 0;
            for (int i = 0; i < p->childCount(); ++i) {
                auto* ch = p->child(i);
                const QString key = ch->data(0, Qt::UserRole + 1).toString();
                int sub = aggregateUnread(ch);
                if (!key.isEmpty()) {
                    sub += MailStore::instance().unreadCount(a.id, key);
                } else if (sub > 0) {
                    ch->setText(0, QString("%1 (%2)").arg(ch->text(0)).arg(sub));
                    QFont ff = ch->font(0); ff.setBold(true); ch->setFont(0, ff);
                }
                sum += sub;
            }
            return sum;
        };
        for (int i = 0; i < accItem->childCount(); ++i) {
            auto* ch = accItem->child(i);
            if (ch->data(0, Qt::UserRole + 1).toString().isEmpty())
                aggregateUnread(ch);   // 仅从最顶层容器进入，避免重复追加计数
        }

        return accItem;
    };

    QTreeWidgetItem* targetAccItem = nullptr;
    for (const auto& a : accounts) {
        auto* it = buildForAccount(a);
        if (a.id == prevAcc) targetAccItem = it;
    }

    // 恢复选择：优先按 (账号, 文件夹) 递归匹配（多级树）；其次按账号顶级；最后默认第一个顶级
    QTreeWidgetItem* toSelect = nullptr;
    if (targetAccItem && !prevFold.isEmpty()) {
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> findByKey =
            [&](QTreeWidgetItem* p) -> QTreeWidgetItem* {
                for (int i = 0; i < p->childCount(); ++i) {
                    auto* ch = p->child(i);
                    if (ch->data(0, Qt::UserRole + 1).toString() == prevFold) return ch;
                    if (auto* r = findByKey(ch)) return r;
                }
                return nullptr;
            };
        toSelect = findByKey(targetAccItem);
    }
    if (!toSelect) toSelect = targetAccItem;
    if (!toSelect && m_tree->topLevelItemCount() > 0) {
        toSelect = m_tree->topLevelItem(0);
    }
    if (toSelect) {
        m_tree->setCurrentItem(toSelect);
        // 展开选中节点的全部祖先（多级树下保证可见），账号级始终展开
        for (auto* p = toSelect; p; p = p->parent())
            m_tree->expandItem(p);
    }

    m_tree->blockSignals(false);
}

void MailFolderPanel::onContextMenu(const QPoint& pos) {
    QMenu menu(this);
    auto* it = m_tree->itemAt(pos);

    // 解析右键目标对应的账号 id（所有层级节点的 UserRole 均携带 accountId）
    QString accId;
    if (it) accId = it->data(0, Qt::UserRole).toString();

    // 空白处：仅提供添加账号
    if (accId.isEmpty()) {
        menu.addAction("+ 添加邮箱账号", this, &MailFolderPanel::addAccountRequested);
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
        return;
    }

    menu.addAction("+ 添加邮箱账号", this, &MailFolderPanel::addAccountRequested);
    menu.addSeparator();
    menu.addAction("编辑", this, [this, accId]{ emit editAccountRequested(accId); });
    menu.addAction("删除", this, [this, accId]{ emit deleteAccountRequested(accId); });
    menu.addAction("设为默认", this, [this, accId]{ emit setDefaultRequested(accId); });
    menu.addSeparator();
    // 右键目标：文件夹行（非顶级、非草稿）→ 仅同步该文件夹；
    // 顶级账号行 → 全量刷新（文件夹列表 + 邮件）
    QString folderKey;
    if (it && it->parent())
        folderKey = it->data(0, Qt::UserRole + 1).toString();
    menu.addAction("刷新", this, [this, accId, folderKey, it]{
        if (!folderKey.isEmpty() && folderKey != "Drafts") {
            if (it) m_tree->setCurrentItem(it);   // 选中该文件夹（本地加载列表）
            emit refreshFolderRequested(accId, folderKey);
        } else {
            emit refreshAccountRequested(accId);
        }
    });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}
