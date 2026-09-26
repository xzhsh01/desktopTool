#include "mail/ui/list/MailListPanel.h"

#include "mail/MailStore.h"
#include "mail/ui/list/MailListDelegate.h"
#include "app/Theme.h"
#include "core/Logger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString fmtDate(const QDateTime& dt) {
    if (!dt.isValid()) return "";
    QDateTime now = QDateTime::currentDateTime();
    if (dt.date() == now.date()) return dt.toString("HH:mm");
    if (dt.date().year() == now.date().year()) return dt.toString("MM-dd");
    return dt.toString("yyyy-MM-dd");
}
} // namespace

MailListPanel::MailListPanel(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);

    auto* bar = new QHBoxLayout;
    // "写邮件"按钮已迁移到 MailFolderPanel（侧边栏搜索框上方）
    m_deleteBtn = new QPushButton("删除");
    m_deleteBtn->setCursor(Qt::PointingHandCursor);
    m_deleteBtn->setStyleSheet(Theme::flatBtnDanger());
    m_deleteBtn->setEnabled(false);
    m_deleteBtn->hide();   // 默认隐藏：选中邮件后才显示（setDeleteEnabled(true) 同时 setVisible(true)）
    connect(m_deleteBtn, &QPushButton::clicked, this, &MailListPanel::deleteRequested);
    bar->addWidget(m_deleteBtn);
    bar->addStretch();
    lay->addLayout(bar);

    // 单列块状列表：每行一个邮件块（delegate 自绘 发件人/时间/主题/摘要）
    m_table = new QTableWidget(0, 1, this);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setItemDelegate(new MailListDelegate(m_table));
    // 左键：toggle 复选框；右键：customContextMenuRequested 透出
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &MailListPanel::selectionChanged);
    connect(m_table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* it){
        if (it) emit itemDoubleClicked(it->data(Qt::UserRole).toString());
    });
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &MailListPanel::contextMenuRequested);
    connect(m_table, &QTableWidget::cellClicked, this, [this](int, int){
        emit rowClicked();
    });
    lay->addWidget(m_table, 2);
}

QString MailListPanel::selectedKey() const {
    if (!m_table || m_table->currentRow() < 0) return QString();
    auto* it = m_table->item(m_table->currentRow(), 0);
    return it ? it->data(Qt::UserRole).toString() : QString();
}

QString MailListPanel::selectedMessageId() const {
    QString key = selectedKey();
    if (key.startsWith("draft:")) return QString();
    return key;
}

QString MailListPanel::selectedDraftId() const {
    QString key = selectedKey();
    if (!key.startsWith("draft:")) return QString();
    return key.mid(6);   // 去掉 "draft:" 前缀
}

bool MailListPanel::hasSelection() const {
    return m_table && m_table->currentRow() >= 0;
}

void MailListPanel::setDeleteEnabled(bool on) {
    if (m_deleteBtn) {
        m_deleteBtn->setEnabled(on);
        m_deleteBtn->setVisible(on);   // 选中才显示，未选中隐藏
    }
}

int MailListPanel::rowAt(const QPoint& pos) const {
    return m_table ? m_table->rowAt(pos.y()) : -1;
}

bool MailListPanel::isGroupRow(int row) const {
    auto* it = m_table ? m_table->item(row, 0) : nullptr;
    return it && it->data(Qt::UserRole + 1).toString() == "group";
}

QString MailListPanel::keyAt(int row) const {
    auto* it = m_table ? m_table->item(row, 0) : nullptr;
    return it ? it->data(Qt::UserRole).toString() : QString();
}

bool MailListPanel::isChecked(int row) const {
    auto* it = m_table ? m_table->item(row, 0) : nullptr;
    return it && it->data(Qt::CheckStateRole).value<Qt::CheckState>() == Qt::Checked;
}

void MailListPanel::setChecked(int row, bool on) {
    if (auto* it = m_table ? m_table->item(row, 0) : nullptr)
        it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
}

void MailListPanel::setCurrentRow(int row) {
    if (m_table) m_table->setCurrentCell(row, 0);
}

