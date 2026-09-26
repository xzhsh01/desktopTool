#include "mail/ui/list/MailListDelegate.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionButton>

void MailListDelegate::drawElided(QPainter* p, const QRect& r, const QColor& c,
                                  const QString& text, const QFont& f) {
    p->setFont(f);
    p->setPen(c);
    p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(text, Qt::ElideRight,
                                            qMax(0, r.width())));
}

QFont MailListDelegate::bumpFont(const QFont& base, bool bold, qreal delta) {
    QFont f = base;
    if (bold) f.setBold(true);
    if (f.pointSizeF() > 0) f.setPointSizeF(f.pointSizeF() + delta);
    return f;
}

void MailListDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt,
                             const QModelIndex& idx) const {
    QStyleOptionViewItem o = opt;
    initStyleOption(&o, idx);
    // 阻止默认 checkbox 渲染（避免与下方自绘复选框重叠成两个）
    o.features &= ~QStyleOptionViewItem::HasCheckIndicator;
    QStyle* st = o.widget ? o.widget->style() : QApplication::style();

    if (idx.data(Qt::UserRole + 1).toString() == "group") {
        p->fillRect(opt.rect, QColor(0x25, 0x28, 0x30));   // kSurface（比表底亮一档）
        drawElided(p, opt.rect.adjusted(10, 0, -8, 0),
                   QColor(0xe0, 0xe0, 0xe0),               // kTextBright
                   o.text, bumpFont(o.font, true));
        return;
    }

    // 背景（含选中/悬停高亮），不绘制默认 indicator
    st->drawControl(QStyle::CE_ItemViewItem, &o, p, o.widget);

    const bool    selected = o.state.testFlag(QStyle::State_Selected);
    const bool    unread   = idx.data(Qt::UserRole + 6).toBool();
    const QString from     = idx.data(Qt::UserRole + 2).toString();
    const QString time     = idx.data(Qt::UserRole + 3).toString();
    const QString subj     = idx.data(Qt::UserRole + 4).toString();
    const QString summ     = idx.data(Qt::UserRole + 5).toString();

    // 选中态用亮色，否则用深色主题文本色（表底 #1e2128 为全局 QSS 所设，
    // 浅色主题的深字在深底上不可读）：主题/发件人 kText，时间/摘要 kMuted，
    // 未读发件人 kAccent
    QColor txtSec  = selected ? QColor(225, 232, 245) : QColor(0x88, 0x88, 0x88);
    QColor txtSubj = selected ? QColor(255, 255, 255) : QColor(0xc8, 0xc8, 0xc8);
    QColor txtTime = selected ? QColor(210, 220, 235) : QColor(0x88, 0x88, 0x88);
    QColor txtFrom = selected ? QColor(255, 255, 255)
                              : (unread ? QColor(0x4f, 0xc3, 0xf7)
                                        : QColor(0xc8, 0xc8, 0xc8));

    const int x0 = opt.rect.left() + 10, x1 = opt.rect.right() - 10;

    // 复选框：唯一一处，垂直居中（行高 ~78，框 18×18）
    const int cbSize = 18;
    const int cbY    = opt.rect.top() + (opt.rect.height() - cbSize) / 2;
    QStyleOptionButton cb;
    cb.rect = QRect(x0, cbY, cbSize, cbSize);
    cb.state = QStyle::State_Enabled
             | (idx.data(Qt::CheckStateRole).value<Qt::CheckState>() == Qt::Checked
                    ? QStyle::State_On : QStyle::State_Off);
    st->drawControl(QStyle::CE_CheckBox, &cb, p, o.widget);
    const int tx = x0 + cbSize + 10;

    // 行1：发件人（未读粗体）+ 时间（右对齐），基准 y = cb 顶
    int y = cbY;
    drawElided(p, QRect(tx, y, x1 - tx - 155, 20), txtFrom, from,
               bumpFont(o.font, unread));
    p->setFont(o.font);
    p->setPen(txtTime);
    p->drawText(QRect(x1 - 150, y, 150, 20), Qt::AlignVCenter | Qt::AlignRight,
                p->fontMetrics().elidedText(time, Qt::ElideRight, 150));
    y += 22;

    // 行2：主题（粗体）
    drawElided(p, QRect(tx, y, x1 - tx, 20), txtSubj, subj,
               bumpFont(o.font, true));
    y += 22;

    // 行3：摘要（明显灰）
    drawElided(p, QRect(tx, y, x1 - tx, 18), txtSec, summ,
               bumpFont(o.font, false));
}

QSize MailListDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex& idx) const {
    if (idx.data(Qt::UserRole + 1).toString() == "group")
        return QSize(120, 28);
    return QSize(360, 78);
}

bool MailListDelegate::editorEvent(QEvent* ev, QAbstractItemModel* model,
                                   const QStyleOptionViewItem& opt,
                                   const QModelIndex& idx) {
    if (ev->type() == QEvent::MouseButtonRelease) {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() == Qt::LeftButton) {
            // 跳过分组行
            if (idx.data(Qt::UserRole + 1).toString() == "group") return false;
            bool on = idx.data(Qt::CheckStateRole).value<Qt::CheckState>() == Qt::Checked;
            return model->setData(idx, on ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
        }
    }
    return QStyledItemDelegate::editorEvent(ev, model, opt, idx);
}
