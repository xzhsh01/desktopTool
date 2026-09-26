#include "WeChatDb.h"
#include "core/Logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDateTime>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <algorithm>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "sqlite3.h"

namespace {

constexpr int kPageSize = 4096;
constexpr int kKeySize = 32;
constexpr int kMacIter = 2;               // HMAC key 派生迭代
constexpr char kSqliteHeader[] = "SQLite format 3\x00";
// SQLCipher 默认 HMAC salt mask（3.x 与 4.x 相同：0x3a = 58）
constexpr unsigned char kMacSaltMask = 0x3a;
// 4.x message_content 为 zstd 压缩时的魔数
constexpr char kZstdMagic[] = "\x28\xb5\x2f\xfd";
// runQuery 对 zstd 内容返回的标记（避免二进制经 UTF-8 转换损坏）
const QString kZstdMark = QStringLiteral("\x01__ZSTD__");

// ── 版本参数（3.x: SQLCipher 旧版；4.x: SQLCipher 4 默认）──────────────────
struct DbParams {
    int kdfIter;         // PBKDF2 迭代次数
    int hmacSize;        // HMAC 输出长度
    int reserve;         // 每页保留区大小（IV + HMAC，16 对齐）
    const EVP_MD* hash;  // KDF / HMAC 哈希
};

DbParams v3Params() { return {64000, 20, 48, EVP_sha1()}; }    // IV16+HMAC-SHA1(20)+pad12
DbParams v4Params() { return {256000, 64, 80, EVP_sha512()}; }  // IV16+HMAC-SHA512(64)

// AES-256-CBC 解密（无填充；数据长度恒为 16 的倍数）
bool aesCbcDecrypt(const unsigned char* key, const unsigned char* iv,
                  const unsigned char* in, int inLen, unsigned char* out) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool ok = false;
    int outLen = 0, total = 0;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key, iv) != 1) break;
        EVP_CIPHER_CTX_set_padding(ctx, 0);   // 关闭填充
        if (EVP_DecryptUpdate(ctx, out, &outLen, in, inLen) != 1) break;
        total = outLen;
        if (EVP_DecryptFinal_ex(ctx, out + total, &outLen) != 1) break;
        total += outLen;
        ok = (total == inLen);
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

// 微信 3.x SQLCipher 密钥体系
struct DbKeys {
    unsigned char aesKey[kKeySize];
    unsigned char macKey[kKeySize];
    bool valid = false;
};

DbKeys deriveKeys(const QByteArray& keyHex, const unsigned char* salt, const DbParams& p) {
    DbKeys k;
    QByteArray hex = keyHex.trimmed();
    if (hex.size() != kKeySize * 2) return k;
    QByteArray raw = QByteArray::fromHex(hex);
    if (raw.size() != kKeySize) return k;

    // aes_key = PBKDF2-HMAC-{sha}(用户密钥, salt, iter, 32)
    if (PKCS5_PBKDF2_HMAC(raw.constData(), raw.size(), salt, 16,
                          p.kdfIter, p.hash, kKeySize, k.aesKey) != 1)
        return k;

    // mac_salt = salt XOR 0x3a
    unsigned char macSalt[16];
    for (int i = 0; i < 16; ++i) macSalt[i] = salt[i] ^ kMacSaltMask;

    // mac_key = PBKDF2-HMAC-{sha}(aes_key, mac_salt, 2, 32)
    if (PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(k.aesKey), kKeySize,
                          macSalt, 16, kMacIter, p.hash, kKeySize, k.macKey) != 1)
        return k;
    k.valid = true;
    return k;
}

// 校验首页 HMAC：HMAC-{sha}(mac_key, 首页[salt后..IV尾] + 页号LE32(1))
bool verifyFirstPage(const DbKeys& keys, const QByteArray& page1, const DbParams& p) {
    // page1: [salt 16][密文 ...][IV 16][HMAC hmacSize][pad?]
    const int contentEnd = kPageSize - p.reserve + 16;
    unsigned char pageNo[4] = {1, 0, 0, 0};
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int macLen = 0;
    HMAC_CTX* ctx = HMAC_CTX_new();
    if (!ctx) return false;
    bool ok = false;
    do {
        if (HMAC_Init_ex(ctx, keys.macKey, kKeySize, p.hash, nullptr) != 1) break;
        if (HMAC_Update(ctx, reinterpret_cast<const unsigned char*>(page1.constData() + 16),
                        contentEnd - 16) != 1) break;
        if (HMAC_Update(ctx, pageNo, 4) != 1) break;
        if (HMAC_Final(ctx, mac, &macLen) != 1) break;
        ok = (macLen == unsigned(p.hmacSize)) &&
             (memcmp(mac, page1.constData() + contentEnd, p.hmacSize) == 0);
    } while (false);
    HMAC_CTX_free(ctx);
    return ok;
}

