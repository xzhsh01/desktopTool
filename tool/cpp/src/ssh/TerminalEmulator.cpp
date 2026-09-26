#include "ssh/TerminalEmulator.h"
#include "core/Logger.h"

#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QClipboard>
#include <QEvent>

// 16 色调色板（xterm 默认色）
const QColor TerminalEmulator::PALETTE[16] = {
    QColor(0x00, 0x00, 0x00),    // 0 黑
    QColor(0xcd, 0x00, 0x00),    // 1 红
    QColor(0x00, 0xcd, 0x00),    // 2 绿
    QColor(0xcd, 0xcd, 0x00),    // 3 黄
    QColor(0x00, 0x00, 0xee),    // 4 蓝
    QColor(0xcd, 0x00, 0xcd),    // 5 洋红
    QColor(0x00, 0xcd, 0xcd),    // 6 青
    QColor(0xe5, 0xe5, 0xe5),    // 7 白（浅灰）
    QColor(0x7f, 0x7f, 0x7f),    // 8 亮黑（深灰）
    QColor(0xff, 0x00, 0x00),    // 9 亮红
    QColor(0x00, 0xff, 0x00),    // 10 亮绿
    QColor(0xff, 0xff, 0x00),    // 11 亮黄
    QColor(0x5c, 0x5c, 0xff),    // 12 亮蓝
    QColor(0xff, 0x00, 0xff),    // 13 亮洋红
    QColor(0x00, 0xff, 0xff),    // 14 亮青
    QColor(0xff, 0xff, 0xff)     // 15 亮白
};

TerminalEmulator::TerminalEmulator(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::IBeamCursor);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFont(QFont("Consolas", m_fontSize));  // Windows 等宽字体

    // 光标闪烁
    connect(&m_cursorTimer, &QTimer::timeout, this, [this]() {
        m_cursorVisible = !m_cursorVisible;
        update();
    });
    m_cursorTimer.start(530);

    // 批量渲染：数据到达后 16ms 内合并重绘
    m_repaintTimer.setSingleShot(true);
    m_repaintTimer.setInterval(16);
    connect(&m_repaintTimer, &QTimer::timeout, this, [this]() { update(); });

    m_scrollBottom = m_rows - 1;
    updateMetrics();
    ensureBuffer();
}

// ── 公共接口 ─────────────────────────────────────────────────────────────────

void TerminalEmulator::write(const QByteArray& data) {
    processData(data);
    scheduleUpdate();
}

void TerminalEmulator::clear() {
    for (int r = 0; r < m_rows; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            m_screen[r][c] = Cell{};
        }
    }
    m_curRow = m_curCol = 0;
    update();
}

void TerminalEmulator::setFontSize(int size) {
    m_fontSize = size;
    auto f = font();
    f.setPointSize(size);
    setFont(f);
    updateMetrics();
    ensureBuffer();
    update();
}

// ── 事件处理 ─────────────────────────────────────────────────────────────────

