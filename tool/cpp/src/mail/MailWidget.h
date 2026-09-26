#pragma once

#include <QSemaphore>
#include <QLabel>
#include <QWidget>
#include <QStackedWidget>
#include <QString>
#include <QStringList>
#include <QPoint>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <QList>
#include <QQueue>
#include <QPair>
#include <QDateTime>
#include <QMessageBox>
#include <QByteArray>

#include "mail/ImapClient.h"
#include "mail/SmtpClient.h"
#include "mail/MailStore.h"
#include "mail/TemplateStore.h"   // mail::Template (slot 签名里用到)

class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QLabel;
class QTabBar;
class QTextEdit;
class QTableWidget;
class QCheckBox;
class QComboBox;
class QVBoxLayout;
class MailFolderPanel;
class MailListPanel;
class MailContentPanel;

/**
 * MailWidget: 邮件主界面（协调者）
 *
 * 三栏 UI 拆分为独立面板（src/mail/ui/ 分层存放）：
 * - ui/folders/MailFolderPanel  邮件文件夹（左栏：搜索框 + 账号/文件夹树）
 * - ui/list/MailListPanel       邮件列表（中栏：按钮栏 + 块状邮件表格）
 * - ui/content/MailContentPanel 邮件内容（右栏：回复/转发 + 富文本/原文预览）
 *
 * MailWidget 负责编排：
 * - 多账号管理（CRUD）+ 默认账号选择
 * - 写邮件编辑器多 tab（新建 / 回复 / 转发 / 草稿）
 * - 手动刷新 / 全量同步流程（文件夹 → 邮件 → 正文）+ 同步日志面板
 * - 附件后台下载、正文按需拉取（IMAP 线程调度）
 */
class MailWidget : public QWidget {
    Q_OBJECT

public:
    explicit MailWidget(QWidget* parent = nullptr);
    ~MailWidget() override;

    // 在文件夹树中选中指定账号（供应用管理等外部入口跳转使用）
    void selectAccount(const QString& id);

    // ── 嵌套类型必须在 private slots 之前定义（moc 解析槽签名需要 EditorPage 可见） ──
    struct EditorAttachment {
        QString filePath;
        QString displayName;
        qint64  sizeBytes = 0;
        QString mimeType;
        QString contentId;
        bool    insertedInline = false;
    };
    struct EditorPage {
        QString key;
        QWidget*        panel = nullptr;
        QLabel*         contextLabel = nullptr;   // 顶部上下文标签（新邮件/回复/转发…）
        QLineEdit*      fromEdit = nullptr;
        QLineEdit*      toEdit = nullptr;
        QLineEdit*      ccEdit = nullptr;
        QLineEdit*      bccEdit = nullptr;
        QWidget*        ccRowHost = nullptr;   // 抄送整行容器（含 ccEdit + ×），默认隐藏
        QWidget*        bccRowHost = nullptr;  // 密送整行容器（含 bccEdit + ×），默认隐藏
        QLabel*         ccLabel = nullptr;     // QFormLayout 里的"抄送:"标签，需随 ccRowHost 同步隐藏
        QLabel*         bccLabel = nullptr;    // QFormLayout 里的"密送:"标签，需随 bccRowHost 同步隐藏
        QLineEdit*      subjectEdit = nullptr;
        QTextEdit*      bodyEdit = nullptr;
        // Markdown 编辑器（UI 切换按钮已移除，保留底层以兼容旧草稿/历史邮件）
        QPlainTextEdit* mdEdit = nullptr;        // Markdown 源码编辑器
        QStackedWidget* bodyStack = nullptr;     // 富文本 / Markdown 切换容器
        bool            markdownMode = false;    // Markdown 模式开关（草稿还原用）
        QTableWidget*   attachTable = nullptr;    // 编辑器附件表格（拖拽排序 / 右键菜单）
        QPushButton*    sendBtn = nullptr;
        QCheckBox*      scheduleChk = nullptr;   // 定时发送复选框：勾选=定时，未勾选=发送按钮变"立即发送"
        QDateTime       scheduledAt;
        bool            urgent       = false;
        bool            readReceipt  = false;
        bool            plainText    = false;      // 纯文本发送正文（不发送 HTML）
        QString         encryptMode;
        QString         encryptPassword;
        QCheckBox*      urgentChk       = nullptr;
        QCheckBox*      readReceiptChk  = nullptr;
        QCheckBox*      plainTextChk    = nullptr; // 选项行"纯文本"
        QComboBox*      encryptCombo    = nullptr;
        QPushButton*    scheduleBtn     = nullptr;
        QLabel*         scheduleChkLabel = nullptr;
        QToolButton*    templateBtn    = nullptr;
        QPushButton*    addAttBtn      = nullptr;  // 选项行首位的"+ 添加附件"按钮
        QWidget*        attListHost   = nullptr;  // 选项行下方显示附件的容器
        class FlowLayout* attListLayout = nullptr; // attListHost 内的流式布局：宽度不足时附件自动换行
        QString         draftId;
        QString         forwardOf;
        QStringList     references;
        QList<EditorAttachment> attachments;
    };

private slots:
    void onFolderChanged();