// 用 3.x / 4.x 参数依次尝试首页验证；成功返回参数并输出密钥
const DbParams* detectParams(const QByteArray& keyHex, const QByteArray& page1,
                             DbKeys* keysOut) {
    static const DbParams v3 = v3Params();
    static const DbParams v4 = v4Params();
    const unsigned char* salt =
        reinterpret_cast<const unsigned char*>(page1.constData());
    for (const DbParams* p : {&v3, &v4}) {
        DbKeys k = deriveKeys(keyHex, salt, *p);
        if (k.valid && verifyFirstPage(k, page1, *p)) {
            if (keysOut) *keysOut = k;
            return p;
        }
    }
    return nullptr;
}

bool isAllZeros(const char* p, int n) {
    for (int i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}

} // namespace

// 消息预览文本：由类型生成（供会话列表与消息气泡共用）
QString previewOf(int type, int subType, const QString& content, const QString& compressed);

// ── 静态：密钥校验 ──────────────────────────────────────────────────────────

bool WeChatDb::verifyKey(const QString& keyHex, const QString& dbPath, QString* errOut) {
    QFile f(dbPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = "无法打开数据库文件（微信可能正在占用，请确认路径）";
        return false;
    }
    QByteArray page1 = f.read(kPageSize);
    f.close();
    if (page1.size() < kPageSize) {
        if (errOut) *errOut = "数据库文件太小或已损坏";
        return false;
    }
    if (page1.startsWith("SQLite format 3")) {
        if (errOut) *errOut = "该文件已是明文 SQLite（无需解密）";
        return false;
    }
    if (!detectParams(keyHex.toUtf8(), page1, nullptr)) {
        if (errOut) *errOut = "密钥校验失败：密钥不正确";
        return false;
    }
    return true;
}

// ── 静态：整库解密 ──────────────────────────────────────────────────────────

bool WeChatDb::decryptDatabase(const QString& keyHex, const QString& srcPath,
                               const QString& dstPath, QString* errOut) {
    QFile src(srcPath);
    if (!src.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = "无法打开源数据库（微信可能正在占用）";
        return false;
    }
    const QByteArray all = src.readAll();
    src.close();

    if (all.size() < kPageSize || all.size() % kPageSize != 0) {
        if (errOut) *errOut = "数据库文件大小异常（可能不是微信加密数据库）";
        return false;
    }

    // 探测版本参数（含首页 HMAC 校验，快速失败）
    DbKeys keys;
    const DbParams* prm = detectParams(keyHex.toUtf8(), all.left(kPageSize), &keys);
    if (!prm) {
        if (errOut) *errOut = "密钥校验失败：密钥不正确";
        return false;
    }
    const int reserve = prm->reserve;

    QFile dst(dstPath);
    if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errOut) *errOut = "无法创建解密输出文件（缓存目录不可写）";
        return false;
    }

    // 首页：SQLite 头 + 解密内容 + 保留区
    {
        const char* b = all.constData();
        QByteArray out;
        out.reserve(kPageSize);
        out.append(kSqliteHeader, 16);
        QByteArray content = QByteArray::fromRawData(b + 16, kPageSize - reserve - 16);
        QByteArray iv      = QByteArray::fromRawData(b + kPageSize - reserve, 16);
        QByteArray plain(content.size(), 0);
        if (!aesCbcDecrypt(keys.aesKey,
                           reinterpret_cast<const unsigned char*>(iv.constData()),
                           reinterpret_cast<const unsigned char*>(content.constData()),
                           content.size(),
                           reinterpret_cast<unsigned char*>(plain.data()))) {
            dst.close();
            if (errOut) *errOut = "首页解密失败";
            return false;
        }
        out.append(plain);
        out.append(b + kPageSize - reserve, reserve);
        dst.write(out);
    }

    // 后续页（全零页原样输出，不做解密）
    const int pageCount = all.size() / kPageSize;
    for (int i = 1; i < pageCount; ++i) {
        const char* b = all.constData() + i * kPageSize;
        if (isAllZeros(b, kPageSize)) {
            dst.write(b, kPageSize);
            continue;
        }
        QByteArray content = QByteArray::fromRawData(b, kPageSize - reserve);
        QByteArray iv     = QByteArray::fromRawData(b + kPageSize - reserve, 16);
        QByteArray plain(content.size(), 0);
        if (!aesCbcDecrypt(keys.aesKey,
                           reinterpret_cast<const unsigned char*>(iv.constData()),
                           reinterpret_cast<const unsigned char*>(content.constData()),
                           content.size(),
                           reinterpret_cast<unsigned char*>(plain.data()))) {
            dst.close();
            if (errOut) *errOut = QString("第 %1 页解密失败").arg(i + 1);
            return false;
        }
        dst.write(plain);
        dst.write(b + kPageSize - reserve, reserve);
    }
    dst.close();
    return true;
}

