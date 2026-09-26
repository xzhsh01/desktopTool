// Theme.h — 全局统一配色与样式片段（深色主题）
// 所有 Widget 的样式统一从这里取色，禁止在界面代码中硬编码颜色值。
#pragma once

#include <QString>

namespace Theme {

// ── 背景色 ──────────────────────────────────────────────────────
inline const QString kBg          = "#1a1b23";  // 主背景
inline const QString kTitleBar    = "#16171e";  // 标题栏 / 表头 / 状态栏
inline const QString kSidebar     = "#1e1f26";  // 侧边栏 / 卡片
inline const QString kSurface     = "#252830";  // 输入框 / 悬浮
inline const QString kSurfaceAlt  = "#1e2128";  // 表格 / 菜单 / 页签
inline const QString kBorder      = "#2a2d36";  // 边框
inline const QString kBorderLight = "#3a3d46";  // 亮边框 / 滚动条
inline const QString kBorderHover = "#4a4d56";  // 滚动条 hover

// ── 文本色 ──────────────────────────────────────────────────────
inline const QString kText        = "#c8c8c8";  // 正文
inline const QString kTextBright  = "#e0e0e0";  // 标题 / 强调
inline const QString kMuted       = "#888888";  // 次要文本
inline const QString kFaint       = "#666666";  // 占位 / 空状态
inline const QString kDisabled    = "#555555";  // 禁用

// ── 语义色 ──────────────────────────────────────────────────────
inline const QString kAccent      = "#4fc3f7";  // 主色调（选中 / 聚焦 / 链接）
inline const QString kSuccess     = "#81c784";  // 成功
inline const QString kWarning     = "#ffb74d";  // 警告
inline const QString kDanger      = "#e57373";  // 错误 / 危险操作
inline const QString kInfo        = "#64b5f6";  // 信息 / 编辑操作

// ── 连接类型分类色（仪表盘统计卡）────────────────────────────────
inline const QString kCatRedis    = "#ef5350";
inline const QString kCatRdp      = "#9575cd";
inline const QString kCatWeChat   = "#07c160";  // 微信绿

// ── 样式片段 ────────────────────────────────────────────────────

// 页面大标题（各功能页顶部）
inline QString pageHeader() {
    return QStringLiteral("font-size: 20px; font-weight: 600; color: %1;").arg(kTextBright);
}

// 区块小标题（页内分节）
inline QString sectionHeader() {
    return QStringLiteral("font-size: 14px; font-weight: 600; color: %1;").arg(kTextBright);
}

// 次要文本（状态信息、计数等，12px 灰）
inline QString mutedText() {
    return QStringLiteral("color: %1; font-size: 12px;").arg(kMuted);
}

// 占位 / 空状态文本
inline QString faintText(int fontSize = 14) {
    return QStringLiteral("color: %1; font-size: %2px;").arg(kFaint).arg(fontSize);
}

// 状态文本：成功 / 警告 / 错误
inline QString statusOk()   { return QStringLiteral("color: %1; font-size: 12px;").arg(kSuccess); }
inline QString statusWarn() { return QStringLiteral("color: %1; font-size: 12px;").arg(kWarning); }
inline QString statusErr()  { return QStringLiteral("color: %1; font-size: 12px;").arg(kDanger); }

// 卡片容器（指定 QSS 选择器，默认 QFrame）
inline QString card(const QString& selector = QStringLiteral("QFrame")) {
    return QStringLiteral("%1 { background: %2; border: 1px solid %3; border-radius: 8px; }")
        .arg(selector, kSidebar, kBorder);
}

// 表格行内操作按钮（编辑 / 连接 / 删除）
inline QString tableActionBtn(const QString& color) {
    return QStringLiteral("QPushButton { padding: 2px 10px; color: %1; }").arg(color);
}

// 扁平按钮（默认 / 主色调 / 危险）— 无渐变、无阴影、无圆角，纯色填充
// 用法：setStyleSheet(Theme::flatBtn()) / flatBtnPrimary() / flatBtnDanger()
// 按需追加 setFlat(true) 与 setCursor(Qt::PointingHandCursor)
inline QString flatBtn() {
    return QStringLiteral(
        "QPushButton {"
        "  background: %1; color: %2; border: none;"
        "  padding: 6px 14px; font-size: 13px;"
        "}"
        "QPushButton:hover    { background: %3; }"
        "QPushButton:pressed  { background: %4; }"
        "QPushButton:disabled { background: %5; color: %6; }")
        .arg(kSurface, kText, kBorderLight, kBorder, kSidebar, kDisabled);
}
inline QString flatBtnPrimary() {
    return QStringLiteral(
        "QPushButton {"
        "  background: %1; color: #0d1116; border: none;"
        "  padding: 6px 14px; font-size: 13px; font-weight: 600;"
        "}"
        "QPushButton:hover    { background: #6ed1fb; }"
        "QPushButton:pressed  { background: #3ba9dd; }"
        "QPushButton:disabled { background: %2; color: %3; }")
        .arg(kAccent, kSidebar, kDisabled);
}
inline QString flatBtnDanger() {
    return QStringLiteral(
        "QPushButton {"
        "  background: %1; color: #2a1010; border: none;"
        "  padding: 6px 14px; font-size: 13px; font-weight: 600;"
        "}"
        "QPushButton:hover    { background: #ef8a8a; }"
        "QPushButton:pressed  { background: #d65a5a; }"
        "QPushButton:disabled { background: %2; color: %3; }")
        .arg(kDanger, kSidebar, kDisabled);
}

} // namespace Theme
