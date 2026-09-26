#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// MarkdownBridge: 轻量级 Markdown ↔ HTML 转换（基于 Qt 自带 QTextDocument）
//
//   - toHtml(md)     把 Markdown 源码转成 HTML（适合 SMTP 正文 htmlBody）
//   - fromHtml(...)  简单封装 QTextDocument::toMarkdown()，拿到 round-trip 用
//   - markdownToHtmlFragment(md)
//                    转完后只保留 <body> 内 fragment（去掉 <html><head> 壳）
//
// 注：Qt 5.14+ 提供 QTextDocument::setMarkdown()/toMarkdown()，支持 GitHub 风格
// Markdown（标题、列表、引用、代码、链接、图片、加粗/斜体/删除线、表格）。
// 这是 Microsoft Outlook / Foxmail 等客户端不依赖第三方库的轻量方案。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>

namespace mail::markdown {

// 转换主入口：MD → HTML（含 <html><body>…</body></html> 完整文档）
QString toHtml(const QString& md);

// 转换主入口：MD → HTML（仅 body 内的 fragment，适合作为 multipart/alternative
// 的 html 部分）
QString toHtmlFragment(const QString& md);

// HTML → Markdown（用 QTextDocument::toMarkdown，可用于"以 Markdown 模式打开"）
QString fromHtml(const QString& html);

} // namespace mail::markdown