void TerminalEmulator::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), PALETTE[0]);

    p.setFont(font());

    for (int r = 0; r < m_rows; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            const Cell& cell = m_screen[r][c];
            int x = c * m_charWidth;
            int y = r * m_charHeight;

            // 宽字符续列：由主格统一绘制
            if (cell.wide) continue;

            QColor bg = PALETTE[cell.bg & 0x0F];
            QColor fg = PALETTE[cell.fg & 0x0F];
            if (cell.reverse) std::swap(bg, fg);

            // 字符显示宽度（宽字符占 2 列）
            int cw = m_charWidth;
            if (c + 1 < m_cols && m_screen[r][c + 1].wide) cw = m_charWidth * 2;

            if (cell.bg != 0 || cell.reverse) {
                p.fillRect(x, y, cw, m_charHeight, bg);
            }

            if (cell.ch != ' ' && cell.ch != QChar(0)) {
                if (cell.bold) {
                    auto f = p.font();
                    f.setBold(true);
                    p.setFont(f);
                }
                p.setPen(fg);
                p.drawText(QPoint(x, y + m_fontAscent), QString(cell.ch));
                if (cell.bold) {
                    auto f = p.font();
                    f.setBold(false);
                    p.setFont(f);
                }
                if (cell.underline) {
                    p.drawLine(x, y + m_fontAscent + 2, x + cw, y + m_fontAscent + 2);
                }
            }
        }
    }

    // 选区高亮（半透明覆盖，行优先归一化）
    if (m_hasSelection) {
        int sr = m_selAnchorRow, sc = m_selAnchorCol;
        int er = m_selEndRow, ec = m_selEndCol;
        if (sr > er || (sr == er && sc > ec)) {
            std::swap(sr, er);
            std::swap(sc, ec);
        }
        const QColor selColor(0x2f, 0x6f, 0xd0, 110);
        for (int r = sr; r <= er && r < m_rows; ++r) {
            const int c0 = (r == sr) ? sc : 0;
            const int c1 = (r == er) ? qMin(ec, m_cols - 1) : m_cols - 1;
            if (c1 >= c0) {
                p.fillRect(c0 * m_charWidth, r * m_charHeight,
                           (c1 - c0 + 1) * m_charWidth, m_charHeight, selColor);
            }
        }
    }

    // 光标（位置钳制到可见区域：行尾写入后 m_curCol 可能 == m_cols）
    int cx = qBound(0, m_curCol, m_cols - 1) * m_charWidth;
    int cy = qBound(0, m_curRow, m_rows - 1) * m_charHeight;
    if (hasFocus()) {
        // 有焦点：细竖线（beam）光标，随定时器闪烁
        if (m_cursorVisible) {
            const int barW = qMax(2, m_charWidth / 6);
            p.fillRect(cx, cy, barW, m_charHeight, QColor(0xc8, 0xc8, 0xc8));
        }
    } else {
        // 无焦点：暗色细竖线常显（不闪烁，避免间歇性完全消失）
        const int barW = qMax(2, m_charWidth / 6);
        p.fillRect(cx, cy, barW, m_charHeight, QColor(0x77, 0x77, 0x77));
    }
}

bool TerminalEmulator::event(QEvent* e) {
    // Tab/Backtab 在事件层直接拦截并交给 keyPressEvent：
    // 绕过 QWidget 的焦点遍历逻辑，确保远端 shell 补全（\t / \x1b[Z）始终可用
    if (e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            keyPressEvent(ke);
            return true;
        }
    }
    return QWidget::event(e);
}

