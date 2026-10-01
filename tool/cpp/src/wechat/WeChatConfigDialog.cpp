#include "WeChatConfigDialog.h"
#include "WeChatAccountManager.h"
#include "WeChatDb.h"
#include "WeChatImageDecoder.h"
#include "WeChatKeyExtractor.h"
#include "WeChatWorker.h"
#include "app/Theme.h"
#include "core/Logger.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QStackedWidget>
#include <QListWidgetItem>
#include <QFile>
#include <QPointer>
#include <QThread>
#include <QToolButton>
#include <thread>
#include <atomic>

using Account = WeChatAccountManager::Account;

// 生成一个小眼睛切换按钮：点击切换 QLineEdit 的 Password/Normal 显示
static QToolButton* makeEyeButton(QLineEdit* edit) {
    auto* eye = new QToolButton(edit);
    eye->setText(QStringLiteral("👁"));
    eye->setToolTip(QStringLiteral("显示/隐藏明文"));
    eye->setCheckable(true);
    eye->setCursor(Qt::PointingHandCursor);
    eye->setFocusPolicy(Qt::NoFocus);
    eye->setFixedSize(34, 28);
    // 独立按钮外观：背景 + 边框，与输入框区分开；
    // 选中（明文显示）时用主题强调色高亮，状态一目了然。
    eye->setStyleSheet(QString(
        "QToolButton{background:%1;border:1px solid %2;border-radius:4px;"
        "font-size:14px;color:%3;}"
        "QToolButton:hover{border-color:%4;color:%5;}"
        "QToolButton:checked{background:%4;border-color:%4;color:%6;}")
        .arg(Theme::kSurfaceAlt, Theme::kBorder, Theme::kMuted,
             Theme::kAccent, Theme::kTextBright, Theme::kTextBright));
    QObject::connect(eye, &QToolButton::toggled, edit, [edit](bool on) {
        edit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    return eye;
}

WeChatConfigDialog::WeChatConfigDialog(QWidget* parent, const QString& editId)
    : QDialog(parent), m_editId(editId) {
    setWindowTitle(editId.isEmpty() ? "添加微信账号" : "编辑微信账号");
    setMinimumWidth(560);
    setStyleSheet(QString("QDialog{background:%1;}").arg(Theme::kBg));
    buildUi();
    if (!editId.isEmpty()) loadAccount();

    // 单次密钥提取专用 worker（避免 extractKey 阻塞 UI）
    m_extractThread = new QThread(this);
    m_extractWorker = new WeChatWorker;
    m_extractWorker->moveToThread(m_extractThread);
    connect(m_extractThread, &QThread::finished,
            m_extractWorker, &QObject::deleteLater);
    connect(m_extractWorker, &WeChatWorker::keyExtracted,
            this, &WeChatConfigDialog::onExtractKeyDone);
    connect(m_extractWorker, &WeChatWorker::imageKeyExtracted,
            this, &WeChatConfigDialog::onExtractImageKeyDone);
    m_extractThread->start();
}

WeChatConfigDialog::~WeChatConfigDialog() {
    // 取消并等待后台扫描结束，避免线程读 m_hintLabel 等已析构对象
    m_scanCancel.store(true);
    cleanupScanThread();

    // 关闭密钥提取线程
    if (m_extractThread) {
        m_extractThread->quit();
        if (!m_extractThread->wait(3000)) {
            Logger::instance().warn("WeChatConfigDialog extract thread didn't exit in 3s",
                                    "wechat");
            m_extractThread->terminate();
            m_extractThread->wait();
        }
    }
}

void WeChatConfigDialog::cleanupScanThread() {
    if (m_scanThread) {
        if (m_scanThread->joinable()) m_scanThread->join();
        delete m_scanThread;
        m_scanThread = nullptr;
    }
}

void WeChatConfigDialog::buildUi() {
    auto* root = new QVBoxLayout(this);

    // ── 扫描区 ──
    auto* scanRow = new QHBoxLayout;
    auto* scanBtn = new QPushButton("扫描本机微信");
    scanBtn->setObjectName("secondaryBtn");
    scanRow->addWidget(scanBtn);
    auto* scanHint = new QLabel("自动查找本机已登录/使用过的微信数据（支持多个账号）");
    scanHint->setStyleSheet(Theme::mutedText());
    scanRow->addWidget(scanHint, 1);
    root->addLayout(scanRow);

    m_scanList = new QListWidget;
    m_scanList->setMaximumHeight(110);
    m_scanList->setVisible(false);
    m_scanList->setStyleSheet(QString(
        "QListWidget{background:%1;border:1px solid %2;border-radius:4px;"
        "color:%3;outline:0;}"
        "QListWidget::item{border-radius:4px;}"
        "QListWidget::item:hover{background:%4;}"
        "QListWidget::item:selected{background:%4;color:%5;}")
        .arg(Theme::kSurfaceAlt, Theme::kBorder, Theme::kText,
             Theme::kBorder, Theme::kTextBright));
    root->addWidget(m_scanList);

    // ── 表单 ──
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    m_nameEdit = new QLineEdit;
    m_nameEdit->setPlaceholderText("账号显示名称（如：我的微信）");
    form->addRow("名称:", m_nameEdit);

    m_wxidEdit = new QLineEdit;
    m_wxidEdit->setPlaceholderText("wxid_xxx（扫描选择后自动填充）");
    form->addRow("微信 ID:", m_wxidEdit);

    auto* dirRow = new QHBoxLayout;
    m_dirEdit = new QLineEdit;
    m_dirEdit->setPlaceholderText("微信数据目录（…\\WeChat Files\\wxid_xxx）");
    dirRow->addWidget(m_dirEdit, 1);
    auto* browse = new QPushButton("浏览…");
    browse->setObjectName("secondaryBtn");
    connect(browse, &QPushButton::clicked, this, &WeChatConfigDialog::browseDataDir);
    dirRow->addWidget(browse);
    form->addRow("数据目录:", dirRow);

    m_versionCombo = new QComboBox;
    m_versionCombo->addItem("微信 3.x", "3.x");
    m_versionCombo->addItem("微信 4.x", "4.x");
    form->addRow("微信版本:", m_versionCombo);

    m_keyEdit = new QLineEdit;
    m_keyEdit->setPlaceholderText("64 位十六进制数据库密钥（登录微信后可自动提取）");
    m_keyEdit->setEchoMode(QLineEdit::Password);
    auto* keyEditRow = new QHBoxLayout;
    keyEditRow->setContentsMargins(0, 0, 0, 0);
    keyEditRow->addWidget(m_keyEdit, 1);
    keyEditRow->addWidget(makeEyeButton(m_keyEdit));
    form->addRow("数据库密钥:", keyEditRow);

    auto* keyRow = new QHBoxLayout;
    m_extractBtn = new QPushButton("从微信自动提取");
    m_extractBtn->setObjectName("secondaryBtn");
    connect(m_extractBtn, &QPushButton::clicked, this, &WeChatConfigDialog::extractKey);
    keyRow->addWidget(m_extractBtn);
    m_testBtn = new QPushButton("测试密钥");
    m_testBtn->setObjectName("secondaryBtn");
    connect(m_testBtn, &QPushButton::clicked, this, &WeChatConfigDialog::testKey);
    keyRow->addWidget(m_testBtn);
    m_hintLabel = new QLabel;
    m_hintLabel->setStyleSheet(Theme::mutedText());
    keyRow->addWidget(m_hintLabel, 1);
    form->addRow("", keyRow);

    // ── V2 图片 AES key ──
    m_imageKeyEdit = new QLineEdit;
    m_imageKeyEdit->setPlaceholderText("32 位十六进制图片密钥（V2 .dat 解密；登录微信后可自动提取）");
    m_imageKeyEdit->setEchoMode(QLineEdit::Password);
    auto* imgKeyEditRow = new QHBoxLayout;
    imgKeyEditRow->setContentsMargins(0, 0, 0, 0);
    imgKeyEditRow->addWidget(m_imageKeyEdit, 1);
    imgKeyEditRow->addWidget(makeEyeButton(m_imageKeyEdit));
    form->addRow("图片密钥:", imgKeyEditRow);

    auto* imgKeyRow = new QHBoxLayout;
    m_extractImageKeyBtn = new QPushButton("从微信自动提取");
    m_extractImageKeyBtn->setObjectName("secondaryBtn");
    connect(m_extractImageKeyBtn, &QPushButton::clicked,
            this, &WeChatConfigDialog::extractImageKey);
    imgKeyRow->addWidget(m_extractImageKeyBtn);
    m_verifyImageKeyBtn = new QPushButton("验证");
    m_verifyImageKeyBtn->setObjectName("secondaryBtn");
    m_verifyImageKeyBtn->setToolTip("用当前图片密钥试解密一张 .dat，验证密钥是否正确");
    connect(m_verifyImageKeyBtn, &QPushButton::clicked,
            this, &WeChatConfigDialog::verifyImageKey);
    imgKeyRow->addWidget(m_verifyImageKeyBtn);
    imgKeyRow->addStretch(1);
    form->addRow("", imgKeyRow);

    // 图片密钥提示（放在图片密钥下方，与数据库密钥提示分开）
    m_imageHintLabel = new QLabel;
    m_imageHintLabel->setStyleSheet(Theme::mutedText());
    m_imageHintLabel->setWordWrap(true);
    form->addRow("", m_imageHintLabel);

    root->addLayout(form, 1);

    auto* tip = new QLabel(
        "提示：微信在本机登录的状态下，密钥可自动从微信进程内存提取；"
        "密钥仅用于本机解密聊天记录，使用 Windows DPAPI 加密保存。");
    tip->setStyleSheet(Theme::mutedText());
    tip->setWordWrap(true);
    root->addWidget(tip);

    // ── 按钮 ──
    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* cancel = new QPushButton("取消");
    cancel->setObjectName("secondaryBtn");
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* save = new QPushButton("保存");
    save->setObjectName("primaryBtn");
    connect(save, &QPushButton::clicked, this, [this]() {
        const QString name = m_nameEdit->text().trimmed();
        const QString wxid = m_wxidEdit->text().trimmed();
        const QString dir  = m_dirEdit->text().trimmed();
        const QString key  = m_keyEdit->text().trimmed();
        const QString ver  = m_versionCombo->currentData().toString();

        if (name.isEmpty() || wxid.isEmpty() || dir.isEmpty()) {
            QMessageBox::warning(this, "微信账号", "请填写名称、微信 ID 与数据目录");
            return;
        }
        if (!QFile::exists(dir)) {
            QMessageBox::warning(this, "微信账号", "数据目录不存在");
            return;
        }
        if (!key.isEmpty() && key.size() != 64) {
            QMessageBox::warning(this, "微信账号",
                                  "密钥格式错误：需要 64 位十六进制字符串");
            return;
        }
        if (m_editId.isEmpty() && key.isEmpty()) {
            QMessageBox::warning(this, "微信账号",
                                  "请填写数据库密钥；微信登录状态下可点「从微信自动提取」");
            return;
        }
        // 编辑模式：密钥已自动带出，若被清空则要求重新填写
        if (!m_editId.isEmpty() && key.isEmpty()) {
            QMessageBox::warning(this, "微信账号",
                                  "数据库密钥为空；可点「从微信自动提取」重新获取");
            return;
        }

        const QString imageKey = m_imageKeyEdit->text().trimmed();
        if (!imageKey.isEmpty() && imageKey.size() != 32) {
            QMessageBox::warning(this, "微信账号",
                                  "图片密钥格式错误：需要 32 位十六进制字符串（16 字节）");
            return;
        }

        QVariantMap data;
        data["name"]    = name;
        data["wxid"]    = wxid;
        data["dataDir"] = dir;
        data["version"] = ver;
        if (!key.isEmpty()) data["keyHex"] = key;
        if (!imageKey.isEmpty()) data["imageKeyHex"] = imageKey;

        auto& mgr = WeChatAccountManager::instance();
        if (m_editId.isEmpty()) {
            auto* added = mgr.add(data);
            if (!added) {
                QMessageBox::warning(this, "微信账号",
                                      QString("微信 ID「%1」已在账号列表中，不能重复添加")
                                          .arg(wxid));
                return;
            }
            Logger::instance().info("已添加微信账号: " + name, "wechat");
        } else {
            mgr.update(m_editId, data);
            Logger::instance().info("已更新微信账号: " + name, "wechat");
        }
        accept();
    });
    btns->addWidget(cancel);
    btns->addWidget(save);
    root->addLayout(btns);

    connect(scanBtn, &QPushButton::clicked, this, &WeChatConfigDialog::scanLocal);
    connect(m_scanList, &QListWidget::currentRowChanged,
            this, &WeChatConfigDialog::onDiscoveredSelected);
}

void WeChatConfigDialog::scanLocal() {
    // 若后台提取正在进行，再次点击即视为「取消并重试」
    if (m_scanThread) {
        m_scanCancel.store(true);
        cleanupScanThread();
        m_scanCancel.store(false);
    }

    const auto found = WeChatAccountManager::instance().discoverLocalAccounts();
    m_scanList->clear();
    m_scanList->setVisible(!found.isEmpty());
    m_scanKeys.clear();
    m_hintLabel->setStyleSheet(Theme::mutedText());
    m_hintLabel->setText(found.isEmpty() ? "未在本机发现微信数据目录" : QString());
    for (const auto& d : found) {
        auto* item = new QListWidgetItem(m_scanList);
        item->setText(QString("%1  （微信 %2）\n%3")
                          .arg(d.wxid, d.version, d.dataDir));
        item->setData(Qt::UserRole, d.wxid);
        item->setData(Qt::UserRole + 1, d.dataDir);
        item->setData(Qt::UserRole + 2, d.version);
        m_scanList->addItem(item);
    }

    // 微信在运行时：后台线程收集候选密钥并逐账号验证（避免阻塞 UI）
    if (found.isEmpty() || WeChatKeyExtractor::findRunningWeChat().isEmpty()) return;
    if (!WeChatKeyExtractor::findRunningWeChat().isEmpty()) {
        m_hintLabel->setText("正在从微信进程提取密钥（后台进行中，可继续操作）…");
        QApplication::setOverrideCursor(Qt::WaitCursor);

        // 在主线程固化 procs/accounts（避免后台线程调 QWidget 不可重入的 API）
        const QList<WeChatKeyExtractor::ProcessInfo> procs =
            WeChatKeyExtractor::findRunningWeChat();
        QList<WeChatAccountManager::DiscoveredAccount> accounts = found;
        QPointer<WeChatConfigDialog> self = this;

        m_scanThread = new std::thread([self, procs, accounts]() {
            QStringList allCandidates;
            for (const auto& p : procs) {
                if (!self || self->m_scanCancel.load()) break;
                QStringList keys = WeChatKeyExtractor::extractAllKeys(p.pid, p.version);
                for (const QString& k : keys)
                    if (!allCandidates.contains(k)) allCandidates.append(k);
            }

            QMap<QString, QString> scanKeys;
            int keyCount = 0;
            const int totalAccounts = accounts.size();
            for (int i = 0; i < totalAccounts; ++i) {
                if (!self || self->m_scanCancel.load()) break;
                const auto& d = accounts[i];
                const QString db = self->verifyDbFor(d.dataDir, d.version);
                if (db.isEmpty()) {
                    QMetaObject::invokeMethod(self, "onScanProgress",
                        Qt::QueuedConnection,
                        Q_ARG(QString, QString("账号 %1/%2：未找到数据库").arg(i + 1).arg(totalAccounts)));
                    continue;
                }
                bool matched = false;
                for (int k = 0; k < allCandidates.size(); ++k) {
                    if (!self || self->m_scanCancel.load()) break;
                    const QString& key = allCandidates[k];
                    if (WeChatDb::verifyKey(key, db)) {
                        scanKeys[d.wxid] = key;
                        ++keyCount;
                        matched = true;
                        break;
                    }
                    if ((k & 7) == 7) {
                        QMetaObject::invokeMethod(self, "onScanProgress",
                            Qt::QueuedConnection,
                            Q_ARG(QString, QString("账号 %1/%2：已验证 %3/%4 个候选…")
                                       .arg(i + 1).arg(totalAccounts).arg(k + 1).arg(allCandidates.size())));
                    }
                }
                QMetaObject::invokeMethod(self, "onScanProgress",
                    Qt::QueuedConnection,
                    Q_ARG(QString, matched
                        ? QString("账号 %1/%2：已匹配 ✓").arg(i + 1).arg(totalAccounts)
                        : QString("账号 %1/%2：未匹配（%3 个候选均失败）")
                              .arg(i + 1).arg(totalAccounts).arg(allCandidates.size())));
            }

            if (!self) return;
            // 把结果传回主线程：scanKeys + keyCount 用单次调用打包
            const int finalCount = keyCount;
            QMetaObject::invokeMethod(self, [self, scanKeys, finalCount]() {
                if (!self) return;
                self->m_scanKeys = scanKeys;
                QApplication::restoreOverrideCursor();
                if (finalCount > 0) {
                    self->m_hintLabel->setText(
                        QString("已自动提取 %1 个账号的密钥，选择账号即可").arg(finalCount));
                    self->m_hintLabel->setStyleSheet(Theme::statusOk());
                } else if (self->m_scanCancel.load()) {
                    self->m_hintLabel->setText("扫描已取消");
                    self->m_hintLabel->setStyleSheet(Theme::statusWarn());
                } else {
                    self->m_hintLabel->setText(
                        "未能提取密钥（当前登录的账号可能不在扫描结果中，或版本结构不兼容）");
                    self->m_hintLabel->setStyleSheet(Theme::statusWarn());
                }
            }, Qt::QueuedConnection);
        });
    }
}

void WeChatConfigDialog::onScanProgress(const QString& msg) {
    m_hintLabel->setText(msg);
    m_hintLabel->setStyleSheet(Theme::mutedText());
}

void WeChatConfigDialog::onScanExtractDone() {
    // 备用入口：若需要在 lambda 之外被调用（当前 lambda 已直接更新 UI，本函数保留）
    cleanupScanThread();
}

void WeChatConfigDialog::onDiscoveredSelected(int row) {
    if (row < 0) return;
    auto* item = m_scanList->item(row);
    const QString wxid = item->data(Qt::UserRole).toString();
    const QString dir  = item->data(Qt::UserRole + 1).toString();
    const QString ver  = item->data(Qt::UserRole + 2).toString();

    m_wxidEdit->setText(wxid);
    m_dirEdit->setText(dir);
    if (m_nameEdit->text().trimmed().isEmpty()) m_nameEdit->setText(wxid);
    const int idx = m_versionCombo->findData(ver);
    if (idx >= 0) m_versionCombo->setCurrentIndex(idx);

    // 自动填充提取到的密钥
    const QString key = m_scanKeys.value(wxid);
    if (!key.isEmpty()) {
        m_keyEdit->setText(key);
        m_hintLabel->setText("已自动填充密钥 ✓");
        m_hintLabel->setStyleSheet(Theme::statusOk());
    } else if (WeChatKeyExtractor::findRunningWeChat().isEmpty()) {
        m_hintLabel->setText("微信未运行，无法自动提取密钥；请登录微信后点「从微信自动提取」");
        m_hintLabel->setStyleSheet(Theme::statusWarn());
    }
}

void WeChatConfigDialog::browseDataDir() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, "选择微信数据目录", m_dirEdit->text().isEmpty()
            ? WeChatAccountManager::defaultWeChatFilesRoot()
            : m_dirEdit->text());
    if (!dir.isEmpty()) m_dirEdit->setText(QDir::toNativeSeparators(dir));
}

