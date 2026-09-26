# Release 一键打包：编译 + 部署为免安装绿色目录 + zip
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package_release.ps1
#
# 产物：
#   out\build\x64-Release-manual\   Release 构建中间目录（.o / .exe）
#   dist\<Target>\                  免安装绿色目录（exe + Qt 运行库 + 插件）
#   dist\<Target>-win64-<日期>.zip  可分发压缩包
#
# 为什么不用 CMake 构建：
#   受限沙箱会拦截 CMake AutoMoc 内部经 libuv 派生 moc.exe 的调用
#   （报错 "libuv process spawn failed: operation not permitted"）。
#   实测 moc / rcc / g++ / mingw32-make 单独调用都正常，因此改由
#   scripts/gen_makefile.py 自行驱动代码生成，再交给 make 并行编译。
#   同理，windeployqt 要派生 qtpaths 也跑不起来，依赖收集改由
#   scripts/deploy_release.py 用 objdump 解析 PE 导入表完成。
#
# 注意：本文件保持 ASCII，Windows PowerShell 5.1 按 ANSI 解析 .ps1，
#       含中文的字符串（尤其 here-string）会导致语法解析错乱。

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BuildDir    = Join-Path $ProjectRoot 'out\build\x64-Release-manual'
$DistRoot    = Join-Path $ProjectRoot 'dist'

$MingwBin  = 'C:\Qt\Tools\mingw1310_64\bin'
$QtBin     = 'C:\Qt\6.10.3\mingw_64\bin'
$OpenSslBin = 'C:\Qt\Tools\mingw1310_64\opt\bin'
$Make      = Join-Path $MingwBin 'mingw32-make.exe'
$GenPy     = Join-Path $ScriptDir 'gen_makefile.py'
$DeployPy  = Join-Path $ScriptDir 'deploy_release.py'

# 编译与依赖收集都要能从工具链目录读取
$env:Path = "$MingwBin;$QtBin;$OpenSslBin;$env:Path"

# 受限并行度能避免 mingw32-make 在高负载下被任务管理器/CI 杀死
$Jobs = [Math]::Max(2, [Environment]::ProcessorCount - 2)

function Stop-WithError([string]$msg, [int]$code) {
    Write-Host "[FAIL] $msg" -ForegroundColor Red
    exit $code
}

foreach ($t in @($Make, $GenPy, $DeployPy)) {
    if (-not (Test-Path $t)) { Stop-WithError "required file not found: $t" 3 }
}

# ---- 1/4 Generate Makefile -------------------------------------------------
Write-Host '=== [1/4] generate Makefile ===' -ForegroundColor Cyan
& python $GenPy
if ($LASTEXITCODE -ne 0) { Stop-WithError "gen_makefile.py failed ($LASTEXITCODE)" $LASTEXITCODE }

# ---- 2/4 Build (Release) ---------------------------------------------------
Write-Host "=== [2/4] build (Release, -j $Jobs) ===" -ForegroundColor Cyan
$log = Join-Path $BuildDir 'build.log'
Push-Location $BuildDir
try {
    & $Make -f Makefile -j $Jobs all *>&1 | Tee-Object -FilePath $log
    $buildExit = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($buildExit -ne 0) {
    Write-Host '--- errors ---' -ForegroundColor Yellow
    Get-Content $log | Where-Object { $_ -match 'error:|Error \d|\*\*\*' } | Select-Object -First 30
    Stop-WithError "build failed ($buildExit), full log: $log" $buildExit
}

foreach ($t in @('bambooRat', 'DesktopTool')) {
    $exe = Join-Path $BuildDir "$t.exe"
    if (-not (Test-Path $exe)) { Stop-WithError "$t.exe was not produced" 2 }
    Write-Host ("  built {0}.exe  {1:N2} MB" -f $t, ((Get-Item $exe).Length / 1MB))
}

# ---- 3/4 Deploy runtime deps -----------------------------------------------
Write-Host '=== [3/4] deploy runtime deps ===' -ForegroundColor Cyan
& python $DeployPy
if ($LASTEXITCODE -ne 0) { Stop-WithError "deploy_release.py failed ($LASTEXITCODE)" $LASTEXITCODE }

# ---- 4/4 Summary -----------------------------------------------------------
Write-Host '=== [4/4] summary ===' -ForegroundColor Cyan
if (-not (Test-Path $DistRoot)) { Stop-WithError 'dist directory was not created' 4 }
Get-ChildItem $DistRoot -File -Filter '*.zip' | Sort-Object Name | ForEach-Object {
    Write-Host ("  {0}  {1:N2} MB" -f $_.Name, ($_.Length / 1MB))
}
Get-ChildItem $DistRoot -Directory | Sort-Object Name | ForEach-Object {
    $n = (Get-ChildItem $_.FullName -Recurse -File).Count
    $sz = (Get-ChildItem $_.FullName -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
    Write-Host ("  {0}\  {1} files, {2:N2} MB" -f $_.Name, $n, $sz)
}

Write-Host ''
Write-Host '=== DONE ===' -ForegroundColor Green
Write-Host "  dist: $DistRoot"
