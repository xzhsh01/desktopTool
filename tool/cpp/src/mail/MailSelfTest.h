#pragma once

/**
 * MailSelfTest: 邮件功能自测
 *
 * 不依赖真实 IMAP 服务器，验证关键链路：
 *  - MailAccountManager add/remove（含加密往返）
 *  - MailPoller 调度（用不可达主机验证 worker/超时/日志链路）
 *  - MailStore upsertMessages（含本次修复的 m.id 分配）
 *  - UI 信号联动：accountsChanged / messagesChanged
 *
 * 用法：main 收到 --mail-selftest 时调用 runMailSelfTest()，
 *       完成后调用 qApp->exit(0)。
 */
namespace mailSelfTest {
void run();   // 同步执行，内部用 QEventLoop 等待轮询结束
}