void WeChatConfigDialog::testKey() {
    const QString key = m_keyEdit->text().trimmed();
    const QString dir = m_dirEdit->text().trimmed();
    if (key.size() != 64) {
        m_hintLabel->setText("密钥格式错误：需要 64 位十六进制字符串");
        m_hintLabel->setStyleSheet(Theme::statusErr());
        return;
    }
    const QString ver = m_versionCombo->currentData().toString();
    const QString db = verifyDbFor(dir, ver);
    if (db.isEmpty()) {
        m_hintLabel->setText(ver == "4.x"
                                 ? "未找到 db_storage 数据库，请确认数据目录"
                                 : "未找到 Msg/MicroMsg.db，请确认数据目录");
        m_hintLabel->setStyleSheet(Theme::statusErr());
        return;
    }
    QString err;
    if (WeChatDb::verifyKey(key, db, &err)) {
        m_hintLabel->setText("密钥正确 ✓");
        m_hintLabel->setStyleSheet(Theme::statusOk());
    } else {
        m_hintLabel->setText(err);
        m_hintLabel->setStyleSheet(Theme::statusErr());
    }
}

void WeChatConfigDialog::extractKey() {
    const QString dir = m_dirEdit->text().trimmed();
    const QString ver = m_versionCombo->currentData().toString();
    const QString db = verifyDbFor(dir, ver);
    if (db.isEmpty()) {
        m_hintLabel->setText(ver == "4.x"
                                 ? "未找到 db_storage 数据库，请先填写数据目录"
                                 : "未找到 Msg/MicroMsg.db，请先填写数据目录");
        m_hintLabel->setStyleSheet(Theme::statusErr());
        return;
    }
    // 提交后台线程执行（不阻塞 UI）
    m_hintLabel->setStyleSheet(Theme::mutedText());
    m_hintLabel->setText("正在从微信进程提取密钥（后台进行中，可继续操作）…");
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_extractBtn->setEnabled(false);
    QMetaObject::invokeMethod(m_extractWorker, "extractKey",
                              Qt::QueuedConnection,
                              Q_ARG(QString, db));
}