void TerminalEmulator::keyPressEvent(QKeyEvent* event) {
    QString text = event->text();
    Qt::KeyboardModifiers mods = event->modifiers();

    // ── 剪贴板快捷键（优先于终端控制字符）──
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    if (ctrl && shift && event->key() == Qt::Key_C) { copySelection(); return; }
    if (ctrl && shift && event->key() == Qt::Key_V) { pasteClipboard(); return; }
    if (ctrl && shift && event->key() == Qt::Key_A) { selectAll(); return; }
    if (ctrl && !shift && event->key() == Qt::Key_V) { pasteClipboard(); return; }
    if (ctrl && !shift && event->key() == Qt::Key_C && m_hasSelection) {
        copySelection();   // 有选区时 Ctrl+C 复制；无选区时落到下方发送 ^C 中断
        return;
    }
    if (event->key() == Qt::Key_Escape && m_hasSelection) {
        clearSelection();  // 清除选区，Esc 仍继续发给远端
    }

    // 修饰键前缀
    QByteArray modPrefix;
    if (mods & Qt::ShiftModifier && !(mods & Qt::ControlModifier) && !(mods & Qt::AltModifier)) {
        // Shift 单独按下通常由 text 体现
    } else if ((mods & Qt::ControlModifier) || (mods & Qt::AltModifier) || (mods & Qt::MetaModifier)) {
        modPrefix = encodeModifier(mods);
    }

    switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            emit inputData("\r");
            return;
        case Qt::Key_Backspace:
            emit inputData(modPrefix.isEmpty() ? "\x7f" : modPrefix + "\x7f");
            return;
        case Qt::Key_Tab:
            // Shift+Tab → 反向补全（backtab），普通 Tab → 远端 shell 补全
            Logger::instance().info(
                QStringLiteral("Tab 键已捕获，发送 %1 到远端")
                    .arg((mods & Qt::ShiftModifier) ? "ESC[Z" : "\\t"), "term");
            emit inputData((mods & Qt::ShiftModifier) ? "\x1b[Z" : "\t");
            return;
        case Qt::Key_Up:      emit inputData(modPrefix + "\x1b[A"); return;
        case Qt::Key_Down:    emit inputData(modPrefix + "\x1b[B"); return;
        case Qt::Key_Right:   emit inputData(modPrefix + "\x1b[C"); return;
        case Qt::Key_Left:    emit inputData(modPrefix + "\x1b[D"); return;
        case Qt::Key_Home:    emit inputData(modPrefix + "\x1b[H"); return;
        case Qt::Key_End:     emit inputData(modPrefix + "\x1b[F"); return;
        case Qt::Key_Insert:  emit inputData(modPrefix + "\x1b[2~"); return;
        case Qt::Key_Delete:  emit inputData(modPrefix + "\x1b[3~"); return;
        case Qt::Key_PageUp:  emit inputData(modPrefix + "\x1b[5~"); return;
        case Qt::Key_PageDown:emit inputData(modPrefix + "\x1b[6~"); return;
        case Qt::Key_F1:      emit inputData(modPrefix + "\x1bOP"); return;
        case Qt::Key_F2:      emit inputData(modPrefix + "\x1bOQ"); return;
        case Qt::Key_F3:      emit inputData(modPrefix + "\x1bOR"); return;
        case Qt::Key_F4:      emit inputData(modPrefix + "\x1bOS"); return;
        case Qt::Key_F5:      emit inputData(modPrefix + "\x1b[15~"); return;
        case Qt::Key_F6:      emit inputData(modPrefix + "\x1b[17~"); return;
        case Qt::Key_F7:      emit inputData(modPrefix + "\x1b[18~"); return;
        case Qt::Key_F8:      emit inputData(modPrefix + "\x1b[19~"); return;
        case Qt::Key_F9:      emit inputData(modPrefix + "\x1b[20~"); return;
        case Qt::Key_F10:     emit inputData(modPrefix + "\x1b[21~"); return;
        case Qt::Key_F11:     emit inputData(modPrefix + "\x1b[23~"); return;
        case Qt::Key_F12:     emit inputData(modPrefix + "\x1b[24~"); return;
    }

    // Ctrl+字母 → 控制字符
    if (mods & Qt::ControlModifier && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
        char c = static_cast<char>(event->key() - Qt::Key_A + 1);
        emit inputData(QByteArray(1, c));
        return;
    }

    // 普通文本
    if (!text.isEmpty()) {
        emit inputData(text.toUtf8());
    }
}

bool TerminalEmulator::focusNextPrevChild(bool /*next*/) {
    // Tab/Shift+Tab 留给终端输入（远端 shell 自动补全），不做控件焦点切换
    return false;
}

void TerminalEmulator::resizeEvent(QResizeEvent*) {
    int newCols = qMax(1, width() / m_charWidth);
    int newRows = qMax(1, height() / m_charHeight);
    if (newCols != m_cols || newRows != m_rows) {
        m_cols = newCols;
        m_rows = newRows;
        m_scrollBottom = m_rows - 1;
        ensureBuffer();
        emit resized(m_cols, m_rows);
    }
}

void TerminalEmulator::focusInEvent(QFocusEvent*) {
    m_cursorVisible = true;
    update();
}

void TerminalEmulator::focusOutEvent(QFocusEvent*) {
    update();  // 重绘以切换为空心光标
}

void TerminalEmulator::wheelEvent(QWheelEvent* event) {
    // 转发滚动为方向键（简单方案，vim/less 可用）
    int delta = event->angleDelta().y();
    if (delta > 0) emit inputData("\x1b[5~");      // PageUp
    else if (delta < 0) emit inputData("\x1b[6~"); // PageDown
}

// ── 鼠标选区与剪贴板 ─────────────────────────────────────────────────────────

void TerminalEmulator::cellAt(const QPoint& pos, int& row, int& col) const {
    col = qBound(0, pos.x() / m_charWidth, m_cols - 1);
    row = qBound(0, pos.y() / m_charHeight, m_rows - 1);
}

void TerminalEmulator::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        int row, col;
        cellAt(event->position().toPoint(), row, col);
        m_selecting = true;
        m_hasSelection = false;
        m_selAnchorRow = m_selEndRow = row;
        m_selAnchorCol = m_selEndCol = col;
        update();
    }
}

