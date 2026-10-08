#pragma once

#include <QDialog>
#include <QMap>
#include <atomic>
#include <thread>

class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QListWidget;
class QStackedWidget;
class QThread;
class WeChatWorker;

/**
 * WeChatConfigDialog: 微信账号配置对话框
 *
 * 流程：
 *   1. 「扫描本机微信」→ 列出发现的微信账号（支持多账号，逐个添加）
 *      微信在运行时自动从进程内存提取密钥并填充
 *   2. 选择账号 → 自动填充 wxid / 数据目录 / 版本 / 密钥
 *   3. 「测试密钥」→ 校验密钥能否解开数据库
 *   4. 保存
 */
class WeChatConfigDialog : public QDialog {
    Q_OBJECT

public:
    // editId 非空时为编辑模式
    explicit WeChatConfigDialog(QWidget* parent = nullptr,
                                const QString& editId = QString());
    ~WeChatConfigDialog() override;

private slots:
    void scanLocal();
    void onNameComboChanged(int index);
    void browseDataDir();
    void testKey();
    void extractKey();
    void extractImageKey();
    void verifyImageKey();
    void onScanProgress(const QString& msg);
    void onScanExtractDone();
    void onExtractKeyDone(const QString& key, const QString& err);
    void onExtractImageKeyDone(const QString& key16Hex, const QString& err);

private:
    void buildUi();
    void loadAccount();
    // 数据目录对应的验证数据库（4.x: message_0.db/contact.db；3.x: Msg/MicroMsg.db）
    static QString verifyDbFor(const QString& dir, const QString& version);
    // 找一个 .dat 文件用于图片 key 提取 oracle（优先用本账号最早的非缩略图 .dat）
    static QString pickOracleDat(const QString& dataDir);

    QString m_editId;
    QComboBox* m_nameCombo = nullptr;     // 名称（昵称 / 自定义）下拉选择
    QLineEdit* m_wxidEdit = nullptr;
    QLineEdit* m_dirEdit = nullptr;
    QLineEdit* m_keyEdit = nullptr;
    QLineEdit* m_imageKeyEdit = nullptr;
    QComboBox* m_versionCombo = nullptr;
    QMap<QString, QString> m_scanKeys;  // 扫描提取的密钥：wxid → keyHex
    QMap<QString, QString> m_nameByWxid;  // 扫描结果：wxid → 显示名（昵称/wxid）
    QLabel* m_hintLabel = nullptr;
    QLabel* m_imageHintLabel = nullptr;  // 图片密钥提示（放在图片密钥输入框下方）
    QPushButton* m_testBtn = nullptr;
    QPushButton* m_extractBtn = nullptr;
    QPushButton* m_extractImageKeyBtn = nullptr;
    QPushButton* m_verifyImageKeyBtn = nullptr;

    // 后台密钥提取（避免 50+ 候选 × 多账号 verifyKey 在主线程阻塞 UI）
    std::atomic<bool> m_scanCancel{false};
    std::thread* m_scanThread = nullptr;

    void cleanupScanThread();      // join + delete（仅主线程调用）

    // 单次密钥提取（点「从微信自动提取」按钮时）走专用 worker 线程，
    // 避免扫描进程内存时阻塞 UI
    QThread*      m_extractThread = nullptr;
    WeChatWorker* m_extractWorker = nullptr;
};
