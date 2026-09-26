#pragma once
#include <QString>
#include <QVariantMap>

/**
 * Labels — UI 文本外部化（用配置文件管理中文 Label 名称）
 *
 * 用法：
 *     QString s = Labels::instance().get("mail.editor.send");
 *     // 失败回退到 key 自身（或可选的 defaultText）
 *     QString s = Labels::instance().get("mail.editor.send", "发送");
 *
 * 配置加载顺序（后加载覆盖前加载）：
 *   1. Qt 嵌入默认 labels.json（资源路径 :/labels/default.json）
 *   2. 用户目录 %APPDATA%/KFrame/bambooRat/labels.json（不存在则跳过）
 *   3. 用户可通过 Settings 触发 reload() 后生效
 *
 * 路径命名规范（点号分隔）：
 *   <模块>.<页面>.<元素>      例如 mail.editor.sendBtn
 *   common.*                  通用按钮（确定/取消/保存/...）
 *   settings.*                设置页
 *   mail.editor.*             邮件编辑器
 *   mail.list.*               邮件列表
 *   mail.content.*            邮件内容
 *   mail.folders.*            文件夹面板
 *   mail.account.*            账号管理
 *   conn.*                    连接管理
 *   db.*                      数据库
 *   ssh.*                     SSH/SFTP
 *   redis.*                   Redis
 *
 * 用户自定义示例（%APPDATA%/KFrame/bambooRat/labels.json）：
 *   {
 *     "mail": {
 *       "editor": {
 *         "sendBtn": "发送 →"
 *       }
 *     }
 *   }
 */
class Labels {
public:
    static Labels& instance();

    /// 用点号路径取值；缺失返回 defaultText（默认 = key 本身）
    QString get(const QString& path, const QString& defaultText = QString()) const;

    /// 重新加载（用户覆盖文件改了之后手动调用）
    void reload();

    /// 用户文件路径（只读，调试用）
    QString userFilePath() const;

    /// 诊断信息：已加载的 key 数量
    int size() const;

private:
    Labels();
    void loadDefault();
    void loadUserOverlay();

    QVariantMap m_map;            // 合并后的最终 map（已 flatten 成点号路径）
    QString m_userPath;
};