void TerminalEmulator::mouseMoveEvent(QMouseEvent* event) {
    if (!m_selecting) return;
    int row, col;
    cellAt(event->position().toPoint(), row, col);
    if (row != m_selEndRow || col != m_selEndCol) {
        m_selEndRow = row;
        m_selEndCol = col;
        m_hasSelection = true;
        update();
    }
}

void TerminalEmulator::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        // 单击（未拖动）视为清除选区
        if (m_selAnchorRow == m_selEndRow && m_selAnchorCol == m_selEndCol) {
            m_hasSelection = false;
        }
        update();
    }
}

void TerminalEmulator::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    int row, col;
    cellAt(event->position().toPoint(), row, col);
    // 双击选词（以空格为分隔）
    auto isWordCell = [this](int r, int c) {
        const Cell& cell = m_screen[r][c];
        return cell.ch != ' ' && cell.ch != QChar(0);
    };
    if (!isWordCell(row, col)) {
        clearSelection();
        return;
    }
    int c0 = col, c1 = col;
    while (c0 > 0 && isWordCell(row, c0 - 1)) --c0;
    while (c1 < m_cols - 1 && isWordCell(row, c1 + 1)) ++c1;
    m_selAnchorRow = m_selEndRow = row;
    m_selAnchorCol = c0;
    m_selEndCol = c1;
    m_hasSelection = true;
    update();
}

QString TerminalEmulator::selectedText() const {
    if (!m_hasSelection) return QString();
    int sr = m_selAnchorRow, sc = m_selAnchorCol;
    int er = m_selEndRow, ec = m_selEndCol;
    if (sr > er || (sr == er && sc > ec)) {
        std::swap(sr, er);
        std::swap(sc, ec);
    }
    QString result;
    for (int r = sr; r <= er && r < m_rows; ++r) {
        const int c0 = (r == sr) ? sc : 0;
        const int c1 = (r == er) ? qMin(ec, m_cols - 1) : m_cols - 1;
        QString line;
        for (int c = c0; c <= c1 && c < m_cols; ++c) {
            const Cell& cell = m_screen[r][c];
            if (cell.wide) continue;   // 宽字符续列由主格输出
            line += cell.ch;
        }
        while (line.endsWith(' ')) line.chop(1);   // 去行尾空格
        result += line;
        if (r < er) result += '\n';
    }
    return result;
}

void TerminalEmulator::copySelection() {
    const QString text = selectedText();
    if (!text.isEmpty()) {
        QGuiApplication::clipboard()->setText(text);
    }
}

void TerminalEmulator::pasteClipboard() {
    QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty()) return;
    text.replace("\r\n", "\n").replace('\n', '\r');   // 换行转终端回车
    emit inputData(text.toUtf8());
}

void TerminalEmulator::selectAll() {
    m_selAnchorRow = 0;
    m_selAnchorCol = 0;
    m_selEndRow = m_rows - 1;
    m_selEndCol = m_cols - 1;
    m_hasSelection = true;
    update();
}

void TerminalEmulator::clearSelection() {
    if (m_hasSelection) {
        m_hasSelection = false;
        update();
    }
}

QSize TerminalEmulator::sizeHint() const {
    return QSize(m_cols * m_charWidth, m_rows * m_charHeight);
}

// ── 数据解析 ─────────────────────────────────────────────────────────────────

void TerminalEmulator::processData(const QByteArray& data) {
    for (char c : data) {
        unsigned char uc = static_cast<unsigned char>(c);

        // 正在累积 UTF-8 多字节序列
        if (m_utf8Remaining > 0) {
            if ((uc & 0xC0) == 0x80) {
                m_utf8Code = (m_utf8Code << 6) | (uc & 0x3F);
                if (--m_utf8Remaining == 0) flushUtf8Char();
                continue;
            }
            // 无效续字节：丢弃已累积部分，按新字节重新处理
            m_utf8Remaining = 0;
        }

        // UTF-8 多字节起始（0xC2-0xF4 为合法起始范围）
        if (uc >= 0xC2 && uc <= 0xF4) {
            if ((uc & 0xE0) == 0xC0)      { m_utf8Remaining = 1; m_utf8Code = uc & 0x1F; }
            else if ((uc & 0xF0) == 0xE0) { m_utf8Remaining = 2; m_utf8Code = uc & 0x0F; }
            else                          { m_utf8Remaining = 3; m_utf8Code = uc & 0x07; }
            continue;
        }

        // 孤立续字节 / 无效序列：丢弃
        if (uc >= 0x80) continue;

        parseChar(c);
    }
}

