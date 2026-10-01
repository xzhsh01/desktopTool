#include "wechat/ui/WeChatDetailPanel.h"
#include "app/Theme.h"
#include "wechat/WeChatImageDecoder.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
const QString kBubbleSelf  = "#95EC69";   // 微信绿
const QString kBubbleOther = Theme::kSurfaceAlt;

// 单张图片在气泡内的最大边长（宽高都限）
constexpr int kImageBubbleMaxSide = 240;
} // namespace

// ── 图片解密上下文 ──────────────────────────────────────────────────────────

QString WeChatDetailPanel::resolveDatPath(const QString& talker, const QDateTime& msgTime,
                                          const QString& md5, const QString& sub) const {
    if (m_dataDir.isEmpty() || md5.isEmpty() || !msgTime.isValid()) return {};
    const QString talkerMd5 = QString::fromLatin1(
        QCryptographicHash::hash(talker.toUtf8(), QCryptographicHash::Md5).toHex());
    const QString base = m_dataDir + "/msg/attach/" + talkerMd5 + "/"
                       + msgTime.toString(QStringLiteral("yyyy-MM")) + "/" + sub + "/";
    QString p = base + md5 + ".dat";
    if (QFileInfo::exists(p)) return p;
    // 也尝试缩略图（_t.dat）作为兜底
    p = base + md5 + "_t.dat";
    if (QFileInfo::exists(p)) return p;
    return base + md5 + ".dat";
}

void WeChatDetailPanel::setImageContext(const QString& dataDir, QByteArray imageKey) {
    m_dataDir = dataDir;
    m_imageKey = std::move(imageKey);
}

void WeChatDetailPanel::setImageKey(const QByteArray& key) {
    m_imageKey = key;
}

QByteArray WeChatDetailPanel::decryptAttachImage(const QString& talker,
                                                 const QDateTime& msgTime,
                                                 const QString& md5,
                                                 QString* outPath, QString* outErr) {
    auto err = [&](const QString& s) {
        if (outErr) *outErr = s;
        return QByteArray();
    };
    if (m_dataDir.isEmpty()) return err(QStringLiteral("no dataDir"));
    if (m_imageKey.size() != 16)
        return err(QStringLiteral("image key not set (size=%1)").arg(m_imageKey.size()));
    if (md5.isEmpty() || talker.isEmpty() || !msgTime.isValid())
        return err(QStringLiteral("missing md5/talker/time"));
    const QString datPath = resolveDatPath(talker, msgTime, md5, QStringLiteral("Img"));
    if (outPath) *outPath = datPath;
    if (datPath.isEmpty() || !QFile::exists(datPath))
        return err(QStringLiteral("dat not found: %1").arg(datPath));
    QFile f(datPath);
    if (!f.open(QIODevice::ReadOnly))
        return err(QStringLiteral("open dat failed: %1").arg(f.errorString()));
    const QByteArray dat = f.readAll();
    f.close();
    QByteArray plain = WeChatImageDecoder::decryptV2(dat, m_imageKey);
    if (plain.isEmpty()) return err(QStringLiteral("decryptV2 returned empty"));
    return plain;
}

WeChatDetailPanel::WeChatDetailPanel(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(makeDetailPanel(), 1);
}

