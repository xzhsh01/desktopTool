#include "mail/ui/MarkdownBridge.h"

#include <QRegularExpression>
#include <QTextDocument>

namespace mail::markdown {

QString toHtml(const QString& md) {
    if (md.isEmpty()) return {};
    QTextDocument doc;
    doc.setMarkdown(md, QTextDocument::MarkdownDialectGitHub);
    return doc.toHtml();
}

QString toHtmlFragment(const QString& md) {
    QString full = toHtml(md);
    if (full.isEmpty()) return {};
    // QTextDocument::toHtml() 输出形如：
    //   <!DOCTYPE html ...><html><head><meta .../></head><body>...frag...</body></html>
    // 提取 <body> 内的内容。
    static const QRegularExpression bodyRe(
        QStringLiteral("<body[^>]*>(.*)</body>\\s*$"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    auto m = bodyRe.match(full);
    if (m.hasMatch()) return m.captured(1).trimmed();
    // 兜底：去掉 <html> 包装
    static const QRegularExpression htmlWrap(
        QStringLiteral("</?html[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    QString stripped = full;
    stripped.remove(htmlWrap);
    return stripped;
}

QString fromHtml(const QString& html) {
    if (html.isEmpty()) return {};
    QTextDocument doc;
    doc.setHtml(html);
    return doc.toMarkdown(QTextDocument::MarkdownDialectGitHub);
}

} // namespace mail::markdown