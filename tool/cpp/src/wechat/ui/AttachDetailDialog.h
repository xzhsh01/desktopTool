// AttachDetailDialog.h — 视频/文件附件详情对话框
// 双击微信聊天中的视频/文件卡片时弹出，展示附件元数据（md5 / CDN URL / AES key 等）。
#pragma once

#include <QDialog>
#include <QVariantMap>

class AttachDetailDialog : public QDialog {
    Q_OBJECT
public:
    explicit AttachDetailDialog(const QVariantMap& msg, QWidget* parent = nullptr);
};
