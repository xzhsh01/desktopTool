#pragma once

#include <QWidget>
#include <QVector>
#include <QTimer>

class QMouseEvent;

/**
 * TerminalEmulator: 轻量级 ANSI/VT100 终端模拟器
 * 替代原 xterm.js，负责：
 *  - 解析 ANSI 转义序列（光标移动、颜色、擦除等）
 *  - 维护屏幕缓冲区（字符 + 属性网格）
 *  - 渲染（QPainter 绘制等宽字体网格）
 *  - 键盘输入 → 转义序列输出
 */
class TerminalEmulator : public QWidget {
    Q_OBJECT

public:
    explicit TerminalEmulator(QWidget* parent = nullptr);

    // 终端尺寸（列/行）
    int columns() const { return m_cols; }
    int rows() const { return m_rows; }

    // 写入数据（来自远端），解析并渲染
    void write(const QByteArray& data);

    // 清屏
    void clear();

    // 设置字号
    void setFontSize(int size);

    // ── 复制/粘贴 ──
    bool hasSelection() const { return m_hasSelection; }
    QString selectedText() const;
    void copySelection();
    void pasteClipboard();
    void selectAll();
    void clearSelection();

signals:
    // 用户输入（键盘），需要发送到远端
    void inputData(const QByteArray& data);
    // 终端尺寸变化
    void resized(int cols, int rows);
    // 标题变化（OSC 0/2）
    void titleChanged(const QString& title);

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;

private:
    struct Cell {
        QChar ch = ' ';
        quint8 fg = 7;        // 前景色索引 0-15
        quint8 bg = 0;        // 背景色索引 0-15
        bool bold = false;
        bool underline = false;
        bool reverse = false;
        bool wide = false;    // 宽字符（CJK）续列标记，由前一格绘制
    };

    // ── 解析器状态 ──────────────────────────────────────────
    void processData(const QByteArray& data);
    void parseChar(char c);
    void parseEscape(char c);
    void parseCSI(char c);
    void parseOSC(char c);
    void executeCSI();
    void executeSGR();
    void flushUtf8Char();
    static bool isWideChar(QChar c);

    // ── 屏幕操作 ────────────────────────────────────────────
    void ensureBuffer();
    void scrollUp(int lines = 1);
    void scrollDown(int lines = 1);
    void newLine();
    void carriageReturn();
    void backspace();
    void tab();
    void eraseInDisplay(int mode);
    void eraseInLine(int mode);
    void putChar(QChar ch);
    void moveCursor(int row, int col);

    // ── 渲染 ────────────────────────────────────────────────
    void updateMetrics();
    void scheduleUpdate();

    // ── 输入辅助 ────────────────────────────────────────────
    void sendKeyText(const QString& text);
    QByteArray encodeModifier(int qtModifiers) const;

    // ── 鼠标选区 ────────────────────────────────────────────
    void cellAt(const QPoint& pos, int& row, int& col) const;

    // ── 数据成员 ────────────────────────────────────────────
    int m_cols = 80;
    int m_rows = 24;
    int m_curRow = 0;
    int m_curCol = 0;
    int m_savedRow = 0, m_savedCol = 0;

    // 滚动区
    int m_scrollTop = 0;
    int m_scrollBottom = 0;   // 含

    // 颜色状态
    quint8 m_curFg = 7;
    quint8 m_curBg = 0;
    bool m_curBold = false;
    bool m_curUnderline = false;
    bool m_curReverse = false;

    // 缓冲区
    QVector<QVector<Cell>> m_screen;

    // 解析器状态机
    enum ParseState { Ground, Esc, CSI, OSC, OSC_Esc } m_state = Ground;
    QString m_csiParams;      // CSI 参数缓冲
    QString m_oscBuffer;      // OSC 缓冲
    bool m_csiPrivate = false;

    // UTF-8 增量解码状态（多字节序列跨包累积）
    int m_utf8Remaining = 0;  // 还缺的续字节数
    uint m_utf8Code = 0;      // 已累积的码点

    // 字体
    QFont m_font;
    int m_charWidth = 8;
    int m_charHeight = 16;
    int m_fontAscent = 12;
    int m_fontSize = 12;

    // 光标闪烁
    QTimer m_cursorTimer;
    bool m_cursorVisible = true;

    // 鼠标选区（屏幕网格坐标，行优先）
    bool m_hasSelection = false;
    bool m_selecting = false;
    int m_selAnchorRow = 0, m_selAnchorCol = 0;
    int m_selEndRow = 0, m_selEndCol = 0;

    // 批量渲染优化
    QTimer m_repaintTimer;

    // 颜色表（16 色 + 256 色扩展简化为 16 色）
    static const QColor PALETTE[16];
};
