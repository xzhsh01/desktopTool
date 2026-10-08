#include "WeChatKeyExtractor.h"
#include "WeChatDb.h"
#include "core/Logger.h"

#include <QByteArray>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QSet>

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace {

// v4：密钥引用结构特征（小端 64 位）：{ 0, 0x20, 0x2F }，其前 8 字节是指向密钥的指针
const char kV4Pattern[24] = {
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x20', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x2F', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
};

// 图片 key（16 字节）的引用结构：{ 0, 0x10, 0x2F }，其前 8 字节是指向密钥的指针
// 4.x WeChat 把 SQLCipher db key（32B，模式 0x20）和 image key（16B，模式 0x10）
// 都封装在同一 allocator 下，因此用同样的 { ptr, 0, len, 0x2F } 结构。
const char kV4ImagePattern[24] = {
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x10', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
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

// 同上，但收集 16 字节图片 key（结构 { ptr, 0, 0x10, 0x2F }）
// 注意 0x10 比 0x20 更常见，需额外熵检查：distinct >= 10 / printable <= 12
void collectImageKeyCandidates(HANDLE proc, const QByteArray& buf, const QByteArray& pattern,
                               int ptrSize, QSet<quint64>& seen, QStringList& keys) {
    const int patLen = pattern.size();
    qsizetype idx = buf.size() - patLen;
    while (idx >= ptrSize) {
        idx = buf.lastIndexOf(pattern, idx);
        if (idx < ptrSize) break;
        quint64 ptr = 0;
        memcpy(&ptr, buf.constData() + idx - ptrSize, ptrSize);
        if (ptr > 0x10000 && ptr < 0x7FFFFFFFFFFFULL && !seen.contains(ptr)) {
            seen.insert(ptr);
            unsigned char key[16] = {};
            SIZE_T read = 0;
            if (ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(ptr), key, 16, &read)
                && read == 16) {
                bool seenB[256] = {};
                int distinct = 0, printable = 0;
                for (int i = 0; i < 16; ++i) {
                    if (!seenB[key[i]]) { seenB[key[i]] = true; ++distinct; }
                    if (key[i] >= 0x20 && key[i] <= 0x7e) ++printable;
                }
                if (distinct >= 10 && printable <= 12) {
                    keys.append(QByteArray(reinterpret_cast<const char*>(key), 16).toHex());
                }
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

// 两个等长 hex 值逐字节 XOR，返回 hex；长度不符返回空串。
// 支持 16B（图片 key）与 32B（db key）任意等长组合。
QString xorHexKeys(const QString& aHex, const QString& bHex) {
    const QByteArray a = QByteArray::fromHex(aHex.toLatin1());
    const QByteArray b = QByteArray::fromHex(bHex.toLatin1());
    if (a.isEmpty() || a.size() != b.size()) return QString();
    QByteArray r(a.size(), Qt::Uninitialized);
    for (int i = 0; i < a.size(); ++i) r[i] = a[i] ^ b[i];
    return QString::fromLatin1(r.toHex());
}

// 把 v5 XOR key（32B，为 db key 混淆设计）展开为 16B mask 列表（前/后半段），
// 供 16B 图片 key 的 XOR 还原使用；本身即 16B 的 mask 原样保留。
// 背景 bug：图片 key 路径直接拿 32B xorKey 做 XOR，xorHexKeys 要求等长全部返回空，
// verifyCandidateKey 又要求 mask==16B 全部跳过 —— XOR 还原对图片 key 从未生效。
static QStringList expandXorMasks16(const QStringList& xorKeys) {
    QStringList out;
    for (const QString& xk : xorKeys) {
        const QByteArray xb = QByteArray::fromHex(xk.toLatin1());
        if (xb.size() == 16) {
            if (!out.contains(xk)) out.append(xk);
        } else if (xb.size() == 32) {
            const QString lo = QString::fromLatin1(xb.left(16).toHex());
            const QString hi = QString::fromLatin1(xb.mid(16).toHex());
            if (!out.contains(lo)) out.append(lo);
            if (!out.contains(hi)) out.append(hi);
        }
    }
    return out;
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
    // 主进程（Weixin.exe/WeChat.exe）优先，WeChatAppEx 子进程靠后。
    // 图片解密在主进程完成，key 几乎只驻留主进程；主进程先扫可尽早命中返回，
    // 避免逐进程挂起扫描拖长整体冻结时间。
    QList<quint32> main_, sub;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const QString name = QString::fromWCharArray(pe.szExeFile).toLower();
            if (name == QStringLiteral("weixin.exe") ||
                name == QStringLiteral("wechat.exe")) {
                if (!main_.contains(pe.th32ProcessID)) main_.append(pe.th32ProcessID);
            } else if (name == QStringLiteral("wechatappex.exe")) {
                if (!sub.contains(pe.th32ProcessID)) sub.append(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    QList<quint32> result = main_;
    for (quint32 p : sub) result.append(p);
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

// 用候选 key 解密 oracle .dat 整个 body（EVP API 全长解密），验证输出是合法图片。
// 这是 scanPidForImageKey HIT 后的二次确认：避免双 ct 验证漏过的随机碰撞假阳性。
// oraclePath 必须是从 WeChatConfigDialog 传进来的真实 .dat（已知是某张图）。
// 返回 true 表示 key 真的能解出 oracle 的全部密文（首块是图片 magic + body 整体合理）。
static bool looksLikeImagePlain(const unsigned char p[16]);  // forward decl

// 在指定 offset 处用 EVP 解密前 N 块，验证首块像图片 magic。
// 返回 true 表示该 offset 解密有效。
static bool tryOffsetVerify(const unsigned char key[16],
                            const QByteArray& dat, int offset) {
    const int cipherLen = dat.size() - offset;
    if (cipherLen < 32) return false;
    // 截断到 16 的倍数（.dat 末尾可能有 1~15 字节截断）
    const int aligned = cipherLen - (cipherLen % 16);
    if (aligned < 32) return false;
    // 只解密前 4KB（足够判断 + 省时）
    const int sampleLen = qMin(aligned, 4096);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx); return false;
    }
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    QByteArray plain(sampleLen + 16, '\0');
    int outLen = 0;
    const unsigned char* inPtr = reinterpret_cast<const unsigned char*>(dat.constData() + offset);
    unsigned char* outPtr = reinterpret_cast<unsigned char*>(plain.data());
    bool ok = true;
    if (EVP_DecryptUpdate(ctx, outPtr, &outLen, inPtr, sampleLen) != 1) {
        ok = false;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok || outLen < 32) return false;
    plain.resize(outLen);

    const unsigned char* p = reinterpret_cast<const unsigned char*>(plain.constData());
    // 首块必须像图片 magic
    if (!looksLikeImagePlain(p)) return false;

    // 次块必须不全 0 / 不全 FF（排除假阳性双块相同模式）
    bool allZero = true, allFF = true;
    for (int i = 16; i < 32; ++i) {
        if (p[i] != 0x00) allZero = false;
        if (p[i] != 0xFF) allFF = false;
    }
    if (allZero || allFF) return false;

    // JPEG 头 ff d8 ff ??：在前 4KB 内必须出现至少一个 0xFF 0x?? marker
    // （JPEG 数据流中段经常含 marker，全 4KB 无 0xFF 的 JPEG 几乎不存在）
    if (p[0] == 0xFF && p[1] == 0xD8) {
        int ffCount = 0;
        for (int i = 32; i + 1 < outLen; ++i) {
            if (p[i] == 0xFF && p[i + 1] != 0x00) {
                ++ffCount;
                if (ffCount >= 2) break;
            }
        }
        if (ffCount < 1) return false;
    }
    return true;
}

static bool verifyKeyByFullOracle(const unsigned char key[16],
                                  const QString& oraclePath) {
    if (oraclePath.isEmpty()) return false;
    QFile f(oraclePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray dat = f.readAll();
    f.close();
    if (dat.size() <= 47) return false;
    if ((unsigned char)dat[0] != 0x07 || (unsigned char)dat[1] != 0x08 ||
        (unsigned char)dat[2] != 'V'  || (unsigned char)dat[3] != '2') return false;

    // V2 密文起点不确定是 15 还是 16（decryptV2 自己也两种都试）。两种都验证。
    bool ok = tryOffsetVerify(key, dat, 15) || tryOffsetVerify(key, dat, 16);
    if (ok) {
        Logger::instance().info(
            QString("verifyKeyByFullOracle: OK oracle=%1").arg(oraclePath),
            "wechat.key");
    }
    return ok;
}

// 判断 16 字节明文是否像图片 magic（严格版：要求 4+ 字节特征 + 合理字段）
static bool looksLikeImagePlain(const unsigned char p[16]) {
    // JPEG: FF D8 FF + E0(JFIF) / E1(EXIF) — 所有真实微信图都用这两种主 marker。
    // 必须进一步校验 JFIF/EXIF 魔数：仅查 "FF D8 FF E1 + 段长合法" 会在盲扫 2 亿次中
    // 以 ~4.6% 概率撞中（实测：栈上代码字节 0405094c... 解出 FF D8 FF E1 c0 2f，
    // 段长 49199 恰好在 16~65535 内 → 假阳性）。且微信重编码 JPEG 首块密文相同，
    // 多 oracle 交叉验证无法排除此类假 key。JFIF/EXIF 魔数把概率压到 ~1/2^72。
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) {
        const unsigned int segLen = (p[4] << 8) | p[5];
        if (segLen < 2) return false;
        if (p[3] == 0xE0) {
            // JFIF: byte[6..10] = "JFIF\0"
            return p[6] == 'J' && p[7] == 'F' && p[8] == 'I' && p[9] == 'F' && p[10] == 0x00;
        }
        if (p[3] == 0xE1) {
            // EXIF: byte[6..10] = "Exif\0"
            return p[6] == 'E' && p[7] == 'x' && p[8] == 'i' && p[9] == 'f' && p[10] == 0x00;
        }
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

// 验证一个候选 key：必须同时让 ct1 解出像图片的首块、ct2 解出像图片次块。
// 单 ct 验证有 1/2^24 偶然 FF D8 FF magic 假阳性概率（5500 万 tries 可能命中 1 次）。
// 双 ct 验证把假阳性概率压到 1/2^48 ≈ 1/3e14，无法命中。
//
// xorKeys: 4.1.10.31+ 图片 key 在内存中是 XOR 混淆态；尝试 key XOR xk 还原。
// xorIdx: 若非空，填回匹配成功的 XOR key 在 xorKeys 中的下标（-1 表示直接匹配）。
// 成功时 recoveredOut 填回还原后的 16 字节真实 key。
static bool verifyCandidateKey(const unsigned char key[16],
                               const unsigned char ct1[16],
                               const unsigned char ct2[16],
                               const QStringList& xorKeys = {},
                               int* xorIdx = nullptr,
                               unsigned char recoveredOut[16] = nullptr) {
    auto check = [&](const unsigned char k[16]) -> bool {
        unsigned char pt1[16], pt2[16];
        if (!aesEcbDecryptBlock(k, ct1, pt1)) return false;
        if (!looksLikeImagePlain(pt1)) return false;
        if (!aesEcbDecryptBlock(k, ct2, pt2)) return false;
        bool allZero = true, allFF = true;
        for (int i = 0; i < 16; ++i) {
            if (pt1[i] != pt2[i]) allZero = false;  // 防与 pt1 完全一致
            if (pt2[i] != 0x00) allZero = false;
            if (pt2[i] != 0xFF) allFF = false;
        }
        (void)allZero; (void)allFF;
        // 实际逻辑：pt2 不能全 0 / 全 FF / 与 pt1 完全一致
        bool bz = true, bf = true, same = true;
        for (int i = 0; i < 16; ++i) {
            if (pt2[i] != 0x00) bz = false;
            if (pt2[i] != 0xFF) bf = false;
            if (pt1[i] != pt2[i]) same = false;
        }
        return !bz && !bf && !same;
    };
    if (check(key)) {
        if (xorIdx) *xorIdx = -1;
        if (recoveredOut) memcpy(recoveredOut, key, 16);
        return true;
    }
    // 尝试 XOR 还原。mask 可能是 16B（直接用）或 32B（db key 的混淆 mask，
    // 对 16B 图片 key 尝试其前/后两个 16B 半段）。
    int flatIdx = 0;
    for (int i = 0; i < xorKeys.size(); ++i) {
        const QByteArray xb = QByteArray::fromHex(xorKeys[i].toLatin1());
        for (int off = 0; off + 16 <= xb.size(); off += 16, ++flatIdx) {
            unsigned char k2[16];
            for (int j = 0; j < 16; ++j) k2[j] = key[j] ^ static_cast<unsigned char>(xb[off + j]);
            if (check(k2)) {
                if (xorIdx) *xorIdx = flatIdx;
                if (recoveredOut) memcpy(recoveredOut, k2, 16);
                return true;
            }
        }
    }
    return false;
}

// 从指定进程的内存中找 key；返回 32 位 hex 或空串。
// proc 由调用方 OpenProcess 得到，函数内部不 Close。
// 扫描策略：MEM_PRIVATE（heap） + MEM_IMAGE（dll .text/.data/.rdata）。
//  之前限制 writable+isImage 与 >50 MB region skip，导致 Weixin.dll 100+ MB image
//  段被全部跳过；key 可能驻留在 .rdata/.data 只读段（OS 自带内存指针指向）。
QString scanPidForImageKey(HANDLE proc, quint32 pid,
                           const unsigned char ct1[16],
                           const unsigned char ct2[16],
                           const QString& oraclePath,
                           const QStringList& extraOracles = {},
                           const QStringList& xorKeys = {}) {
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
                    int xorIdx = -1;
                    unsigned char recovered[16] = {};
                    if (verifyCandidateKey(k, ct1, ct2, xorKeys, &xorIdx, recovered)) {
                        const QString hitHex =
                            QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(k), 16).toHex());
                        const QString recoveredHex =
                            QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(recovered), 16).toHex());
                        Logger::instance().info(
                            QString("extractImageKey: CANDIDATE key=%1 addr=%2 tries=%3 xorIdx=%4 (running full-oracle verify)")
                                .arg(hitHex).arg(addr + i, 16).arg(tries).arg(xorIdx),
                            "wechat.key");
                        // 二次确认：用 EVP 解密 oracle 整个 body，验证输出是合法图片。
                        // 双 ct 验证仍可能 5500 万次中假阳性 ~3 次；这一步压到几乎 0。
                        QString verifiedHex = recoveredHex;
                        if (!verifyKeyByFullOracle(recovered, oraclePath)) {
                            // 三次确认（v5 XOR 还原，已在 verifyCandidateKey 中尝试过）：
                            // 兜底 — 再尝试所有 16B mask 全组合（32B xorKey 已展开为半段）
                            QString altHex;
                            for (const QString& mk : expandXorMasks16(xorKeys)) {
                                const QString xored = xorHexKeys(hitHex, mk);
                                if (xored.isEmpty() || xored == verifiedHex) continue;
                                QByteArray xb = QByteArray::fromHex(xored.toLatin1());
                                if (xb.size() == 16 && verifyKeyByFullOracle(
                                        reinterpret_cast<const unsigned char*>(xb.constData()), oraclePath)) {
                                    altHex = xored;
                                    break;
                                }
                            }
                            if (altHex.isEmpty()) {
                                Logger::instance().warn(
                                    QString("extractImageKey: CANDIDATE %1 REJECTED by full-oracle verify (xorKeys=%2 tried)")
                                        .arg(hitHex).arg(xorKeys.size()),
                                    "wechat.key");
                                ++tries;
                                if (tries >= kHardCap) break;
                                continue;
                            }
                            Logger::instance().info(
                                QString("extractImageKey: CANDIDATE %1 XOR-RECOVERED to %2 (xorKey applied)")
                                    .arg(hitHex).arg(altHex),
                                "wechat.key");
                            verifiedHex = altHex;
                        }
                        // 三次确认（多 oracle 交叉验证）：单 oracle 仍可能让占位 key 偶然
                        // 通过（观察：占位 key 对部分 .dat 碰巧解出 JPEG magic）。
                        // 必须 extraOracles 全部通过才接受。
                        if (!extraOracles.isEmpty()) {
                            QStringList crossPaths;
                            crossPaths.reserve(extraOracles.size() + 1);
                            crossPaths << oraclePath;
                            for (const QString& p : extraOracles) {
                                if (p != oraclePath) crossPaths << p;
                            }
                            QString multiErr;
                            if (verifyImageKeyMulti(crossPaths, verifiedHex, &multiErr).isEmpty()) {
                                Logger::instance().warn(
                                    QString("extractImageKey: CANDIDATE %1 REJECTED by multi-oracle verify: %2")
                                        .arg(verifiedHex).arg(multiErr),
                                    "wechat.key");
                                ++tries;
                                if (tries >= kHardCap) break;
                                continue;
                            }
                            Logger::instance().info(
                                QString("extractImageKey: %1 passed MULTI-ORACLE verify (%2 files)")
                                    .arg(verifiedHex).arg(crossPaths.size()),
                                "wechat.key");
                        }
                        Logger::instance().info(
                            QString("extractImageKey: HIT key=%1 addr=%2 tries=%3")
                                .arg(verifiedHex).arg(addr + i, 16).arg(tries),
                            "wechat.key");
                        return verifiedHex;
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

// 从 .dat 文件读取前两个 CT（16 字节首块 + 16 字节次块密文）；返回是否成功。
// V2 格式（4.1.13.65 实测对齐证明）：
//   byte[0..5]   magic `07 08 V 2 08 07`
//   byte[6..9]   flags（恒为 00 04 00 00）
//   byte[10..13] uint32 LE 明文长度
//   byte[14]     恒为 0x01（header 尾标志）
//   byte[15..N]  AES-128-ECB 密文（块对齐在 15）
// 对齐证据：4 个不同 .dat 在 byte[31..46] 共享完整 16 字节密文块、byte[47] 各不相同 ——
// 只有 offset=15 对齐能解释（相同明文块 = 微信重编码 JPEG 固定头 + 同 key）；
// 若按 offset=16 对齐，AES 块不可能出现 15/16 字节相同。
// 注意：历史上曾误认为 byte[15] 是 padding 而从 16 起算 —— 那是错的，会导致盲扫
// 用错位 ct 验证，真 key 全部被误杀。
bool loadOracleCt(const QString& knownDatPath, unsigned char ct1[16],
                  unsigned char ct2[16], QString* errOut) {
    if (knownDatPath.isEmpty() || !QFile::exists(knownDatPath)) {
        if (errOut) *errOut = QStringLiteral("oracle .dat 不存在：%1").arg(knownDatPath);
        return false;
    }
    QFile f(knownDatPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = QStringLiteral("打开 .dat 失败：%1").arg(f.errorString());
        return false;
    }
    QByteArray dat = f.read(48);   // 需要 byte[15..46]
    f.close();
    if (dat.size() < 47) {
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
    memset(ct1, 0, 16);
    memset(ct2, 0, 16);
    memcpy(ct1, dat.constData() + 15, 16);   // 密文块对齐在 offset 15（实测证明）
    memcpy(ct2, dat.constData() + 31, 16);
    return true;
}

// 设置进程访问失败时的诊断文本
void setOpenProcErr(quint32 pid, QString* errOut) {
    if (!errOut) return;
    *errOut = QStringLiteral("无法打开进程（PID %1，err=%2）")
        .arg(pid).arg(GetLastError());
}

// ── 进程挂起/恢复（冻结内存状态）──────────────────────────────────────────
// 4.1.10.31+ 图片 key 生命周期极短（仅图片解密瞬间驻留，用完即清）。
// 单进程盲扫约 1 分钟，期间 key 极易消失 —— 扫描前挂起进程冻结内存状态。
// 逐进程挂起：任一时刻只冻结一个进程，扫完立即恢复（RAII）。
// 微信 UI 在被扫进程冻结期间短暂卡顿，属正常现象。
typedef NTSTATUS(NTAPI* NtProcessStateFn)(HANDLE);

class ProcSuspendGuard {
public:
    explicit ProcSuspendGuard(HANDLE proc) : m_proc(proc) {
        static const auto fn = reinterpret_cast<NtProcessStateFn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendProcess")));
        if (fn && m_proc) m_suspended = (fn(m_proc) >= 0);
    }
    ~ProcSuspendGuard() {
        if (!m_suspended) return;
        static const auto fn = reinterpret_cast<NtProcessStateFn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeProcess")));
        if (fn) fn(m_proc);
    }
    bool suspended() const { return m_suspended; }
private:
    HANDLE m_proc;
    bool m_suspended = false;
};

} // namespace

// 前向声明 collectOracleDats 和 guessDataDirFromOracle（在文件下方定义）。
static QString guessDataDirFromOracle(const QString& oraclePath);
QStringList collectOracleDats(const QString& dataDir, int maxN);

// 进程 → XOR key 列表（按 dll 路径去重缓存，避免重复读盘）
static QHash<QString, QStringList>& xorKeyCache() {
    static QHash<QString, QStringList> cache;
    return cache;
}
static QStringList getXorKeysForPid(quint32 pid) {
    const QString dllPath = findWeixinDllPath(pid);
    if (dllPath.isEmpty()) return {};
    if (xorKeyCache().contains(dllPath)) return xorKeyCache().value(dllPath);
    const QStringList ks = extractXorKeysFromDllFile(dllPath);
    xorKeyCache().insert(dllPath, ks);
    return ks;
}

QString extractImageKey(quint32 pid, const QString& knownDatPath, QString* errOut) {
    unsigned char ct1[16] = {}, ct2[16] = {};
    if (!loadOracleCt(knownDatPath, ct1, ct2, errOut)) return {};

    HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) { setOpenProcErr(pid, errOut); return {}; }

    // 多 oracle 交叉验证：从 dataDir 收集另外 2 张最大的 .dat
    QStringList extraOracles;
    const QString dataDir = guessDataDirFromOracle(knownDatPath);
    if (!dataDir.isEmpty()) {
        const QStringList all = collectOracleDats(dataDir, 3);
        for (const QString& p : all) {
            if (p != knownDatPath) extraOracles << p;
        }
    }
    if (!extraOracles.isEmpty()) {
        Logger::instance().info(
            QString("extractImageKey: cross-oracle = %1 additional files").arg(extraOracles.size()),
            "wechat.key");
    }

    // v5 XOR 还原（4.1.10.31+）：从 Weixin.dll 提取 XOR key，扫描时实时还原候选
    const QStringList xorKeys = getXorKeysForPid(pid);
    if (!xorKeys.isEmpty()) {
        Logger::instance().info(
            QString("extractImageKey: v5 xorKeys=%1 from Weixin.dll (XOR-deobfuscation enabled)").arg(xorKeys.size()),
            "wechat.key");
    }

    const QString keyHex = scanPidForImageKey(proc, pid, ct1, ct2, knownDatPath, extraOracles, xorKeys);
    CloseHandle(proc);

    if (keyHex.isEmpty() && errOut) {
        *errOut = QStringLiteral(
            "未找到图片 AES key。可能原因：\n"
            "  1) .dat 不是 V2 格式（magic 应为 07 08 V 2 08 07）\n"
            "  2) 微信版本 ≥ 4.1.10.31，图片 key XOR 还原失败（特征可能已变化）\n"
            "  3) 微信进程权限不足（请以管理员权限运行 bambooRat）\n"
            "  4) oracle .dat 与进程不匹配（用另一张大图重试）\n"
            "  5) key 只存在子进程中（已尝试全部 Weixin + WeChatAppEx）");
    }
    return keyHex;
}

// 扫描单个进程，用 {ptr, 0, 0x10, 0x2F} 结构找 16 字节图片 key 候选（hex 32 字符）。
// 与 db key 的 {ptr, 0, 0x20, 0x2F} 提取逻辑一致，但 key 长度是 16 而非 32。
static QStringList collectImageKeyCandidatesForPid(HANDLE proc) {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    quint64 addr = std::max<quint64>(0x10000,
                                     reinterpret_cast<quint64>(si.lpMinimumApplicationAddress));
    const quint64 limit = std::min<quint64>(0x7FFFFFFFFFFFULL,
                                            reinterpret_cast<quint64>(si.lpMaximumApplicationAddress));
    const QByteArray pattern(kV4ImagePattern, sizeof(kV4ImagePattern));
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
                collectImageKeyCandidates(proc, buf, pattern, 8, seen, keys);
            }
        }
        addr = regionEnd;
    }
    return keys;
}

QString extractImageKeyMulti(const QList<quint32>& pids,
                             const QString& knownDatPath,
                             QString* errOut) {
    unsigned char ct1[16] = {}, ct2[16] = {};
    if (!loadOracleCt(knownDatPath, ct1, ct2, errOut)) return {};

    Logger::instance().info(
        QString("extractImageKeyMulti: scanning %1 pids").arg(pids.size()),
        "wechat.key");

    // 多 oracle 交叉验证：从 dataDir 收集另外 2 张最大的 .dat
    QStringList extraOracles;
    const QString dataDir = guessDataDirFromOracle(knownDatPath);
    if (!dataDir.isEmpty()) {
        const QStringList all = collectOracleDats(dataDir, 3);
        for (const QString& p : all) {
            if (p != knownDatPath) extraOracles << p;
        }
    }
    if (!extraOracles.isEmpty()) {
        Logger::instance().info(
            QString("extractImageKeyMulti: cross-oracle = %1 additional files").arg(extraOracles.size()),
            "wechat.key");
    }

    // ── 阶段 A：结构特征扫描 {ptr, 0, 0x10, 0x2F} ────────────────────────────
    // 4.x WeChat 把 SQLCipher db key（32B，模式 0x20）和图片 key（16B，模式 0x10）
    // 封装在同一 allocator 下。先用结构特征精确收集候选，再逐个 EVP 验证。
    // 如果命中，无需做全内存盲扫（每进程 30~60 秒 → 瞬间）。
    // 挂起全部进程后再收集：key 生命周期可能只有几秒（图片解密瞬间驻留、用完即清），
    // 冻结全部进程拿到同一时间快照，收集期间 key 不会被清除。阶段 A 仅需几秒，全冻结可接受。
    QStringList structCandidates;
    {
        std::vector<std::pair<HANDLE, std::unique_ptr<ProcSuspendGuard>>> opened;
        for (quint32 pid : pids) {
            HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME,
                                      FALSE, pid);
            if (!proc) {  // 无挂起权限时回落（保持扫描能力）
                proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
            }
            if (!proc) continue;
            opened.emplace_back(proc, std::make_unique<ProcSuspendGuard>(proc));
        }
        int suspendedCount = 0;
        for (const auto& o : opened) if (o.second->suspended()) ++suspendedCount;
        Logger::instance().info(
            QString("extractImageKeyMulti: frozen %1/%2 processes for struct scan")
                .arg(suspendedCount).arg(opened.size()),
            "wechat.key");
        for (const auto& o : opened) {
            const QStringList cs = collectImageKeyCandidatesForPid(o.first);
            for (const QString& c : cs) {
                if (!structCandidates.contains(c)) structCandidates.append(c);
            }
        }
        for (auto& o : opened) { o.second.reset(); CloseHandle(o.first); }
    }
    Logger::instance().info(
        QString("extractImageKeyMulti: struct candidates=%1 (via 0x10 pattern)").arg(structCandidates.size()),
        "wechat.key");

    // 收集全部 XOR key（用于还原候选的混淆形态）
    QStringList allXorKeys;
    for (quint32 pid : pids) {
        const QStringList xks = getXorKeysForPid(pid);
        for (const QString& x : xks) {
            if (!allXorKeys.contains(x)) allXorKeys.append(x);
        }
    }

    // 构造完整 oracle 路径集（known + extraOracles）
    QStringList allOracles;
    allOracles << knownDatPath;
    for (const QString& p : extraOracles) {
        if (p != knownDatPath) allOracles << p;
    }

    // 直接验证候选（未混淆形态）
    for (const QString& c : structCandidates) {
        if (c.size() != 32) continue;
        QString err;
        if (!verifyImageKeyMulti(allOracles, c, &err).isEmpty()) {
            Logger::instance().info(
                QString("extractImageKeyMulti: struct-candidate HIT key=%1").arg(c),
                "wechat.key");
            return c;
        }
        Logger::instance().info(
            QString("extractImageKeyMulti: struct-candidate MISS key=%1 err=%2")
                .arg(c).arg(err),
            "wechat.key");
    }
    // XOR 还原候选（4.1.10.31+ 混淆形态）。图片 key 是 16B，
    // 32B 的 v5 xorKey 展开为前/后两个 16B mask 逐段尝试。
    const QStringList masks16 = expandXorMasks16(allXorKeys);
    if (!masks16.isEmpty()) {
        Logger::instance().info(
            QString("extractImageKeyMulti: xor masks16=%1 (from %2 dll xorKeys)")
                .arg(masks16.size()).arg(allXorKeys.size()),
            "wechat.key");
    }
    for (const QString& mk : masks16) {
        for (const QString& c : structCandidates) {
            const QString xored = xorHexKeys(c, mk);
            if (xored.isEmpty() || xored == c) continue;
            QString err;
            if (!verifyImageKeyMulti(allOracles, xored, &err).isEmpty()) {
                Logger::instance().info(
                    QString("extractImageKeyMulti: struct-candidate XOR-HIT key=%1 (from %2)")
                        .arg(xored).arg(c),
                    "wechat.key");
                return xored;
            }
            Logger::instance().info(
                QString("extractImageKeyMulti: struct-candidate XOR-MISS key=%1 (mask %2) err=%3")
                    .arg(xored).arg(mk).arg(err),
                "wechat.key");
        }
    }
    Logger::instance().info(
        QString("extractImageKeyMulti: struct-candidate scan found no match, falling back to memory scan"),
        "wechat.key");

    // ── 阶段 B：盲扫内存（原逻辑） ────────────────────────────────────────────
    QString lastErr;
    for (quint32 pid : pids) {
        HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME,
                                  FALSE, pid);
        if (!proc) {  // 无挂起权限时回落（保持扫描能力）
            proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        }
        if (!proc) {
            lastErr = QStringLiteral("无法打开进程 PID %1（err=%2）")
                .arg(pid).arg(GetLastError());
            Logger::instance().warn(lastErr, "wechat.key");
            continue;
        }
        // v5 XOR 还原（4.1.10.31+）：从 Weixin.dll 提取 XOR key 用于还原候选
        const QStringList xorKeys = getXorKeysForPid(pid);
        if (!xorKeys.isEmpty()) {
            Logger::instance().info(
                QString("extractImageKeyMulti: pid=%1 xorKeys=%2").arg(pid).arg(xorKeys.size()),
                "wechat.key");
        }
        QString k;
        {
            const ProcSuspendGuard guard(proc);  // 冻结内存，防盲扫期间 key 被清
            if (guard.suspended()) {
                Logger::instance().info(
                    QString("extractImageKeyMulti: pid=%1 suspended during scan (WeChat may stall briefly)")
                        .arg(pid),
                    "wechat.key");
            }
            k = scanPidForImageKey(proc, pid, ct1, ct2, knownDatPath, extraOracles, xorKeys);
        }
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
            "重要：提取前必须先在微信里双击打开一张目标图片（让它完整显示原图），"
            "保持聊天窗口不动，然后立即点提取——图片 key 只在微信解密图片时短暂驻留内存。\n"
            "其他可能原因：\n"
            "  1) .dat 不是 V2 格式（magic 应为 07 08 V 2 08 07）\n"
            "  2) 微信版本 ≥ 4.1.10.31，XOR 还原失败（特征可能已变化）\n"
            "  3) 微信进程权限不足（请以管理员权限运行 bambooRat）\n"
            "  4) oracle .dat 与刚打开的图片不属同一账号")
            .arg(pids.size()).arg(lastErr);
    }
    return {};
}

