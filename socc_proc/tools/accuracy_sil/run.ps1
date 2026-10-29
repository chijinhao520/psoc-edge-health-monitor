$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$out = Join-Path $root 'cache/accuracy_audit'
New-Item -ItemType Directory -Force $out | Out-Null
$vcvars = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Auxiliary/Build/vcvars64.bat'
$algo = Join-Path $root 'socc_proc/applications/algo'
$m55 = Join-Path $root 'Edgi_Talk_M55_LVGL/applications'
$stub = Join-Path $PSScriptRoot 'stubs'
$src = Join-Path $PSScriptRoot 'accuracy_sil.c'
$exe = Join-Path $out 'accuracy_sil.exe'
$cmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /utf-8 /W3 /I`"$algo`" /I`"$m55`" /I`"$stub`" `"$src`" `"$algo/ppg_process.c`" `"$algo/ppg_sqi.c`" `"$algo/gsr_process.c`" `"$algo/ppg_session.c`" `"$m55/health_metrics.c`" `"$m55/arrhythmia_preproc.c`" /Fo:`"$out/`" /Fe:`"$exe`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$sensor = Join-Path $root 'socc_proc/applications/sensor'
$stub = Join-Path $PSScriptRoot 'stubs'
$driverExe = Join-Path $out 'driver_sil.exe'
$cmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /utf-8 /W3 /I`"$stub`" /I`"$sensor`" `"$PSScriptRoot/driver_sil.c`" `"$sensor/max30102.c`" `"$sensor/sht30.c`" `"$sensor/ads1015.c`" `"$sensor/mlx90614.c`" /Fo:`"$out/`" /Fe:`"$driverExe`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $driverExe
exit $LASTEXITCODE