    void onNewAccountClicked();

    void onNewMailClicked();
    void onReplyClicked();
    void onReplyAllClicked();
    void onForwardClicked();
    void onForwardAsAttachmentClicked();
    void onForwardOriginalClicked();
    void onReeditClicked();          // "重新编辑"：载入写邮件窗口预填，可修改后重发
    void onResendClicked();          // "再次发送"：不打开编辑器，原样直接重发
    void onRevokeClicked();          // "撤销"：尝试从已发送文件夹撤回已发邮件
    void onDeleteMailClicked();

    void onSendClicked();
    void onSaveDraftClicked();
    void onDiscardDraftClicked();

    // 模板
    void buildTemplateMenu(QMenu* menu, EditorPage* p);
    void applyTemplateToPage(EditorPage* p, const mail::Template& tpl);
    void saveCurrentPageAsTemplate(EditorPage* p);
    void removeUserTemplateById(const QString& id, EditorPage* p);
    void openDraftInEditor(const QString& draftId);    // 双击草稿 → 打开编辑器
    void onEditorTabCloseRequested(int index);         // 编辑器 tab「×」关闭
    void resendDraft(const QString& draftId);          // 草稿箱预览"再次发送"：真正发送草稿
    // 后台发送并存入"已发送"；draftIdToRemove 非空表示来自草稿箱，成功后移除该草稿
    void resendMailAsync(const SmtpClient::Params& params,
                         const QString& senderEmail, const QString& senderAccId,
                         QStringList toList, QStringList ccList,
                         QString subj, QString body, QString htmlBody,
                         QString draftIdToRemove = QString());

    void onMessageContextMenu(const QPoint& pos);   // 邮件列表右键：预览 + 勾选菜单

    void showSyncTip(const QString& text);      // 同步进度提示（状态栏 + 日志面板）
    void closeSyncTip(const QString& summary);  // 完成摘要（状态栏 + 日志面板）
    void onRefreshClicked();                       // 文件夹树右键"刷新" / 外部调用
    void onFullSyncClicked();                     // 工具栏"全量同步"：文件夹 + 邮件 + 正文一体同步
    void onEmptyStateAddClicked();

private:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void setupUI();
    void connectPanels();                 // 三个面板的信号 → 本类槽/逻辑
    void refreshAccounts();
    void editAccount(const QString& id);       // 弹出编辑对话框
    void deleteAccount(const QString& id);     // 删除账号（带确认）
    void setDefaultAccount(const QString& id); // 设为默认发件账号

    void updateEmptyState();
    void refreshFolders();
    void loadRemoteFolders(const QString& accountId);          // 后台 IMAP LIST，缓存真实文件夹
    void fetchFolderHeaders(const QString& accountId,
                            const QString& folder);            // 后台拉取指定文件夹邮件头
    void refreshMessageList();
    // 高频触发源（messagesChanged/搜索输入）防抖入口：150ms 内合并为一次重建
    void scheduleRefreshList(bool withFolders = false);
    void refreshPreview();
    void onMessageItemSelectionChanged();        // 列表选中变化 → 按钮态 + 预览
    void requestBodyLoad(const MailStore::Message* m);  // 后台单封拉正文（防重入 m_loadingBodyIds）
    // 本地固定 key（INBOX/Sent/Drafts）→ IMAP SELECT 用的服务器真实文件夹名；
    // "Sent" 在部分服务器（如 139）真实名是"已发送"，需从 LIST 缓存解析
    QString folderToImapName(const QString& accountId, const QString& folder) const;