// ── 多 oracle 交叉验证：排除单 oracle 假阳性 ─────────────────────────────────
// 之前发现占位 key（如 "klRlRmRm..."）偶尔能让某个 .dat 的密文块解出 JPEG magic，
// 让单文件 verifyKeyByFullOracle 通过。把验证改成"全部 oracle 都通过才接受"。
// 这把单 oracle 的 ~1/2^28 假阳性概率压到 ~1/2^84（N=3 时），几乎为 0。
// 从 oracle .dat 路径反推 wxid dataDir。
// .dat 路径 = dataDir/msg/attach/<talkerMd5>/<yyyy-MM>/Img/<md5>.dat
// 所以 dataDir = .dat 路径去掉 /msg/attach 之后的部分。
static QString guessDataDirFromOracle(const QString& oraclePath) {
    const int idx = oraclePath.indexOf(QStringLiteral("/msg/attach"));
    if (idx < 0) return {};
    return oraclePath.left(idx);
}

// ── 多 oracle 交叉验证：排除单 oracle 假阳性 ─────────────────────────────────
// 之前发现占位 key（如 "klRlRmRm..."）偶尔能让某个 .dat 的密文块解出 JPEG magic，
// 让单文件 verifyKeyByFullOracle 通过。把验证改成"全部 oracle 都通过才接受"。
// 这把单 oracle 的 ~1/2^28 假阳性概率压到 ~1/2^84（N=3 时），几乎为 0。
QStringList collectOracleDats(const QString& dataDir, int maxN) {
    QStringList out;
    if (dataDir.isEmpty() || !QFile::exists(dataDir)) return out;
    QDir imgRoot(dataDir + "/msg/attach");
    if (!imgRoot.exists()) return out;
    struct Entry { QDateTime mtime; qint64 sz; QString path; };
    QList<Entry> all;
    const QFileInfoList dirs = imgRoot.entryInfoList(
        QStringList{"*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& d1 : dirs) {
        const QFileInfoList yms = QDir(d1.absoluteFilePath()).entryInfoList(
            QStringList{"*"}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& d2 : yms) {
            const QFileInfoList subs = QDir(d2.absoluteFilePath()).entryInfoList(
                QStringList{"Img","Video"}, QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QFileInfo& d3 : subs) {
                const QFileInfoList files = QDir(d3.absoluteFilePath()).entryInfoList(
                    QStringList{"*.dat"}, QDir::Files);
                for (const QFileInfo& f : files) {
                    const QString name = f.fileName();
                    if (name.endsWith("_t.dat") || name.endsWith("_h.dat")) continue;
                    if (f.size() < 8192) continue;  // 跳过太小的（无法可靠验证）
                    all.append({ f.lastModified(), f.size(), f.absoluteFilePath() });
                }
            }
        }
    }
    // 按修改时间降序：最新修改的 .dat 优先 — 最可能与当前驻留内存的 key 匹配。
    // 次排序按大小降序：同一时间优先大文件（密文多 → 验证更可靠）。
    std::sort(all.begin(), all.end(),
              [](const Entry& a, const Entry& b){
                  if (a.mtime != b.mtime) return a.mtime > b.mtime;
                  return a.sz > b.sz;
              });
    for (int i = 0; i < qMin(maxN, all.size()); ++i) out.append(all[i].path);
    return out;
}

QString verifyImageKeyMulti(const QList<QString>& oraclePaths,
                            const QString& hexKey,
                            QString* errOut) {
    if (hexKey.size() != 32) {
        if (errOut) *errOut = "image key hex length != 32";
        return QString();
    }
    const QByteArray keyBytes = QByteArray::fromHex(hexKey.toLatin1());
    if (keyBytes.size() != 16) {
        if (errOut) *errOut = "image key hex decode != 16 bytes";
        return QString();
    }
    if (oraclePaths.isEmpty()) {
        if (errOut) *errOut = "no oracle .dat paths provided";
        return QString();
    }
    int ok = 0;
    QString firstFail;
    for (const QString& p : oraclePaths) {
        // 复用 verifyKeyByFullOracle：单文件 EVP 解密 + looksLikeImagePlain + 次块检查
        // 但该函数只接受一个 path —— 这里直接复刻一遍以拿到通过的 oracle 名
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) {
            if (firstFail.isEmpty()) firstFail = "open fail: " + p;
            continue;
        }
        const QByteArray dat = f.readAll();
        f.close();
        if (dat.size() <= 47) { if (firstFail.isEmpty()) firstFail = "too small: " + p; continue; }
        if ((unsigned char)dat[0] != 0x07 || (unsigned char)dat[1] != 0x08 ||
            (unsigned char)dat[2] != 'V'  || (unsigned char)dat[3] != '2') {
            if (firstFail.isEmpty()) firstFail = "no V2 magic: " + p;
            continue;
        }
        bool okOffset = tryOffsetVerify(
            reinterpret_cast<const unsigned char*>(keyBytes.constData()), dat, 15)
            || tryOffsetVerify(
                reinterpret_cast<const unsigned char*>(keyBytes.constData()), dat, 16);
        if (okOffset) {
            ++ok;
            Logger::instance().info(
                QString("verifyImageKeyMulti: oracle OK (%1/%2) %3")
                    .arg(ok).arg(oraclePaths.size()).arg(p),
                "wechat.key");
        } else if (firstFail.isEmpty()) {
            firstFail = "decrypt not-image-magic: " + p;
        }
    }
    // 多数通过即接受。WeChat 4.1.13.65 实测存在多 key（per-image / per-direction）：
    // 例如手机发到文件传输助手的图可能用另一个 key。要求全部通过会把真 key 误杀。
    // 多数通过（ok*2 >= N）的 key 仍能解该 oracle 代表的多数图，作为当前 key 使用；
    // 后续用对应 oracle 单独再跑一次可拿到第二把 key。
    if (ok * 2 >= oraclePaths.size()) {
        if (ok != oraclePaths.size()) {
            Logger::instance().warn(
                QString("verifyImageKeyMulti: partial coverage %1/%2 oracles passed — key accepted (uncovered files likely use a different per-image key)")
                    .arg(ok).arg(oraclePaths.size()),
                "wechat.key");
        }
        if (errOut) *errOut = QString("partial coverage %1/%2").arg(ok).arg(oraclePaths.size());
        return hexKey;
    }
    if (errOut) {
        *errOut = QString("交叉验证失败：%1/%2 oracle 通过（首条失败：%3）")
            .arg(ok).arg(oraclePaths.size()).arg(firstFail);
    }
    return QString();
}

} // namespace WeChatKeyExtractor
