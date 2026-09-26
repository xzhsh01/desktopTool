#include "mail/ui/content/MailPreviewBrowser.h"

#include <QImage>
#include <QMouseEvent>
#include <QTextCursor>

void MailPreviewBrowser::mousePressEvent(QMouseEvent* e) {
    // anchorAt 返回空字符串说明没命中链接；此时把光标移到点击处
    // → 触发 QTextEdit 的 selection update：若之前有 selection 则取消
    const QString anchor = anchorAt(e->pos());
    if (anchor.isEmpty()) {
        const QPoint p = e->pos();
        QTextCursor cur = cursorForPosition(p);
        cur.clearSelection();
        setTextCursor(cur);
    }
    QTextBrowser::mousePressEvent(e);
}

void MailPreviewBrowser::mouseReleaseEvent(QMouseEvent* e) {
    QTextBrowser::mouseReleaseEvent(e);
    // mouse release 后再清一次 selection：点链接时浏览器内部会保留一个
    // 小高亮（anchor pressed 视觉态），这里强制 cancel。
    if (!textCursor().hasSelection()) {
        QTextCursor c = textCursor();
        c.setPosition(textCursor().position());
        setTextCursor(c);
    }
}

QVariant MailPreviewBrowser::loadResource(int type, const QUrl& url) {
    if (type != QTextDocument::ImageResource) return {};
    const QString scheme = url.scheme().toLower();
    if (scheme == "cid") {
        // cid:image001@xxx → 查内嵌图片（key 小写、无尖括号）
        QString key = url.toString();
        key = key.mid(key.indexOf(':') + 1).toLower();
        const QByteArray data = m_cidImages.value(key);
        if (data.isEmpty()) return {};
        const QImage img = QImage::fromData(data);
        return img.isNull() ? QVariant() : QVariant(img);
    }
    if (scheme == "data") {
        // data:image/png;base64,... → 解码
        const QString s = url.toString(QUrl::None);
        const int comma = s.indexOf(',');
        if (comma < 0) return {};
        QByteArray payload = s.mid(comma + 1).toLatin1();
        if (s.left(comma).contains(";base64", Qt::CaseInsensitive))
            payload = QByteArray::fromBase64(payload);
        const QImage img = QImage::fromData(payload);
        return img.isNull() ? QVariant() : QVariant(img);
    }
    return {};   // 外部资源不加载
}