void TerminalEmulator::flushUtf8Char() {
    if (m_utf8Code <= 0xFFFF) {
        putChar(QChar(static_cast<ushort>(m_utf8Code)));
    } else {
        // 辅助平面（emoji 等），QChar 无法表示，显示替换符
        putChar(QChar(0xFFFD));
    }
}

// CJK 等双宽字符判定（BMP 范围，简化版 wcwidth）
bool TerminalEmulator::isWideChar(QChar c) {
    ushort u = c.unicode();
    return (u >= 0x1100 && u <= 0x115F) ||   // Hangul Jamo
           (u >= 0x2E80 && u <= 0x303E) ||   // CJK 部首/符号
           (u >= 0x3041 && u <= 0x33FF) ||   // 假名/注音/CJK 兼容
           (u >= 0x3400 && u <= 0x4DBF) ||   // CJK 扩展 A
           (u >= 0x4E00 && u <= 0x9FFF) ||   // CJK 统一表意文字
           (u >= 0xA000 && u <= 0xA4CF) ||   // 彝文
           (u >= 0xAC00 && u <= 0xD7A3) ||   // Hangul 音节
           (u >= 0xF900 && u <= 0xFAFF) ||   // CJK 兼容表意
           (u >= 0xFE30 && u <= 0xFE4F) ||   // CJK 兼容形式
           (u >= 0xFF00 && u <= 0xFF60) ||   // 全角形式
           (u >= 0xFFE0 && u <= 0xFFE6);
}

void TerminalEmulator::parseChar(char c) {
    switch (m_state) {
        case Ground: {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc == 0x1b) {                       // ESC
                m_state = Esc;
            } else if (uc == '\r') {
                carriageReturn();
            } else if (uc == '\n') {
                newLine();
            } else if (uc == '\b') {
                backspace();
            } else if (uc == '\t') {
                tab();
            } else if (uc == 0x07) {
                // BEL: 忽略（可做响铃）
            } else if (uc >= 0x20) {
                putChar(QChar(c));
            }
            // 其他控制字符忽略
            break;
        }
        case Esc:
            parseEscape(c);
            break;
        case CSI:
            parseCSI(c);
            break;
        case OSC:
            parseOSC(c);
            break;
        case OSC_Esc:
            if (c == '\\') {
                // OSC 结束 (ST)
                if (m_oscBuffer.startsWith("0;") || m_oscBuffer.startsWith("2;")) {
                    emit titleChanged(m_oscBuffer.mid(2));
                }
                m_state = Ground;
            } else {
                // 不是 ST，回到 OSC 继续收集
                m_oscBuffer += QChar('\x1b');
                m_oscBuffer += QChar(c);
                m_state = OSC;
            }
            break;
    }
}

void TerminalEmulator::parseEscape(char c) {
    switch (c) {
        case '[':  // CSI
            m_state = CSI;
            m_csiParams.clear();
            m_csiPrivate = false;
            break;
        case ']':  // OSC
            m_state = OSC;
            m_oscBuffer.clear();
            break;
        case '7':  // 保存光标
            m_savedRow = m_curRow;
            m_savedCol = m_curCol;
            m_state = Ground;
            break;
        case '8':  // 恢复光标
            m_curRow = m_savedRow;
            m_curCol = m_savedCol;
            m_state = Ground;
            break;
        case 'D':  // IND（换行）
            newLine();
            m_state = Ground;
            break;
        case 'E':  // NEL（回车换行）
            carriageReturn();
            newLine();
            m_state = Ground;
            break;
        case 'M':  // RI（反向换行）
            if (m_curRow == m_scrollTop) scrollDown(1);
            else if (m_curRow > 0) m_curRow--;
            m_state = Ground;
            break;
        case 'c':  // RIS（全重置）
            clear();
            m_state = Ground;
            break;
        default:
            // 未知转义，忽略
            m_state = Ground;
            break;
    }
}