// ── 实例逻辑 ────────────────────────────────────────────────────────────────

WeChatDb::WeChatDb(const QString& accountId, const QString& dataDir, const QString& keyHex)
    : m_accountId(accountId), m_dataDir(dataDir), m_keyHex(keyHex) {
    m_cacheDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                 + "/wechat_cache/" + accountId;
    QDir().mkpath(m_cacheDir);
}

WeChatDb::~WeChatDb() = default;

bool WeChatDb::ensureDecrypted() {
    m_lastError.clear();
    m_namesLoaded = false;   // 版本可能变化，名称缓存失效
    if (m_keyHex.trimmed().size() != kKeySize * 2) {
        m_lastError = "未配置数据库密钥（64 位十六进制）";
        return false;
    }
    m_version = detectVersion();
    if (m_version == 0) {
        m_lastError = "数据目录中未找到微信数据库（3.x 需 Msg/MicroMsg.db；4.x 需 db_storage）";
        return false;
    }
    // 联系人库（3.x: MicroMsg.db / 4.x: contact.db）
    if (contactDbCache().isEmpty()) return false;
    // 会话库（3.x 与联系人库相同；4.x: session.db）
    if (m_version == 4 && QFile::exists(m_dataDir + "/db_storage/session/session.db")) {
        if (sessionDbCache().isEmpty()) return false;
    }
    // 消息库
    if (msgDbCaches().isEmpty()) {
        m_lastError = m_version == 3
                          ? "未找到消息数据库（Msg/Multi/MSG*.db）"
                          : "未找到消息数据库（db_storage/message）";
        return false;
    }
    return true;
}

int WeChatDb::detectVersion() {
    if (QFile::exists(m_dataDir + "/Msg/MicroMsg.db")) return 3;
    if (QFile::exists(m_dataDir + "/db_storage")) return 4;
    return 0;
}

QString WeChatDb::contactDbCache() {
    if (m_version == 3) return cachedDb(m_dataDir + "/Msg/MicroMsg.db");
    return cachedDb(m_dataDir + "/db_storage/contact/contact.db");
}

QString WeChatDb::sessionDbCache() {
    if (m_version == 3) return cachedDb(m_dataDir + "/Msg/MicroMsg.db");
    return cachedDb(m_dataDir + "/db_storage/session/session.db");
}

QStringList WeChatDb::msgDbCaches() {
    QStringList out;
    if (m_version == 3) {
        QDir multi(m_dataDir + "/Msg/Multi");
        const QStringList msgs = multi.entryList(QStringList() << "MSG*.db", QDir::Files);
        for (const QString& name : msgs)
            out << cachedDb(multi.absoluteFilePath(name));
    } else if (m_version == 4) {
        QDir d(m_dataDir + "/db_storage/message");
        const QStringList msgs = d.entryList(QStringList() << "message_*.db", QDir::Files);
        for (const QString& name : msgs)
            out << cachedDb(d.absoluteFilePath(name));
    }
    out.removeAll(QString());
    return out;
}