    // 邮件已读/未读状态：本地 markRead（驱动列表未读态 + 文件夹未读数刷新）
    // 并后台同步到服务器（已读 → markSeen；未读 → markUnseen）
    void setMessageRead(const QString& msgId, bool read);

    // 证书管理器（设置里"管理证书"按钮打开；cpp 已实现）
    void onManageCertsClicked();

    // 附件下载（后台线程 fetchPart → 写盘 → 打开）；已下载则直接打开
    void onAttachmentClicked(const QString& msgId, int idx, const QString& name);
    // 附件下载并发队列：入队去重并尝试启动。至多同时 kAttMaxConcurrent 个附件
    // 以多线程并行下载（每附件独立线程 + 独立 IMAP 连接），下载完成即复用空位继续推进。
    void enqueueAttachmentDownload(const QString& msgId, int idx);
    void pumpAttachmentQueue();   // 填充并发窗口：空闲槽位从队列取出附件启动
    void startAttachmentDownload(const QString& msgId, int idx,
                                 const QString& name);
    // 网络路径：单个附件走后台 fetchPart（独立 IMAP 连接）
    void downloadAttachmentOverNetwork(const QString& msgId, int idx, const QString& name);
    // 附件字节落盘 + 更新下载行/预览（主线程收尾统一入口）
    void finishAttachmentDownload(const QString& savedPath, const QByteArray& data,
                                  const QString& key, const QString& attName,
                                  const QString& mId, bool ok);
    // 已下载附件行上的"打开" / "在资源管理器中显示"
    void onAttachmentOpenRequested(const QString& msgId, int idx,
                                   const QString& name, const QString& savedPath);
    void onAttachmentRevealRequested(const QString& msgId, int idx,
                                     const QString& name, const QString& savedPath);
    // 附件行"另存为"：已下载 → 选位置复制一份；未下载 → 先下载
    void onAttachmentSaveAsRequested(const QString& msgId, int idx,
                                     const QString& name, const QString& savedPath);

    // ── 编辑器多 tab 管理 ──
    EditorPage* createEditorPage(const QString& key, const QString& tabTitle); // 构建独立编辑器页 + 顶部 tab
    EditorPage* findEditorPage(const QString& key) const;
    EditorPage* currentEditorPage() const;       // 当前激活 tab 对应的编辑器页（浏览页返回 nullptr）
    int         editorTabIndex(const EditorPage* p) const;
    void        activateEditorTab(EditorPage* p);           // 激活（已存在则仅切换）
    void        closeEditorTab(int tabIndex);                // 关闭入口：未保存内容先确认
    void        destroyEditorPage(EditorPage* p);            // 销毁 tab + stack 页 + 数据
    bool        saveDraftForPage(EditorPage* p);             // 保存草稿（按钮/关闭确认共用）

    // 编辑器填充（操作指定 page）
    void fillEditorForNew(EditorPage* p, const QString& accountId);
    void fillEditorForReply(EditorPage* p, const QString& messageId);
    void fillEditorForReplyAll(EditorPage* p, const QString& messageId);
    void fillEditorForForward(EditorPage* p, const QString& messageId);
    void fillEditorForForwardAsAttachment(EditorPage* p, const QString& messageId);
    void fillEditorForForwardOriginal(EditorPage* p, const QString& messageId);
    void fillEditorFromDraft(EditorPage* p, const QString& draftId);
    void fillEditorForEdit(EditorPage* p, const QString& messageId);
    // 编辑器附件表格：重建 / 右键菜单 / 行双击（cpp 已实现）
    void refreshAttachmentTable(EditorPage* p);
    void onAttachmentTableContextMenu(EditorPage* p, const QPoint& pos);
    void onAttachmentRowDoubleClicked(EditorPage* p, int row);
    void previewAttachment(EditorPage* p, int row);           // 图片弹窗预览，其它系统打开
    void renameAttachmentInList(EditorPage* p, int row);      // 仅列表显示重命名
    void duplicateAttachmentInList(EditorPage* p, int row);   // 复制列表项
    void removeAttachmentFromList(EditorPage* p, int row);    // 删除列表项（同步正文 cid）
    void insertAttachmentInline(EditorPage* p, int row);      // 图片以 cid: 插入正文
    void onInsertAttachmentInlineClicked(EditorPage* p);      // 工具栏"插入正文(图片)"
    // 切回"邮件"浏览 tab（编辑器 tab 保留存续，由各自「×」关闭）
    void showPreviewPage();

