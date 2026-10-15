param(
    [Parameter(Mandatory = $true)]
    [string]$SavePath,
    [string]$PpssppDirectory = ''
)

$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$workspace = Resolve-Path (Join-Path $project '..\..')
if (!$PpssppDirectory) {
    $PpssppDirectory = Join-Path $workspace 'tools\ppsspp'
}
$PpssppDirectory = (Resolve-Path $PpssppDirectory).Path
$exe = Join-Path $PpssppDirectory 'PPSSPPWindows64.exe'
$memstick = Join-Path $PpssppDirectory 'memstick'
$app = Join-Path $memstick 'PSP\GAME\PUSE-PSP'
$tempgba = Join-Path $memstick 'PSP\GAME\TempGBA'
New-Item -ItemType Directory -Force -Path $app, $tempgba | Out-Null
Copy-Item (Join-Path $project 'dist\PUSE-PSP\*') $app -Recurse -Force
Copy-Item (Resolve-Path $SavePath) (Join-Path $tempgba 'Pokemon Unbound.sav') -Force

$log = Join-Path $PpssppDirectory 'puse-psp-smoke.log'
$eboot = Join-Path $app 'EBOOT.PBP'
$argLine = "--windowed --log=`"$log`" `"$eboot`""
if (Test-Path $log) { Remove-Item -LiteralPath $log -Force }
$process = Start-Process -FilePath $exe `
    -ArgumentList $argLine `
    -WorkingDirectory $PpssppDirectory -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 10
$exitedEarly = $process.HasExited
if (!$exitedEarly) { Stop-Process -Id $process.Id -Force }

if (!(Test-Path $log)) { throw 'PPSSPP did not create a log' }
$content = Get-Content $log -Raw
if ($content -notmatch 'Booted .*EBOOT\.PBP') { throw 'PPSSPP did not boot PUSE-PSP' }
if ($content -match 'Boot failed|Unhandled|Invalid memory access') { throw 'PPSSPP reported a boot/runtime error' }
Write-Host 'PASS: PPSSPP booted PUSE-PSP and it remained running for the smoke-test window.'
