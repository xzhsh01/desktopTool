#include "mail/TemplateStore.h"

#include "core/Logger.h"
#include "core/Settings.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

namespace mail {

// ─────────────────────────────────────────────────────────────────────────────
// 内置模板（hardcoded，首次启动注入；用户不可改）
// ─────────────────────────────────────────────────────────────────────────────
static QList<Template> buildBuiltin() {
    QList<Template> ts;
    auto make = [&](const QString& key, const QString& name, const QString& cat,
                    const QString& desc, const QString& subj, const QString& body) {
        Template t;
        t.id          = QStringLiteral("builtin:") + key;
        t.name        = name;
        t.category    = cat;
        t.description = desc;
        t.subject     = subj;
        t.body        = body;
        t.isBuiltin   = true;
        t.createdAt   = QDateTime::currentDateTime();
        t.updatedAt   = t.createdAt;
        return t;
    };

    // 业务
    ts << make(QStringLiteral("renewal"),
               QStringLiteral("续约通知"),
               QStringLiteral("业务"),
               QStringLiteral("向客户发送合同续约的友好提醒"),
               QStringLiteral("关于 {name} 服务的续约通知"),
               QStringLiteral(
                   "尊敬的 {recipient}：\n\n"
                   "您好！您当前使用的 **{name}** 服务将于 **{date}** 到期。\n\n"
                   "为确保您的业务不中断，我们建议您尽快办理续约手续。本年度续约可享受：\n\n"
                   "- 9 折优惠\n"
                   "- 免费延长 30 天服务期\n"
                   "- 专属技术支持通道\n\n"
                   "如有任何问题，欢迎随时与我联系。\n\n"
                   "此致\n"
                   "敬礼\n\n"
                   "{sender}\n"
                   "{time}"));

    ts << make(QStringLiteral("followup"),
               QStringLiteral("跟进邮件"),
               QStringLiteral("业务"),
               QStringLiteral("销售/合作机会的礼貌跟进"),
               QStringLiteral("跟进：关于上次沟通的 {name} 合作"),
               QStringLiteral(
                   "您好 {recipient}：\n\n"
                   "距离我们上次沟通已经过去几天了，想再简单跟进一下。\n\n"
                   "如果您对 **{name}** 方案有任何疑问，或需要补充资料，请随时告诉我。我会在 "
                   "24 小时内回复您。\n\n"
                   "如果方便的话，我们可以安排一次 30 分钟的线上会议，详细讨论合作细节。\n\n"
                   "祝好\n"
                   "{sender}"));

    // 投诉
    ts << make(QStringLiteral("complaint_reply"),
               QStringLiteral("投诉处理回复"),
               QStringLiteral("投诉"),
               QStringLiteral("客户投诉的致歉 + 解决方案"),
               QStringLiteral("回复：关于您反馈的问题"),
               QStringLiteral(
                   "尊敬的 {recipient}：\n\n"
                   "您好！\n\n"
                   "首先，对于您本次遇到的问题，我们深表歉意。作为 **{name}** 团队的一员，我代表公司向您致以最诚挚的歉意。\n\n"
                   "我们已对您反馈的情况进行了详细调查，初步结论如下：\n\n"
                   "1. 问题原因：\n"
                   "2. 我们的处理方案：\n"
                   "3. 补偿措施：\n\n"
                   "我们将在 **{time}** 前完成所有整改。如您需要进一步沟通，请直接回复本邮件或致电客户热线。\n\n"
                   "再次感谢您的反馈，让我们有机会做得更好。\n\n"
                   "{sender}\n"
                   "客户支持团队"));

    // 邀请
    ts << make(QStringLiteral("meeting"),
               QStringLiteral("会议邀请"),
               QStringLiteral("邀请"),
               QStringLiteral("正式的会议邀请邮件"),
               QStringLiteral("会议邀请：{name} （{date}）"),
               QStringLiteral(
                   "您好 {recipient}：\n\n"
                   "诚挚邀请您参加以下会议：\n\n"
                   "**主题**：{name}\n"
                   "**时间**：{date} {time}\n"
                   "**地点**：线上会议（链接将在会议开始前 15 分钟发送）\n\n"
                   "**议程**：\n\n"
                   "1. 上期回顾\n"
                   "2. 本期重点\n"
                   "3. 讨论与决议\n"
                   "4. 下期安排\n\n"
                   "如时间不便，回复邮件即可，我们再协调。\n\n"
                   "{sender}"));

    // 感谢
    ts << make(QStringLiteral("thanks"),
               QStringLiteral("感谢信"),
               QStringLiteral("感谢"),
               QStringLiteral("对客户/同事帮助的正式感谢"),
               QStringLiteral("衷心感谢"),
               QStringLiteral(
                   "尊敬的 {recipient}：\n\n"
                   "在此，我想对您在 **{name}** 项目中给予的支持与帮助表示衷心的感谢。\n\n"
                   "您的专业建议和及时响应，是项目顺利推进的关键。也正因如此，我们才能在 {date} 前保质保量地完成交付。\n\n"
                   "未来如有任何我能协助的地方，请随时告知，我也很期待继续与您合作。\n\n"
                   "再次感谢！\n\n"
                   "{sender}"));

    // 请假
    ts << make(QStringLiteral("leave"),
               QStringLiteral("请假申请"),
               QStringLiteral("请假"),
               QStringLiteral("正式的请假申请邮件"),
               QStringLiteral("请假申请：{name}"),
               QStringLiteral(
                   "{recipient} 您好：\n\n"
                   "因 **{name}**（事由），本人拟请假 **{date}** （共 1 天）。\n\n"
                   "请假期间，我已安排 *同事名* 负责交接工作，紧急事项可通过手机联系。\n\n"
                   "恳请批准，谢谢！\n\n"
                   "{sender}"));

    // 汇报
    ts << make(QStringLiteral("report"),
               QStringLiteral("工作汇报"),
               QStringLiteral("汇报"),
               QStringLiteral("周报/月报的工作汇报邮件"),
               QStringLiteral("工作汇报：{name}（{date}）"),
               QStringLiteral(
                   "{recipient} 您好：\n\n"
                   "以下是本周（{date}）的工作汇报：\n\n"
                   "**完成事项**：\n\n"
                   "- \n"
                   "- \n"
                   "- \n\n"
                   "**进行中**：\n\n"
                   "- \n"
                   "- \n\n"
                   "**下阶段计划**：\n\n"
                   "- \n"
                   "- \n\n"
                   "**需要支持**：\n\n"
                   "- \n\n"
                   "如有任何问题，欢迎随时沟通。\n\n"
                   "{sender}"));

    // 道歉
    ts << make(QStringLiteral("apology"),
               QStringLiteral("正式道歉"),
               QStringLiteral("道歉"),
               QStringLiteral("对失误/延迟的正式道歉"),
               QStringLiteral("关于 {name} 事件的致歉"),
               QStringLiteral(
                   "{recipient} 您好：\n\n"
                   "对于 **{name}** 事件中给贵方造成的不便，我谨代表团队向您致以最诚挚的歉意。\n\n"
                   "经内部复盘，我们已识别问题根因，并立即启动整改：\n\n"
                   "1. 短期：\n   - 立即下发补丁\n   - 主动补偿方案\n\n"
                   "2. 长期：\n   - 完善 SOP\n   - 增加 review 环节\n\n"
                   "我们将在 **{time}** 前完成全部整改。如您希望进一步沟通，我们随时恭候。\n\n"
                   "再次深表歉意！\n\n"
                   "{sender}"));

    return ts;
}

// ─────────────────────────────────────────────────────────────────────────────
// TemplateStore singleton
// ─────────────────────────────────────────────────────────────────────────────
TemplateStore& TemplateStore::instance() {
    static TemplateStore s;
    return s;
}

TemplateStore::TemplateStore(QObject* parent) : QObject(parent) {}
TemplateStore::~TemplateStore() { flush(); }

void TemplateStore::ensureLoaded() {
    if (m_loaded) return;
    ensureUserDir();
    loadBuiltin();
    loadUserFromDisk();
    m_loaded = true;
}

void TemplateStore::ensureUserDir() {
    QString p = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(p + "/mail");
}

QString TemplateStore::userTemplatesPath() const {
    QString p = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return p + "/mail/templates.json";
}

void TemplateStore::loadBuiltin() {
    m_builtin = buildBuiltin();
}

void TemplateStore::loadUserFromDisk() {
    QFile f(userTemplatesPath());
    if (!f.exists()) return;
    if (!f.open(QIODevice::ReadOnly)) {
        Logger::instance().warn(
            QStringLiteral("TemplateStore: 无法读取用户模板: %1").arg(f.errorString()),
            "mail");
        return;
    }
    QJsonParseError perr;
    auto doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        Logger::instance().warn(
            QStringLiteral("TemplateStore: JSON 解析失败: %1").arg(perr.errorString()),
            "mail");
        return;
    }
    for (auto v : doc.array()) {
        auto o = v.toObject();
        Template t;
        t.id          = o["id"].toString();
        t.name        = o["name"].toString();
        t.category    = o["category"].toString();
        t.description = o["description"].toString();
        t.subject     = o["subject"].toString();
        t.body        = o["body"].toString();
        t.isBuiltin   = false;
        t.createdAt   = QDateTime::fromString(o["createdAt"].toString(), Qt::ISODate);
        t.updatedAt   = QDateTime::fromString(o["updatedAt"].toString(), Qt::ISODate);
        if (t.id.startsWith(QStringLiteral("user:")) && !t.name.isEmpty()) {
            m_user.insert(t.id, t);
        }
    }
    Logger::instance().info(
        QStringLiteral("TemplateStore: 加载用户模板 %1 个").arg(m_user.size()),
        "mail");
}

