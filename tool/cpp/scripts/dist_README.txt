__TARGET__ (Windows x64) - Release 绿色版

构建时间: __STAMP__
构建配置: Release / MinGW 13.1.0 / Qt 6.10.3 / OpenSSL 1.1

使用方法
  1. 解压到任意目录
  2. 双击 __TARGET__.exe 运行
  3. 首次运行会弹出 Windows Defender SmartScreen 提示（程序未做代码签名），
     点击"更多信息" -> "仍要运行"即可

数据目录（首次运行自动创建）
  %APPDATA%\KFrame\bambooRat\settings.json      应用设置
  %APPDATA%\KFrame\bambooRat\connections.json   连接配置（密码 DPAPI 加密）
  %APPDATA%\KFrame\bambooRat\logs\app.log       日志

注意事项
  · 密码使用 Windows DPAPI 加密，与当前 Windows 用户账户绑定。
    把整个目录复制到另一台机器或另一个用户账户后，已保存的密码无法解密，
    需要重新输入。连接配置本身（主机/端口/用户名）可以正常迁移。
  · 远程桌面功能调用系统自带的 mstsc.exe，无需额外安装。
  · 数据库功能中 SQLite 为内置支持；MySQL / PostgreSQL / ODBC 需要在
    本机安装对应的客户端驱动，程序内提供了驱动安装引导。
