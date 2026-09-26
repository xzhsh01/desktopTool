#pragma once

#include <QStyledItemDelegate>

/**
 * MailListDelegate: 邮件列表块状渲染
 *
 * 每封邮件一个块（单列表格，每行一块）：
 *   [✓] 发件人(未读粗体蓝)                    时间
 *       主题(粗体)
 *       摘要(灰)
 * 分组标题行（今天/昨天/本周/更早）由 UserRole+1=="group" 区分。
 *
 * 行数据角色约定：
 *   UserRole   = 邮件 id（"draft:<id>" 表示草稿）
 *   UserRole+1 = 行类型（"mail" / "group" / "draft"）
 *   UserRole+2 = 发件人（草稿行为主题）
 *   UserRole+3 = 显示时间
 *   UserRole+4 = 主题（草稿行为收件人）
 *   UserRole+5 = 摘要
 *   UserRole+6 = 未读标记
 */
class MailListDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override;
    QSize sizeHint(const QStyleOptionViewItem& opt,
                   const QModelIndex& idx) const override;
    // 左键点击整个邮件块切换勾选（用于批量删除）；
    // 右键由面板 customContextMenuRequested 处理
    bool editorEvent(QEvent* ev, QAbstractItemModel* model,
                     const QStyleOptionViewItem& opt,
                     const QModelIndex& idx) override;

private:
    // 单行省略绘制：超宽部分以 "..." 代替。
    // 关键：elide 用 p->fontMetrics()（painter 实际渲染字体）计算，
    // 与 drawText 渲染严格一致；宽度做非负防御（窄窗口下可用宽可为负，
    // elidedText 对负宽返回原文本 → 溢出绘制）。
    static void drawElided(QPainter* p, const QRect& r, const QColor& c,
                           const QString& text, const QFont& f);
    // 字号微调（pointSizeF 为 -1 的像素字体下 +0.5 会变负异常，需防御）
    static QFont bumpFont(const QFont& base, bool bold, qreal delta = 0.5);
};
