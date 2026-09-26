// EmojiFont.h — 全局 emoji 字体注册 + 字符渲染辅助
// 解决问题：Windows 7/Server 或缺 emoji 字体时，UI 字符串里的 emoji 字符显示为方框/空白。
//
// 用法：
//   1) main.cpp 里 `EmojiFont::install()` 调用一次（QApplication 之后）
//   2) QTextOption / QLabel 等设字体时用 EmojiFont::fontForText() 让 Qt 自动选有 emoji 的字体
#pragma once

#include <QFont>
#include <QFontDatabase>
#include <QString>
#include <QStringList>

namespace EmojiFont {

// 在 QApplication 创建后调用一次：扫描系统/资源里的 emoji 字体，注册到 Qt 字体数据库。
// 之后所有 QWidget 默认字体已被替换为带 emoji 回退的复合字体族（emoji 字符由 emoji 字体画，其余字符由主字体画）。
void install();

// 给一段字符串返回合适的 QFont（首字检测是否含 emoji，若含则自动混入 emoji 字体族）。
// 适用于 QLabel / QPushButton / QTextEdit 等单行/多行文本控件。
QFont fontForText(const QString& text, int pointSize = -1);

// 检测给定字符是否属于 emoji/符号 Unicode 区段（比 Qt::Symbol 更宽松，包含 BMP 外字符）。
inline bool isEmoji(QChar c) {
    int cp = c.unicode();
    // ── BMP 段（U+2000–U+27BF + U+E000–U+F8FF 私有及符号区） ──
    if (cp >= 0x2000 && cp <= 0x27BF) return true;
    if (cp == 0x231A || cp == 0x231B) return true;        // ⌚⌛
    if (cp >= 0x23E9 && cp <= 0x23F3) return true;        // ⏩⏪⏫⏬⏭⏮⏯⏰⏱⏲⏳
    if (cp >= 0x23F8 && cp <= 0x23FA) return true;        // ⏸⏹⏺
    if (cp >= 0x2600 && cp <= 0x26FF)  return true;       // ☀-⛿
    if (cp >= 0x2700 && cp <= 0x27BF)  return true;       // ✀-➿
    if (cp >= 0x2900 && cp <= 0x297F)  return true;       // ⤀-⥿
    if (cp >= 0x2B00 && cp <= 0x2BFF)  return true;       // ⬀-⮿（含箭头）
    if (cp >= 0xE000 && cp <= 0xF8FF)  return true;       // 私有区（Apple Color Emoji 等用）
    if (cp >= 0xFE00 && cp <= 0xFE0F)  return true;       // 变体选择符
    // ── SMP 段（U+1F000+）：主要 emoji 块 ──
    if (cp >= 0x1F000 && cp <= 0x1FFFF) return true;      // 🎀-🗿（覆盖所有 SMP emoji）
    if (cp >= 0x20000 && cp <= 0x2FFFF) return true;      // CJK 扩展 B-F
    return false;
}

// 检测字符串里是否含 emoji（用于决定是否给该控件套 emoji 字体）。
inline bool containsEmoji(const QString& s) {
    for (auto c : s) if (isEmoji(c)) return true;
    return false;
}

// 把字符串里的 SMP emoji 字符（BMP 符号几何不在此列）替换为同义 BMP 符号，
// 在缺少彩色 emoji 字体的环境下避免显示成方框。
// 例如：📧 → ✉ ，📎 → ❀ ，⭐ → ★ ，✅ → ✓ ，❌ → ✗ ，🔥 → ※ ，📝 → ▤ ，
//       👁 → ◎ ，🔗 → ⇄ ，📋 → ▤ ，💾 → ⤓ ，🔄 → ↻ ，📧 → ✉
//
// 注意：保留 BMP 段（U+2600-U+27BF）因为它们在主流字体里有对应字符。
QString sanitize(const QString& s);

// 已加载的 emoji 字体名（调试 / UI 提示用）。
QStringList availableEmojiFontFamilies();

} // namespace EmojiFont
