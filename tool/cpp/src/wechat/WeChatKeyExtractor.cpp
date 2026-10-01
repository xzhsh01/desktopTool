#include "WeChatKeyExtractor.h"
#include "WeChatDb.h"
#include "core/Logger.h"

#include <QByteArray>
#include <QFile>
#include <QSet>

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>

namespace {

// v4：密钥引用结构特征（小端 64 位）：{ 0, 0x20, 0x2F }，其前 8 字节是指向密钥的指针
const char kV4Pattern[24] = {
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x20', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x2F', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
};

// v3：64 位进程特征（长度字段 0x20）；32 位进程截取前 4 字节
const char kV3Pattern64[8] = {
    '\x20', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
};

// SQLCipher 密钥为 32 字节高熵随机数据；排除文本（hex 字符串/protobuf 描述符）
// 与低熵数据（指针数组/重复模式），避免海量误报进入昂贵的 PBKDF2 验证
// 阈值与社区实现（WeChatDataAnalysis）对齐：distinct >= 15，printable <= 24
bool looksLikeRandomKey(const unsigned char* key) {
    bool seen[256] = {};
    int distinct = 0, printable = 0;
    for (int i = 0; i < 32; ++i) {
        if (!seen[key[i]]) { seen[key[i]] = true; ++distinct; }
        if (key[i] >= 0x20 && key[i] <= 0x7e) ++printable;
    }
    return distinct >= 15 && printable <= 24;
}

// 在 buf 中从后往前搜索 pattern，取每个匹配【之前】ptrSize 字节为指针
// （结构布局：{ ptr→key, 0, 0x20, 0x2F }，指针位于特征之前）
// 解引用读 32 字节密钥（按指针地址去重）加入候选
void collectCandidates(HANDLE proc, const QByteArray& buf, const QByteArray& pattern,
                       int ptrSize, QSet<quint64>& seen, QStringList& keys) {
    const int patLen = pattern.size();
    qsizetype idx = buf.size() - patLen;
    while (idx >= ptrSize) {
        idx = buf.lastIndexOf(pattern, idx);
        if (idx < ptrSize) break;  // 未找到或太靠前（放不下指针）
        quint64 ptr = 0;
        memcpy(&ptr, buf.constData() + idx - ptrSize, ptrSize);
        if (ptr > 0x10000 && ptr < 0x7FFFFFFFFFFFULL && !seen.contains(ptr)) {
            seen.insert(ptr);
            unsigned char key[32] = {};
            SIZE_T read = 0;
            if (ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(ptr), key, 32, &read)
                && read == 32 && looksLikeRandomKey(key)) {
                keys.append(QByteArray(reinterpret_cast<const char*>(key), 32).toHex());
            }
        }
        --idx;
    }
}

// ── v4：扫描进程私有可读写内存 ─────────────────────────────────────────────
QStringList extractV4(HANDLE proc) {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    quint64 addr = std::max<quint64>(0x10000,
                                     reinterpret_cast<quint64>(si.lpMinimumApplicationAddress));
    const quint64 limit = std::min<quint64>(0x7FFFFFFFFFFFULL,
                                            reinterpret_cast<quint64>(si.lpMaximumApplicationAddress));
    const QByteArray pattern(kV4Pattern, sizeof(kV4Pattern));

    QStringList keys;
    QSet<quint64> seen;
    while (addr < limit) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQueryEx(proc, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
            break;
        const quint64 regionEnd = reinterpret_cast<quint64>(mbi.BaseAddress) + mbi.RegionSize;

        if (mbi.State == MEM_COMMIT
                && mbi.Type == MEM_PRIVATE
                && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            QByteArray buf(static_cast<int>(mbi.RegionSize), Qt::Uninitialized);
            SIZE_T read = 0;
            if (ReadProcessMemory(proc, mbi.BaseAddress, buf.data(),
                                  static_cast<SIZE_T>(mbi.RegionSize), &read)
                && read > 0) {
                buf.resize(static_cast<int>(read));
                collectCandidates(proc, buf, pattern, 8, seen, keys);
            }
        }
        addr = regionEnd;
    }
    return keys;
}

