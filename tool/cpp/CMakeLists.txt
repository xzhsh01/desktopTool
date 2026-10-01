cmake_minimum_required(VERSION 3.16)
project(bambooRat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── 源文件字符集：全部按 UTF-8 处理 ───────────────────────────────
# MSVC: /utf-8 让编译器把字符串字面量当 UTF-8 读，并写入 UTF-8 字节到 .rdata
# MinGW GCC: -finput-charset=UTF-8 让 GCC 按 UTF-8 解析源文件字节
# （Windows MSYS2 的 GCC 默认 locale 行为在不同 shell 下可能切换到 GBK，
#  导致源文件里的中文 / emoji 被错误解码为 GBK 字节序列，运行时按 UTF-8 显示就是乱码）
if(MSVC)
    add_compile_options(/utf-8)
else()
    add_compile_options(-finput-charset=UTF-8 -fwide-exec-charset=UTF-8)
endif()

# app.rc 中使用相对路径引用图标，需让 windres 能从源码根目录解析
if(MINGW)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I ${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Qt6 在桌面 UI 模块里是 REQUIRED；没有 Qt 时降级为只编纯 C/C++ 子集 + smoke 测试。
# BR_REQUIRE_QT=OFF 选项强制走降级路径（用于"装 Qt 之前先用 smoke 验证工具链"的场景）。
option(BR_REQUIRE_QT "Require Qt6 (fails if not found)" ON)
if(BR_REQUIRE_QT)
    find_package(Qt6 REQUIRED COMPONENTS Widgets Network Sql Gui Core)
else()
    find_package(Qt6 COMPONENTS Widgets Network Sql Gui Core QUIET)
    if(NOT Qt6_FOUND)
        message(WARNING
            "============================================================\n"
            " Qt6 not found — building Qt-free subset only:\n"
            "   - sqlite3 (static)\n"
            "   - bamboo_toolchain_check (smoke exe)\n"
            " Re-run with -DBR_REQUIRE_QT=ON (default) after installing Qt\n"
            " to build the full bambooRat / verifykey / keytest executables.\n"
            "============================================================")
    endif()
endif()

# OpenSSL: 邮件加密 (PBKDF2 + AES-256-GCM, S/MIME via CMS API)
# Windows 下没有系统级 OpenSSL，回退到 Qt MinGW 发行版自带的 MSYS2 opt 目录
# （与 scripts/setup_libssh.ps1 使用的路径一致）；已通过 -DOPENSSL_ROOT_DIR 指定时以用户设置为准
if(NOT DEFINED OPENSSL_ROOT_DIR)
    foreach(_openssl_candidate
            "E:/Qt/Tools/mingw1310_64/opt"
            "C:/Qt/Tools/mingw1310_64/opt")
        if(EXISTS "${_openssl_candidate}/include/openssl/ssl.h")
            set(OPENSSL_ROOT_DIR "${_openssl_candidate}")
            break()
        endif()
    endforeach()
endif()
# Qt-less 模式允许 OpenSSL 也缺（不编 bambooRat / openssl_shim / libssh 链接的产物）。
# 装 Qt 之后 Qt 自带 MSYS2 opt 会让 OpenSSL 自动可被找到。
find_package(OpenSSL QUIET)
if(NOT OPENSSL_FOUND)
    message(WARNING
        "  OpenSSL not found — openssl_shim / bambooRat / libssh 链接的 target 会跳过。"
        " 装 Qt 后重新配置即可（Qt 自带 MSYS2 opt 里有 OpenSSL）。")
endif()

# ---------------------------------------------------------------------------
# SQLite (amalgamation 源码直接编译，纯 C 接口，无 ABI 风险)
# ---------------------------------------------------------------------------
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
if(WIN32)
    target_compile_definitions(sqlite3 PRIVATE SQLITE_OS_WIN=1)
endif()

# ---------------------------------------------------------------------------
# zstd 1.5.2 (仅 decompress + 必要的 common，单线程) ——
#   微信 4.x 把 message_content 用 zstd 压缩后存进 cache.sqlite，要解压才能拿到
#   <emoji md5="..."/> 等 XML 元数据。third_party/zstd/ 通过
#   scripts/setup_zstd.ps1 或手工 `tar -xzf` 解压 zstd-1.5.2 lib/{common,decompress} 取得。
# ---------------------------------------------------------------------------
add_library(zstd STATIC
    third_party/zstd/common/entropy_common.c
    third_party/zstd/common/error_private.c
    third_party/zstd/common/fse_decompress.c
    third_party/zstd/common/xxhash.c
    third_party/zstd/common/zstd_common.c
    third_party/zstd/decompress/huf_decompress.c
    third_party/zstd/decompress/zstd_ddict.c
    third_party/zstd/decompress/zstd_decompress.c
    third_party/zstd/decompress/zstd_decompress_block.c
)
target_include_directories(zstd PUBLIC third_party/zstd)
# 单线程编译（multi-thread 解压需要 pool.c/threading.c，我们没要）
target_compile_definitions(zstd PRIVATE
    ZSTD_MULTITHREAD=0
    ZSTD_NO_UNUSED_FUNCTIONS=0
)

# ---------------------------------------------------------------------------
# openssl_shim：OpenSSL 1.1 -> 3.x ABI 兼容垫片（libssh 静态库依赖）
# 仅在需要 Qt 全量编译时构建（Qt-less 子集不需要）。
# ---------------------------------------------------------------------------
if(Qt6_FOUND AND OPENSSL_FOUND)
add_library(openssl_shim STATIC src/compat/openssl_shim.c)
target_link_libraries(openssl_shim PUBLIC OpenSSL::Crypto)
# 纯 C 桩库，不需要 Qt 元对象处理；显式关闭 AUTOMOC 避免触发 g++ 预处理开销
set_target_properties(openssl_shim PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
endif()

# ---------------------------------------------------------------------------
# libssh (C 语言 SSH/SFTP 客户端库)
#   静态库 + 头文件需放在 third_party/libssh/，使用 scripts/setup_libssh.ps1 构建
#   缺失时给出清晰提示，不静默回退
# ---------------------------------------------------------------------------
set(LIBSSH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libssh")
if(EXISTS "${LIBSSH_DIR}/include/libssh/libssh.h" AND
   EXISTS "${LIBSSH_DIR}/lib/libssh.a")
    # 找到 libssh
else()
    message(FATAL_ERROR
        "libssh not found at ${LIBSSH_DIR}.\n"
        "Run: powershell scripts/setup_libssh.ps1\n"
        "to download and build libssh from source.")
endif()

# ---------------------------------------------------------------------------
# FreeRDP（可选）：RDP 内嵌渲染后端
#   用 scripts/setup_freerdp.ps1 下载并从源码编译 FreeRDP 到 third_party/freerdp/
#   然后以 -DBR_WITH_FREERDP=ON 重新配置。
#   未启用时不编译 FreeRDP 相关代码（RDPEmbedded 保持桩实现），不影响构建。
# ---------------------------------------------------------------------------
option(BR_WITH_FREERDP "Build with embedded FreeRDP RDP backend" OFF)
set(FREERDP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freerdp")
set(FREERDP_LIBS "")
set(FREERDP_INCLUDE "")
set(FREERDP_DEFINE "")
if(BR_WITH_FREERDP)
    # FreeRDP 3.x 安装布局：头文件在 include/freerdp3 与 include/winpr3（带版本后缀），
    # 库为 libfreerdp3.a / libwinpr3.a / libfreerdp-client3.a（通道与 client common）
    if(EXISTS "${FREERDP_DIR}/lib/libfreerdp3.a" AND
       EXISTS "${FREERDP_DIR}/lib/libfreerdp-client3.a" AND
       EXISTS "${FREERDP_DIR}/include/freerdp3/freerdp/freerdp.h" AND
       EXISTS "${FREERDP_DIR}/include/winpr3/winpr/winpr.h")
        set(FREERDP_INCLUDE
            "${FREERDP_DIR}/include/freerdp3"
            "${FREERDP_DIR}/include/winpr3"
            # 官方 Windows 剪贴板后端源码目录：br_cliprdr_win.c 复用 wf_cliprdr.c
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freerdp_src/client/Windows")
        set(FREERDP_LIBS
            # 静态链接顺序：freerdp-client 依赖 freerdp/winpr，必须放在其前
            ${FREERDP_DIR}/lib/libfreerdp-client3.a
            ${FREERDP_DIR}/lib/libfreerdp3.a
            ${FREERDP_DIR}/lib/libwinpr3.a
            # winpr/freerdp 在 Windows 上的系统依赖（静态 .a 手动链接时需显式补齐）：
            -lntdll      # RtlNtStatusToDosError / NtWaitForSingleObject / NtDeviceIoControlFile
            -lncrypt     # NCryptOpenStorageProvider 等（CNG 密钥存储）
            -lrpcrt4     # UuidCreate/UuidToStringA/RpcStringFreeA
            -ldbghelp    # SymInitialize/SymFromAddr（崩溃回溯）
            -lshlwapi    # PathFileExistsW
            -lwinmm      # rdpsnd 音频后端（waveOut*）
            -lcredui     # freerdp passphrase.c: CredUICmdLinePromptForCredentialsW
            # libssh 与 winpr 均为 MinGW 打包了 strndup 兼容实现（语义一致），
            # 静态链接时冲突，允许重复定义取先者
            -Wl,--allow-multiple-definition)
        set(FREERDP_DEFINE "BR_WITH_FREERDP=1")
        message(STATUS "FreeRDP embedded backend enabled: ${FREERDP_DIR}")
    else()
        message(FATAL_ERROR
            "BR_WITH_FREERDP=ON but FreeRDP not found at ${FREERDP_DIR}.\n"
            "Run: powershell scripts/setup_freerdp.ps1\n"
            "to download and build FreeRDP from source.")
    endif()
endif()

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------
# 仅在 Qt6 找到时定义 COMMON_SOURCES（这些 .cpp 全部依赖 QtCore 或 QtWidgets），
# 找不到 Qt 时只编纯 C 子集 + smoke 测试。
set(COMMON_SOURCES
    resources/resources.qrc
    resources/app.rc
    src/main.cpp
    src/app/AboutWidget.cpp
    src/app/EmojiFont.cpp
    src/app/DashboardWidget.cpp
    src/app/LogsWidget.cpp
    src/app/MainWindow.cpp
    src/app/SettingsWidget.cpp
    src/connections/ConnectionManager.cpp
    src/connections/ConnectionsWidget.cpp
    src/core/Crypto.cpp
    src/core/Labels.cpp
    src/core/Logger.cpp
    src/core/Settings.cpp
    src/database/DatabaseClient.cpp
    src/database/DatabaseWidget.cpp
    src/database/DbSessionPane.cpp
    src/database/DriverInstaller.cpp
    src/database/SqlEditor.cpp
    src/database/TableDataIO.cpp
    src/document/AttachmentStore.cpp
    src/document/DocumentWidget.cpp
    src/mail/AccountDialog.cpp
    src/mail/ImapClient.cpp
    src/mail/MailAccountManager.cpp
    src/mail/MailPoller.cpp
    src/mail/MailSelfTest.cpp
    src/mail/MailStore.cpp
    src/mail/MailWidget.cpp
    src/mail/ScheduledQueue.cpp
    src/mail/SmtpClient.cpp
    src/mail/TemplateStore.cpp
    src/mail/crypto/CertificateManager.cpp
    src/mail/crypto/MailEncryptor.cpp
    src/mail/crypto/PasswordCrypto.cpp
    src/mail/crypto/SmimeCrypto.cpp
    src/mail/ui/CertManagerDialog.cpp
    src/mail/ui/folders/MailFolderPanel.cpp
    src/mail/ui/list/MailListPanel.cpp
    src/mail/ui/list/MailListDelegate.cpp
    src/mail/ui/content/MailContentPanel.cpp
    src/mail/ui/content/MailPreviewBrowser.cpp
    src/mail/ui/content/AttachmentPreviewPane.cpp
    src/mail/ui/MarkdownBridge.cpp
    src/rdp/RDPClient.cpp
    src/rdp/RDPRender.cpp
    src/rdp/RDPEmbedded.cpp
    src/rdp/br_cliprdr_win.c
    src/rdp/KeyGrabber.cpp
    src/rdp/RDPSessionView.cpp
    src/rdp/RDPTabArea.cpp
    src/rdp/RDPWidget.cpp
    src/redis/RedisClient.cpp
    src/redis/RedisWidget.cpp
    src/ssh/FileManagerWidget.cpp
    src/ssh/SFTPClient.cpp
    src/ssh/SSHClient.cpp
    src/ssh/SSHTermWidget.cpp
    src/ssh/TerminalEmulator.cpp
    src/wechat/WeChatAccountManager.cpp
    src/wechat/CacheDb.cpp
    src/wechat/WeChatConfigDialog.cpp
    src/wechat/WeChatDb.cpp
    src/wechat/WeChatImageDecoder.cpp
    src/wechat/WeChatKeyExtractor.cpp
    src/wechat/WeChatSyncWorker.cpp
    src/wechat/WeChatWidget.cpp
    src/wechat/WeChatWorker.cpp
    src/wechat/ui/WeChatSidebar.cpp
    src/wechat/ui/WeChatDetailPanel.cpp
    src/wechat/ui/WeChatListPanel.cpp
)

# ---------------------------------------------------------------------------
# bambooRat（含嵌入式 SQLite 配置存储）—— 需要 Qt6
# ---------------------------------------------------------------------------
if(TARGET Qt6::Widgets)
add_executable(bambooRat WIN32
    ${COMMON_SOURCES}
    src/core/ConfigStore.cpp
)
target_include_directories(bambooRat PRIVATE src)
target_include_directories(bambooRat PRIVATE "${LIBSSH_DIR}/include")
target_include_directories(bambooRat PRIVATE ${FREERDP_INCLUDE})
# libssh 头文件里 LIBSSH_API 在 Win 下默认是 __declspec(dllimport)，
# 静态链接时必须定义 LIBSSH_STATIC 让它变成空宏
target_compile_definitions(bambooRat PRIVATE LIBSSH_STATIC)
target_compile_definitions(bambooRat PRIVATE ${FREERDP_DEFINE})
target_link_libraries(bambooRat PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Sql
    Qt6::Gui
    Qt6::Core
    sqlite3
    zstd
    openssl_shim
    # 注: libssh.a 是按 WITH_ZLIB=OFF 编译的，不引用 deflate/inflate/gz_*；
    # Qt 6.12 自身又 QT_FEATURE_system_zlib=OFF，所以这里不再链接系统 zlib。
)
# libssh.a (static) 与 libcrypto.dll.a 之间存在循环引用：用 --start-group/--end-group
# 让链接器反复扫描直到收敛；同一 group 内还需要 ws2_32 / crypt32 / iphlpapi 这些
# Windows 系统库（libssh 会调用 WSAStartup / getaddrinfo 等）。
target_link_libraries(bambooRat PRIVATE
    "-Wl,--start-group"
    ${LIBSSH_DIR}/lib/libssh.a
    ${FREERDP_LIBS}
    OpenSSL::Crypto
    OpenSSL::SSL
    crypt32
    ws2_32
    iphlpapi
    winmm
    ole32
    uuid       # wf_cliprdr: IID_IDataObject / IID_IStream 等 COM 接口 ID
    gdi32
    user32
    dwmapi
    "-Wl,--end-group"
)
endif() # /bambooRat

# ---------------------------------------------------------------------------
# verifykey: 手动密钥验证工具（验证 4.1.x 恢复的 passphrase）—— 需要 Qt6
# ---------------------------------------------------------------------------
if(TARGET Qt6::Core)
add_executable(verifykey
    src/verifykey.cpp
    src/wechat/WeChatDb.cpp
    src/core/Logger.cpp
    src/core/Settings.cpp
)
target_include_directories(verifykey PRIVATE src)
target_link_libraries(verifykey PRIVATE
    Qt6::Core
    Qt6::Sql
    OpenSSL::Crypto
    sqlite3
)
endif()

# ---------------------------------------------------------------------------
# keytest: 微信密钥提取端到端验证工具（临时，验证后删除）—— 需要 Qt6
# ---------------------------------------------------------------------------
if(TARGET Qt6::Core)
add_executable(keytest
    src/keytest.cpp
    src/wechat/WeChatKeyExtractor.cpp
    src/wechat/WeChatDb.cpp
    src/core/Logger.cpp
    src/core/Settings.cpp
)
target_include_directories(keytest PRIVATE src)
target_link_libraries(keytest PRIVATE
    Qt6::Core
    Qt6::Sql
    OpenSSL::Crypto
    sqlite3
    zstd
    crypt32
    psapi
)
endif()

# ---------------------------------------------------------------------------
# zstd_test: 验证 4.x emoji zstd 解压 + md5 提取 (临时)
# ---------------------------------------------------------------------------
if(TARGET Qt6::Core)
add_executable(zstd_test
    src/zstd_test.cpp
    src/wechat/WeChatDb.cpp
    src/core/Logger.cpp
    src/core/Settings.cpp
)
target_include_directories(zstd_test PRIVATE src)
target_link_libraries(zstd_test PRIVATE
    Qt6::Core
    Qt6::Sql
    OpenSSL::Crypto
    sqlite3
    zstd
)
endif()

# ---------------------------------------------------------------------------
# bamboo_toolchain_check: Qt-free 烟雾测试（验证 cmake+ninja+gcc 链 + sqlite3）
#   不依赖 Qt / OpenSSL，作为 Qt-less 子集的主输出。装 Qt 之后仍可作为 bambooRat.exe 启动前的基础检查。
# ---------------------------------------------------------------------------
add_executable(bamboo_toolchain_check src/toolchain_check.cpp)
target_include_directories(bamboo_toolchain_check PRIVATE src)
target_link_libraries(bamboo_toolchain_check PRIVATE sqlite3)


