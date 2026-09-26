#pragma once

#include <QLayout>
#include <QLayoutItem>
#include <QRect>
#include <QStyle>
#include <QWidget>

// 简单的流式布局：每个子控件按其 sizeHint 横向排列，宽度不足时自动换行。
// 派生自 Qt 官方示例，行为接近 QLabel word-wrap 自适应。
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr,
                        int margin = 0,
                        int hSpacing = 4,
                        int vSpacing = 4)
        : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing) {
        setContentsMargins(margin, margin, margin, margin);
    }

    explicit FlowLayout(int margin,
                        int hSpacing = 4,
                        int vSpacing = 4)
        : m_hSpace(hSpacing), m_vSpace(vSpacing) {
        setContentsMargins(margin, margin, margin, margin);
    }

    ~FlowLayout() override {
        QLayoutItem* item;
        while ((item = takeAt(0)) != nullptr) {
            delete item;
        }
    }

    void addItem(QLayoutItem* item) override {
        m_items.append(item);
    }

    int horizontalSpacing() const {
        if (m_hSpace >= 0) return m_hSpace;
        return smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
    }

    int verticalSpacing() const {
        if (m_vSpace >= 0) return m_vSpace;
        return smartSpacing(QStyle::PM_LayoutVerticalSpacing);
    }

    Qt::Orientations expandingDirections() const override {
        return {};
    }

    bool hasHeightForWidth() const override {
        return true;
    }

    int heightForWidth(int width) const override {
        return doLayout(QRect(0, 0, width, 0), true);
    }

    QSize minimumSize() const override {
        QSize size;
        for (const QLayoutItem* item : m_items) {
            size = size.expandedTo(item->minimumSize());
        }
        const QMargins m = contentsMargins();
        size += QSize(m.left() + m.right(), m.top() + m.bottom());
        return size;
    }

    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }

    QSize sizeHint() const override {
        return minimumSize();
    }

    int count() const override {
        return m_items.size();
    }

    QLayoutItem* itemAt(int index) const override {
        if (index < 0 || index >= m_items.size()) return nullptr;
        return m_items.at(index);
    }

    QLayoutItem* takeAt(int index) override {
        if (index < 0 || index >= m_items.size()) return nullptr;
        return m_items.takeAt(index);
    }

private:
    // 单行布局记录：每行持有的 item 列表 + 该行的最大高度
    struct LineRec {
        QList<QLayoutItem*> items;  // 该行的所有 item
        QList<int> itemX;           // 每个 item 的 x 坐标
        QList<int> itemY;           // 每个 item 的 y 起点（行顶）
        int lineHeight = 0;         // 该行的最大高度
    };

    int doLayout(const QRect& rect, bool testOnly) const {
        const QMargins margins = contentsMargins();
        const QRect effective = rect.adjusted(margins.left(), margins.top(),
                                             -margins.right(), -margins.bottom());

        // ── 第一遍：按行分组，计算每行高度 ──────────────────────────────
        QList<LineRec> lines;
        int x = effective.x();
        int y = effective.y();
        int lineHeight = 0;
        int curLine = -1;

        for (QLayoutItem* item : m_items) {
            const QWidget* w = item->widget();
            int spaceX = horizontalSpacing();
            int spaceY = verticalSpacing();
            if (spaceX == -1 && w) spaceX = w->style()->layoutSpacing(
                QSizePolicy::PushButton, QSizePolicy::PushButton, Qt::Horizontal);
            if (spaceY == -1 && w) spaceY = w->style()->layoutSpacing(
                QSizePolicy::PushButton, QSizePolicy::PushButton, Qt::Vertical);

            // 关键：使用 widget 的最小宽度（hint 的紧凑版），而不是完整 sizeHint，
            // 让一行能放尽可能多的附件。
            int itemW = item->sizeHint().width();
            int itemH = item->sizeHint().height();
            int nextX = x + itemW + spaceX;
            if (nextX - spaceX > effective.right() && lineHeight > 0) {
                x = effective.x();
                y = y + lineHeight + spaceY;
                nextX = x + itemW + spaceX;
                lineHeight = 0;
            }

            if (lineHeight == 0) {
                // 开始新的一行
                lines.append(LineRec{});
                curLine++;
            }
            lines[curLine].items.append(item);
            lines[curLine].itemX.append(x);
            lines[curLine].itemY.append(y);
            lines[curLine].lineHeight = qMax(lines[curLine].lineHeight, itemH);

            x = nextX;
            lineHeight = lines[curLine].lineHeight;
        }

        // ── 第二遍：放置。同一行内，按该行最大高度垂直居中 ──────────────
        int totalH = 0;
        int curY = effective.y();
        for (const LineRec& line : lines) {
            for (int i = 0; i < line.items.size(); ++i) {
                QLayoutItem* item = line.items[i];
                int h = item->sizeHint().height();
                int centeredY = curY + (line.lineHeight - h) / 2;
                if (!testOnly) {
                    item->setGeometry(QRect(QPoint(line.itemX[i], centeredY),
                                            item->sizeHint()));
                }
            }
            curY += line.lineHeight;
            if (line.lineHeight > 0) totalH += line.lineHeight;
        }
        // 行间距（仅在多行时累加）
        if (lines.size() > 1) {
            for (int i = 0; i < lines.size() - 1; ++i) {
                totalH += verticalSpacing() >= 0 ? verticalSpacing()
                       : smartSpacing(QStyle::PM_LayoutVerticalSpacing);
            }
        }

        return totalH + margins.bottom();
    }

    int smartSpacing(QStyle::PixelMetric pm) const {
        QObject* parentObj = parent();
        if (!parentObj) return -1;
        if (parentObj->isWidgetType()) {
            auto* pw = static_cast<QWidget*>(parentObj);
            return pw->style()->pixelMetric(pm, nullptr, pw);
        }
        return static_cast<QLayout*>(parentObj)->spacing();
    }

    QList<QLayoutItem*> m_items;
    int m_hSpace;
    int m_vSpace;
};