// ── v3：扫描 WeChatWin.dll 模块可写段 ──────────────────────────────────────
QStringList extractV3(HANDLE proc, quint32 pid) {
    // 定位 WeChatWin.dll 模块
    quint64 base = 0, size = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = {};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (QString::fromWCharArray(me.szModule)
                        .compare(QStringLiteral("WeChatWin.dll"), Qt::CaseInsensitive) == 0) {
                    base = reinterpret_cast<quint64>(me.modBaseAddr);
                    size = me.modBaseSize;
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    if (!base) return {};

    // 目标进程位数（WOW64 → 32 位微信）
    BOOL wow64 = FALSE;
    IsWow64Process(proc, &wow64);
    const bool is64 = !wow64;
    const QByteArray pattern(kV3Pattern64, is64 ? 8 : 4);
    const int ptrSize = is64 ? 8 : 4;

    const quint64 end = base + size;
    quint64 addr = base;
    const DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY
                            | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    QStringList keys;
    QSet<quint64> seen;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQueryEx(proc, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
            break;
        const quint64 regionEnd = reinterpret_cast<quint64>(mbi.BaseAddress) + mbi.RegionSize;

        if (mbi.State == MEM_COMMIT && (mbi.Protect & kWritable)
            && mbi.RegionSize >= 100 * 1024) {
            quint64 rSize = mbi.RegionSize;
            if (addr + rSize > end) rSize = end - addr;
            QByteArray buf(static_cast<int>(rSize), Qt::Uninitialized);
            SIZE_T read = 0;
            if (ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(addr), buf.data(),
                                  static_cast<SIZE_T>(rSize), &read)
                && read > 0) {
                buf.resize(static_cast<int>(read));
                collectCandidates(proc, buf, pattern, ptrSize, seen, keys);
            }
        }
        addr = regionEnd;
    }
    return keys;
}

// ── v5（4.1.10.31+）：Weixin.dll 代码段 XOR key 提取 ────────────────────────

// 定位进程加载的 Weixin.dll 完整路径
QString findWeixinDllPath(quint32 pid) {
    QString path;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = {};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (QString::fromWCharArray(me.szModule)
                        .compare(QStringLiteral("Weixin.dll"), Qt::CaseInsensitive) == 0) {
                    path = QString::fromWCharArray(me.szExePath);
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    return path;
}

// 匹配混淆桩特征：
//   mov rdx,imm64 (48 BA) + gap[3..8] ×4，第 4 个 gap 后是 test rax,rax (48 85 C0)
// p 指向首个 48 BA；成功时 imm[4] 返回 4 个 imm64 的偏移
bool matchObfuscationStub(const unsigned char* d, qsizetype size, qsizetype p,
                          qsizetype imm[4]) {
    qsizetype q = p;
    for (int k = 0; k < 4; ++k) {
        if (q + 10 > size || d[q] != 0x48 || d[q + 1] != 0xBA) return false;
        imm[k] = q + 2;
        q += 10;
        qsizetype next = -1;
        for (int g = 3; g <= 8; ++g) {
            if (q + g + 2 >= size) break;
            if (d[q + g] != 0x48) continue;
            if (k < 3) {
                if (d[q + g + 1] == 0xBA) { next = q + g; break; }
            } else if (d[q + g + 1] == 0x85 && d[q + g + 2] == 0xC0) {
                next = q + g;
                break;
            }
        }
        if (next < 0) return false;
        q = next;
    }
    return true;
}

// 解析 PE，在所有可执行段中搜索特征，提取 32 字节 XOR key（hex 去重）
QStringList extractXorKeysFromDllFile(const QString& dllPath) {
    QStringList keys;
    QFile f(dllPath);
    if (!f.open(QIODevice::ReadOnly)) return keys;

    const QByteArray hdr = f.read(0x2000);
    const auto rd16 = [&hdr](int off) -> quint16 {
        quint16 v = 0;
        memcpy(&v, hdr.constData() + off, 2);
        return v;
    };
    const auto rd32 = [&hdr](int off) -> quint32 {
        quint32 v = 0;
        memcpy(&v, hdr.constData() + off, 4);
        return v;
    };
    if (hdr.size() < 0x40 || memcmp(hdr.constData(), "MZ", 2) != 0) return keys;
    const quint32 peOff = rd32(0x3C);
    if (peOff + 24 > static_cast<quint32>(hdr.size())
            || memcmp(hdr.constData() + peOff, "PE\0\0", 4) != 0) {
        return keys;
    }
    const int numSections = rd16(peOff + 6);
    const int optSize = rd16(peOff + 20);
    const int secTable = peOff + 24 + optSize;
    if (secTable + numSections * 40 > hdr.size()) return keys;

    QSet<QString> seen;
    for (int s = 0; s < numSections; ++s) {
        const int sh = secTable + s * 40;
        const quint32 characteristics = rd32(sh + 36);
        if (!(characteristics & 0x20000000)) continue;  // 仅可执行段
        const quint32 rawSize = rd32(sh + 16);
        const quint32 rawOff = rd32(sh + 20);
        if (!rawSize || rawOff + rawSize > static_cast<quint32>(f.size())) continue;

        f.seek(rawOff);
        const QByteArray sec = f.read(rawSize);
        if (sec.size() < 85) continue;
        const unsigned char* d = reinterpret_cast<const unsigned char*>(sec.constData());

        for (qsizetype p = 0; p + 85 <= sec.size(); ++p) {
            if (d[p] != 0x48 || d[p + 1] != 0xBA) continue;
            qsizetype imm[4] = {};
            if (!matchObfuscationStub(d, sec.size(), p, imm)) continue;
            QByteArray key(32, Qt::Uninitialized);
            for (int k = 0; k < 4; ++k) memcpy(key.data() + k * 8, d + imm[k], 8);
            const QString hex = QString::fromLatin1(key.toHex());
            if (!seen.contains(hex)) {
                seen.insert(hex);
                keys.append(hex);
            }
            p = imm[3] + 8;  // 跳过已匹配部分
        }
    }
    return keys;
}

// 两个 32 字节 hex 值逐字节 XOR，返回 hex；长度不符返回空串
QString xorHexKeys(const QString& aHex, const QString& bHex) {
    const QByteArray a = QByteArray::fromHex(aHex.toLatin1());
    const QByteArray b = QByteArray::fromHex(bHex.toLatin1());
    if (a.size() != 32 || b.size() != 32) return QString();
    QByteArray r(32, Qt::Uninitialized);
    for (int i = 0; i < 32; ++i) r[i] = a[i] ^ b[i];
    return QString::fromLatin1(r.toHex());
}

} // namespace

