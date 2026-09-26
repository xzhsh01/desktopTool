#include "wechat/ui/WeChatDetailPanel.h"
#include "app/Theme.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {
const QString kBubbleSelf  = "#95EC69";   // 微信绿
const QString kBubbleOther = Theme::kSurfaceAlt;
} // namespace

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
}

void WeChatDetailPanel::showChatHeader(const QString& title) {
    m_chatTitle->setText(title);
    m_detail->setCurrentWidget(m_chatPage);
}

void WeChatDetailPanel::renderMessages(const QList<QVariantMap>& msgs,
                                       const QString& currentTalker) {
    m_currentTalker = currentTalker;
    // 清掉旧消息（保留末位的 stretch）
    while (m_msgLayout->count() > 1) {
        auto* it = m_msgLayout->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    // 按时间升序插入（数据库默认已升序；若 reverse 把 list 倒一下即可）
    QDateTime lastDate;
    for (const auto& m : msgs) {
        const QDateTime t = QDateTime::fromString(m["time"].toString(), Qt::ISODate);
        if (!t.isValid() || t.date() != lastDate.date()) {
            m_msgLayout->insertWidget(m_msgLayout->count() - 1, makeDateSeparator(t));
            lastDate = t;
        }
        m_msgLayout->insertWidget(m_msgLayout->count() - 1, makeBubble(m));
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
    const bool isSystem = (type == 10000 || type == 10002);

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
    // 清掉 contactPage 除 header 之外的旧内容
    QLayout* old = m_contactPage->layout();
    // 仅删除 stretch / 非 header 子项
    for (int i = old->count() - 1; i >= 0; --i) {
        auto* it = old->itemAt(i);
        QWidget* w = it->widget();
        if (!w) continue;
        if (w->minimumHeight() == 48 && w->y() == 0) continue;   // header
        old->removeWidget(w);
        w->deleteLater();
    }

    auto* card = new QWidget;
    card->setStyleSheet(QString("background:%1;").arg(Theme::kBg));
    auto* cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(0, 24, 0, 0);
    cardLay->setSpacing(14);
    cardLay->setAlignment(Qt::AlignHCenter);

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
    form->setSpacing(6);
    form->setContentsMargins(40, 12, 40, 0);
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
    if (c["isRoom"].toBool()) {
        addField("类型", "群聊");
    }
    cardLay->addLayout(form);

    // 占位 stretch（让卡片置顶）
    auto* bodyWrap = new QWidget;
    bodyWrap->setStyleSheet(QString("background:%1;").arg(Theme::kBg));
    auto* bodyLay = new QVBoxLayout(bodyWrap);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->addWidget(card, 0, Qt::AlignHCenter | Qt::AlignTop);

    auto* oldLayout = m_contactPage->layout();
    oldLayout->addWidget(bodyWrap);
    m_detail->setCurrentWidget(m_contactPage);
}