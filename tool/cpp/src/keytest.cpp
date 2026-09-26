// placeholder — 密钥验证工具文件丢失导致 CMake 配置失败
// 之前用于微信密钥提取端到端验证，已经废弃。
// 重新生成空文件仅为了让 `add_executable(keytest ...)` configure 通过；
// 此 binary 不会被实际使用，可删除对应 CMakeLists.txt target。
int main() { return 0; }