namespace WeChatKeyExtractor {

QList<ProcessInfo> findRunningWeChat() {
    QList<ProcessInfo> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const QString name = QString::fromWCharArray(pe.szExeFile).toLower();
            if (name == QStringLiteral("weixin.exe")) {
                result.append({pe.th32ProcessID, pe.th32ParentProcessID, 4,
                               QStringLiteral("Weixin.exe")});
            } else if (name == QStringLiteral("wechat.exe")) {
                result.append({pe.th32ProcessID, pe.th32ParentProcessID, 3,
                               QStringLiteral("WeChat.exe")});
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    // 主进程（父进程不是微信进程）优先，其次 4.x 优先
    QSet<quint32> pids;
    for (const auto& p : result) pids.insert(p.pid);
    std::stable_sort(result.begin(), result.end(),
                     [&](const ProcessInfo& a, const ProcessInfo& b) {
                         const bool mainA = !pids.contains(a.parentPid);
                         const bool mainB = !pids.contains(b.parentPid);
                         if (mainA != mainB) return mainA;
                         return a.version > b.version;
                     });
    return result;
}

QList<quint32> findAllWeChatRelatedPids() {
    QList<quint32> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const QString name = QString::fromWCharArray(pe.szExeFile).toLower();
            if (name == QStringLiteral("weixin.exe") ||
                name == QStringLiteral("wechatappex.exe") ||
                name == QStringLiteral("wechat.exe")) {
                if (!result.contains(pe.th32ProcessID)) {
                    result.append(pe.th32ProcessID);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

QStringList extractAllKeys(quint32 pid, int version, QString* errOut) {
    HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) {
        if (errOut) *errOut = QStringLiteral("无法打开微信进程（PID %1），请以管理员身份运行").arg(pid);
        return {};
    }
    const QStringList keys = version == 4 ? extractV4(proc) : extractV3(proc, pid);
    CloseHandle(proc);
    if (keys.isEmpty() && errOut) {
        *errOut = QStringLiteral("未在微信进程内存中找到密钥结构");
    }
    Logger::instance().info(
        QStringLiteral("wechat key scan: pid=%1 v=%2 candidates=%3")
            .arg(pid).arg(version).arg(keys.size()), "wechat");
    return keys;
}

QStringList extractXorKeys(quint32 pid, QString* errOut) {
    const QString dllPath = findWeixinDllPath(pid);
    if (dllPath.isEmpty()) {
        if (errOut) *errOut = QStringLiteral("未找到进程（PID %1）加载的 Weixin.dll").arg(pid);
        return {};
    }
    const QStringList keys = extractXorKeysFromDllFile(dllPath);
    Logger::instance().info(
        QStringLiteral("wechat dll xor scan: pid=%1 dll=%2 xorKeys=%3")
            .arg(pid).arg(dllPath).arg(keys.size()), "wechat");
    if (keys.isEmpty() && errOut) {
        *errOut = QStringLiteral("未能从 %1 提取 XOR key（特征可能已变化）").arg(dllPath);
    }
    return keys;
}

QString extractKey(quint32 pid, int version, const QString& dbPath, QString* errOut) {
    if (dbPath.isEmpty() || !QFile::exists(dbPath)) {
        if (errOut) *errOut = QStringLiteral("验证数据库不存在：%1").arg(dbPath);
        return QString();
    }
    QString err;
    const QStringList candidates = extractAllKeys(pid, version, &err);
    // 第一轮：直接验证（≤4.0.x 明文密钥驻留内存）
    for (const QString& key : candidates) {
        if (WeChatDb::verifyKey(key, dbPath)) return key;
    }
    // 第二轮：XOR 还原验证（4.1.10.31+ 密钥被混淆，需 DLL 中的 XOR key 还原）
    if (version == 4 && !candidates.isEmpty()) {
        const QStringList xorKeys = extractXorKeys(pid);
        Logger::instance().info(
            QStringLiteral("wechat v5 xor scan: pid=%1 candidates=%2 xorKeys=%3")
                .arg(pid).arg(candidates.size()).arg(xorKeys.size()), "wechat");
        for (const QString& xk : xorKeys) {
            for (const QString& c : candidates) {
                const QString pass = xorHexKeys(c, xk);
                if (!pass.isEmpty() && WeChatDb::verifyKey(pass, dbPath)) {
                    Logger::instance().success(
                        QStringLiteral("wechat v5 key recovered: pid=%1").arg(pid), "wechat");
                    return pass;
                }
            }
        }
    }
    if (errOut) *errOut = err;
    return QString();
}

QString extractFromRunningWeChat(const QString& dbPath, QString* errOut) {
    if (dbPath.isEmpty() || !QFile::exists(dbPath)) {
        if (errOut) *errOut = QStringLiteral("验证数据库不存在：%1").arg(dbPath);
        return QString();
    }
    const QList<ProcessInfo> procs = findRunningWeChat();
    if (procs.isEmpty()) {
        if (errOut) *errOut = QStringLiteral("未检测到运行中的微信，请先登录微信");
        return QString();
    }
    // 先在所有进程上收集候选（不逐进程 verify），再统一验证——提升 O(候选) 体验
    QStringList allCandidates;
    for (const ProcessInfo& p : procs) {
        const QStringList keys = extractAllKeys(p.pid, p.version);
        for (const QString& k : keys) {
            if (!allCandidates.contains(k)) allCandidates.append(k);
        }
    }
    // 第一轮：直接验证（≤4.0.x）
    for (const QString& c : allCandidates) {
        if (WeChatDb::verifyKey(c, dbPath)) return c;
    }
    // 第二轮：XOR 还原验证（4.1.10.31+）
    if (!allCandidates.isEmpty()) {
        QStringList xorKeys;
        for (const ProcessInfo& p : procs) {
            if (p.version != 4) continue;
            const QStringList xks = extractXorKeys(p.pid);
            for (const QString& x : xks) {
                if (!xorKeys.contains(x)) xorKeys.append(x);
            }
        }
        Logger::instance().info(
            QStringLiteral("wechat v5 xor scan: candidates=%1 xorKeys=%2")
                .arg(allCandidates.size()).arg(xorKeys.size()), "wechat");
        for (const QString& xk : xorKeys) {
            for (const QString& c : allCandidates) {
                const QString pass = xorHexKeys(c, xk);
                if (!pass.isEmpty() && WeChatDb::verifyKey(pass, dbPath)) {
                    Logger::instance().success("wechat v5 key recovered", "wechat");
                    return pass;
                }
            }
        }
        if (errOut) {
            *errOut = QStringLiteral(
                "已收集 %1 个候选密钥，直接验证与 XOR 还原验证（4.1.10.31+ 方案）均未通过；"
                "请确认微信已登录目标账号，且本工具以管理员身份运行")
                .arg(allCandidates.size());
        }
        return QString();
    }
    if (errOut) {
        if (allCandidates.isEmpty()) {
            *errOut = QStringLiteral(
                "未在微信进程内存中找到密钥结构（请确认已登录微信，并以管理员身份运行）");
        } else {
            *errOut = QStringLiteral(
                "已从进程内存中收集到 %1 个候选密钥但均无法解密当前数据库；"
                "可能是微信 4.1.x 及更新版本调整了密钥保护方式，自动提取暂不支持，"
                "请从其他渠道获取密钥后手动填写").arg(allCandidates.size());
        }
    }
    return QString();
}

// ── V2 图片 AES-128-ECB key 提取 ─────────────────────────────────────────────

#include <openssl/evp.h>
#include <openssl/aes.h>

// AES-128-ECB 解密单块 16 字节。
// 用 OpenSSL low-level AES_set_decrypt_key + AES_decrypt（单块调用），
// 比 EVP_CIPHER_CTX 完整 init/update/final 快 ~10×（EVP 每块要重新做 ctx 分配与 key schedule）。
static inline bool aesEcbDecryptBlock(const unsigned char key[16],
                                       const unsigned char in[16],
                                       unsigned char out[16]) {
    AES_KEY k;
    AES_set_decrypt_key(key, 128, &k);
    AES_decrypt(in, out, &k);
    return true;
}

// 判断 16 字节明文是否像图片 magic（严格版：要求 4+ 字节特征 + 合理字段）
static bool looksLikeImagePlain(const unsigned char p[16]) {
    // JPEG: FF D8 FF + E0/E1/DB/EE 或 C0~CF（SOI + marker）
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) {
        const unsigned char m = p[3];
        if (m == 0xE0 || m == 0xE1 || m == 0xE2 || m == 0xE3 ||
            m == 0xDB || m == 0xEE ||
            m == 0xC0 || m == 0xC1 || m == 0xC2 || m == 0xC3 ||
            m == 0xC4 || m == 0xC5 || m == 0xC6 || m == 0xC7 ||
            m == 0xC8 || m == 0xC9 || m == 0xCA || m == 0xCB ||
            m == 0xCC || m == 0xCD || m == 0xCE || m == 0xCF) return true;
        return false;
    }
    // PNG: 8 字节 magic + IHDR 长度 (0x00 0x00 0x00 0x0D) + 'I''H''D''R'
    if (p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4E && p[3] == 0x47 &&
        p[4] == 0x0D && p[5] == 0x0A && p[6] == 0x1A && p[7] == 0x0A) return true;
    // GIF: 6 字节 (47 49 46 38 37/39 61)
    if (p[0] == 0x47 && p[1] == 0x49 && p[2] == 0x46 && p[3] == 0x38 &&
        (p[4] == 0x37 || p[4] == 0x39) && p[5] == 0x61) return true;
    // WebP: RIFF????WEBP (12 字节)
    if (p[0] == 0x52 && p[1] == 0x49 && p[2] == 0x46 && p[3] == 0x46 &&
        p[8] == 0x57 && p[9] == 0x45 && p[10] == 0x42 && p[11] == 0x50) return true;
    // BMP: 42 4D + 合理文件大小（1KB ~ 100MB 小端）+ 0x00 0x00 0x00 0x00 保留
    if (p[0] == 0x42 && p[1] == 0x4D) {
        const unsigned int sz = p[2] | (p[3] << 8) | (p[4] << 16) | (p[5] << 24);
        if (sz >= 1024 && sz <= 100u * 1024u * 1024u &&
            p[6] == 0x00 && p[7] == 0x00 && p[8] == 0x00 && p[9] == 0x00) return true;
        return false;
    }
    return false;
}

namespace {

// 从指定进程的内存中找 key；返回 32 位 hex 或空串。
// proc 由调用方 OpenProcess 得到，函数内部不 Close。
// 扫描策略：MEM_PRIVATE（heap） + MEM_IMAGE（dll .text/.data/.rdata）。
//  之前限制 writable+isImage 与 >50 MB region skip，导致 Weixin.dll 100+ MB image
//  段被全部跳过；key 可能驻留在 .rdata/.data 只读段（OS 自带内存指针指向）。
QString scanPidForImageKey(HANDLE proc, quint32 pid, const unsigned char ct[16]) {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    quint64 addr = std::max<quint64>(0x10000,
                                     reinterpret_cast<quint64>(si.lpMinimumApplicationAddress));
    const quint64 limit = std::min<quint64>(0x7FFFFFFFFFFFULL,
                                            reinterpret_cast<quint64>(si.lpMaximumApplicationAddress));
    quint64 tries = 0;
    // 单进程硬上限 200M tries ≈ 200 MB 线性扫描：足以扫完 Weixin.dll .text/.data/.rdata
    constexpr quint64 kHardCap = 200'000'000ULL;
    constexpr quint64 kReportEvery = 5'000'000ULL;
    Logger::instance().info(
        QString("extractImageKey: start scanning pid=%1 region=0x%2..0x%3 hardCap=%4")
            .arg(pid).arg(addr, 16).arg(limit, 16).arg(kHardCap),
        "wechat.key");
    quint64 lastReport = 0;
    while (addr < limit && tries < kHardCap) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQueryEx(proc, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
            break;
        const quint64 regionEnd = reinterpret_cast<quint64>(mbi.BaseAddress) + mbi.RegionSize;

        // 过滤：committed 且可读；只跳过 NOACCESS/GUARD；mapped>200MB 跳过避免误判。
        const bool readable = (mbi.State == MEM_COMMIT)
            && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        const bool tooBig = (mbi.RegionSize > 200ull * 1024 * 1024);
        const bool skip = !readable || tooBig;
        if (!skip) {
            QByteArray buf(static_cast<int>(mbi.RegionSize), Qt::Uninitialized);
            SIZE_T read = 0;
            if (ReadProcessMemory(proc, mbi.BaseAddress, buf.data(),
                                  static_cast<SIZE_T>(mbi.RegionSize), &read)
                && read >= 16) {
                buf.resize(static_cast<int>(read));
                for (qsizetype i = 0; i + 16 <= buf.size(); ++i) {
                    const unsigned char* k = reinterpret_cast<const unsigned char*>(buf.constData()) + i;
                    unsigned char pt[16];
                    if (aesEcbDecryptBlock(k, ct, pt) && looksLikeImagePlain(pt)) {
                        const QString hitHex =
                            QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(k), 16).toHex());
                        Logger::instance().info(
                            QString("extractImageKey: HIT key=%1 addr=%2 tries=%3")
                                .arg(hitHex).arg(addr + i, 16).arg(tries),
                            "wechat.key");
                        return hitHex;
                    }
                    ++tries;
                    if (tries >= kHardCap) break;
                    if (tries - lastReport >= kReportEvery) {
                        lastReport = tries;
                        qDebug().noquote() << "[wechat.key] progress tries=" << tries
                                          << "addr=0x" + QString::number(addr + i, 16);
                    }
                }
            }
        }
        addr = regionEnd;
    }
    Logger::instance().warn(
        QString("extractImageKey: exhausted memory scan, total tries=%1").arg(tries),
        "wechat.key");
    return {};
}

// 从 .dat 文件读取 CT（16 字节首块密文）；返回是否成功。
// V2 格式：byte[0..5] magic, byte[6..14] reserved (9 字节), byte[15..30] 首块密文。
bool loadOracleCt(const QString& knownDatPath, unsigned char ct[16], QString* errOut) {
    if (knownDatPath.isEmpty() || !QFile::exists(knownDatPath)) {
        if (errOut) *errOut = QStringLiteral("oracle .dat 不存在：%1").arg(knownDatPath);
        return false;
    }
    QFile f(knownDatPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = QStringLiteral("打开 .dat 失败：%1").arg(f.errorString());
        return false;
    }
    QByteArray dat = f.read(31);
    f.close();
    if (dat.size() < 31) {
        if (errOut) *errOut = QStringLiteral(".dat 太小（%1 字节）").arg(dat.size());
        return false;
    }
    if ((unsigned char)dat[0] != 0x07 || (unsigned char)dat[1] != 0x08 ||
        (unsigned char)dat[2] != 'V' || (unsigned char)dat[3] != '2') {
        if (errOut) *errOut = QStringLiteral("不是 V2 .dat 文件（magic=%1 %2 %3 %4）")
            .arg((unsigned char)dat[0], 2, 16, QChar('0'))
            .arg((unsigned char)dat[1], 2, 16, QChar('0'))
            .arg((unsigned char)dat[2], 2, 16, QChar('0'))
            .arg((unsigned char)dat[3], 2, 16, QChar('0'));
        return false;
    }
    memset(ct, 0, 16);
    memcpy(ct, dat.constData() + 15, 16);
    return true;
}

// 设置进程访问失败时的诊断文本
void setOpenProcErr(quint32 pid, QString* errOut) {
    if (!errOut) return;
    *errOut = QStringLiteral("无法打开进程（PID %1，err=%2）")
        .arg(pid).arg(GetLastError());
}

} // namespace

QString extractImageKey(quint32 pid, const QString& knownDatPath, QString* errOut) {
    unsigned char ct[16] = {};
    if (!loadOracleCt(knownDatPath, ct, errOut)) return {};

    HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) { setOpenProcErr(pid, errOut); return {}; }