QWidget* WeChatDetailPanel::makeDetailPanel() {
    m_detail = new QStackedWidget;

    // ── emptyPage ──
    m_emptyPage = new QWidget;
    m_emptyPage->setStyleSheet(QString("background:%1;").arg(Theme::kBg));
    auto* emptyLay = new QVBoxLayout(m_emptyPage);
    emptyLay->setAlignment(Qt::AlignCenter);
    m_emptyHint = new QLabel("请在左侧选择会话或联系人");
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setStyleSheet(QString("color:%1; font-size:14px;").arg(Theme::kFaint));
    emptyLay->addWidget(m_emptyHint);

    // ── chatPage：标题 + 消息流 ──
    m_chatPage = new QWidget;
    m_chatPage->setStyleSheet(QString("background:%1;").arg(Theme::kBg));
    auto* chatLay = new QVBoxLayout(m_chatPage);
    chatLay->setContentsMargins(0, 0, 0, 0);
    chatLay->setSpacing(0);

    auto* header = new QWidget;
    header->setFixedHeight(48);
    header->setStyleSheet(QString("background:%1;border-bottom:1px solid %2;")
                              .arg(Theme::kBg, Theme::kBorder));
    auto* headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(16, 0, 16, 0);
    m_chatTitle = new QLabel;
    m_chatTitle->setStyleSheet(QString("font-size:14px; font-weight:600; color:%1;")
                                   .arg(Theme::kTextBright));
    headerLay->addWidget(m_chatTitle);
    headerLay->addStretch(1);
    chatLay->addWidget(header);

    m_msgScroll = new QScrollArea;
    m_msgScroll->setWidgetResizable(true);
    m_msgScroll->setStyleSheet(QString(
        "QScrollArea{background:%1;border:none;}"
        "QScrollBar:vertical{background:%1;width:8px;margin:0;}"
        "QScrollBar::handle:vertical{background:%2;border-radius:4px;min-height:24px;}"
        "QScrollBar::handle:vertical:hover{background:%3;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}")
        .arg(Theme::kBg, Theme::kBorder, Theme::kBorderHover));
    m_msgContainer = new QWidget;
    m_msgContainer->setStyleSheet(QString("background:%1;").arg(Theme::kBg));
    m_msgLayout = new QVBoxLayout(m_msgContainer);
    m_msgLayout->setContentsMargins(16, 12, 16, 12);
    m_msgLayout->setSpacing(10);
    m_msgLayout->addStretch(1);   // 始终把消息顶到下面
    m_msgScroll->setWidget(m_msgContainer);
    chatLay->addWidget(m_msgScroll, 1);

    // ── contactPage：联系人 / 群详情 ──
    m_contactPage = new QWidget;
    m_contactPage->setStyleSheet(QString("background:%1;").arg(Theme::kBg));
    auto* contactLay = new QVBoxLayout(m_contactPage);
    contactLay->setContentsMargins(0, 0, 0, 0);
    contactLay->setSpacing(0);
    auto* cHeader = new QWidget;
    cHeader->setObjectName("contactHeader");
    cHeader->setFixedHeight(48);
    cHeader->setStyleSheet(QString("background:%1;border-bottom:1px solid %2;")
                               .arg(Theme::kBg, Theme::kBorder));
    auto* cHeaderLay = new QHBoxLayout(cHeader);
    cHeaderLay->setContentsMargins(16, 0, 16, 0);
    auto* cTitle = new QLabel("联系人详情");
    cTitle->setStyleSheet(QString("font-size:14px; font-weight:600; color:%1;")
                              .arg(Theme::kTextBright));
    cHeaderLay->addWidget(cTitle);
    cHeaderLay->addStretch(1);
    contactLay->addWidget(cHeader);
    contactLay->addStretch(1);   // showContact() 时再把内容卡片 add 进来

    m_detail->addWidget(m_emptyPage);
    m_detail->addWidget(m_chatPage);
    m_detail->addWidget(m_contactPage);
    return m_detail;
}

QLabel* WeChatDetailPanel::makeAvatar(const QString& name, const QString& key, int size) {
    auto* al = new QLabel;
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    quint32 h = 0;
    for (const QChar c : key) h = h * 131 + c.unicode();
    const QColor colors[] = {
        QColor("#5B8DEF"), QColor("#27AE60"), QColor("#E67E22"), QColor("#9B59B6"),
        QColor("#16A085"), QColor("#C0392B"), QColor("#2980B9"), QColor("#D4A017"),
    };
    p.setBrush(colors[h % 8]);
    p.setPen(Qt::NoPen);
    p.drawEllipse(0, 0, size, size);

    p.setPen(Qt::white);
    p.setFont(QFont("Microsoft YaHei", size / 3, QFont::Bold));
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter,
               name.isEmpty() ? QStringLiteral("?") : QString(name.at(0).toUpper()));
    al->setPixmap(pm);
    return al;
}

void WeChatDetailPanel::showEmpty(const QString& hint) {
    m_detail->setCurrentWidget(m_emptyPage);
    if (!hint.isEmpty()) m_emptyHint->setText(hint);
    m_currentShownContact.clear();                          // 切走：清掉联系人体
}

void WeChatDetailPanel::showChatHeader(const QString& title) {
    m_chatTitle->setText(title);
    m_detail->setCurrentWidget(m_chatPage);
    m_currentShownContact.clear();                          // 切到聊天页：清掉联系人体
}

