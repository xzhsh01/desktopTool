#pragma once

#include <QString>
#include <QList>

/**
 * WeChatKeyExtractor: 从运行中的微信进程内存提取数据库密钥
 *
 * 微信运行时数据库密钥（32 字节）驻留在进程内存中：
 *   - 4.x（Weixin.exe）：密钥对象被堆上某结构引用，
 *     结构特征（小端 64 位）：{ ptr→key, 0, 0x20, 0x2F }
 *     在 MEM_PRIVATE 可读写区域搜索该特征，解引用 ptr 得到密钥。
 *   - 4.1.10.31+：明文密钥被移出内存，结构中 ptr 指向的是 XOR 混淆值；
 *     XOR key 藏在 Weixin.dll 代码段（mov rdx,imm64 ×4 + test rax,rax 特征）。
 *     候选 XOR 还原后才是 passphrase，再经 WeChatDb::verifyKey 校验。
 *   - 3.x（WeChat.exe）：WeChatWin.dll 模块可写段中存在
 *     { ptr→key, 0x20 } 结构，同样解引用验证。
 *
 * 候选密钥用 WeChatDb::verifyKey（解密数据库首页做 HMAC 校验）确认。
 */
namespace WeChatKeyExtractor {

struct ProcessInfo {
    quint32 pid = 0;
    quint32 parentPid = 0;
    int version = 0;      // 3 / 4
    QString exeName;     // Weixin.exe / WeChat.exe
};

// 枚举运行中的微信进程（主进程优先：父进程不是微信进程；4.x 优先于 3.x）
QList<ProcessInfo> findRunningWeChat();

// 从指定进程提取密钥；dbPath 用于验证（4.x: message_0.db；3.x: Msg/MicroMsg.db）
// 成功返回 64 位十六进制密钥，失败返回空串并设置 errOut
QString extractKey(quint32 pid, int version, const QString& dbPath,
                   QString* errOut = nullptr);

// 提取指定进程中的所有候选密钥（结构特征筛选 + 去重，未做数据库验证）
// 用于多账号场景：一次内存扫描，逐账号快速验证
QStringList extractAllKeys(quint32 pid, int version, QString* errOut = nullptr);

// 从进程加载的 Weixin.dll 代码段提取 XOR key（4.1.10.31+ 密钥混淆用）
// 返回 64 位十六进制列表（通常 1~2 枚）；仅 4.x 有效
QStringList extractXorKeys(quint32 pid, QString* errOut = nullptr);

// 便捷入口：自动寻找运行中的微信进程并提取（逐个进程尝试，用 dbPath 验证）
QString extractFromRunningWeChat(const QString& dbPath, QString* errOut = nullptr);

} // namespace WeChatKeyExtractor