void TerminalEmulator::parseCSI(char c) {
    if (c == '?' || c == '>' || c == '<' || c == '=') {
        m_csiPrivate = true;
        m_csiParams += QChar(c);
        return;
    }
    // 参数字符
    if ((c >= '0' && c <= '9') || c == ';' || c == ':' || c == ' ') {
        m_csiParams += QChar(c);
        return;
    }
    // 终止字符 → 执行
    m_csiParams += QChar(c);  // 最后一个字符是命令
    executeCSI();
    m_state = Ground;
}

void TerminalEmulator::parseOSC(char c) {
    if (c == 0x07) {  // BEL 结束
        if (m_oscBuffer.startsWith("0;") || m_oscBuffer.startsWith("2;")) {
            emit titleChanged(m_oscBuffer.mid(2));
        }
        m_state = Ground;
    } else if (c == 0x1b) {
        m_state = OSC_Esc;
    } else {
        m_oscBuffer += QChar(c);
    }
}

void TerminalEmulator::executeCSI() {
    if (m_csiParams.isEmpty()) return;

    QChar cmd = m_csiParams.back();
    QString paramStr = m_csiParams.left(m_csiParams.size() - 1);

    // 解析参数列表
    QList<int> params;
    for (const QString& s : paramStr.split(';', Qt::SkipEmptyParts)) {
        // 去掉可能的私有标记字符
        QString cleaned;
        for (QChar ch : s) {
            if (ch.isDigit()) cleaned += ch;
        }
        params.append(cleaned.isEmpty() ? 0 : cleaned.toInt());
    }
    auto P = [this, &params](int idx, int def) {
        return (idx < params.size() && params[idx] > 0) ? params[idx] : def;
    };

    if (m_csiPrivate) {
        // 私有序列（DEC 模式设置等），大多忽略
        return;
    }

    switch (cmd.unicode()) {
        case 'A':  // CUU 光标上移
            m_curRow = qMax(m_scrollTop, m_curRow - P(0, 1));
            break;
        case 'B':  // CUD 光标下移
            m_curRow = qMin(m_scrollBottom, m_curRow + P(0, 1));
            break;
        case 'C':  // CUF 光标右移
            m_curCol = qMin(m_cols - 1, m_curCol + P(0, 1));
            break;
        case 'D':  // CUB 光标左移
            m_curCol = qMax(0, m_curCol - P(0, 1));
            break;
        case 'E':  // CNL 下一行行首
            m_curRow = qMin(m_scrollBottom, m_curRow + P(0, 1));
            m_curCol = 0;
            break;
        case 'F':  // CPL 上一行行首
            m_curRow = qMax(m_scrollTop, m_curRow - P(0, 1));
            m_curCol = 0;
            break;
        case 'G':  // CHA 列定位
            m_curCol = qBound(0, P(0, 1) - 1, m_cols - 1);
            break;
        case 'H':  // CUP 光标定位
        case 'f':
            moveCursor(P(0, 1) - 1, P(1, 1) - 1);
            break;
        case 'J':  // ED 屏幕擦除
            eraseInDisplay(params.isEmpty() ? 0 : params[0]);
            break;
        case 'K':  // EL 行擦除
            eraseInLine(params.isEmpty() ? 0 : params[0]);
            break;
        case 'L': {  // IL 插入行
            int lines = P(0, 1);
            for (int i = 0; i < lines; ++i) {
                m_screen.insert(m_curRow, QVector<Cell>(m_cols));
                m_screen.removeAt(m_scrollBottom + 1);
            }
            break;
        }
        case 'M': {  // DL 删除行
            int lines = P(0, 1);
            for (int i = 0; i < lines; ++i) {
                m_screen.removeAt(m_curRow);
                m_screen.insert(m_scrollBottom, QVector<Cell>(m_cols));
            }
            break;
        }
        case 'P': {  // DCH 删除字符
            int count = P(0, 1);
            auto& line = m_screen[m_curRow];
            line.remove(m_curCol, qMin(count, line.size() - m_curCol));
            while (line.size() < m_cols) line.append(Cell{});
            break;
        }
        case '@': {  // ICH 插入空字符
            int count = P(0, 1);
            auto& line = m_screen[m_curRow];
            for (int i = 0; i < count && line.size() < m_cols; ++i) {
                line.insert(m_curCol, Cell{});
            }
            line.resize(m_cols);
            break;
        }
        case 'S':  // SU 向上滚动
            scrollUp(P(0, 1));
            break;
        case 'T':  // SD 向下滚动
            scrollDown(P(0, 1));
            break;
        case 'd':  // VPA 行定位
            m_curRow = qBound(m_scrollTop, P(0, 1) - 1, m_scrollBottom);
            break;
        case 'r': {  // DECSTBM 滚动区设置
            int top = P(0, 1) - 1;
            int bot = P(1, m_rows) - 1;
            if (top < bot && top >= 0 && bot < m_rows) {
                m_scrollTop = top;
                m_scrollBottom = bot;
                moveCursor(0, 0);
            }
            break;
        }
        case 'm':  // SGR 颜色
            executeSGR();
            break;
        case 'h':  // SM 设置模式（忽略）
        case 'l':  // RM 重置模式（忽略）
            break;
        default:
            // 未实现的命令，忽略
            break;
    }
}