QString WeChatDb::cachedDb(const QString& srcPath) {
    const QFileInfo src(srcPath);
    const QString dst = m_cacheDir + "/" + src.fileName() + ".sqlite";

    // 源未更新且缓存存在 → 直接复用
    const QFileInfo cache(dst);
    if (cache.exists() && cache.lastModified() >= src.lastModified())
        return dst;

    QString err;
    if (!decryptDatabase(m_keyHex, srcPath, dst, &err)) {
        m_lastError = QString("解密 %1 失败: %2").arg(src.fileName(), err);
        return {};
    }
    Logger::instance().info(
        QString("微信数据库已解密: %1 -> %2").arg(src.fileName(), dst), "wechat");
    return dst;
}

// ── SQLite 查询辅助 ─────────────────────────────────────────────────────────

namespace {

// 打开库 → prepare → bind → step 收集结果（按列名无关，调用方按 SELECT 顺序取值）
QList<QVariantList> runQuery(const QString& dbPath, const QString& sql,
                             const QStringList& args, QString* errOut) {
    QList<QVariantList> rows;
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.toUtf8().constData(), &db) != SQLITE_OK) {
        if (errOut) *errOut = QString("打开数据库失败: %1").arg(dbPath);
        if (db) sqlite3_close(db);
        return rows;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
        for (int i = 0; i < args.size(); ++i)
            sqlite3_bind_text(stmt, i + 1, args[i].toUtf8().constData(), -1, SQLITE_TRANSIENT);
        const int ncols = sqlite3_column_count(stmt);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            QVariantList row;
            row.reserve(ncols);
            for (int c = 0; c < ncols; ++c) {
                switch (sqlite3_column_type(stmt, c)) {
                    case SQLITE_INTEGER: row << qint64(sqlite3_column_int64(stmt, c)); break;
                    case SQLITE_FLOAT:   row << sqlite3_column_double(stmt, c);       break;
                    case SQLITE_NULL:   row << QVariant();                           break;
                    default: {
                        // TEXT/BLOB 统一按原始字节读取（zstd 压缩内容打标记）
                        const char* data = static_cast<const char*>(sqlite3_column_blob(stmt, c));
                        const int n = sqlite3_column_bytes(stmt, c);
                        if (n >= 4 && memcmp(data, kZstdMagic, 4) == 0)
                            row << kZstdMark;
                        else
                            row << QString::fromUtf8(data, n);
                    }
                }
            }
            rows.append(row);
        }
    } else if (errOut) {
        *errOut = QString("查询失败: %1").arg(QString::fromUtf8(sqlite3_errmsg(db)));
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rows;
}

} // namespace

// 在指定解密库上执行查询
QList<QVariantList> WeChatDb::runOn(const QString& dbPath, const QString& sql,
                                    const QStringList& args) {
    if (dbPath.isEmpty()) return {};
    QString err;
    auto rows = runQuery(dbPath, sql, args, &err);
    if (!err.isEmpty()) m_lastError = err;
    return rows;
}

// ── 名称缓存 ────────────────────────────────────────────────────────────────

QString WeChatDb::displayName(const QString& wxid) {
    if (!m_namesLoaded) {
        m_namesLoaded = true;
        if (m_version == 3) {
            // 3.x Contact: UserName, NickName, Remark
            auto rows = runOn(contactDbCache(),
                "SELECT UserName, NickName, Remark FROM Contact", {});
            for (const auto& r : rows) {
                const QString id = r.value(0).toString();
                QString name = r.value(2).toString();          // 备注
                if (name.isEmpty()) name = r.value(1).toString(); // 昵称
                if (name.isEmpty()) name = id;
                m_nameCache[id] = name;
            }
            // Session.strNickName 兜底（部分联系人不在 Contact 表）
            auto ses = runOn(sessionDbCache(),
                "SELECT strUsrName, strNickName FROM Session", {});
            for (const auto& r : ses) {
                const QString id = r.value(0).toString();
                if (!m_nameCache.contains(id) || m_nameCache.value(id).toString() == id) {
                    const QString nick = r.value(1).toString();
                    if (!nick.isEmpty()) m_nameCache[id] = nick;
                }
            }
        } else {
            // 4.x contact 表: username, nick_name, remark
            auto rows = runOn(contactDbCache(),
                "SELECT username, nick_name, remark FROM contact", {});
            for (const auto& r : rows) {
                const QString id = r.value(0).toString();
                QString name = r.value(2).toString();          // 备注
                if (name.isEmpty()) name = r.value(1).toString(); // 昵称
                if (name.isEmpty()) name = id;
                m_nameCache[id] = name;
            }
        }
    }
    return m_nameCache.value(wxid, wxid).toString();
}

