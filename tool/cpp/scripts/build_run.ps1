# 一键构建 + 启动 bambooRat
# 用法：在 PowerShell 里跑 .\scripts\build_run.ps1
#
# 工具链（手工拼接，环境里没有 vcvars/cmake 自动路径）：
#   cmake 4.4.3         C:\Users\Administrator\AppData\Local\Programs\Python\Python312\Scripts\cmake.exe
#   gcc/g++ 13.1.0      C:\Qt\Tools\mingw1310_64\bin
#   mingw32-make        C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe
#   Qt 6.10.3           C:\Qt\6.10.3\mingw_64
#   OpenSSL 1.1 dev     C:\Qt\Tools\mingw1310_64\opt   (编译时) + ..\bin (运行时)
#   libssh 0.11.x       third_party/libssh/   (由 scripts/setup_libssh.ps1 构建)

$ErrorActionPreference = 'Stop'
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BuildDir    = Join-Path $ProjectRoot 'out\build\x64-Debug'
$ScriptsDir  = Join-Path $ProjectRoot 'scripts'
$Cmake       = 'C:\Users\Administrator\AppData\Local\Programs\Python\Python312\Scripts\cmake.exe'
$MingwBin    = 'C:\Qt\Tools\mingw1310_64\bin'
$QtPrefix    = 'C:\Qt\6.10.3\mingw_64'
$Gcc         = Join-Path $MingwBin 'gcc.exe'
$Gxx         = Join-Path $MingwBin 'g++.exe'
$Make        = Join-Path $MingwBin 'mingw32-make.exe'
$OpenSslRoot = 'C:\Qt\Tools\mingw1310_64\opt'

# PATH：先把 MinGW + Qt bin + OpenSSL bin 提前，这样 cmake/moc/qmake 找得到 Qt6 + libssl
$env:Path = "$MingwBin;$QtPrefix\bin;$OpenSslRoot\bin;$env:Path"

# 0) 确保 libssh 静态库已构建（首次需要 ~2 分钟，之后秒跳）
$LibSshRoot = Join-Path $ProjectRoot 'third_party\libssh'
$LibSshLib  = Join-Path $LibSshRoot 'lib\libssh.a'
$LibSshHdr  = Join-Path $LibSshRoot 'include\libssh\libssh.h'
if (-not (Test-Path $LibSshLib) -or -not (Test-Path $LibSshHdr)) {
    Write-Host "=== setup libssh (first run, ~2min) ===" -ForegroundColor Cyan
    & (Join-Path $ScriptsDir 'setup_libssh.ps1')
    if ($LASTEXITCODE -ne 0) {
        Write-Error "setup_libssh.ps1 failed ($LASTEXITCODE)"
        exit $LASTEXITCODE
    }
} else {
    Write-Host "=== libssh already built ===" -ForegroundColor DarkCyan
}

# 1) 确保运行时 DLL 在 exe 同目录（minGW runtime + OpenSSL；缺一个就 0xC0000135 秒退）
$RuntimeDlls = @(
    'libgcc_s_seh-1.dll'
    'libstdc++-6.dll'
    'libwinpthread-1.dll'
    'libssl-1_1-x64.dll'
    'libcrypto-1_1-x64.dll'
)
$DllSources = @{
    'libssl-1_1-x64.dll'    = "$OpenSslRoot\bin"
    'libcrypto-1_1-x64.dll' = "$OpenSslRoot\bin"
}
foreach ($d in $RuntimeDlls) {
    $srcDir = if ($DllSources.ContainsKey($d)) { $DllSources[$d] } else { $MingwBin }
    $src = Join-Path $srcDir $d
    $dst = Join-Path $BuildDir $d
    if (Test-Path $src) {
        if (-not (Test-Path $dst) -or (Get-Item $src).LastWriteTime -gt (Get-Item $dst).LastWriteTime) {
            Copy-Item $src $dst -Force
        }
    } else {
        Write-Warning "DLL not found: $src"
    }
}

# 2) Configure（如果 build dir 不存在或 CMakeLists 改了才需要完整 configure）
# BR_WITH_FREERDP=ON：内嵌 FreeRDP 后端（磁盘重定向/剪贴板依赖通道库，静态链接）
$NeedConfigure = -not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))
if (-not $NeedConfigure) {
    $cached = Select-String -Path (Join-Path $BuildDir 'CMakeCache.txt') `
        -Pattern '^BR_WITH_FREERDP:BOOL=ON$' -Quiet
    if (-not $cached) {
        Write-Host "=== BR_WITH_FREERDP=OFF in cache, re-configure ===" -ForegroundColor Yellow
        $NeedConfigure = $true
    }
}
if ($NeedConfigure) {
    Write-Host "=== cmake configure ===" -ForegroundColor Cyan
    $CmakeArgs = @(
        '-S', $ProjectRoot
        '-B', $BuildDir
        '-G', 'Unix Makefiles'
        '-DCMAKE_BUILD_TYPE=Debug'
        "-DCMAKE_PREFIX_PATH=$QtPrefix"
        "-DCMAKE_C_COMPILER=$Gcc"
        "-DCMAKE_CXX_COMPILER=$Gxx"
        "-DCMAKE_MAKE_PROGRAM=$Make"
        "-DOPENSSL_ROOT_DIR=$OpenSslRoot"
        '-DBR_WITH_FREERDP=ON'
    )
    & $Cmake @CmakeArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error "cmake configure failed ($LASTEXITCODE)"
        exit $LASTEXITCODE
    }
} else {
    Write-Host "=== cmake configure (skip, cached) ===" -ForegroundColor DarkCyan
}

# 3) Build
Write-Host "=== cmake build ===" -ForegroundColor Cyan
& $Cmake --build $BuildDir --config Debug -j
if ($LASTEXITCODE -ne 0) {
    Write-Error "cmake build failed ($LASTEXITCODE)"
    exit $LASTEXITCODE
}

# 4) 找生成的 exe
$exe = Get-ChildItem -Path $BuildDir -Filter 'bambooRat.exe' -Recurse -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $exe) {
    Write-Error "bambooRat.exe not found under $BuildDir"
    exit 2
}
Write-Host "=== built: $exe ===" -ForegroundColor Green

# 5) 启动（不阻塞脚本返回；用户可用 Ctrl+C 终止）
Write-Host "=== launching ===" -ForegroundColor Cyan
Remove-Item "$env:TEMP\bambooRat.out.log","$env:TEMP\bambooRat.err.log" -ErrorAction SilentlyContinue
Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) `
    -RedirectStandardOutput "$env:TEMP\bambooRat.out.log" `
    -RedirectStandardError  "$env:TEMP\bambooRat.err.log"
Write-Host "logs: $env:TEMP\bambooRat.out.log / .err.log"