void TerminalEmulator::executeSGR() {
    // m_csiParams 形如 "1;31m"
    QString paramStr = m_csiParams;
    if (paramStr.endsWith('m')) paramStr.chop(1);

    QList<int> params;
    if (paramStr.isEmpty()) {
        params.append(0);
    } else {
        for (const QString& s : paramStr.split(';', Qt::SkipEmptyParts)) {
            QString cleaned;
            for (QChar ch : s) if (ch.isDigit()) cleaned += ch;
            if (!cleaned.isEmpty()) params.append(cleaned.toInt());
        }
    }

    int i = 0;
    while (i < params.size()) {
        int p = params[i];
        switch (p) {
            case 0:   // 全重置
                m_curFg = 7; m_curBg = 0;
                m_curBold = m_curUnderline = m_curReverse = false;
                break;
            case 1:   m_curBold = true; break;
            case 4:   m_curUnderline = true; break;
            case 7:   m_curReverse = true; break;
            case 22:  m_curBold = false; break;
            case 24:  m_curUnderline = false; break;
            case 27:  m_curReverse = false; break;
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                m_curFg = static_cast<quint8>(p - 30); break;
            case 38: {  // 扩展前景色（256色/RGB），跳过参数
                if (i + 1 < params.size() && params[i+1] == 5) i += 2;      // 256色
                else if (i + 1 < params.size() && params[i+1] == 2) i += 4; // RGB
                m_curFg = 7;
                break;
            }
            case 39:  m_curFg = 7; break;
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                m_curBg = static_cast<quint8>(p - 40); break;
            case 48: {  // 扩展背景色，跳过
                if (i + 1 < params.size() && params[i+1] == 5) i += 2;
                else if (i + 1 < params.size() && params[i+1] == 2) i += 4;
                break;
            }
            case 49:  m_curBg = 0; break;
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                m_curFg = static_cast<quint8>(p - 90 + 8); break;
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                m_curBg = static_cast<quint8>(p - 100 + 8); break;
            default: break;
        }
        ++i;
    }
}

// ── 屏幕操作 ─────────────────────────────────────────────────────────────────

void TerminalEmulator::ensureBuffer() {
    // 调整行数
    while (m_screen.size() < m_rows) {
        m_screen.append(QVector<Cell>(m_cols));
    }
    while (m_screen.size() > m_rows) {
        m_screen.removeFirst();
        if (m_curRow > 0) m_curRow--;
    }
    // 调整每行列数
    for (auto& line : m_screen) {
        line.resize(m_cols);
    }
    m_curRow = qBound(0, m_curRow, m_rows - 1);
    m_curCol = qBound(0, m_curCol, m_cols - 1);
    m_scrollTop = qBound(0, m_scrollTop, m_rows - 1);
    m_scrollBottom = m_rows - 1;
}

void TerminalEmulator::scrollUp(int lines) {
    for (int i = 0; i < lines; ++i) {
        m_screen.removeAt(m_scrollTop);
        m_screen.insert(m_scrollBottom, QVector<Cell>(m_cols));
    }
}

void TerminalEmulator::scrollDown(int lines) {
    for (int i = 0; i < lines; ++i) {
        m_screen.removeAt(m_scrollBottom);
        m_screen.insert(m_scrollTop, QVector<Cell>(m_cols));
    }
}