QString WeChatDb::selfDisplayName() {
    // 数据目录名即本人 wxid（4.x 目录名带实例后缀：wxid_xxx_1ffd）
    QString wxid = QFileInfo(m_dataDir).fileName();
    static const QRegularExpression v4SuffixRe(
        "^(wxid_[a-zA-Z0-9]+)_[0-9a-fA-F]{4}$");
    const auto m = v4SuffixRe.match(wxid);
    if (m.hasMatch()) wxid = m.captured(1);
    return displayName(wxid);
}

// ── 会话列表 ────────────────────────────────────────────────────────────────

QList<WeChatDb::ChatSession> WeChatDb::loadSessions() {
    QList<ChatSession> out;
    if (m_version == 4) {
        // 4.x session.db: SessionTable
        auto rows = runOn(sessionDbCache(),
            "SELECT username, summary, last_timestamp, last_msg_type, last_msg_sub_type "
            "FROM SessionTable ORDER BY sort_timestamp DESC", {});
        for (const auto& r : rows) {
            ChatSession s;
            s.talker = r.value(0).toString();
            const QString raw = r.value(1).toString();
            const qint64 ts = r.value(2).toLongLong();
            const int type = r.value(3).toInt();
            const int subType = r.value(4).toInt();
            s.lastTime = QDateTime::fromSecsSinceEpoch(ts);
            s.isChatRoom = s.talker.endsWith("@chatroom");
            s.title = displayName(s.talker);
            s.lastMsg = previewOf(type, subType, raw, QString());
            out.append(s);
        }
        return out;
    }
    // 3.x MicroMsg.db Session 表: strUsrName, nUnReadCount, strContent, nTime, nMsgType, Reserved2(子类型)
    auto rows = runOn(sessionDbCache(),
        "SELECT strUsrName, nUnReadCount, strContent, nTime, nMsgType, Reserved2 "
        "FROM Session ORDER BY nTime DESC", {});
    for (const auto& r : rows) {
        ChatSession s;
        s.talker = r.value(0).toString();
        s.unread = r.value(1).toInt();
        const QString raw = r.value(2).toString();
        const qint64 ts  = r.value(3).toLongLong();
        const int type    = r.value(4).toInt();
        const int subType = r.value(5).toInt();
        s.lastTime = QDateTime::fromSecsSinceEpoch(ts);
        s.isChatRoom = s.talker.endsWith("@chatroom");
        s.title = displayName(s.talker);
        s.lastMsg = previewOf(type, subType, raw, QString());
        out.append(s);
    }
    return out;
}

// ── 聊天记录 ─────────────────────────────────────────────────────────────────

