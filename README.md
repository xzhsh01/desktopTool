# bambooRat (BR) (C++ / Qt6)

Electron + Vue 版本的纯 C++ 重构。使用 Qt6 Widgets 构建原生桌面应用。

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

## 目录结构

```
cpp/
├── CMakeLists.txt          # 构建配置
├── resources/              # 资源文件
│   └── resources.qrc
└── src/
    ├── main.cpp            # 入口
    ├── core/               # 核心基础设施
    │   ├── Settings        # 设置持久化 (settings.json)
    │   ├── ConnectionManager  # 连接管理 (connections.json, 密码 DPAPI 加密)
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

## 构建

### 依赖

- CMake 3.21+
- Qt 6.5+ (Widgets, Network, Sql)
- QSsh：从 Qt Creator 仓库抽取到 `third_party/qssh/`（脚本自动完成）
- 可选第三方库（未安装时对应功能禁用）：
    - hiredis（Redis）

### Windows

```powershell
# 1. 抽取 QSsh 源码（首次构建时）
powershell scripts/setup_qssh.ps1

# 2. 安装可选依赖
vcpkg install hiredis --triplet x64-windows

# 3. 配置构建
cmake -B build -S . `
  -DCMAKE_PREFIX_PATH=<Qt安装路径> `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake `
  -DHAVE_HIREDIS=ON

# 4. 编译
cmake --build build --config Release
```

QSsh 是构建硬依赖：源码缺失时 CMake 会报错并指引运行 `setup_qssh.ps1`。

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

### 数据存储

- `%APPDATA%/KFrame/bambooRat/settings.json` — 应用设置
- `%APPDATA%/KFrame/bambooRat/connections.json` — 连接配置（密码 DPAPI 加密）
- `%APPDATA%/KFrame/bambooRat/logs/app.log` — 日志文件

## 与 Electron 版的对应关系

| Electron 版 | C++ 版 |
|-------------|--------|
| main.ts (窗口/托盘/IPC) | MainWindow + main.cpp |
| preload.ts (IPC 桥) | 直接 C++ 方法调用 |
| ssh.ts | SSHTermWidget (内嵌 QSsh::SshConnection) |
| sftp.ts | FileManagerWidget (内嵌 QSsh::SftpChannel) |
| redis.ts | backend/RedisClient |
| database.ts | backend/DatabaseClient |
| rdp.ts | backend/RDPClient |
| Vue 视图 | Qt Widgets |
| xterm.js | ui/TerminalEmulator |
| Pinia stores | core 单例类 |
| electron-log | core/Logger |
| safeStorage | core/Crypto (DPAPI) |