void WeChatDetailPanel::renderMessages(const QList<QVariantMap>& msgs,
                                       const QString& currentTalker) {
    // 短路1：同 talker + 同消息数 + 已渲染过 → 不重建气泡
    if (m_currentTalker == currentTalker &&
        m_renderedMsgCount == msgs.size() &&
        m_msgLayout->count() > 1) {
        return;
    }

    // 短路2：同 talker + 消息数增加 + 已渲染过 → 增量追加（关键卡顿优化点）
    // watcher 持续写入 / 新消息到来 → 只 append N 个新气泡，不清空重建 1000+ 旧气泡
    if (m_currentTalker == currentTalker &&
        msgs.size() > m_renderedMsgCount &&
        m_renderedMsgCount > 0 &&
        m_msgLayout->count() > 1) {
        const int startIdx = m_renderedMsgCount;
        for (int i = startIdx; i < msgs.size(); ++i) {
            m_msgLayout->insertWidget(m_msgLayout->count() - 1, makeBubble(msgs[i]));
        }
        m_renderedMsgCount = msgs.size();
        QScrollBar* sb = m_msgScroll->verticalScrollBar();
        sb->setValue(sb->maximum());
        return;
    }

    // 全量重建（talker 切换 / 消息减少 / 首次渲染）
    m_currentTalker = currentTalker;
    m_renderedMsgCount = msgs.size();
    m_chunkAppendTalker = currentTalker;   // 标记分块追加目标

    // 清掉旧消息（保留末位的 stretch）
    while (m_msgLayout->count() > 1) {
        auto* it = m_msgLayout->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }

    // 大消息列表分块渲染：先插入首批 200 条气泡，滚到底部/再加载更多
    // 避免一次性插入 5000+ QWidget 导致 UI 卡顿
    constexpr int kFirstChunk = 200;
    const int total = msgs.size();
    int inserted = 0;
    QDateTime lastDate;
    auto insertChunk = [&](int from, int to) {
        for (int i = from; i < to; ++i) {
            const auto& m = msgs[i];
            // time 是 qint64 (秒), 直接构造, 不再 QString → ISODate → QDateTime 解析
            const qint64 ts = m["time"].toLongLong();
            const QDateTime t = ts > 0 ? QDateTime::fromSecsSinceEpoch(ts) : QDateTime();
            if (!t.isValid() || t.date() != lastDate.date()) {
                m_msgLayout->insertWidget(m_msgLayout->count() - 1, makeDateSeparator(t));
                lastDate = t;
            }
            m_msgLayout->insertWidget(m_msgLayout->count() - 1, makeBubble(m));
        }
    };

    if (total <= kFirstChunk * 2) {
        // 小列表：一次插入
        insertChunk(0, total);
    } else {
        // 大列表：先插入首批，后续异步追加
        insertChunk(0, kFirstChunk);
        inserted = kFirstChunk;
        QPointer<WeChatDetailPanel> self = this;
        // msgs 按值捕获（按引用会随 onListOpenChat 返回而失效）
        auto appendMore = [self, msgsList = msgs, inserted, lastDate]() mutable {
            if (!self) return;
            constexpr int kStep = 200;
            int pos = inserted;
            // 重新计算 lastDate (来自最后一个已渲染的 bubble): 用 msgsList[pos-1] 的时间
            QDateTime ld;
            if (pos > 0) {
                const qint64 ts = msgsList[pos - 1]["time"].toLongLong();
                ld = ts > 0 ? QDateTime::fromSecsSinceEpoch(ts) : QDateTime();
            }
            while (pos < msgsList.size()) {
                const int end = qMin(pos + kStep, msgsList.size());
                for (int i = pos; i < end; ++i) {
                    const auto& m = msgsList[i];
                    const qint64 ts = m["time"].toLongLong();
                    const QDateTime t = ts > 0 ? QDateTime::fromSecsSinceEpoch(ts) : QDateTime();
                    if (!t.isValid() || t.date() != ld.date()) {
                        self->m_msgLayout->insertWidget(
                            self->m_msgLayout->count() - 1, self->makeDateSeparator(t));
                        ld = t;
                    }
                    self->m_msgLayout->insertWidget(
                        self->m_msgLayout->count() - 1, self->makeBubble(m));
                }
                pos = end;
                // 给 Qt 事件循环机会刷新 UI（不要一口气塞完 5000 个）
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);
            }
            Q_UNUSED(lastDate);
        };
        // 0 延迟：在本函数 return 后, Qt 才会进入事件循环, 此时下一帧才会真正绘制首批
        // 首批完成后, 通过 queued 单次定时器追加剩余部分
        // 用 weak 守卫：用户已切换 talker → 取消后续追加（避免脏气泡注入）
        const QString guardTalker = currentTalker;
        auto guarded = [self, guardTalker, appendMore]() mutable {
            if (!self) return;
            if (self->m_currentTalker != guardTalker ||
                self->m_chunkAppendTalker != guardTalker) return;
            appendMore();
        };
        QTimer::singleShot(0, this, guarded);
    }

    // 滚到底部
    QScrollBar* sb = m_msgScroll->verticalScrollBar();
    sb->setValue(sb->maximum());
}