QList<WeChatDb::ChatMessage> WeChatDb::loadMessages(const QString& talker, int limit) {
    QList<ChatMessage> out;
    const bool isRoom = talker.endsWith("@chatroom");

    if (m_version == 4) {
        // 4.x: 每个会话独立表 Msg_<md5(talker)>，real_sender_id 需 join Name2Id
        const QString tableName = "Msg_" + QString::fromLatin1(
            QCryptographicHash::hash(talker.toUtf8(), QCryptographicHash::Md5).toHex());
        const QStringList dbs = msgDbCaches();
        // 表存在性检查（任一消息库命中即可）
        bool exists = false;
        for (const QString& db : dbs) {
            auto chk = runOn(db,
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ?1",
                {tableName});
            if (!chk.isEmpty()) { exists = true; break; }
        }
        if (!exists) return out;

        QString sql = QString(
            "SELECT m.sort_seq, m.server_id, m.local_type, IFNULL(n.user_name,''), "
            "m.create_time, m.status, m.message_content "
            "FROM %1 m LEFT JOIN Name2Id n ON m.real_sender_id = n.rowid "
            "ORDER BY m.sort_seq DESC").arg(tableName);
        if (limit > 0) sql += QString(" LIMIT %1").arg(limit);
        for (const QString& db : dbs) {
            auto rows = runOn(db, sql, {});
            for (const auto& r : rows) {
                ChatMessage m;
                m.msgId = r.value(1).toLongLong();
                m.talker = talker;
                m.type = r.value(2).toInt();
                const QString sender = r.value(3).toString();
                const qint64 ts = r.value(4).toLongLong();
                const int status = r.value(5).toInt();
                QString content = r.value(6).toString();
                m.time = QDateTime::fromSecsSinceEpoch(ts);
                // status==2 表示已发送（自己）；单聊时发送者≠对方也视为自己
                m.isSender = (status == 2) || (!isRoom && !sender.isEmpty() && sender != talker);
                // 群聊消息：内容形如 "wxid_xxx:\n内容"
                if (isRoom && !m.isSender && content.contains(":\n")) {
                    const int idx = content.indexOf(":\n");
                    m.senderId = content.left(idx);
                    content = content.mid(idx + 2);
                }
                if (m.senderId.isEmpty()) m.senderId = sender;
                m.senderName = displayName(m.senderId);
                m.content = content;
                m.display = previewOf(m.type, 0, content, QString());
                out.append(m);
            }
        }
        // 时间升序
        std::sort(out.begin(), out.end(),
                  [](const ChatMessage& a, const ChatMessage& b) { return a.time < b.time; });
        return out;
    }

    // 3.x: MSG 表按 StrTalker 查询（时间倒序取最近 limit 条再反转）
    QString sql =
        "SELECT localId, MsgSvrID, StrTalker, Type, SubType, IsSender, CreateTime, "
        "StrContent, CompressContent FROM MSG WHERE StrTalker = ?1 ORDER BY CreateTime DESC";
    if (limit > 0) sql += QString(" LIMIT %1").arg(limit);
    for (const QString& db : msgDbCaches()) {
        auto rows = runOn(db, sql, {talker});
        for (const auto& r : rows) {
            ChatMessage m;
            m.msgId = r.value(1).toLongLong();
            m.talker = r.value(2).toString();
            m.type = r.value(3).toInt();
            m.subType = r.value(4).toInt();
            m.isSender = r.value(5).toInt() != 0;
            m.time = QDateTime::fromSecsSinceEpoch(r.value(6).toLongLong());
            QString content = r.value(7).toString();
            const QString compressed = r.value(8).toString();

            // 群聊消息：StrContent 形如 "wxid_xxx:\n内容"
            if (isRoom && !m.isSender && content.contains(":\n")) {
                const int idx = content.indexOf(":\n");
                m.senderId = content.left(idx);
                content = content.mid(idx + 2);
            }
            if (!m.senderId.isEmpty())
                m.senderName = displayName(m.senderId);

            m.content = content;
            m.display = previewOf(m.type, m.subType, content, compressed);
            out.append(m);
        }
    }

    // 时间升序
    std::sort(out.begin(), out.end(),
              [](const ChatMessage& a, const ChatMessage& b) { return a.time < b.time; });
    return out;
}

// ── 联系人 ───────────────────────────────────────────────────────────────────

QList<WeChatDb::Contact> WeChatDb::loadContacts() {
    QList<Contact> out;
    if (m_version == 4) {
        // 4.x contact.db: contact 表
        auto rows = runOn(contactDbCache(),
            "SELECT username, alias, nick_name, remark, local_type FROM contact", {});
        for (const auto& r : rows) {
            Contact c;
            c.userName = r.value(0).toString();
            c.alias = r.value(1).toString();
            c.nickname = r.value(2).toString();
            c.remark = r.value(3).toString();
            c.type = r.value(4).toInt();
            c.isChatRoom = c.userName.endsWith("@chatroom");
            c.display = c.remark.isEmpty()
                            ? (c.nickname.isEmpty() ? c.userName : c.nickname)
                            : c.remark;
            out.append(c);
        }
        std::sort(out.begin(), out.end(),
                  [](const Contact& a, const Contact& b) { return a.display < b.display; });
        return out;
    }
    auto rows = runOn(contactDbCache(),
        "SELECT UserName, Alias, NickName, Remark, Type, VerifyFlag FROM Contact "
        "WHERE DelFlag = 0", {});
    for (const auto& r : rows) {
        Contact c;
        c.userName = r.value(0).toString();
        c.alias = r.value(1).toString();
        c.nickname = r.value(2).toString();
        c.remark = r.value(3).toString();
        c.type = r.value(4).toInt();
        c.verifyFlag = r.value(5).toInt();
        c.isChatRoom = c.userName.endsWith("@chatroom");
        c.display = c.remark.isEmpty() ? (c.nickname.isEmpty() ? c.userName : c.nickname)
                                       : c.remark;
        // 过滤特殊系统账号
        if (c.userName.startsWith("gh_") && !c.nickname.isEmpty()) {
            // 公众号保留
        } else if (c.userName.contains("@app") || c.userName == "weixin" ||
                   c.userName.startsWith("filehelper")) {
            // 保留文件传输助手等
        }
        out.append(c);
    }
    std::sort(out.begin(), out.end(),
              [](const Contact& a, const Contact& b) { return a.display < b.display; });
    return out;
}

