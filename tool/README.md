# bambooRat

基于 Qt6 / C++ 的多功能桌面工具集（原生应用，无 Electron 依赖）。

## 功能特性

- 🔗 **连接管理** - 统一管理 SSH、数据库、Redis、远程桌面等连接
- 💻 **SSH 终端** - 多标签终端，自研 ANSI/VT100 模拟器
- 🗄️ **数据库管理** - 支持 MySQL、PostgreSQL、SQLite、SQL Server、Oracle
- ⚡ **Redis 客户端** - Redis 键值查看和管理
- 🖥️ **远程桌面** - RDP 远程桌面连接（mstsc / xfreerdp）
- 📁 **文件管理** - SFTP 远程文件浏览、上传、下载
- 📋 **日志系统** - 内存 + 文件双通道日志
- ⚙️ **设置管理** - JSON 持久化配置
- 🔐 **密码加密** - Windows DPAPI
- 🎨 **现代 UI** - 无边框窗口 + 侧边栏导航 + 深色主题

## 技术栈

| 模块 | 技术 |
|------|------|
| UI 框架 | Qt 6.5+ (Widgets) |
| SSH/SFTP | QSsh (Qt Creator) |
| Redis | hiredis |
| 数据库 | Qt SQL (QMYSQL/QPSQL/QSQLITE/QODBC) |
| RDP | 系统 mstsc.exe / xfreerdp |
| 终端模拟 | 自研 TerminalEmulator (ANSI/VT100) |
| 密码加密 | Windows DPAPI |
| 构建 | CMake 3.21+ |

## 构建

### 依赖

- CMake 3.21+
- Qt 6.5+ (Widgets, Network, Sql)
- QSsh：从 Qt Creator 抽取到 `cpp/third_party/qssh/`（首次构建自动完成）
- 可选第三方库（未安装时对应功能禁用）：
  - hiredis（Redis）

### Windows

```powershell
# 1. 抽取 QSsh 源码（首次构建时）
powershell cpp/scripts/setup_qssh.ps1

# 2. 安装可选依赖
vcpkg install hiredis --triplet x64-windows

# 3. 配置构建
cmake -B cpp/build -S cpp `
  -DCMAKE_PREFIX_PATH=<Qt安装路径> `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake `
  -DHAVE_HIREDIS=ON

# 4. 编译
cmake --build cpp/build --config Release
```

QSsh 是构建硬依赖：源码缺失时 CMake 会报错并指引运行 `setup_qssh.ps1`。

## 项目结构

```
tool/
└── cpp/
    ├── CMakeLists.txt          # 构建配置
    ├── resources/              # 资源文件 (图标 / qrc / rc)
    └── src/
        ├── main.cpp            # 入口（跨线程元类型注册）
        ├── core/               # 核心基础设施
        │   ├── Settings        # 设置持久化 (settings.json)
        │   ├── ConnectionManager  # 连接管理 (connections.json)
        │   ├── Logger          # 日志 (内存 + 文件)
        │   └── Crypto          # DPAPI 加密
        ├── backend/            # 协议客户端（工作线程）
        │   ├── RedisClient     # Redis (hiredis)
        │   ├── DatabaseClient  # 数据库 (Qt SQL)
        │   └── RDPClient       # RDP (系统客户端调用)
        └── ui/                 # 视图
            ├── MainWindow      # 主窗口 (无边框 + 侧边栏 + 托盘)
            ├── DashboardWidget # 仪表盘
            ├── ConnectionsWidget  # 连接管理
            ├── SSHTermWidget   # SSH 终端 (多标签)
            ├── TerminalEmulator   # ANSI 终端模拟器
            ├── FileManagerWidget # SFTP 文件管理
            ├── RedisWidget     # Redis GUI
            ├── DatabaseWidget  # 数据库客户端
            ├── RDPWidget       # 远程桌面
            ├── SettingsWidget  # 设置
            ├── LogsWidget      # 日志查看
            └── AboutWidget     # 关于
```

## 架构说明

### SSH/SFTP 集成（QSsh）

`SSHTermWidget` 和 `FileManagerWidget` 直接持有 `QSsh::SshConnection`，无需 worker 线程：

```
Widget (主线程)
  ├─ QSsh::SshConnection       # 连接 + 认证
  ├─ QSsh::SshRemoteProcess    # shell（仅终端）
  └─ QSsh::SftpChannel         # SFTP（仅文件管理）
       └─ QSsh::SftpTransferJob  # 上传/下载任务
```

所有异步操作通过 QSsh 内置信号槽完成（`connected` / `readyReadStandardOutput` / `fileInfoAvailable` / `finished`），无需手动管理线程。

### Redis/Database 线程模型

非 SSH 协议仍走 `QThread` 工作线程 + 命令队列模型：

```
主线程 (UI)                    工作线程
┌─────────────┐   命令队列    ┌──────────────┐
│  Widget     │ ───────────→ │  Worker      │
│             │              │  (hiredis /  │
│             │ ←─────────── │   Qt SQL)    │
└─────────────┘   信号(queued) └──────────────┘
```

- 主线程：UI 渲染、用户交互
- 工作线程：阻塞的协议 I/O，通过互斥锁保护的命令队列接收指令
- 信号槽（自动队列连接）回传数据，线程安全
- 跨线程自定义类型在 `main.cpp` 中通过 `qRegisterMetaType` 注册

### 数据存储

- `%APPDATA%/bambooRat/settings.json` — 应用设置
- `%APPDATA%/bambooRat/connections.json` — 连接配置（密码 DPAPI 加密）
- `%APPDATA%/bambooRat/logs/app.log` — 日志文件

## 许可证

MIT License
