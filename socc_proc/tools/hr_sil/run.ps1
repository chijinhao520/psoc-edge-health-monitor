# 心率算法 SIL 回归：用已知真值的合成 PPG 波形验证心率链路。
#
#   .\run.ps1                       # 生产值 340ms 跑遍各采样率（默认，回归用）
#   .\run.ps1 -DistsMs 300,340,400  # 扫峰距取值，找两侧崩溃边界
#   .\run.ps1 -FsList 100           # 当前 MAX30102 连续样本时钟
#
# 默认覆盖历史 51~105Hz 算法输入。当前采集端固定 100SPS 且禁止断样，
# 其他速率用于回归算法的采样率适配，不再从读取耗时推测 FIFO 样本时钟。
#
# 退出码 0 = 生产值在所有采样率下全量程通过；非 0 = 回归失败。

param(
    [int[]]    $DistsMs = @(340),
    [double[]] $FsList  = @(51, 70, 88, 100, 105),
    [int]      $ProdMs  = 340
)

$ErrorActionPreference = 'Stop'
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$algo   = Join-Path $here '..\..\applications\algo'
$out = Join-Path $here '../../../cache/accuracy_audit/hr_sil'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'

if (-not (Test-Path $vcvars)) {
    Write-Error "找不到 MSVC vcvars64.bat：$vcvars`n换编译器时直接用 hr_sil.c 顶部注释里的命令行。"
}

$prodFail = 0

foreach ($ms in $DistsMs) {
    $exe = Join-Path $out "hr_sil_$ms.exe"
    $obj = Join-Path $out "build_$ms"
    New-Item -ItemType Directory -Force -Path $obj | Out-Null

    # 编译固件本体 ppg_process.c + harness，仅用 -D 覆盖峰距。
    # /Fo 目录必须以正斜杠收尾：路径末尾的 `\` 会转义掉它后面的引号，
    # 使 /Fo 与 /Fe 粘成一个参数（cl 报 D8036「不允许多个源文件」）。
    $cmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /utf-8 /W3 /I`"$algo`" -DHR_MIN_PEAK_DIST_MS=$ms " +
           "`"$(Join-Path $here 'hr_sil.c')`" `"$(Join-Path $algo 'ppg_process.c')`" " +
           "/Fo:`"$obj/`" /Fe:`"$exe`""
    $build = cmd /c $cmd 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[编译失败 dist=${ms}ms]" -ForegroundColor Red
        $build | Select-Object -Last 15
        exit 2
    }

    foreach ($fs in $FsList) {
        & $exe $fs
        if (($ms -eq $ProdMs) -and ($LASTEXITCODE -ne 0)) { $prodFail++ }
    }
}

Write-Host "生产值 HR_MIN_PEAK_DIST_MS=${ProdMs}ms 跨 $($FsList.Count) 档采样率：" -NoNewline
if ($prodFail -eq 0) {
    Write-Host " 全部通过" -ForegroundColor Green
} else {
    Write-Host " $prodFail 档存在超差 — 回归失败" -ForegroundColor Red
}
exit $prodFail