// 单联系人详细信息（详情页用）
// 3.x: Contact 表带 Province/City/Signature/Sex/BigHeadImgUrl/SmallHeadImgUrl
// 4.x: contact 表带 big_head_url/small_head_url；signature/province/city/sex 在 extra_buffer 里（暂不解析）
WeChatDb::ContactDetail WeChatDb::loadContactDetail(const QString& wxid) {
    ContactDetail d;
    d.userName = wxid;
    d.display  = wxid;

    if (wxid.isEmpty()) return d;

    if (m_version == 4) {
        // 4.x contact.db
        auto rows = runOn(contactDbCache(),
            "SELECT username, alias, nick_name, remark, local_type, "
            "big_head_url, small_head_url "
            "FROM contact WHERE username = ? LIMIT 1",
            {wxid});
        if (rows.isEmpty()) return d;
        const auto& r = rows.first();
        d.userName     = r.value(0).toString();
        d.alias        = r.value(1).toString();
        d.nickname     = r.value(2).toString();
        d.remark       = r.value(3).toString();
        d.type         = r.value(4).toInt();
        d.bigHeadUrl   = r.value(5).toString();
        d.smallHeadUrl = r.value(6).toString();
        d.isChatRoom   = d.userName.endsWith("@chatroom");
        d.display      = d.remark.isEmpty()
                            ? (d.nickname.isEmpty() ? d.userName : d.nickname)
                            : d.remark;
        return d;
    }

    // 3.x MicroMsg.db 的 Contact 表
    auto rows = runOn(contactDbCache(),
        "SELECT UserName, Alias, NickName, Remark, Type, VerifyFlag, "
        "Province, City, Signature, Sex, BigHeadImgUrl, SmallHeadImgUrl "
        "FROM Contact WHERE UserName = ? LIMIT 1",
        {wxid});
    if (rows.isEmpty()) return d;
    const auto& r = rows.first();
    d.userName     = r.value(0).toString();
    d.alias        = r.value(1).toString();
    d.nickname     = r.value(2).toString();
    d.remark       = r.value(3).toString();
    d.type         = r.value(4).toInt();
    d.verifyFlag   = r.value(5).toInt();
    d.province     = r.value(6).toString();
    d.city         = r.value(7).toString();
    d.signature    = r.value(8).toString();
    d.sex          = r.value(9).toInt();
    d.bigHeadUrl   = r.value(10).toString();
    d.smallHeadUrl = r.value(11).toString();
    d.isChatRoom   = d.userName.endsWith("@chatroom");
    d.display      = d.remark.isEmpty()
                        ? (d.nickname.isEmpty() ? d.userName : d.nickname)
                        : d.remark;
    return d;
}

// ── 群成员 ───────────────────────────────────────────────────────────────────