void MailListPanel::collectChecked(QStringList& mailIds, QStringList& draftIds) const {
    if (!m_table) return;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        auto* it = m_table->item(r, 0);
        if (!it || it->data(Qt::CheckStateRole).value<Qt::CheckState>() != Qt::Checked)
            continue;
        QString key = it->data(Qt::UserRole).toString();
        if (key.startsWith("draft:")) draftIds << key.mid(6);
        else if (!key.isEmpty())     mailIds << key;
    }
}

void MailListPanel::refreshMessages(const QString& accountId, const QString& folder,
                                    const QString& keyword) {
    if (!m_table) return;
    // 记录当前选中行 key（邮件 id 或 draft:id），重建后恢复选中，
    // 避免 markRead/updateBody 触发重建 → 选中丢失 → 预览被清空
    const QString keepKey = selectedKey();
    // 无感刷新：同一视图（账号/文件夹/关键词均未变）不预清空列表，
    // 旧内容保持可见直到后台构建完成替换；仅切换视图时才立即清空
    const bool sameView = (accountId == m_lastAcc && folder == m_lastFolder
                           && keyword == m_lastKw);
    m_lastAcc = accountId; m_lastFolder = folder; m_lastKw = keyword;
    if (!sameView) {
        m_lastFingerprint.clear();   // 视图切换：指纹失效，强制重建
        m_table->setRowCount(0);
    }
    if (folder == "Drafts") {
        fillDrafts(accountId, keyword, keepKey);
        return;
    }
    if (accountId.isEmpty() || folder.isEmpty()) {
        Logger::instance().info(
            QString("refreshMessages: 跳过 (acc='%1' folder='%2')").arg(accountId, folder),
            "mail");
        return;
    }

    // ── 邮件列表：主线程拷贝数据快照，再交 renderMailRows 过滤/分组/填充 ──
    auto msgs = MailStore::instance().messagesIn(accountId, folder);
    renderMailRows(msgs, keyword, keepKey);
}

