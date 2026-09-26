// EmojiFont.cpp — 注册系统/内置 emoji 字体作为 Qt 字体回退族
#include "app/EmojiFont.h"

#include <QCoreApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHash>
#include <QStringList>

namespace EmojiFont {

namespace {
    QStringList s_emojiFonts;          // 已加载的 emoji 字体族名（按优先级）
    bool s_installed = false;

    // 候选 emoji 字体（按优先级匹配）
    const QStringList kCandidates = {
        // Apple（macOS / iOS）
        QStringLiteral("Apple Color Emoji"),
        // Microsoft（Win 8.1+ / Win 10/11 / Server 2022）
        QStringLiteral("Segoe UI Emoji"),
        // Linux（多数发行版默认安装 Noto）
        QStringLiteral("Noto Color Emoji"),
        QStringLiteral("Noto Emoji"),
        QStringLiteral("Twemoji"),
        // ChromeOS / Android 配套
        QStringLiteral("Android Emoji"),
        // 旧/扩展字体
        QStringLiteral("Symbola"),
        QStringLiteral("EmojiOne Color"),
        QStringLiteral("Twitter Color Emoji"),
    };

    // 探测 Qt 系统字体里是否有任一候选；找到即用其族名作为回退。
    void detectSystemEmoji() {
        const QStringList families = QFontDatabase::families();
        for (const QString& cand : kCandidates) {
            if (families.contains(cand, Qt::CaseInsensitive)) {
                // Qt 大小写敏感，用 families 的精确名
                for (const QString& f : families) {
                    if (f.compare(cand, Qt::CaseInsensitive) == 0) {
                        s_emojiFonts.append(f);
                        break;
                    }
                }
            }
        }
    }