void TerminalEmulator::newLine() {
    if (m_curRow == m_scrollBottom) {
        scrollUp(1);
    } else if (m_curRow < m_rows - 1) {
        m_curRow++;
    }
}

void TerminalEmulator::carriageReturn() {
    m_curCol = 0;
}

void TerminalEmulator::backspace() {
    if (m_curCol > 0) m_curCol--;
}

void TerminalEmulator::tab() {
    int next = (m_curCol / 8 + 1) * 8;
    m_curCol = qMin(next, m_cols - 1);
}

void TerminalEmulator::eraseInDisplay(int mode) {
    switch (mode) {
        case 0: {  // 光标到结尾
            eraseInLine(0);
            for (int r = m_curRow + 1; r < m_rows; ++r) {
                for (int c = 0; c < m_cols; ++c) m_screen[r][c] = Cell{};
            }
            break;
        }
        case 1: {  // 开头到光标
            eraseInLine(1);
            for (int r = 0; r < m_curRow; ++r) {
                for (int c = 0; c < m_cols; ++c) m_screen[r][c] = Cell{};
            }
            break;
        }
        case 2:  // 全屏
        case 3:
            for (int r = 0; r < m_rows; ++r) {
                for (int c = 0; c < m_cols; ++c) m_screen[r][c] = Cell{};
            }
            break;
    }
}

void TerminalEmulator::eraseInLine(int mode) {
    auto& line = m_screen[m_curRow];
    switch (mode) {
        case 0:  // 光标到行尾
            for (int c = m_curCol; c < m_cols; ++c) line[c] = Cell{};
            break;
        case 1:  // 行首到光标
            for (int c = 0; c <= m_curCol && c < m_cols; ++c) line[c] = Cell{};
            break;
        case 2:  // 整行
            for (int c = 0; c < m_cols; ++c) line[c] = Cell{};
            break;
    }
}

void TerminalEmulator::putChar(QChar ch) {
    bool wide = isWideChar(ch);

    if (wide) {
        // 宽字符需要两列：行尾只剩一列时先换行
        if (m_curCol >= m_cols - 1) {
            m_screen[m_curRow][m_curCol] = Cell{};  // 行尾留空
            carriageReturn();
            newLine();
        }
    } else if (m_curCol >= m_cols) {
        // 自动换行（简化：直接换行）
        carriageReturn();
        newLine();
    }
    if (m_curRow < 0 || m_curRow >= m_rows) return;

    Cell cell;
    cell.ch = ch;
    cell.fg = m_curFg;
    cell.bg = m_curBg;
    cell.bold = m_curBold;
    cell.underline = m_curUnderline;
    cell.reverse = m_curReverse;
    m_screen[m_curRow][m_curCol] = cell;
    m_curCol++;

    // 宽字符续列（继承属性，标记 wide，绘制时跳过）
    if (wide && m_curCol < m_cols) {
        Cell cont = cell;
        cont.ch = ' ';
        cont.wide = true;
        m_screen[m_curRow][m_curCol] = cont;
        m_curCol++;
    }
}

void TerminalEmulator::moveCursor(int row, int col) {
    m_curRow = qBound(0, row, m_rows - 1);
    m_curCol = qBound(0, col, m_cols - 1);
}

// ── 渲染辅助 ─────────────────────────────────────────────────────────────────

void TerminalEmulator::updateMetrics() {
    QFontMetrics fm(font());
    m_charWidth = qMax(1, fm.horizontalAdvance('M'));
    m_charHeight = qMax(1, fm.height());
    m_fontAscent = fm.ascent();
}

void TerminalEmulator::scheduleUpdate() {
    if (!m_repaintTimer.isActive()) {
        m_repaintTimer.start();
    }
}

// ── 输入辅助 ─────────────────────────────────────────────────────────────────

QByteArray TerminalEmulator::encodeModifier(int qtMods) const {
    // xterm 修饰键编码：参数值 = 1 + (shift*1 + alt*2 + ctrl*4)
    int val = 1;
    if (qtMods & Qt::ShiftModifier) val += 1;
    if (qtMods & Qt::AltModifier) val += 2;
    if (qtMods & Qt::ControlModifier) val += 4;
    if (val == 1) return {};
    return QString("\x1b[%1").arg(val).toUtf8();
}