void WeChatConfigDialog::onExtractKeyDone(const QString& key, const QString& err) {
    QApplication::restoreOverrideCursor();
    m_extractBtn->setEnabled(true);
    if (!key.isEmpty()) {
        m_keyEdit->setText(key);
        m_hintLabel->setText("密钥提取成功 ✓");
        m_hintLabel->setStyleSheet(Theme::statusOk());
    } else {
        m_hintLabel->setText(err.isEmpty()
                                 ? QStringLiteral("密钥提取失败（未知错误）")
                                 : err);
        m_hintLabel->setStyleSheet(Theme::statusErr());
    }
}

QString WeChatConfigDialog::verifyDbFor(const QString& dir, const QString& version) {
    if (dir.trimmed().isEmpty()) return QString();
    QString db;
    if (version == "4.x") {
        db = dir + "/db_storage/message/message_0.db";
        if (!QFile::exists(db))
            db = dir + "/db_storage/contact/contact.db";
    } else {
        db = dir + "/Msg/MicroMsg.db";
    }
    return QFile::exists(db) ? db : QString();
}

// 在 dataDir 下找一个非缩略图的 V2 .dat 用于图片 key 提取 oracle。
// 优先选最大的（信息熵高 → 解密命中率更高）。
QString WeChatConfigDialog::pickOracleDat(const QString& dataDir) {
    if (dataDir.isEmpty() || !QFile::exists(dataDir)) return {};
    QDir imgRoot(dataDir + "/msg/attach");
    if (!imgRoot.exists()) return {};
    QFileInfoList allDats;
    // 仅扫 Img 子目录下的 .dat（缩略图通常在 _t.dat）；2 层遍历足够
    const QFileInfoList sessionDirs = imgRoot.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo& session : sessionDirs) {
        QDir monthRoot(session.absoluteFilePath());
        const QFileInfoList months = monthRoot.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
        for (const QFileInfo& m : months) {
            QDir imgDir(m.absoluteFilePath() + "/Img");
            if (!imgDir.exists()) continue;
            const QFileInfoList dats = imgDir.entryInfoList(
                QStringList() << "*.dat", QDir::Files, QDir::Time);
            for (const QFileInfo& d : dats) {
                if (!d.fileName().endsWith("_t.dat")) allDats.append(d);
                if (allDats.size() >= 5000) break;
            }
        }
    }
    if (allDats.isEmpty()) return {};
    // 选最大文件
    std::sort(allDats.begin(), allDats.end(),
              [](const QFileInfo& a, const QFileInfo& b) { return a.size() > b.size(); });
    return allDats.first().absoluteFilePath();
}