void TemplateStore::saveUserToDisk() {
    QJsonArray arr;
    for (auto it = m_user.begin(); it != m_user.end(); ++it) {
        const Template& t = it.value();
        QJsonObject o;
        o["id"]          = t.id;
        o["name"]        = t.name;
        o["category"]    = t.category;
        o["description"] = t.description;
        o["subject"]     = t.subject;
        o["body"]        = t.body;
        o["createdAt"]   = t.createdAt.toString(Qt::ISODate);
        o["updatedAt"]   = t.updatedAt.toString(Qt::ISODate);
        arr.append(o);
    }
    QFile f(userTemplatesPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::instance().error(
            QStringLiteral("TemplateStore: 无法写入用户模板: %1").arg(f.errorString()),
            "mail");
        return;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    f.close();
    Logger::instance().info(
        QStringLiteral("TemplateStore: 保存用户模板 %1 个").arg(m_user.size()),
        "mail");
}

QList<Template> TemplateStore::listAll() const {
    const_cast<TemplateStore*>(this)->ensureLoaded();
    QList<Template> all;
    all << m_builtin << m_user.values();
    std::sort(all.begin(), all.end(), [](const Template& a, const Template& b){
        if (a.category != b.category) return a.category < b.category;
        return a.name < b.name;
    });
    return all;
}

QList<Template> TemplateStore::listBuiltin() const {
    const_cast<TemplateStore*>(this)->ensureLoaded();
    return m_builtin;
}

QList<Template> TemplateStore::listUser() const {
    const_cast<TemplateStore*>(this)->ensureLoaded();
    QList<Template> v = m_user.values();
    std::sort(v.begin(), v.end(), [](const Template& a, const Template& b){
        return a.updatedAt > b.updatedAt;
    });
    return v;
}

Template* TemplateStore::findById(const QString& id) {
    ensureLoaded();
    if (id.startsWith(QStringLiteral("builtin:"))) {
        for (auto& t : m_builtin) if (t.id == id) return &t;
    }
    if (id.startsWith(QStringLiteral("user:"))) {
        auto it = m_user.find(id);
        if (it != m_user.end()) return &it.value();
    }
    return nullptr;
}

QString TemplateStore::applyPlaceholders(const QString& src,
                                         const QString& recipient,
                                         const QString& sender,
                                         const QHash<QString,QString>& extras) {
    QString out = src;
    QString date = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd"));
    QString time = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));
    QString rcpt = recipient;
    // 多个收件人时用"、"连接便于称呼
    QStringList rcs = rcpt.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    if (rcs.size() > 1) rcpt = rcs.join(QStringLiteral("、"));
    out.replace(QStringLiteral("{date}"),      date);
    out.replace(QStringLiteral("{time}"),      time);
    out.replace(QStringLiteral("{recipient}"), rcpt);
    out.replace(QStringLiteral("{sender}"),    sender);
    // 用户名：尝试从 settings 读（如果暴露 API）；否则留占位
    out.replace(QStringLiteral("{name}"),
                extras.value(QStringLiteral("name"),
                            Settings::instance().get(QStringLiteral("user/displayName"),
                                                     QStringLiteral("")).toString()));
    // 自定义额外占位符
    for (auto it = extras.begin(); it != extras.end(); ++it) {
        const QString& k = it.key();
        const QString& v = it.value();
        if (k == QStringLiteral("name")) continue;
        out.replace(QStringLiteral("{") + k + QStringLiteral("}"), v);
    }
    return out;
}