QWidget* WeChatDetailPanel::makeDateSeparator(const QDateTime& t) {
    auto* wrap = new QWidget;
    wrap->setStyleSheet("background:transparent;");
    auto* lay = new QHBoxLayout(wrap);
    lay->setContentsMargins(0, 4, 0, 4);
    auto* lbl = new QLabel(t.isValid() ? t.toString("yyyy-MM-dd HH:mm") : QString());
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setStyleSheet(QString("color:%1; background:%2; font-size:11px;"
                               "border-radius:3px; padding:2px 8px;")
                           .arg(Theme::kFaint, Theme::kBorder));
    lay->addStretch(1);
    lay->addWidget(lbl);
    lay->addStretch(1);
    return wrap;
}

// 把字节数格式化为 "1.2 MB" / "356 KB"
static QString fmtSize(qint64 bytes) {
    if (bytes <= 0) return {};
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QString("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 2);
    return QString("%1 GB").arg(bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 2);
}

// 根据 type/subType/attachExt 给出图标字符 + 类型标签
static QString attachIcon(const QString& ext) {
    const QString e = ext.toLower();
    if (e == "pdf") return "📕";
    if (e == "doc" || e == "docx") return "📘";
    if (e == "xls" || e == "xlsx") return "📗";
    if (e == "ppt" || e == "pptx") return "📙";
    if (e == "zip" || e == "rar" || e == "7z" || e == "tar" || e == "gz") return "🗜";
    if (e == "mp3" || e == "wav" || e == "aac" || e == "flac") return "🎵";
    if (e == "mp4" || e == "avi" || e == "mov" || e == "mkv") return "🎬";
    if (e == "image" || e == "png" || e == "jpg" || e == "jpeg" ||
        e == "gif" || e == "bmp" || e == "webp") return "🖼";
    if (e == "voice") return "🎤";
    if (e == "video") return "🎬";
    if (e == "gif") return "🎞";
    if (e == "loc" || e == "location") return "📍";
    return "📎";
}

static QString attachKindLabel(int type, int subType) {
    // 优先用 attachMime（appmsg/type），退而求其次按 type
    if (type == 49) {
        switch (subType) {
        case 4: return "文件";
        case 5: return "链接";
        case 6: return "音乐";
        case 8: return "名片";
        case 19: return "聊天记录";
        case 33:
        case 36: return "小程序";
        case 57: return "引用";
        case 63: return "视频号";
        case 87: return "表情";
        case 88: return "公众号文章";
        case 2000: return "转账";
        case 2003: return "礼物";
        default: return "链接消息";
        }
    }
    switch (type) {
    case 3:   return "图片";
    case 34:  return "语音";
    case 43:  return "视频";
    case 47:  return "动画表情";
    case 48:  return "位置";
    case 49:  return "链接消息";
    case 50:  return "通话";
    default:  return QString("类型%1").arg(type);
    }
}

QWidget* WeChatDetailPanel::makeBubble(const QVariantMap& m) {
    const bool self = m["isSender"].toBool();
    const bool isRoom = m_currentTalker.endsWith("@chatroom");
    const QString senderName = m["senderName"].toString();
    const QString senderId = m["senderId"].toString().isEmpty()
                                 ? m_currentTalker : m["senderId"].toString();

    auto* row = new QWidget;
    row->setStyleSheet("background:transparent;");
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);

    auto* avatar = makeAvatar(
        self ? "我" : (senderName.isEmpty() ? "?" : senderName),
        self ? "self" : senderId, 36);

    auto* colWrap = new QVBoxLayout;
    colWrap->setSpacing(4);
    if (isRoom && !self && !senderName.isEmpty()) {
        auto* name = new QLabel(senderName);
        name->setStyleSheet(QString("color:%1; font-size:11px;").arg(Theme::kMuted));
        colWrap->addWidget(name, 0, self ? Qt::AlignRight : Qt::AlignLeft);
    }

    const int type = m["type"].toInt();
    const int subType = m["subType"].toInt();
    const bool isSystem = (type == 10000 || type == 10002);

    // ── 附件 / 媒体消息：渲染卡片 ──
    // 条件：不是系统消息 + (有 attachTitle / attachUrl / 媒体 type)
    const QString attachTitle = m["attachTitle"].toString();
    const qint64  attachSize  = m["attachSize"].toLongLong();
    const QString attachExt   = m["attachExt"].toString();
    const QString attachUrl   = m["attachUrl"].toString();
    const bool isMedia = (type == 3 || type == 34 || type == 43 || type == 47 || type == 48);
    const bool isAttach = (type == 49) || isMedia;
    const bool isLink   = (type == 49 && subType == 5);

    if (!isSystem && isAttach) {
        // ── 图片 / GIF：尝试就地解码显示 ──
        // 条件：type 3 或 47、attachMd5 非空、image 上下文就绪（db + 16 字节 key）
        const QString attachMd5 = m["attachMd5"].toString();
        const qint64  msgTimeTs  = m["time"].toLongLong();
        const QDateTime msgDt = msgTimeTs > 0 ? QDateTime::fromSecsSinceEpoch(msgTimeTs) : QDateTime();
        const bool isImage = (type == 3 || type == 47);
        bool imageDecoded = false;
        if (isImage && !attachMd5.isEmpty() && hasImageContext() && msgDt.isValid()) {
            QString err;
            QString datPath;
            QByteArray plain = decryptAttachImage(m_currentTalker, msgDt, attachMd5, &datPath, &err);
            if (!plain.isEmpty()) {
                QImage img;
                if (img.loadFromData(plain)) {
                    // 等比缩放到 maxSide
                    QPixmap pm = QPixmap::fromImage(img);
                    if (pm.width() > kImageBubbleMaxSide || pm.height() > kImageBubbleMaxSide) {
                        pm = pm.scaled(kImageBubbleMaxSide, kImageBubbleMaxSide,
                                       Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                    auto* card = new QWidget;
                    card->setObjectName("imageCard");
                    card->setStyleSheet(QString(
                        "QWidget#imageCard{background:%1;border-radius:6px;padding:0;}"
                        "QWidget#imageCard QLabel{background:transparent;}")
                        .arg(self ? kBubbleSelf : kBubbleOther));
                    auto* cl = new QVBoxLayout(card);
                    cl->setContentsMargins(4, 4, 4, 4);
                    cl->setSpacing(2);
                    auto* pic = new QLabel;
                    pic->setPixmap(pm);
                    pic->setStyleSheet("background:transparent;");
                    pic->setToolTip(QStringLiteral("点击查看大图（%1）").arg(attachMd5));
                    cl->addWidget(pic);
                    // 下方加 type 标签 + 大小
                    auto* metaRow = new QHBoxLayout;
                    metaRow->setSpacing(6);
                    metaRow->setContentsMargins(0,0,0,0);
                    auto* kind = new QLabel(attachKindLabel(type, subType));
                    kind->setStyleSheet(QString("color:%1;font-size:11px;font-weight:600;"
                                                "background:transparent;")
                                        .arg(self ? "#2A6B1F" : Theme::kMuted));
                    metaRow->addWidget(kind);
                    metaRow->addStretch(1);
                    if (attachSize > 0) {
                        auto* sizeLbl = new QLabel(fmtSize(attachSize));
                        sizeLbl->setStyleSheet(QString("color:%1;font-size:11px;"
                                                      "background:transparent;")
                                               .arg(self ? "#2A6B1F" : Theme::kFaint));
                        metaRow->addWidget(sizeLbl);
                    }
                    cl->addLayout(metaRow);
                    // 保存原始字节到 widget 属性（供点击放大用）
                    card->setProperty("imgBytes", plain);
                    card->setProperty("imgMd5",   attachMd5);
                    card->setProperty("datPath",  datPath);
                    card->setCursor(Qt::PointingHandCursor);
                    // 事件过滤：本 panel 已重写 eventFilter 转发
                    card->installEventFilter(this);
                    pic->installEventFilter(this);
                    colWrap->addWidget(card, 0, self ? Qt::AlignRight : Qt::AlignLeft);
                    imageDecoded = true;
                }
            }
        }

        // 没解码出图片 → 原附件卡片
        if (!imageDecoded) {
        // 卡片
        auto* card = new QWidget;
        card->setObjectName("attachCard");
        card->setStyleSheet(QString(
            "QWidget#attachCard{background:%1;border-radius:6px;padding:0;}"
            "QWidget#attachCard QLabel{background:transparent;}")
            .arg(self ? kBubbleSelf : kBubbleOther));

        auto* cl = new QVBoxLayout(card);
        cl->setContentsMargins(10, 8, 10, 8);
        cl->setSpacing(2);

        // 顶部：图标 + 类型标签 + 大小
        auto* topRow = new QHBoxLayout;
        topRow->setSpacing(6);
        topRow->setContentsMargins(0, 0, 0, 0);

        auto* iconLbl = new QLabel(attachIcon(isMedia ? attachExt : attachExt));
        iconLbl->setStyleSheet("font-size:20px;background:transparent;");
        topRow->addWidget(iconLbl);

        auto* kind = new QLabel(attachKindLabel(type, subType));
        kind->setStyleSheet(QString("color:%1;font-size:11px;font-weight:600;"
                                    "background:transparent;")
                            .arg(self ? "#2A6B1F" : Theme::kMuted));
        topRow->addWidget(kind);

        topRow->addStretch(1);

        if (attachSize > 0) {
            auto* sizeLbl = new QLabel(fmtSize(attachSize));
            sizeLbl->setStyleSheet(QString("color:%1;font-size:11px;"
                                          "background:transparent;")
                                   .arg(self ? "#2A6B1F" : Theme::kFaint));
            topRow->addWidget(sizeLbl);
        }
        cl->addLayout(topRow);

        // 主体：文件名 / 标题（可点击）
        const QString titleText = attachTitle.isEmpty()
                                      ? attachKindLabel(type, subType)
                                      : attachTitle;
        auto* titleLbl = new QLabel(titleText);
        titleLbl->setWordWrap(true);
        titleLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        titleLbl->setStyleSheet(QString("color:%1;font-size:13px;font-weight:500;"
                                        "background:transparent;margin-top:2px;")
                                .arg(self ? "#000000" : Theme::kTextBright));
        titleLbl->setMaximumWidth(360);
        cl->addWidget(titleLbl);

        // 链接：附 URL
        if (isLink && !attachUrl.isEmpty()) {
            auto* urlLbl = new QLabel(attachUrl);
            urlLbl->setWordWrap(true);
            urlLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            urlLbl->setStyleSheet(QString("color:%1;font-size:11px;"
                                          "background:transparent;margin-top:2px;")
                                  .arg(self ? "#2A6B1F" : Theme::kFaint));
            urlLbl->setMaximumWidth(360);
            urlLbl->setProperty("urlToOpen", attachUrl);
            cl->addWidget(urlLbl);
        }

        // 小程序 / 视频号：显示 appid
        if ((subType == 33 || subType == 36 || subType == 63) && !attachExt.isEmpty()) {
            auto* appid = new QLabel(QString("ID: %1").arg(attachExt));
            appid->setStyleSheet(QString("color:%1;font-size:10px;"
                                        "background:transparent;margin-top:2px;")
                                  .arg(Theme::kFaint));
            cl->addWidget(appid);
        }

        colWrap->addWidget(card, 0, self ? Qt::AlignRight : Qt::AlignLeft);
        } // imageDecoded
    } else {
        // ── 文本 / 系统：普通气泡 ──
        QString text = m["display"].toString();
        if (text.isEmpty()) text = m["content"].toString();
        text = text.toHtmlEscaped().replace("\n", "<br>");

        auto* bubble = new QLabel(text);
        bubble->setWordWrap(true);
        bubble->setTextFormat(Qt::RichText);
        bubble->setMaximumWidth(460);
        bubble->setStyleSheet(QString(
            "QLabel{background:%1;color:%2;border-radius:6px;padding:8px 12px;"
            "font-size:13px;line-height:1.4;}")
            .arg(isSystem ? Theme::kBorder : (self ? kBubbleSelf : kBubbleOther),
                 isSystem ? Theme::kMuted : (self ? "#000000" : Theme::kText)));
        colWrap->addWidget(bubble, 0, self ? Qt::AlignRight : Qt::AlignLeft);
    }

    if (isSystem) {
        auto* wrap = new QWidget;
        wrap->setStyleSheet("background:transparent;");
        auto* wlay = new QHBoxLayout(wrap);
        wlay->setContentsMargins(0, 0, 0, 0);
        wlay->addStretch(1);
        wlay->addLayout(colWrap);
        wlay->addStretch(1);
        return wrap;
    }

    if (self) {
        lay->addStretch(1);
        lay->addLayout(colWrap);
        lay->addWidget(avatar);
    } else {
        lay->addWidget(avatar);
        lay->addLayout(colWrap);
        lay->addStretch(1);
    }
    return row;
}

void WeChatDetailPanel::showContact(const QVariantMap& c) {
    const QString wxid = c["userName"].toString();

    // 短路：已经渲染过这个联系人 → 仅切回 contactPage（不重渲染）
    // 这样：从聊天切回联系人 / 重复点击同一联系人 / 双击都不重建卡片
    if (m_currentShownContact == wxid) {
        m_detail->setCurrentWidget(m_contactPage);
        return;
    }

    // 清掉 contactPage 除 header 之外的所有 widget（用 takeAt + objectName 识别 header）
    QLayout* old = m_contactPage->layout();
    QList<QWidget*> toDelete;
    for (int i = old->count() - 1; i >= 0; --i) {
        auto* it = old->itemAt(i);
        if (!it) continue;
        QWidget* w = it->widget();
        if (!w) continue;                                     // stretch / 子布局
        if (w->objectName() == "contactHeader") continue;     // 保留 header
        toDelete.append(w);
    }
    for (auto* w : toDelete) {
        old->removeWidget(w);
        w->deleteLater();
    }

    auto* card = new QWidget;
    card->setStyleSheet(QString("background:%1;").arg(Theme::kBg));
    auto* cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(0, 8, 0, 0);
    cardLay->setSpacing(8);
    cardLay->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    auto* avatar = makeAvatar(
        c["display"].toString(), c["userName"].toString(), 80);
    avatar->setAlignment(Qt::AlignCenter);
    cardLay->addWidget(avatar, 0, Qt::AlignHCenter);

    auto* name = new QLabel(c["display"].toString());
    name->setAlignment(Qt::AlignCenter);
    name->setStyleSheet(QString("color:%1; font-size:18px; font-weight:600;")
                            .arg(Theme::kTextBright));
    cardLay->addWidget(name, 0, Qt::AlignHCenter);

    auto* form = new QVBoxLayout;
    form->setSpacing(4);
    form->setContentsMargins(40, 4, 40, 0);
    auto addField = [&](const QString& label, const QString& val) {
        auto* row = new QWidget;
        row->setStyleSheet("background:transparent;");
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(12);
        auto* l = new QLabel(label);
        l->setFixedWidth(72);
        l->setStyleSheet(QString("color:%1; font-size:13px;").arg(Theme::kMuted));
        rl->addWidget(l);
        auto* v = new QLabel(val.isEmpty() ? "—" : val);
        v->setStyleSheet(QString("color:%1; font-size:13px;").arg(Theme::kText));
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        rl->addWidget(v, 1);
        form->addWidget(row);
    };
    addField("微信 ID", c["userName"].toString());
    if (!c["nickname"].toString().isEmpty())
        addField("昵称", c["nickname"].toString());
    if (!c["remark"].toString().isEmpty())
        addField("备注", c["remark"].toString());
    if (!c["alias"].toString().isEmpty())
        addField("微信号", c["alias"].toString());

    // 异步详细字段（来自 contactDetailReady；showContact 时缓存可能没有这些字段）
    if (!c["signature"].toString().isEmpty())
        addField("签名", c["signature"].toString());
    const QString region = QString("%1 %2 %3")
        .arg(c["country"].toString(),
             c["province"].toString(),
             c["city"].toString()).trimmed();
    if (!region.isEmpty() && region != "  ")
        addField("地区", region);
    const int sex = c["sex"].toInt();
    if (sex == 1)      addField("性别", "男");
    else if (sex == 2) addField("性别", "女");

    if (c["isRoom"].toBool()) {
        addField("类型", "群聊");
    }
    cardLay->addLayout(form);

    // 占位 stretch（让卡片置顶）
    auto* bodyWrap = new QWidget;
    bodyWrap->setStyleSheet(QString("background:%1;").arg(Theme::kBg));
    auto* bodyLay = new QVBoxLayout(bodyWrap);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(0);
    bodyLay->addWidget(card, 0, Qt::AlignHCenter | Qt::AlignTop);
    bodyLay->addStretch(1);              // 强制顶部对齐：把多余空间推到下方

    auto* oldLayout = m_contactPage->layout();
    oldLayout->addWidget(bodyWrap);     // 默认 stretch=0，不拉伸 bodyWrap 自身
    m_detail->setCurrentWidget(m_contactPage);
    m_currentShownContact = wxid;                            // 记录：下次同 wxid 直接短路
}

// 异步详细信息到达 → 更新当前联系人详情（如果还在显示同一联系人）
void WeChatDetailPanel::updateContactDetail(const QVariantMap& detail) {
    const QString wxid = detail["userName"].toString();
    if (wxid.isEmpty()) return;
    if (m_currentShownContact != wxid) return;         // 用户已经切到其他联系人
    if (m_detail->currentWidget() != m_contactPage) return;  // 已切到聊天页/空页

    // 数据更新了（带详细字段）→ 清短路标记 + 强制重新渲染
    m_currentShownContact.clear();
    showContact(detail);
}

// ── 图片放大预览 ────────────────────────────────────────────────────────────

class ImageViewerDialog : public QWidget {
public:
    explicit ImageViewerDialog(const QByteArray& bytes, QWidget* parent = nullptr)
        : QWidget(parent, Qt::Dialog | Qt::FramelessWindowHint) {
        setAttribute(Qt::WA_DeleteOnClose);
        setStyleSheet("background:rgba(0,0,0,220);");
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        m_lbl = new QLabel;
        m_lbl->setAlignment(Qt::AlignCenter);
        m_lbl->setStyleSheet("background:transparent;");
        QImage img;
        if (img.loadFromData(bytes)) {
            m_source = QPixmap::fromImage(img);
            m_lbl->setPixmap(m_source);
        }
        lay->addWidget(m_lbl);
        if (parent) {
            const QPoint gp = parent->mapToGlobal(QPoint(0, 0));
            setGeometry(gp.x(), gp.y(), parent->width(), parent->height());
        } else {
            showFullScreen();
        }
        setCursor(Qt::ArrowCursor);
    }
    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        if (m_source.isNull()) return;
        const int maxW = width() - 32;
        const int maxH = height() - 32;
        if (maxW < 64 || maxH < 64) return;
        m_lbl->setPixmap(m_source.scaled(maxW, maxH, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation));
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Return ||
            e->key() == Qt::Key_Space) close();
        QWidget::keyPressEvent(e);
    }
    void mousePressEvent(QMouseEvent* e) override { Q_UNUSED(e); close(); }
private:
    QLabel*  m_lbl   = nullptr;
    QPixmap  m_source;
};

bool WeChatDetailPanel::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() == QEvent::MouseButtonRelease) {
        auto* w = qobject_cast<QWidget*>(obj);
        if (w) {
            // 找到带 imgBytes 属性的祖先
            QWidget* card = w;
            while (card && !card->property("imgBytes").isValid()) card = card->parentWidget();
            if (card && card->property("imgBytes").isValid()) {
                const QByteArray bytes = card->property("imgBytes").toByteArray();
                if (!bytes.isEmpty()) {
                    auto* dlg = new ImageViewerDialog(bytes, this);
                    dlg->show();
                    dlg->raise();
                    dlg->activateWindow();
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(obj, ev);
}