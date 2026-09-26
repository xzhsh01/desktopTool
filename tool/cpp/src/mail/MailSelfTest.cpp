#include "mail/MailSelfTest.h"
#include "mail/MailAccountManager.h"
#include "mail/MailPoller.h"
#include "mail/MailStore.h"
#include "mail/ImapClient.h"
#include "mail/crypto/PasswordCrypto.h"
#include "mail/crypto/MailEncryptor.h"
#include "mail/crypto/SmimeCrypto.h"
#include "mail/ui/MarkdownBridge.h"
#include "mail/TemplateStore.h"
#include "core/Logger.h"

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

namespace mailSelfTest {

// ── 自签证书工具（仅用于签名/验签自测，不写磁盘、不污染仓库） ──
namespace openssl_self_test {

// 生成一对 RSA-2048 + 一个自签 X.509 证书
bool makeRsa2048SelfSigned(EVP_PKEY** outKey, X509** outCert) {
    *outKey = EVP_PKEY_new();
    if (!*outKey) return false;
    EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0
              || EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) <= 0
              || EVP_PKEY_keygen(kctx, outKey) <= 0) {
        if (kctx) EVP_PKEY_CTX_free(kctx);
        return false;
    }
    EVP_PKEY_CTX_free(kctx);

    *outCert = X509_new();
    if (!*outCert) return false;
    ASN1_INTEGER_set(X509_get_serialNumber(*outCert), 1);
    X509_gmtime_adj(X509_getm_notBefore(*outCert), 0);
    X509_gmtime_adj(X509_getm_notAfter(*outCert),  60L * 60 * 24 * 365);   // 1 年
    X509_set_pubkey(*outCert, *outKey);

    X509_NAME* name = X509_get_subject_name(*outCert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const uchar*>("bambooRat-SelfTest"), -1, -1, 0);
    X509_set_issuer_name(*outCert, name);

    if (!X509_sign(*outCert, *outKey, EVP_sha256())) return false;
    return true;
}

} // namespace openssl_self_test

