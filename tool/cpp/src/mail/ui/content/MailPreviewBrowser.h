#pragma once

#include <QHash>
#include <QTextBrowser>

/**
 * MailPreviewBrowser: 邮件预览浏览器
 *
 * HTML 富文本渲染 + 内嵌图片（cid:/data:）支持。
 * 只加载本地内嵌图片；http(s)/file 等外部资源一律拒绝（防追踪像素 + 离线安全）。
 *
 * 点击效果清理：点击正文/非链接位置时立即清掉之前的高亮选区，
 * 避免 QTextBrowser 把上次链接的"按下态"高亮一直挂着。
 */
class MailPreviewBrowser : public QTextBrowser {
public:
    using QTextBrowser::QTextBrowser;

    void setCidImages(const QHash<QString, QByteArray>& imgs) { m_cidImages = imgs; }

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    QVariant loadResource(int type, const QUrl& url) override;

private:
    QHash<QString, QByteArray> m_cidImages;
};
