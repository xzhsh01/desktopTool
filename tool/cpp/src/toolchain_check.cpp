// bamboo_toolchain_check
// Qt-free 工具链烟雾测试：验证 cmake + ninja + MinGW + sqlite3 链路打通。
// 装 Qt 之后这个 target 仍然保留，作为 bambooRat.exe 启动前的基础检查。
//
// 用法: build\bamboo_toolchain_check.exe
//   退出码 0 = 全部 OK；非 0 = 失败（控制台有诊断信息）

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sqlite3.h>

namespace {

// ── 1. sqlite3 编译/链接/运行 ────────────────────────────────────────
bool check_sqlite() {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "  [X] sqlite3_open: %s\n", sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        return false;
    }
    const char* sql =
        "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT);"
        "INSERT INTO t(name) VALUES('bamboo');";
    char* errmsg = nullptr;
    rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "  [X] sqlite3_exec: %s\n", errmsg ? errmsg : "?");
        if (errmsg) sqlite3_free(errmsg);
        sqlite3_close(db);
        return false;
    }

    // SELECT count(*) 用回调函数验证查询路径通
    auto cb = [](void* arg, int n, char** vals, char** cols) -> int {
        int* count = static_cast<int*>(arg);
        if (n > 0 && vals[0]) *count = std::atoi(vals[0]);
        return 0;
    };
    int count = -1;
    rc = sqlite3_exec(db, "SELECT count(*) FROM t", cb, &count, &errmsg);
    if (rc != SQLITE_OK || count != 1) {
        std::fprintf(stderr, "  [X] sqlite3 SELECT count=%d rc=%d\n", count, rc);
        sqlite3_close(db);
        return false;
    }
    sqlite3_close(db);
    std::printf("  [OK] sqlite3 %s (CREATE/INSERT/SELECT, count=%d)\n",
                SQLITE_VERSION, count);
    return true;
}

// ── 2. Win32 系统 API + GetSystemInfo（验证链接到 kernel32 / user32）───
bool check_win32() {
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    if (si.dwNumberOfProcessors == 0) {
        std::fprintf(stderr, "  [X] GetNativeSystemInfo 返回 0 处理器\n");
        return false;
    }
    char host[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD sz = sizeof(host);
    if (!::GetComputerNameA(host, &sz)) {
        std::fprintf(stderr, "  [X] GetComputerNameA 失败: %lu\n", ::GetLastError());
        return false;
    }
    std::printf("  [OK] Win32 API: %u 处理器 | 计算机名=%s\n",
                si.dwNumberOfProcessors, host);
    return true;
}

// ── 3. vcpkg hiredis.dll（运行时依赖探测）──────────────────────────────
bool check_hiredis_runtime() {
    HMODULE m = LoadLibraryW(L"hiredis.dll");
    if (!m) {
        std::printf("  [!] hiredis.dll 未在 PATH 中（Qt-less 子集不依赖 redis）\n");
        return true;  // 不视为错误
    }
    FreeLibrary(m);
    std::printf("  [OK] hiredis.dll 运行时加载成功\n");
    return true;
}

}  // namespace

int main() {
    std::printf("bamboo_toolchain_check (Qt-less / OpenSSL-less)\n");
#if defined(__GNUC__)
    std::printf("  GCC %d.%d.%d | %s\n",
                __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__,
                "x86_64-w64-mingw32");
#endif

    struct Step { const char* name; bool (*run)(); };
    Step steps[] = {
        { "sqlite3",                       &check_sqlite          },
        { "Win32 API (kernel32)",          &check_win32           },
        { "hiredis.dll (runtime optional)",&check_hiredis_runtime },
    };

    int total = (int)(sizeof(steps) / sizeof(steps[0]));
    int ok = 0;
    for (auto& s : steps) {
        std::printf("  · %s\n", s.name);
        if (s.run()) ++ok;
    }

    std::printf("\nResult: %d/%d passed\n", ok, total);
    return (ok == total) ? 0 : 1;
}