static QString mkAccId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void run() {
    Logger::instance().info("========== MailSelfTest 开始 ==========", "selftest");

    auto& mgr  = MailAccountManager::instance();
    auto& store = MailStore::instance();
    auto& poll = MailPoller::instance();

    // ── 0. 清理上次自测遗留的测试账号（避免污染） ──
    QStringList toRemove;
    for (const auto& a : mgr.accounts()) {
        if (a.email.contains("selftest@") || a.email.contains("@invalid.example.com")) {
            toRemove.append(a.id);
        }
    }
    for (const auto& id : toRemove) {
        mgr.remove(id);
        Logger::instance().info(QString("[0] 清理遗留测试账号: %1").arg(id), "selftest");
    }

    // ── 1. 注入一个指向不可达主机的账号（验证 worker 启动 + 日志链路） ──
    Logger::instance().info("[1] 注入测试账号（不可达 IMAP 主机）", "selftest");
    QVariantMap data;
    data["name"] = "selftest-account";
    data["email"] = "selftest@invalid.example.com";
    data["password"] = "fake-pass";
    data["recvProto"] = "IMAP";
    data["imapHost"] = "127.0.0.1";
    data["imapPort"] = 1;                // 不可达
    data["imapSsl"] = false;
    data["isDefault"] = false;
    // 用 add() 返回的 Account 拿真实 id（不要自己 mkAccId，避免 id 不匹配）
    QString accId = mgr.add(data).id;
    Logger::instance().info(
        QString("[1] 注入完成: accId=%1, 共 %2 个账号").arg(accId).arg(mgr.accounts().size()),
        "selftest");

    // ── 2. 直接调用 upsertMessages 验证 m.id 修复 ──
    Logger::instance().info("[2] 验证 MailStore.upsertMessages 的 m.id 修复", "selftest");
    QList<MailStore::Message> fake;
    MailStore::Message m;
    m.accountId  = accId;
    m.folder     = "INBOX";
    m.messageId  = "<selftest-1@example.com>";
    m.from       = "tester@example.com";
    m.subject    = "自测邮件 1";
    m.body       = "这是 MailSelfTest 注入的测试邮件。";
    m.date       = QDateTime::currentDateTime();
    m.read       = false;
    fake.append(m);

    m.messageId  = "<selftest-2@example.com>";
    m.subject    = "自测邮件 2（验证去重）";
    fake.append(m);                          // 同 messageId 应去重

    m.messageId  = "<selftest-3@example.com>";
    m.subject    = "自测邮件 3";
    fake.append(m);

    store.upsertMessages(fake);

    int myCount = 0;
    int myWithId = 0;
    for (const auto& mm : store.messages()) {
        if (mm.accountId != accId) continue;
        ++myCount;
        if (!mm.id.isEmpty()) ++myWithId;
    }
    Logger::instance().info(
        QString("[2] 注入后: 测试账号邮件=%1 (其中 m.id 已分配=%2)")
            .arg(myCount).arg(myWithId),
        "selftest");

    // ── 3. 触发轮询（不可达主机，应超时并记录） ──
    //     真实账号 139 在 INBOX 未读很多时，batch FETCH HEADER.FIELDS 162 个 UID
    //     分 6 批 (BATCH_SIZE=30),慢时段每批 ~60s。等待时间放到 600s 留足 buffer。
    //     用 MailPoller::workerFinished 信号触发可更精确，但信号来自 worker
    //     子线程,需要在主线程监听。MailPoller 没有暴露这个信号,简单方案
    //     是固定等待。
    Logger::instance().info("[3] 触发 pollNow() —— 等所有 worker 完成(或兜底 600s)", "selftest");
    QEventLoop loop;
    // 兜底超时(防止单 worker 卡死后整个 selftest 永不结束)
    QTimer::singleShot(600000, &loop, &QEventLoop::quit);
    // 监听 MailPoller 的 accountPolled，全部 worker 完成时退出 loop
    int expected = mgr.accounts().size();
    int done = 0;
    QObject::connect(&poll, &MailPoller::accountPolled, &loop,
        [&done, expected, &loop](const QString&) {
            if (++done >= expected) loop.quit();
        });
    poll.pollNow();
    loop.exec();
    Logger::instance().info(
        QString("[3] pollNow 结束: MailStore 当前消息总数=%1").arg(store.messages().size()),
        "selftest");

    // ── 4. 测试 setDefault + remove ──
    Logger::instance().info("[4] 测试 setDefault + remove", "selftest");
    poll.stop();
    mgr.setDefault(accId);
    Logger::instance().info("[4] setDefault 完成", "selftest");
    // 先清理注入的测试邮件，避免账号删除后邮件成为孤儿数据残留 messages.json
    int removedMsgs = store.removeMessagesByAccount(accId);
    Logger::instance().info(
        QString("[4] 清理注入的测试邮件: %1 封").arg(removedMsgs), "selftest");
    bool removed = mgr.remove(accId);
    Logger::instance().info(
        QString("[4] remove: targetId=%1 result=%2, 剩余账号=%3")
            .arg(accId).arg(removed).arg(mgr.accounts().size()),
        "selftest");

    // 兜底清理：按 email 关键字清理任何 selftest 残留（防御性，避免 id 不匹配留下脏数据）
    QStringList leftover;
    for (const auto& a : mgr.accounts()) {
        if (a.email.contains("selftest@") || a.email.contains("@invalid.example.com")) {
            leftover.append(a.id);
        }
    }
    for (const auto& id : leftover) {
        bool ok = mgr.remove(id);
        Logger::instance().info(
            QString("[4] 兜底删除: id=%1 result=%2").arg(id).arg(ok), "selftest");
    }
    Logger::instance().info(
        QString("[4] 最终账号数=%1").arg(mgr.accounts().size()), "selftest");

    // 兜底清理历史遗留的孤儿邮件（accountId 已不存在于任何账号）
    QStringList validIds;
    for (const auto& a : mgr.accounts()) validIds.append(a.id);
    int orphanRemoved = store.removeOrphanMessages(validIds);
    Logger::instance().info(
        QString("[4] 孤儿邮件清理: 移除 %1 封").arg(orphanRemoved), "selftest");

    // ── 5. 真实账号 fetchBody / fetchRawSource / QP 解码验证 ──
    //     若存在默认账号且有 imapUid，拉第一封真实邮件的正文与原件
    Logger::instance().info("[5] 真实邮件 fetchBody / fetchRawSource 验证", "selftest");
    for (const auto& a : mgr.accounts()) {
        if (a.email.contains("@invalid.example.com")) continue;
        if (a.imapHost.isEmpty() || a.password.isEmpty()) continue;
        // 取该账号下第一封有 imapUid 的邮件
        QStringList msgIds;
        for (const auto& m : MailStore::instance().messagesIn(a.id, "INBOX")) {
            if (!m.imapUid.isEmpty()) { msgIds << m.id; break; }
        }
        if (msgIds.isEmpty()) {
            Logger::instance().info("[5] 该账号暂无可测邮件（imapUid 为空）", "selftest");
            continue;
        }
        auto* msg = MailStore::instance().message(msgIds.first());
        if (!msg) continue;

        // 5a. 单元测试 decodeTransferEncoding 自身
        //     输入: "=E4=B8=AD=E6=96=87 hello=20world" (UTF-8 QP) → "中文 hello world"
        QString qpInput = "=E4=B8=AD=E6=96=87 hello=20world";
        QString qpExpected = QString::fromUtf8(QByteArray::fromHex("E4B8ADE69687")) + " hello world";

        Logger::instance().info("[5] 准备拉取正文 (uid=" + msg->imapUid + ", folder=" + msg->folder + ")", "selftest");
        ImapClient::Config cfg;
        cfg.host     = a.imapHost;
        cfg.port     = a.imapPort;
        cfg.ssl      = a.imapSsl;
        cfg.username = a.email;
        cfg.password = a.password;
        cfg.timeoutSec = 20;

        // 同步拉取正文
        QString body, htmlBody; QString err;
        if (ImapClient::fetchBody(cfg, msg->folder, msg->imapUid, &body, &htmlBody, nullptr, nullptr, &err)) {
            // 检测是否还残留 QP 字符(=XX):若有说明解码未生效
            bool hasQP = QRegularExpression("=[0-9A-Fa-f]{2}").match(body).hasMatch();
            Logger::instance().info(
                QString("[5] fetchBody 成功: 长度=%1 htmlLen=%2 仍含 QP=%3 前 80 字=%4")
                    .arg(body.size()).arg(htmlBody.size())
                    .arg(hasQP ? "是(可能非 UTF-8 编码)" : "否")
                    .arg(body.left(80).replace("\r", " ").replace("\n", " ")),
                "selftest");
            MailStore::instance().updateBody(msg->id, body, htmlBody);
        } else {
            Logger::instance().warn("[5] fetchBody 失败: " + err, "selftest");
        }

        // 5b. 拉取原件
        QByteArray raw; QString err2;
        if (ImapClient::fetchRawSource(cfg, msg->folder, msg->imapUid, &raw, &err2)) {
            Logger::instance().info(
                QString("[5] fetchRawSource 成功: 字节=%1").arg(raw.size()), "selftest");
            // 解析 raw: 检测关键 header
            QString rawText = QString::fromUtf8(raw);
            bool hasQP    = QRegularExpression("Content-Transfer-Encoding:\\s*quoted-printable",
                                                QRegularExpression::CaseInsensitiveOption)
                              .match(rawText).hasMatch();
            bool hasB64   = QRegularExpression("Content-Transfer-Encoding:\\s*base64",
                                                QRegularExpression::CaseInsensitiveOption)
                              .match(rawText).hasMatch();
            bool hasUtf8  = QRegularExpression("charset=\"?utf-?8\"?",
                                                QRegularExpression::CaseInsensitiveOption)
                              .match(rawText).hasMatch();
            bool hasMultipart = QRegularExpression("Content-Type:\\s*multipart/",
                                                QRegularExpression::CaseInsensitiveOption)
                              .match(rawText).hasMatch();
            Logger::instance().info(
                QString("[5] 原件编码分析: QP=%1 Base64=%2 UTF8=%3 Multipart=%4")
                    .arg(hasQP ? "Y" : "N").arg(hasB64 ? "Y" : "N")
                    .arg(hasUtf8 ? "Y" : "N").arg(hasMultipart ? "Y" : "N"),
                "selftest");
            MailStore::instance().updateRawSource(msg->id, raw);
        } else {
            Logger::instance().warn("[5] fetchRawSource 失败: " + err2, "selftest");
        }
        break;  // 只测一个账号
    }

    // ── [6] 加密模块自测 (PasswordCrypto + MailEncryptor) ──
    Logger::instance().info("[6] 加密模块自测: PasswordCrypto encrypt/decrypt 往返", "selftest");
    {
        const QString pwd = "my-secret-123";
        const QByteArray plain =
            "From: a@b\r\nTo: c@d\r\nSubject: hi\r\n\r\nHello, world! 你好世界！";
        QByteArray enc = PasswordCrypto::encrypt(pwd, plain);
        if (enc.isEmpty()) {
            Logger::instance().error("[6] PasswordCrypto::encrypt 失败: " + PasswordCrypto::getLastError(), "selftest");
        } else {
            QByteArray dec = PasswordCrypto::decrypt(pwd, enc);
            bool ok = (dec == plain);
            Logger::instance().info(
                QString("[6] PasswordCrypto 往返: enc=%1B dec=%2B match=%3 fp=%4")
                    .arg(enc.size()).arg(dec.size())
                    .arg(ok ? "OK" : "FAIL")
                    .arg(PasswordCrypto::fingerprint(enc)),
                "selftest");
            // 错误口令
            QByteArray wrong = PasswordCrypto::decrypt("wrong-pwd", enc);
            Logger::instance().info(
                QString("[6] 错误口令应解不出明文: ok=%1 (应为 true)")
                    .arg(wrong.isEmpty() ? "true" : "false"),
                "selftest");
        }
        // MailEncryptor::wrap None 模式（原样透传）
        MailEncryptor::Policy nonePolicy; nonePolicy.mode = MailEncryptor::None;
        QString hdr, body;
        QString err;
        bool wrapOk = MailEncryptor::wrap("X-Test: 1\r\n", "body content", nonePolicy,
            {"test@x.com"}, hdr, body, &err);
        Logger::instance().info(
            QString("[6] MailEncryptor::wrap None: %1 (hdr='%2')")
                .arg(wrapOk ? "OK" : "FAIL").arg(hdr.left(20)),
            "selftest");
        // MailEncryptor::wrap Password 模式
        MailEncryptor::Policy pwdPolicy; pwdPolicy.mode = MailEncryptor::Password;
        pwdPolicy.password = "shared-secret";
        QString err2;
        bool wrapPwdOk = MailEncryptor::wrap("X-Test: 1\r\nSubject: hi", "hello world",
            pwdPolicy, {"test@x.com"}, hdr, body, &err2);
        Logger::instance().info(
            QString("[6] MailEncryptor::wrap Password: ok=%1 hdr%2B-body=%3B")
                .arg(wrapPwdOk ? "true" : "false")
                .arg(body.left(80))
                .arg(body.size()),
            "selftest");
        if (wrapPwdOk) {
            // 验证 detectKind 能识别为 password
            QString kind = MailEncryptor::detectKind(
                "multipart/alternative; boundary=\"...\"",
                body,
                body.toUtf8());
            Logger::instance().info(
                QString("[6] detectKind 识别结果: '%1' (应为 'password')").arg(kind),
                "selftest");
        }
    }

    // ── [7] S/MIME 签名 + 验证自测（用临时自签证书，避开本地仓库） ──
    Logger::instance().info("[7] S/MIME 签名 + verify 自测（自签 RSA-2048）", "selftest");
    {
        using namespace openssl_self_test;
        EVP_PKEY* key = nullptr;
        X509* cert  = nullptr;
        if (!makeRsa2048SelfSigned(&key, &cert)) {
            Logger::instance().error("[7] 生成自签证书失败: " + QString::fromUtf8(ERR_error_string(ERR_get_error(), nullptr)), "selftest");
        } else {
            const QByteArray mime =
                "From: alice@example.com\r\n"
                "To: bob@example.com\r\n"
                "Subject: signed test\r\n"
                "\r\n"
                "Hello, this is a signed message. 你好世界。";
            QString err;
            QByteArray sig = SmimeCrypto::sign(mime, cert, key, &err);
            if (sig.isEmpty()) {
                Logger::instance().error("[7] SmimeCrypto::sign 失败: " + err, "selftest");
            } else {
                Logger::instance().info(
                    QString("[7] sign 成功，签名块大小 %1 字节（DER）").arg(sig.size()),
                    "selftest");
                // 构造 multipart/signed 容器后验签
                const QString boundary = "=_test_sig_001";
                QString mp;
                mp += QStringLiteral("--%1\r\n").arg(boundary);
                mp += QStringLiteral("Content-Type: message/rfc822; charset=utf-8\r\n\r\n");
                mp += QString::fromUtf8(mime);
                mp += QStringLiteral("\r\n--%1\r\n").arg(boundary);
                mp += QStringLiteral("Content-Type: application/pkcs7-signature; name=\"smime.p7s\"\r\n");
                mp += QStringLiteral("Content-Transfer-Encoding: base64\r\n\r\n");
                mp += QString::fromUtf8(sig.toBase64());
                mp += QStringLiteral("\r\n--%1--\r\n").arg(boundary);
                const QString mpHeaders = QStringLiteral(
                    "Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; "
                    "micalg=\"sha-256\"; boundary=\"%1\"").arg(boundary);
                const QByteArray signedMime = (mpHeaders + "\r\n\r\n" + mp).toUtf8();

                SmimeCrypto::VerifyResult vr = SmimeCrypto::verify(signedMime, QString(), &err);
                if (vr.ok) {
                    Logger::instance().info(
                        QString("[7] verify 成功：ok=true, subject='%1', email='%2', alg='%3'")
                            .arg(vr.signerSubject, vr.signerEmail, vr.digestAlg),
                        "selftest");
                } else {
                    Logger::instance().error(
                        QString("[7] verify 失败: %1").arg(vr.errorReason),
                        "selftest");
                }

                // 篡改测试：把原 MIME 改一个字节，验签应失败
                QByteArray tampered = signedMime;
                int bodyPos = tampered.indexOf("Hello, this is a signed message");
                if (bodyPos > 0) {
                    tampered[bodyPos] = 'X';   // 把 'H' 改成 'X'
                    SmimeCrypto::VerifyResult vr2 = SmimeCrypto::verify(tampered, QString(), &err);
                    Logger::instance().info(
                        QString("[7] 篡改后 verify: ok=%1 reason='%2' (应为 ok=false)")
                            .arg(vr2.ok ? "true" : "false", vr2.errorReason),
                        "selftest");
                }
            }
            X509_free(cert);
            EVP_PKEY_free(key);
        }
    }

    // ── [8] MarkdownBridge 自测：toHtml / toHtmlFragment / fromHtml ──
    Logger::instance().info("[8] MarkdownBridge 自测", "selftest");
    {
        QString md = QStringLiteral(
            "# 标题 H1\n"
            "## 标题 H2\n\n"
            "**加粗** *斜体* ~~删除线~~ `行内代码`\n\n"
            "- 列表项 A\n"
            "- 列表项 B\n\n"
            "1. 有序 1\n"
            "2. 有序 2\n\n"
            "> 引用一段\n\n"
            "[链接](https://example.com)\n\n"
            "```cpp\n"
            "int x = 42;\n"
            "```\n\n"
            "| 列1 | 列2 |\n"
            "|----|----|\n"
            "| A   | B   |");
        QString full = mail::markdown::toHtml(md);
        QString frag = mail::markdown::toHtmlFragment(md);
        QString back = mail::markdown::fromHtml(frag);
        bool ok = !full.isEmpty()
                && !frag.isEmpty()
                && frag.size() < full.size()
                && frag.contains(QStringLiteral("加粗"))
                && back.contains(QStringLiteral("加粗"));
        Logger::instance().info(
            QString("[8] MarkdownBridge: ok=%1 full=%2B frag=%3B back=%4B")
                .arg(ok ? "yes" : "NO")
                .arg(full.size()).arg(frag.size()).arg(back.size()),
            "selftest");
        if (!ok) {
            Logger::instance().error("[8] MarkdownBridge 转换异常", "selftest");
        }
    }

    // ── [9] TemplateStore 自测：内置模板 / 占位符 / 用户模板 CRUD ──
    Logger::instance().info("[9] TemplateStore 自测", "selftest");
    {
        auto& ts = mail::TemplateStore::instance();
        ts.reload();   // 重新载入（确保拿到最新内置列表）
        auto all = ts.listAll();
        auto builtin = ts.listBuiltin();
        Logger::instance().info(
            QString("[9] TemplateStore: total=%1 builtin=%2")
                .arg(all.size()).arg(builtin.size()), "selftest");
        // 占位符替换
        QString src = QStringLiteral("Hi {recipient}! From {sender} on {date}. Time {time}. Name={name}.");
        QString out = mail::TemplateStore::applyPlaceholders(
            src, QStringLiteral("alice@x.com, bob@x.com"),
                 QStringLiteral("me@x.com"));
        bool ok1 = out.contains(QStringLiteral("alice@x.com、bob@x.com"))
                && out.contains(QStringLiteral("me@x.com"))
                && out.contains(QRegularExpression(QStringLiteral("\\d{4}-\\d{2}-\\d{2}")));
        Logger::instance().info(
            QString("[9] TemplateStore 占位符替换: ok=%1 out='%2'")
                .arg(ok1 ? "yes" : "NO").arg(out), "selftest");
        // 用户模板 CRUD
        mail::Template tpl;
        tpl.name = QStringLiteral("selftest-template");
        tpl.category = QStringLiteral("selftest");
        tpl.subject = QStringLiteral("测试 {date}");
        tpl.body    = QStringLiteral("正文 {recipient}");
        QString newId = ts.addUserTemplate(tpl);
        bool ok2 = !newId.isEmpty() && newId.startsWith(QStringLiteral("user:"));
        auto found = newId.isEmpty() ? nullptr : ts.findById(newId);
        bool ok3 = found && found->name == tpl.name;
        bool ok4 = !newId.isEmpty() && ts.removeUserTemplate(newId);
        Logger::instance().info(
            QString("[9] TemplateStore CRUD: add=%1 find=%2 remove=%3")
                .arg(ok2 ? "ok" : "NO").arg(ok3 ? "ok" : "NO").arg(ok4 ? "ok" : "NO"),
            "selftest");
    }

    Logger::instance().info("========== MailSelfTest 结束 ==========", "selftest");
}

} // namespace mailSelfTest