// 4.x 群成员存储在 chat_room.ext_buffer（protobuf RoomData）
// RoomData { repeated RoomDataUser users = 1; }
// RoomDataUser { string userName = 1; string displayName = 2; ... }
static QStringList parseRoomData(const QByteArray& buf) {
    QStringList members;
    int pos = 0;
    auto readVarint = [&](int& p, quint64& out) -> bool {
        quint64 v = 0;
        int shift = 0;
        while (p < buf.size()) {
            const quint8 b = quint8(buf[p++]);
            v |= quint64(b & 0x7F) << shift;
            if (!(b & 0x80)) { out = v; return true; }
            shift += 7;
            if (shift > 63) return false;
        }
        return false;
    };
    while (pos < buf.size()) {
        quint64 tag = 0;
        if (!readVarint(pos, tag)) break;
        const quint64 field = tag >> 3;
        const quint64 wt = tag & 7;
        if (field == 1 && wt == 2) {   // RoomDataUser
            quint64 len = 0;
            if (!readVarint(pos, len)) break;
            const QByteArray user = buf.mid(pos, int(len));
            pos += int(len);
            // 解析 RoomDataUser 内 field 1 (userName)
            int up = 0;
            while (up < user.size()) {
                quint64 utag = 0;
                if (!readVarint(up, utag)) break;
                const quint64 uf = utag >> 3;
                const quint64 uwt = utag & 7;
                if (uwt == 2) {
                    quint64 ulen = 0;
                    if (!readVarint(up, ulen)) break;
                    const QByteArray val = user.mid(up, int(ulen));
                    up += int(ulen);
                    if (uf == 1 && !val.isEmpty())
                        members << QString::fromUtf8(val);
                } else if (uwt == 0) {
                    quint64 v = 0;
                    if (!readVarint(up, v)) break;
                } else if (uwt == 5) up += 4;
                else if (uwt == 1) up += 8;
                else break;
            }
        } else if (wt == 2) {           // 跳过其他 length-delimited 字段
            quint64 len = 0;
            if (!readVarint(pos, len)) break;
            pos += int(len);
        } else if (wt == 0) {
            quint64 v = 0;
            if (!readVarint(pos, v)) break;
        } else if (wt == 5) pos += 4;
        else if (wt == 1) pos += 8;
        else break;
    }
    return members;
}

QStringList WeChatDb::chatRoomMembers(const QString& chatRoomId) {
    QStringList members;
    if (m_version == 4) {
        // 4.x: chat_room 表 ext_buffer（protobuf）
        auto rows = runOn(contactDbCache(),
            "SELECT ext_buffer FROM chat_room WHERE username = ?1", {chatRoomId});
        if (!rows.isEmpty()) {
            const QByteArray buf = rows.first().value(0).toByteArray();
            members = parseRoomData(buf);
        }
        return members;
    }
    auto rows = runOn(contactDbCache(),
        "SELECT UserNameList FROM ChatRoom WHERE ChatRoomName = ?1", {chatRoomId});
    if (!rows.isEmpty()) {
        const QString list = rows.first().value(0).toString();
        members = list.split(';', Qt::SkipEmptyParts);
    }
    return members;
}

// ── 消息预览文本 ─────────────────────────────────────────────────────────────

// 去除 XML 标签（系统消息常为 XML 片段）
static QString stripXml(const QString& s) {
    QString out = s;
    static const QRegularExpression re("<[^>]*>");
    out.remove(re);
    return out.simplified();
}

// 由类型生成预览（供会话列表与消息气泡共用）
QString previewOf(int type, int subType, const QString& content, const QString& compressed) {
    Q_UNUSED(compressed);
    switch (type) {
        case 1:   // 文本
            if (content == kZstdMark) return "[压缩内容]";
            return stripXml(content);
        case 3:   return "[图片]";
        case 34:  return "[语音]";
        case 42:  return "[名片]";
        case 43:  return "[视频]";
        case 47:  return "[动画表情]";
        case 48:  return "[位置]";
        case 49:  // XML 复合消息
            switch (subType) {
                case 4:  return "[文件]";
                case 5:  return "[链接]";
                case 6:  return "[音乐]";
                case 8:  return "[用户名片]";
                case 19: return "[聊天记录]";
                case 33:
                case 36: return "[小程序]";
                case 57: return "[引用消息]";
                case 63: return "[视频号]";
                case 87: return "[表情]";
                case 88: return "[公众号文章]";
                case 2000: return "[转账]";
                case 2003: return "[礼物]";
                default: return "[链接消息]";
            }
        case 50:  return "[语音/视频通话]";
        case 10000: // 系统消息（撤回、拍一拍、入群等）
            return stripXml(content);
        case 10002: return "[系统通知]";
        default:
            return QString("[类型 %1 消息]").arg(type);
    }
}