void WeChatConfigDialog::extractImageKey() {
    const QString dir = m_dirEdit->text().trimmed();
    if (dir.isEmpty() || !QFile::exists(dir)) {
        m_imageHintLabel->setText("请先填写数据目录");
        m_imageHintLabel->setStyleSheet(Theme::statusWarn());
        return;
    }
    const QString oracle = pickOracleDat(dir);
    if (oracle.isEmpty()) {
        m_imageHintLabel->setText(QStringLiteral("数据目录下未找到 V2 .dat 图片（%1/msg/attach/.../Img/*.dat）")
            .arg(dir));
        m_imageHintLabel->setStyleSheet(Theme::statusWarn());
        return;
    }
    m_imageHintLabel->setStyleSheet(Theme::mutedText());
    m_imageHintLabel->setText(QStringLiteral("正在从微信进程提取图片 AES key（oracle=%1）…").arg(oracle));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_extractImageKeyBtn->setEnabled(false);
    QMetaObject::invokeMethod(m_extractWorker, "extractImageKey",
                              Qt::QueuedConnection,
                              Q_ARG(QString, oracle));
}

void WeChatConfigDialog::onExtractImageKeyDone(const QString& key16Hex, const QString& err) {
    QApplication::restoreOverrideCursor();
    m_extractImageKeyBtn->setEnabled(true);
    if (!key16Hex.isEmpty()) {
        m_imageKeyEdit->setText(key16Hex);
        m_imageHintLabel->setText("图片 key 提取成功 ✓（可点「验证」确认能解开 .dat）");
        m_imageHintLabel->setStyleSheet(Theme::statusOk());
    } else {
        m_imageHintLabel->setText(QStringLiteral("图片 key 提取失败：%1")
            .arg(err.isEmpty() ? QStringLiteral("未知错误") : err));
        m_imageHintLabel->setStyleSheet(Theme::statusErr());
    }
}

