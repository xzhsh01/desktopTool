#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// TemplateStore: 邮件模板管理
//
//   设计原则：
//     1. 启动时把一组 hardcoded 内置模板（续约 / 投诉 / 邀请 / 感谢 / 请假 / 汇报 / 道歉）
//        提供给用户，覆盖最常见的 7 个商务场景。
//     2. 用户可保存自己的模板（"另存为模板"）到 ~/.bambooRat/mail/templates.json
//     3. 用户模板可在模板菜单里删除；内置模板只读。
//     4. 模板支持简单占位符替换：{date} / {time} / {recipient} / {sender} / {custom:<name>}
//
//   单例：TemplateStore::instance()
//
//   用法：
//     auto* ts = TemplateStore::instance();
//     auto templates = ts->listAll();               // 全部（内置 + 用户）
//     auto builtin   = ts->listBuiltin();           // 仅内置
//     auto user      = ts->listUser();              // 仅用户
//     auto tpl       = ts->findById("builtin:renewal");
//     QString applied = ts->applyTemplate(*tpl, replacements);  // 占位符替换
//     QString id = ts->addUserTemplate(tpl);
//     bool ok = ts->removeUserTemplate(id);
// ─────────────────────────────────────────────────────────────────────────────

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class QSignalMapper;

namespace mail {

struct Template {
    QString id;             // builtin:<key> 或 user:<uuid>
    QString name;           // 显示名（"续约通知"）
    QString category;       // 分类（"业务" / "投诉" / "邀请" / "感谢" / "请假" / "汇报" / "道歉" / "自定义"）
    QString description;    // 一句话说明（tooltip 展示）
    QString subject;        // 主题（支持占位符）
    QString body;           // 正文（Markdown / 纯文本；支持占位符）
    bool    isBuiltin = false;  // true=内置只读；false=用户可编辑
    QDateTime createdAt;
    QDateTime updatedAt;
};

class TemplateStore : public QObject {
    Q_OBJECT
public:
    static TemplateStore& instance();

    // 列出全部 / 仅内置 / 仅用户模板（按 category 排序）
    QList<Template> listAll() const;
    QList<Template> listBuiltin() const;
    QList<Template> listUser() const;

    // 按 id 查模板；找不到返回 nullptr
    Template* findById(const QString& id);

    // 应用模板：把 subject + body 中的占位符替换为实际值
    //   - {date}      当前日期 yyyy-MM-dd
    //   - {time}      当前时间 HH:mm
    //   - {recipient} 收件人列表（多个用"、"连接）
    //   - {sender}    发件人邮箱（可选）
    //   - {name}      用户名（可选，从 settings 读）
    //   - 任何 {key:val} 自定义占位符 → 由 extras 提供
    // 未匹配到 {xxx} 保持原样（让用户手动填）
    static QString applyPlaceholders(const QString& src,
                                     const QString& recipient = QString(),
                                     const QString& sender = QString(),
                                     const QHash<QString,QString>& extras = {});

    // 添加用户模板（生成 id，存盘），返回新 id；失败返回空
    QString addUserTemplate(Template tpl);

    // 删除用户模板；内置拒绝删除。返回是否成功
    bool removeUserTemplate(const QString& id);

    // 重新加载 / 落盘
    void reload();
    void flush();

signals:
    void templatesChanged();

private:
    explicit TemplateStore(QObject* parent = nullptr);
    ~TemplateStore() override;

    void ensureLoaded();
    void ensureUserDir();
    void loadBuiltin();    // 注入 hardcoded 内置模板（仅首次或缺失时）
    void loadUserFromDisk();
    void saveUserToDisk();

    QString userTemplatesPath() const;

    QList<Template> m_builtin;
    QHash<QString, Template> m_user;        // user templates indexed by id
    bool m_loaded = false;
};

} // namespace mail