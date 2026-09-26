#include "database/SqlEditor.h"

#include <QCompleter>
#include <QStringListModel>
#include <QKeyEvent>
#include <QAbstractItemView>
#include <QScrollBar>
#include <QTextCursor>

const QStringList& SqlEditor::sqlKeywords() {
    static const QStringList keywords = {
        // 查询与 DML
        "SELECT", "INSERT", "UPDATE", "DELETE", "FROM", "WHERE", "INTO", "VALUES", "SET",
        "DISTINCT", "ALL", "AS", "TOP", "LIMIT", "OFFSET", "RETURNING",
        // 连接与分组
        "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "CROSS", "ON", "USING",
        "GROUP BY", "ORDER BY", "HAVING", "UNION", "INTERSECT", "EXCEPT", "MINUS",
        "ASC", "DESC", "NULLS FIRST", "NULLS LAST",
        // 条件
        "AND", "OR", "NOT", "NULL", "IS", "IN", "BETWEEN", "LIKE", "ILIKE", "EXISTS",
        "CASE", "WHEN", "THEN", "ELSE", "END", "ANY", "SOME",
        // DDL
        "CREATE", "ALTER", "DROP", "TABLE", "VIEW", "INDEX", "UNIQUE", "SEQUENCE",
        "TRIGGER", "FUNCTION", "PROCEDURE", "SCHEMA", "DATABASE", "TRUNCATE", "RENAME",
        "ADD", "COLUMN", "CONSTRAINT", "PRIMARY KEY", "FOREIGN KEY", "REFERENCES",
        "DEFAULT", "CHECK", "AUTO_INCREMENT", "IF NOT EXISTS", "IF EXISTS", "CASCADE",
        // 事务
        "BEGIN", "COMMIT", "ROLLBACK", "SAVEPOINT", "START TRANSACTION",
        // 权限与其他
        "GRANT", "REVOKE", "USE", "SHOW", "DESCRIBE", "DESC", "EXPLAIN", "ANALYZE",
        "WITH", "RECURSIVE", "OVER", "PARTITION BY", "WINDOW",
        // 聚合函数
        "COUNT", "SUM", "AVG", "MAX", "MIN", "GROUP_CONCAT", "STRING_AGG",
        // 常用函数
        "NOW", "CURRENT_DATE", "CURRENT_TIMESTAMP", "CURRENT_USER",
        "COALESCE", "NULLIF", "IFNULL", "NVL", "CAST", "CONVERT",
        "UPPER", "LOWER", "LENGTH", "CHAR_LENGTH", "SUBSTRING", "SUBSTR", "TRIM",
        "CONCAT", "REPLACE", "INSTR", "POSITION", "LEFT", "RIGHT", "LPAD", "RPAD",
        "ABS", "ROUND", "CEIL", "CEILING", "FLOOR", "MOD", "POWER", "SQRT", "RAND", "RANDOM",
        "DATE", "TIME", "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND",
        "DATE_FORMAT", "TO_CHAR", "TO_DATE", "DATEDIFF", "DATEADD", "DATE_SUB", "DATE_ADD",
        "ROW_NUMBER", "RANK", "DENSE_RANK", "LAG", "LEAD", "FIRST_VALUE", "LAST_VALUE",
        // 常用片段
        "SELECT * FROM ", "INSERT INTO ", "ORDER BY ", "GROUP BY ",
    };
    return keywords;
}

SqlEditor::SqlEditor(QWidget* parent) : QPlainTextEdit(parent) {
    m_model = new QStringListModel(this);
    rebuildModel();

    m_completer = new QCompleter(m_model, this);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setWrapAround(true);
    m_completer->setMaxVisibleItems(12);

    // 深色主题，与全局样式一致
    if (auto* popup = m_completer->popup()) {
        popup->setStyleSheet(
            "QListView { background: #252830; color: #c8c8c8; border: 1px solid #3a3d46;"
            " border-radius: 4px; font-size: 13px; padding: 2px; }"
            "QListView::item { padding: 4px 10px; border-radius: 3px; }"
            "QListView::item:selected { background: #2a4a5e; color: #4fc3f7; }");
    }

    connect(m_completer, QOverload<const QString&>::of(&QCompleter::activated),
            this, &SqlEditor::insertCompletion);
}

void SqlEditor::setExtraWords(const QStringList& words) {
    m_extraWords = words;
    rebuildModel();
}

void SqlEditor::rebuildModel() {
    QStringList all = sqlKeywords();
    for (const QString& w : m_extraWords) {
        if (!w.isEmpty() && !all.contains(w, Qt::CaseInsensitive)) all << w;
    }
    m_model->setStringList(all);
}

QString SqlEditor::textUnderCursor() const {
    QTextCursor tc = textCursor();
    // 向左扩展单词字符（含下划线、点），取光标前的标识符前缀
    while (tc.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor)) {
        const QString s = tc.selectedText();
        const QChar c = s.isEmpty() ? QChar() : s.at(0);
        if (!(c.isLetterOrNumber() || c == '_' || c == '.')) {
            // 多选了一个非单词字符，回退
            tc.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            break;
        }
    }
    return tc.selectedText();
}

void SqlEditor::insertCompletion(const QString& completion) {
    QTextCursor tc = textCursor();
    const QString prefix = textUnderCursor();
    for (int i = 0; i < prefix.length(); ++i) tc.deletePreviousChar();
    tc.insertText(completion);
    setTextCursor(tc);
}

void SqlEditor::keyPressEvent(QKeyEvent* e) {
    // 补全弹窗可见：导航/确认键交给弹窗处理
    if (m_completer->popup()->isVisible()) {
        switch (e->key()) {
            case Qt::Key_Enter:
            case Qt::Key_Return:
            case Qt::Key_Escape:
            case Qt::Key_Tab:
            case Qt::Key_Backtab:
                e->ignore();
                return;
            default:
                break;
        }
    }

    // Ctrl+Space / Ctrl+Enter 放行给外层快捷键
    const bool isShortcut = (e->modifiers() & Qt::ControlModifier) && e->key() == Qt::Key_Space;

    QPlainTextEdit::keyPressEvent(e);

    const bool ctrlOrAlt = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier);
    const QString prefix = textUnderCursor();

    if (!isShortcut && (ctrlOrAlt || e->text().isEmpty())) {
        m_completer->popup()->hide();
        return;
    }

    // 至少 1 个字符才自动提示（避免弹窗常驻）
    if (!isShortcut && prefix.length() < 1) {
        m_completer->popup()->hide();
        return;
    }

    if (prefix != m_completer->completionPrefix()) {
        m_completer->setCompletionPrefix(prefix);
        m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));
    }

    if (m_completer->completionCount() == 0) {
        m_completer->popup()->hide();
        return;
    }

    QRect cr = cursorRect();
    cr.setWidth(m_completer->popup()->sizeHintForColumn(0)
                + m_completer->popup()->verticalScrollBar()->sizeHint().width());
    m_completer->complete(cr);
}