    const QString keyHex = scanPidForImageKey(proc, pid, ct);
    CloseHandle(proc);

    if (keyHex.isEmpty() && errOut) {
        *errOut = QStringLiteral(
            "未找到图片 AES key。可能原因：\n"
            "  1) .dat 不是 V2 格式（magic 应为 07 08 V 2 08 07）\n"
            "  2) 微信版本 ≥ 4.1.10.31，图片 key 可能被 XOR 混淆存储（本工具暂不支持）\n"
            "  3) 微信进程权限不足（请以管理员权限运行 bambooRat）\n"
            "  4) oracle .dat 与进程不匹配（用另一张大图重试）\n"
            "  5) key 只存在子进程中（已尝试全部 Weixin + WeChatAppEx）");
    }
    return keyHex;
}

QString extractImageKeyMulti(const QList<quint32>& pids,
                             const QString& knownDatPath,
                             QString* errOut) {
    unsigned char ct[16] = {};
    if (!loadOracleCt(knownDatPath, ct, errOut)) return {};

    Logger::instance().info(
        QString("extractImageKeyMulti: scanning %1 pids").arg(pids.size()),
        "wechat.key");

    QString lastErr;
    for (quint32 pid : pids) {
        HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!proc) {
            lastErr = QStringLiteral("无法打开进程 PID %1（err=%2）")
                .arg(pid).arg(GetLastError());
            Logger::instance().warn(lastErr, "wechat.key");
            continue;
        }
        const QString k = scanPidForImageKey(proc, pid, ct);
        CloseHandle(proc);
        if (!k.isEmpty()) {
            Logger::instance().info(
                QString("extractImageKeyMulti: HIT on pid=%1 key=%2").arg(pid).arg(k),
                "wechat.key");
            return k;
        }
    }

    if (errOut) {
        *errOut = QStringLiteral(
            "已扫描 %1 个微信进程仍未找到图片 AES key。\n"
            "最后错误：%2\n"
            "可能原因：\n"
            "  1) .dat 不是 V2 格式（magic 应为 07 08 V 2 08 07）\n"
            "  2) 微信版本 ≥ 4.1.10.31，图片 key 可能被 XOR 混淆存储\n"
            "  3) 微信进程权限不足（请以管理员权限运行 bambooRat）\n"
            "  4) oracle .dat 与进程不匹配（用另一张大图重试）\n"
            "  5) key 仅在特定会话窗口打开时驻留内存（请重试）")
            .arg(pids.size()).arg(lastErr);
    }
    return {};
}

} // namespace WeChatKeyExtractor