// 通用邮件行渲染（refreshMessages 用）：
// 主线程仅做数据拷贝（MailStore 非线程安全，只允许主线程访问；
// QList/QString 隐式共享，主线程后续写会 detach，不影响本拷贝），
// 过滤 / 分组 / 格式化放后台线程，完成后回主线程填充控件。
void MailListPanel::renderMailRows(const QList<MailStore::Message>& msgs,
                                   const QString& keyword, const QString& keepKey) {
    if (!m_table) return;
    const QString kw = keyword;
    const quint64 gen = ++m_listGen;   // 代次号：快速切换文件夹时丢弃过期结果

    QThread* t = QThread::create([this, msgs, kw, gen, keepKey]() {
        // 搜索过滤（纯数据操作，不碰 MailStore / UI）
        QList<MailStore::Message> view = msgs;
        if (!kw.isEmpty()) {
            QString kl = kw.toLower();
            QList<MailStore::Message> hit;
            for (const auto& m : view) {
                if (m.subject.toLower().contains(kl)
                        || m.from.toLower().contains(kl)
                        || m.body.toLower().contains(kl)
                        || m.to.join(" ").toLower().contains(kl))
                    hit.append(m);
            }
            view = hit;
        }
        // 分组：未读置顶（组 0）+ 今天 / 昨天 / 本周（周一起）/ 更早（组 1..4）。
        // view 已按日期降序；未读邮件单独成组放最前实现"置顶"，
        // 已读邮件进入对应时间组（未读不再重复出现在时间组里）
        QDate today  = QDate::currentDate();
        QDate monday = today.addDays(-(today.dayOfWeek() - 1));
        auto groupOf = [today, monday](const QDateTime& dt) -> int {
            QDate d = dt.date();
            if (d == today) return 0;
            if (d == today.addDays(-1)) return 1;
            if (d >= monday) return 2;
            return 3;
        };
        const QString groupTitles[5] = { "未读", "今天", "昨天", "本周", "更早" };
        QList<QList<int>> byGroup(5);
        for (int i = 0; i < view.size(); ++i)
            byGroup[view[i].read ? groupOf(view[i].date) + 1 : 0].append(i);

        // 行数据（纯值类型，跨线程传递安全）
        QList<ListRow> rows;
        for (int g = 0; g < 5; ++g) {
            if (byGroup[g].isEmpty()) continue;
            ListRow head;
            head.kind    = "group";
            head.subject = QString("%1 (%2封)").arg(groupTitles[g]).arg(byGroup[g].size());
            rows.append(head);
            for (int idx : byGroup[g]) {
                const auto& m = view[idx];
                ListRow r;
                r.key     = m.id;
                r.kind    = "mail";
                r.from    = m.from;
                r.unread  = !m.read;
                // 时间：今天只显示时刻，其余完整日期时间
                r.time    = (m.date.date() == today)
                                ? m.date.time().toString("HH:mm:ss")
                                : m.date.toString("yyyy-MM-dd HH:mm:ss");
                r.subject = m.subject.isEmpty() ? "(无主题)" : m.subject;
                rows.append(r);
            }
        }
        // 回主线程填充（过期代次直接丢弃）
        QMetaObject::invokeMethod(this, [this, rows, gen, keepKey]() {
            if (gen != m_listGen) return;
            fillTable(rows, keepKey);
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

// 草稿文件夹：本地渲染（无 read/星标/附件维度，仅按关键词过滤）
// 无感刷新：预清空由 refreshMessages 按同视图判断控制；此处快照/恢复勾选与滚动
void MailListPanel::fillDrafts(const QString& accountId, const QString& keyword,
                               const QString& keepKey) {
    const QSet<QString> checkedKeys = checkedKeysSnapshot();
    const QString topKey = topVisibleKey();
    const int scrollPos = m_table->verticalScrollBar()->value();

    QList<MailStore::Draft> my;
    for (const auto& d : MailStore::instance().drafts())
        if (d.accountId == accountId) my.append(d);
    QString kw = keyword;
    if (!kw.isEmpty()) {
        QString kwLower = kw.toLower();
        QList<MailStore::Draft> filtered;
        for (const auto& d : my) {
            bool hit = d.subject.toLower().contains(kwLower)
                    || d.body.toLower().contains(kwLower)
                    || d.to.join(" ").toLower().contains(kwLower);
            if (hit) filtered.append(d);
        }
        my = filtered;
    }
    std::sort(my.begin(), my.end(), [](const MailStore::Draft& a, const MailStore::Draft& b){
        return a.updatedAt > b.updatedAt;
    });
    m_table->setUpdatesEnabled(false);
    int row = 0;
    for (const auto& d : my) {
        m_table->insertRow(row);
        auto* it = new QTableWidgetItem();
        it->setData(Qt::UserRole,     "draft:" + d.id);
        it->setData(Qt::UserRole + 1, "draft");
        // 字段顺序必须与 MailListDelegate::paint 一致：
        //   +2 = from（发件人栏；草稿没有"发件人"，显示收件人方便一眼看出给谁写的）
        //   +3 = time（更新时间）
        //   +4 = subject（主题）
        //   +5 = summary（收件人 + 正文摘要，双行）
        // 列表不显示邮件正文：第二行只显示收件人（去掉正文摘要）
        const QString recipientLine = QStringLiteral("收件人: ") +
            (d.to.isEmpty() ? QStringLiteral("(未填)") : d.to.join(QStringLiteral(", ")));
        it->setData(Qt::UserRole + 2, recipientLine);
        it->setData(Qt::UserRole + 3, fmtDate(d.updatedAt));
        it->setData(Qt::UserRole + 4, d.subject.isEmpty() ? QStringLiteral("(无主题草稿)") : d.subject);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        it->setCheckState(checkedKeys.contains("draft:" + d.id) ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, 0, it);
        ++row;
    }
    // 恢复选中
    if (!keepKey.isEmpty()) {
        for (int r = 0; r < m_table->rowCount(); ++r) {
            auto* it = m_table->item(r, 0);
            if (it && it->data(Qt::UserRole).toString() == keepKey) {
                m_table->setCurrentCell(r, 0);
                break;
            }
        }
    }
    restoreScroll(topKey, scrollPos);
    m_table->setUpdatesEnabled(true);
}

// ── 无感刷新辅助 ─────────────────────────────────────────────
// 后台线程构建好的行数据 → 主线程批量填充表格（唯一碰 UI 的地方）
void MailListPanel::fillTable(const QList<ListRow>& rows, const QString& keepKey) {
    // 无感刷新①：行内容指纹与上次一致 → 数据无变化，跳过重建
    // （勾选/滚动/选中状态天然保留；正文回写、轮询空转等高频触发不再闪列表）
    const QByteArray fp = rowsFingerprint(rows);
    if (fp == m_lastFingerprint) {
        if (!keepKey.isEmpty()) {
            for (int r = 0; r < m_table->rowCount(); ++r) {
                auto* it = m_table->item(r, 0);
                if (it && it->data(Qt::UserRole).toString() == keepKey) {
                    if (m_table->currentRow() != r) m_table->setCurrentCell(r, 0);
                    break;
                }
            }
        }
        return;
    }
    m_lastFingerprint = fp;

    // 无感刷新②：重建前快照勾选集合与滚动锚点，填充后恢复
    const QSet<QString> checkedKeys = checkedKeysSnapshot();
    const QString topKey = topVisibleKey();
    const int scrollPos = m_table->verticalScrollBar()->value();

    m_table->setUpdatesEnabled(false);
    m_table->setRowCount(0);
    for (int row = 0; row < rows.size(); ++row) {
        const auto& r = rows[row];
        m_table->insertRow(row);
        auto* it = new QTableWidgetItem();
        it->setData(Qt::UserRole,     r.key);
        it->setData(Qt::UserRole + 1, r.kind);
        it->setData(Qt::UserRole + 2, r.from);
        it->setData(Qt::UserRole + 3, r.time);
        it->setData(Qt::UserRole + 4, r.subject);
        it->setData(Qt::UserRole + 6, r.unread);   // 未读样式（delegate 加粗+高亮）
        // 分组标题行：自定义 paint 用 o.text (Qt::DisplayRole) 画标题，
        // 邮件行：自定义 paint 用 UserRole+2..5 各字段，不读 DisplayRole。
        // 不设 DisplayRole 会让分组行渲染为空（看起来"分类消失"）。
        if (r.kind == "group")
            it->setData(Qt::DisplayRole, r.subject);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        it->setCheckState(checkedKeys.contains(r.key) ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, 0, it);
    }
    // 恢复选中行（避免重建后选中丢失、预览被清空）
    if (!keepKey.isEmpty()) {
        for (int r = 0; r < m_table->rowCount(); ++r) {
            auto* it = m_table->item(r, 0);
            if (it && it->data(Qt::UserRole).toString() == keepKey) {
                m_table->setCurrentCell(r, 0);
                break;
            }
        }
    }
    // 无感刷新③：恢复滚动位置（顶部锚点行优先，像素值兜底）
    restoreScroll(topKey, scrollPos);
    m_table->setUpdatesEnabled(true);
}

// 全部行字段序列化入 hash（key/kind/from/time/subject/unread 任一变化即视为数据变化）
QByteArray MailListPanel::rowsFingerprint(const QList<ListRow>& rows) {
    if (rows.isEmpty()) return QByteArray();
    QCryptographicHash h(QCryptographicHash::Md5);
    for (const auto& r : rows) {
        h.addData(r.key.toUtf8());
        h.addData(r.kind.toUtf8());
        h.addData(r.from.toUtf8());
        h.addData(r.time.toUtf8());
        h.addData(r.subject.toUtf8());
        h.addData(QByteArray::number(int(r.unread)));
    }
    return h.result();
}

QSet<QString> MailListPanel::checkedKeysSnapshot() const {
    QSet<QString> out;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        auto* it = m_table->item(r, 0);
        if (it && it->checkState() == Qt::Checked)
            out.insert(it->data(Qt::UserRole).toString());
    }
    return out;
}

QString MailListPanel::topVisibleKey() const {
    const int topRow = m_table->rowAt(0);   // viewport 顶端的行
    if (topRow < 0) return QString();
    auto* it = m_table->item(topRow, 0);
    return it ? it->data(Qt::UserRole).toString() : QString();
}

void MailListPanel::restoreScroll(const QString& topKey, int fallbackPos) {
    auto* sb = m_table->verticalScrollBar();
    if (!topKey.isEmpty()) {
        // 按顶部锚点行重定位：行数/行高变化时仍能对准同一封邮件
        for (int r = 0; r < m_table->rowCount(); ++r) {
            auto* it = m_table->item(r, 0);
            if (it && it->data(Qt::UserRole).toString() == topKey) {
                sb->setValue(m_table->rowViewportPosition(r));
                return;
            }
        }
    }
    sb->setValue(fallbackPos);
}