QString TemplateStore::addUserTemplate(Template tpl) {
    ensureLoaded();
    if (tpl.name.trimmed().isEmpty()) return {};
    tpl.id = QStringLiteral("user:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    tpl.isBuiltin = false;
    tpl.createdAt = QDateTime::currentDateTime();
    tpl.updatedAt = tpl.createdAt;
    if (tpl.category.isEmpty()) tpl.category = QStringLiteral("自定义");
    m_user.insert(tpl.id, tpl);
    saveUserToDisk();
    emit templatesChanged();
    Logger::instance().info(
        QStringLiteral("TemplateStore: 新增用户模板: %1 (%2)").arg(tpl.name, tpl.id),
        "mail");
    return tpl.id;
}

bool TemplateStore::removeUserTemplate(const QString& id) {
    ensureLoaded();
    if (!id.startsWith(QStringLiteral("user:"))) {
        Logger::instance().warn(
            QStringLiteral("TemplateStore: 拒绝删除非用户模板: %1").arg(id), "mail");
        return false;
    }
    bool ok = m_user.remove(id) > 0;
    if (ok) {
        saveUserToDisk();
        emit templatesChanged();
        Logger::instance().info(
            QStringLiteral("TemplateStore: 删除用户模板: %1").arg(id), "mail");
    }
    return ok;
}

void TemplateStore::reload() {
    m_loaded = false;
    m_builtin.clear();
    m_user.clear();
    ensureLoaded();
    emit templatesChanged();
}

void TemplateStore::flush() {
    if (!m_loaded) return;
    saveUserToDisk();
}

} // namespace mail