    // 重建 p->attListLayout 列表（"选项行下方附件行"）；每次增删附件后调用
    void refreshAttachmentList(EditorPage* p);

    // 根据 p->scheduledAt 同步发送按钮文案（有定时显示时间，否则"定时发送"）
    void refreshSendBtnText(EditorPage* p);

    QString currentAccountId() const;
    QString currentFolder() const;   // INBOX / Sent / Drafts
    QString selectedMessageId() const;

    // ── UI：三栏面板 ──
    MailFolderPanel*  m_folderPanel = nullptr;   // 左栏：邮件文件夹
    MailListPanel*    m_listPanel = nullptr;     // 中栏：邮件列表
    MailContentPanel* m_contentPanel = nullptr;  // 右栏：邮件内容

    // 编辑器多 tab（每封邮件一个 tab，可并存）
    QStackedWidget* m_editorStack = nullptr;   // m_contentStack page 1：编辑器页容器
    QList<EditorPage*> m_editorPages;          // 存活的编辑器页
    int m_editorSeq = 0;                       // "写邮件"自增序号（每封新邮件独立 tab）

    QSet<QString> m_loadingBodyIds;            // 正在按需拉正文的邮件 id（防重复触发）

    QTabBar*       m_filterTabs = nullptr;   // 顶部 tab：tab 0 浏览页 / tab 1..N 编辑器
    void          onFilterTabChanged(int idx);

    QLabel*       m_statusLabel = nullptr;

    // 空账号引导页（中央堆叠 widget 的第 0 页）
    QStackedWidget* m_mainStack = nullptr;
    QWidget*      m_emptyPage   = nullptr;
    QWidget*      m_workPage    = nullptr;
    // 顶层内容堆叠：page 0 = 三栏浏览（splitter），page 1 = 编辑器 stack（多 tab）
    QStackedWidget* m_contentStack = nullptr;

    // ── 状态 ──
    QSet<QString> m_foldersLoading;   // 正在 LIST 的账号（防重入）
    QSet<QString> m_headersLoading;
    QTimer*  m_listDebounce   = nullptr;  // 列表刷新防抖定时器
    bool     m_debounceFolders = false;   // 防抖到期时是否连带刷新文件夹树
    int m_pendingFolderSync = 0;      // 刷新流程中待完成的文件夹同步数
    int m_syncRound = 0;              // 同步轮次：兜底定时器据此丢弃过期触发
    QSemaphore m_syncSem{3};        // 文件夹同步全局并发上限（防服务器限流）
    int m_folderSyncTotal = 0;      // 本轮并行同步的文件夹总数
    int m_folderSyncDone  = 0;       // 已完成数（含失败）

    // ── 附件下载并发队列状态 ──
    static constexpr int kAttMaxConcurrent = 4;      // 最多并发下载的附件数（多线程）
    QSet<QString> m_attActive;                       // 正在下载的附件 key（msgId|idx）
    QQueue<QPair<QString, int>> m_attQueue;          // 待下载附件（并发调度）
    QSet<QString> m_attRetried;                      // 已自动重试过一次的附件 key（防无限重试）

    void syncMailAfterFolders();      // 文件夹全部同步完成后拉邮件

    // 附件选择
    void onAddAttachmentClicked();
    void onInsertInlineImageClicked(EditorPage* p);
    // 弹定时发送时间对话框；返回 true = 用户成功选择了时间（写入 p->scheduledAt）
    bool pickScheduleTime(EditorPage* p);
    // DSN 接收方：弹窗询问并发送回执
    void onReadReceiptRequested(const QString& msgId, const QString& rto);
};