    // 如果系统一个都没有，我们尝试从 Windows 自带 Segoe UI Symbol（U+2600-U+26FF 符号字体）
    // 作为最弱回退 —— 即便没有彩色 emoji，也能显示基础符号 / 方框箭头
    void ensureMinimalFallback() {
        if (!s_emojiFonts.isEmpty()) return;
        const QStringList families = QFontDatabase::families();
        for (const QString& cand : { QStringLiteral("Segoe UI Symbol"),
                                     QStringLiteral("DejaVu Sans"),
                                     QStringLiteral("Symbol") }) {
            for (const QString& f : families) {
                if (f.compare(cand, Qt::CaseInsensitive) == 0) {
                    s_emojiFonts.append(f);
                    return;
                }
            }
        }
    }
}

void install() {
    if (s_installed) return;
    s_installed = true;

    detectSystemEmoji();
    ensureMinimalFallback();

    // 把 emoji 字体混入 QApplication 默认字体的 font.families 列表，
    // 这样 Qt 在绘制时遇到 emoji 字符会自动 fallback 到 emoji 字体。
    QFont defaultFont = QGuiApplication::font();
    QStringList fams = defaultFont.families();
    for (const QString& ef : s_emojiFonts) {
        if (!fams.contains(ef)) fams.append(ef);
    }
    defaultFont.setFamilies(fams);
    QGuiApplication::setFont(defaultFont);

    // 同时给所有 QFontDatabase 写入的子样式注入字体族（让新建的 QFont 也带 emoji 回退）
    // —— 上面 setFont 已经覆盖了大部分场景
}

QFont fontForText(const QString& text, int pointSize) {
    QFont f = QGuiApplication::font();
    if (pointSize > 0) f.setPointSize(pointSize);
    if (containsEmoji(text)) {
        QStringList fams = f.families();
        for (const QString& ef : s_emojiFonts) {
            if (!fams.contains(ef)) fams.append(ef);
        }
        f.setFamilies(fams);
    }
    return f;
}

QStringList availableEmojiFontFamilies() {
    return s_emojiFonts;
}

// ── emoji → BMP 符号回退表 ────────────────────────────────────────
// 仅覆盖 UI 装饰用的高频 emoji。每个替换为在所有 Windows/常见字体中都有字形的 BMP 字符。
// 保留 BMP 符号段（U+2600-U+27BF）—— 它们在主流字体中通常能正常渲染。
static const QHash<uint, QChar>& fallbackMap() {
    static const QHash<uint, QChar> m = {
        // ── 邮件 / 文档 ──
        {0x1F4E7, QChar(0x2709)},  // 📧 → ✉
        {0x1F4E8, QChar(0x2709)},  // 📨 → ✉
        {0x1F4E9, QChar(0x2709)},  // 📩 → ✉
        {0x1F4CE, QChar(0x2748)},  // 📎 → ❈
        {0x1F4CB, QChar(0x25AD)},  // 📋 → ▭
        {0x1F4DD, QChar(0x270E)},  // 📝 → ✎
        {0x1F4C4, QChar(0x25AD)},  // 📄 → ▭
        {0x1F4D6, QChar(0x25AF)},  // 📖 → ▯
        {0x1F4D5, QChar(0x25AF)},  // 📕 → ▯
        // ── 链接 / 工具 ──
        {0x1F517, QChar(0x21C4)},  // 🔗 → ⇄
        {0x1F50D, QChar(0x2315)},  // 🔍 → ⌕
        {0x1F4A1, QChar(0x2605)},  // 💡 → ★
        {0x1F4CC, QChar(0x2691)},  // 📌 → ⚑
        {0x1F4BE, QChar(0x25A0)},  // 💾 → ■
        {0x1F4C2, QChar(0x25B6)},  // 📂 → ▶
        {0x1F4C1, QChar(0x25C0)},  // 📁 → ◀
        {0x1F4D1, QChar(0x25A3)},  // 📑 → ▣
        {0x1F4DA, QChar(0x25AF)},  // 📚 → ▯
        {0x1F4F0, QChar(0x25AF)},  // 📰 → ▯
        // ── 表情 / 手势 ──
        {0x1F600, QChar(0x263A)},  // 😀 → ☺
        {0x1F603, QChar(0x263A)},  // 😃 → ☺
        {0x1F604, QChar(0x263A)},  // 😄 → ☺
        {0x1F601, QChar(0x263A)},  // 😁 → ☺
        {0x1F606, QChar(0x263A)},  // 😆 → ☺
        {0x1F605, QChar(0x263A)},  // 😅 → ☺
        {0x1F602, QChar(0x263A)},  // 😂 → ☺
        {0x1F923, QChar(0x263A)},  // 🤣 → ☺
        {0x1F60A, QChar(0x263A)},  // 😊 → ☺
        {0x1F607, QChar(0x263A)},  // 😇 → ☺
        {0x1F642, QChar(0x263A)},  // 🙂 → ☺
        {0x1F643, QChar(0x263A)},  // 🙃 → ☺
        {0x1F609, QChar(0x263A)},  // 😉 → ☺
        {0x1F60C, QChar(0x263A)},  // 😌 → ☺
        {0x1F60D, QChar(0x2665)},  // 😍 → ♥
        {0x1F970, QChar(0x2665)},  // 🥰 → ♥
        {0x1F618, QChar(0x2665)},  // 😘 → ♥
        {0x1F617, QChar(0x2665)},  // 😗 → ♥
        {0x1F619, QChar(0x2665)},  // 😙 → ♥
        {0x1F61A, QChar(0x2665)},  // 😚 → ♥
        {0x1F44D, QChar(0x261D)},  // 👍 → ☝
        {0x1F44E, QChar(0x261D)},  // 👎 → ☝
        {0x1F44F, QChar(0x270A)},  // 👏 → ✊
        {0x1F64C, QChar(0x270A)},  // 🙌 → ✊
        {0x1F64F, QChar(0x270A)},  // 🙏 → ✊
        {0x1F4AA, QChar(0x270A)},  // 💪 → ✊
        {0x1F91D, QChar(0x270A)},  // 🤝 → ✊
        {0x270C,  QChar(0x270A)},  // ✌ → ✊
        // ── 符号 / 装饰 ──
        {0x2705, QChar(0x2713)},  // ✅ → ✓
        {0x274C, QChar(0x2717)},  // ❌ → ✗
        {0x2B50,  QChar(0x2605)},  // ⭐ → ★
        {0x1F31F, QChar(0x2605)},  // 🌟 → ★
        {0x1F4AF, QChar(0x2605)},  // 💯 → ★
        {0x1F525, QChar(0x203B)},  // 🔥 → ※
        {0x2728,  QChar(0x203B)},  // ✨ → ※
        {0x1F308, QChar(0x25C6)},  // 🌈 → ◆
        {0x2600,  QChar(0x25C6)},  // ☀ → ◆
        {0x1F389, QChar(0x2605)},  // 🎉 → ★
        {0x1F38A, QChar(0x2605)},  // 🎊 → ★
        {0x1F381, QChar(0x25B2)},  // 🎁 → ▲
        {0x1F382, QChar(0x25B2)},  // 🎂 → ▲
        {0x2764,  QChar(0x2665)},  // ❤ → ♥
        {0x1F494, QChar(0x2660)},  // 💔 → ♠
        {0x1F496, QChar(0x2665)},  // 💖 → ♥
        // ── 通讯 / 商业 ──
        {0x1F4DE, QChar(0x260E)},  // 📞 → ☎
        {0x1F4F1, QChar(0x260E)},  // 📱 → ☎
        {0x1F4F2, QChar(0x260E)},  // 📲 → ☎
        {0x1F4AC, QChar(0x266B)},  // 💬 → ♫
        {0x1F4AD, QChar(0x266B)},  // 💭 → ♫
        {0x1F4A6, QChar(0x266B)},  // 💦 → ♫
        {0x1F4A8, QChar(0x266B)},  // 💨 → ♫
        {0x1F4A9, QChar(0x266B)},  // 💩 → ♫
        {0x1F680, QChar(0x279C)},  // 🚀 → ➜
        {0x1F4E4, QChar(0x279C)},  // 📤 → ➜
        {0x1F4E5, QChar(0x279C)},  // 📥 → ➜
        {0x1F4E3, QChar(0x279C)},  // 📣 → ➜
        {0x1F50A, QChar(0x279C)},  // 🔊 → ➜
        // ── 其他常见 ──
        {0x1F3E0, QChar(0x25A0)},  // 🏠 → ■
        {0x1F3E2, QChar(0x25A0)},  // 🏢 → ■
        {0x1F514, QChar(0x2606)},  // 🔔 → ☆
        {0x1F515, QChar(0x2606)},  // 🔕 → ☆
        {0x1F3AF, QChar(0x25CE)},  // 🎯 → ◎
        {0x1F3C6, QChar(0x2605)},  // 🏆 → ★
        {0x1F451, QChar(0x2605)},  // 👑 → ★
        {0x1F48C, QChar(0x270D)},  // 💌 → ✍
        {0x1F5DE, QChar(0x261B)},  // 🗞 → ☛
        {0x1F441, QChar(0x00B7)},  // 👁 → ·
        {0x1F440, QChar(0x00B7)},  // 👀 → ··
        {0x1F3A8, QChar(0x270D)},  // 🎨 → ✍
        // ── UI 按钮图标 ──
        {0x1F4BE, QChar(0x25A0)},  // 💾 → ■
        {0x1F504, QChar(0x21BB)},  // 🔄 → ↻
        {0x1F501, QChar(0x21BB)},  // 🔁 → ↻
        {0x1F502, QChar(0x21BA)},  // 🔂 → ↺
        {0x1F519, QChar(0x25C0)},  // 🔙 → ◀
        {0x1F51A, QChar(0x25B6)},  // 🔚 → ▶
        {0x1F51B, QChar(0x2715)},  // 🔛 → ✕
        {0x1F51C, QChar(0x2715)},  // 🔜 → ✕
        {0x1F51D, QChar(0x2715)},  // 🔝 → ✕
        {0x1F4F6, QChar(0x2588)},  // 📶 → █
        {0x1F4F3, QChar(0x2588)},  // 📳 → █
        {0x1F4F4, QChar(0x2588)},  // 📴 → █
        {0x1F4F7, QChar(0x2588)},  // 📷 → █
        {0x1F4F9, QChar(0x2588)},  // 📹 → █
        {0x1F4FA, QChar(0x2588)},  // 📺 → █
        {0x1F4FB, QChar(0x2588)},  // 📻 → █
        {0x1F500, QChar(0x279C)},  // 🔀 → ➜
        {0x1F503, QChar(0x279C)},  // 🔃 → ➜
        {0x1F45F, QChar(0x270A)},  // 🧟 → ✊
        // ── 重要 / 紧急 ──
        {0x2757,  QChar(0x0021)},  // ❗ → !
        {0x2755,  QChar(0x003F)},  // ❕ → ?
        {0x26A0,  QChar(0x0021)},  // ⚠ → !
        {0x2716,  QChar(0x2715)},  // ✖ → ✕
        {0x2702,  QChar(0x2702)},  // ✂ → ✂ (BMP, 保留)
        // ── AI / 特殊 ──
        {0x1F916, QChar(0x269B)},  // 🤖 → ⚛
        {0x1F4A0, QChar(0x2666)},  // 💠 → ♦
        {0x1F31A, QChar(0x25C6)},  // 🌚 → ◆
        {0x1F31D, QChar(0x25C6)},  // 🌝 → ◆
        {0x1F31E, QChar(0x25C6)},  // 🌞 → ◆
        {0x1F319, QChar(0x25C6)},  // 🌙 → ◆
        {0x1F320, QChar(0x2605)},  // 🌠 → ★
        {0x1F30D, QChar(0x25C6)},  // 🌍 → ◆
        {0x1F30E, QChar(0x25C6)},  // 🌎 → ◆
        {0x1F30F, QChar(0x25C6)},  // 🌏 → ◆
        // ── 邮件软件图标 ──
        {0x1F4E7, QChar(0x2709)},  // 📧 → ✉ (重复防御)
        {0x1F4EC, QChar(0x2709)},  // 📬 → ✉
        {0x1F4ED, QChar(0x2709)},  // 📭 → ✉
        {0x1F4EE, QChar(0x2709)},  // 📮 → ✉
        {0x1F4EA, QChar(0x2709)},  // 📪 → ✉
        {0x1F4EB, QChar(0x2709)},  // 📫 → ✉
        // ── 写邮件工具栏专用 ──
        {0x1F5BC, QChar(0x25A6)},  // 🖼 插入图片 → ▦（方形带横线，所有 Windows 默认字体均有）
        {0x1F512, QChar(0x22A0)},  // 🔒 加密     → ⊠（数学否定矩阵，等价视觉：带 X 的锁盒）
        {0x23F0,  QChar(0x231A)},  // ⏰ 定时发送 → ⌚
        {0x1F916, QChar(0x269B)},  // 🤖 → ⚛ (重复防御)
    };
    return m;
}

QString sanitize(const QString& s) {
    const auto& m = fallbackMap();
    QString out;
    out.reserve(s.size());
    for (QChar c : s) {
        uint cp = c.unicode();
        // 跳过 VS16（U+FE0F）—— 表情选择符；切换为 BMP 字符后不应保留
        if (cp == 0xFE0F) continue;
        if (m.contains(cp)) {
            out.append(m.value(cp));
        } else {
            out.append(c);
        }
    }
    return out;
}

} // namespace EmojiFont