void WeChatConfigDialog::verifyImageKey() {
    const QString keyHex = m_imageKeyEdit->text().trimmed();
    if (keyHex.size() != 32) {
        m_imageHintLabel->setText("图片密钥格式错误：需要 32 位十六进制字符串（16 字节）");
        m_imageHintLabel->setStyleSheet(Theme::statusErr());
        return;
    }
    const QString dir = m_dirEdit->text().trimmed();
    const QString oracle = pickOracleDat(dir);
    if (oracle.isEmpty()) {
        m_imageHintLabel->setText("数据目录下未找到 V2 .dat 图片，无法验证");
        m_imageHintLabel->setStyleSheet(Theme::statusWarn());
        return;
    }
    QFile f(oracle);
    if (!f.open(QIODevice::ReadOnly)) {
        m_imageHintLabel->setText("打开 oracle .dat 失败：" + f.errorString());
        m_imageHintLabel->setStyleSheet(Theme::statusErr());
        return;
    }
    const QByteArray dat = f.readAll();
    f.close();
    const QByteArray key16 = QByteArray::fromHex(keyHex.toLatin1());
    QString err;
    WeChatImageDecoder::Format fmt = WeChatImageDecoder::Format::Unknown;
    const QByteArray plain = WeChatImageDecoder::decryptV2(dat, key16, &fmt, &err);
    if (!plain.isEmpty() && fmt != WeChatImageDecoder::Format::Unknown) {
        m_imageHintLabel->setText(
            QStringLiteral("图片密钥验证通过 ✓（解出 %1，%2 字节）")
                .arg(WeChatImageDecoder::formatExtension(fmt))
                .arg(plain.size()));
        m_imageHintLabel->setStyleSheet(Theme::statusOk());
    } else {
        m_imageHintLabel->setText(
            QStringLiteral("图片密钥验证失败：%1")
                .arg(err.isEmpty() ? QStringLiteral("解密结果不是有效图片") : err));
        m_imageHintLabel->setStyleSheet(Theme::statusErr());
    }
}

void WeChatConfigDialog::loadAccount() {
    auto* acc = WeChatAccountManager::instance().getById(m_editId);
    if (!acc) return;
    m_nameEdit->setText(acc->name);
    m_wxidEdit->setText(acc->wxid);
    m_dirEdit->setText(acc->dataDir);
    const int idx = m_versionCombo->findData(acc->version);
    if (idx >= 0) m_versionCombo->setCurrentIndex(idx);
    // 编辑模式：带出已保存的密钥（DPAPI 解密后的明文 hex）。
    // 输入框为 Password 模式（默认掩码），点小眼睛可查看明文。
    const QString plainKey = WeChatAccountManager::instance().keyForAccount(*acc);
    m_keyEdit->setText(plainKey);
    m_keyEdit->setPlaceholderText("64 位十六进制数据库密钥");
    m_imageKeyEdit->setText(acc->imageKeyHex);
}
