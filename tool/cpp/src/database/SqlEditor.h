#pragma once

#include <QPlainTextEdit>

class QCompleter;
class QStringListModel;
class QKeyEvent;

/**
 * SqlEditor: 带自动补全提示的 SQL 编辑器
 *
 * 提示来源：
 *  - SQL 关键字与常用函数（内置静态词表）
 *  - 动态词表（数据库名/表名/视图名等，连接后由 DatabaseWidget 注入）
 *
 * 交互：
 *  - 输入 >= 1 个字符自动弹出匹配列表（大小写不敏感、包含匹配）
 *  - Ctrl+Space 强制弹出
 *  - Enter/Tab 接受补全，Esc 关闭
 */
class SqlEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit SqlEditor(QWidget* parent = nullptr);

    // 注入动态词表（表名、库名等）；会覆盖上一次的动态部分
    void setExtraWords(const QStringList& words);

protected:
    void keyPressEvent(QKeyEvent* e) override;

private slots:
    void insertCompletion(const QString& completion);

private:
    QString textUnderCursor() const;
    void rebuildModel();

    QCompleter* m_completer = nullptr;
    QStringListModel* m_model = nullptr;
    QStringList m_extraWords;

    // 内置 SQL 关键字与函数
    static const QStringList& sqlKeywords();
};
