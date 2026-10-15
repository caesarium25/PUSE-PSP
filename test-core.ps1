param(
    [Parameter(Mandatory = $true)]
    [string]$SavePath
)

$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$build = Join-Path $project 'build\host'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$devShell = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShell
Enter-VsDevShell -VsInstallPath 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools' -Arch amd64 -SkipAutomaticLocation

$sources = @(
    Get-ChildItem (Join-Path $project 'source\core') -Filter '*.cpp' | ForEach-Object FullName
    Get-ChildItem (Join-Path $project 'source\io') -Filter '*.cpp' | ForEach-Object FullName
    Join-Path $project 'tests\core_smoke.cpp'
)
$output = Join-Path $build 'core_smoke.exe'
& cl.exe /nologo /std:c++17 /EHsc /O2 "/I$project\include" @sources "/Fe:$output"
if ($LASTEXITCODE -ne 0) { throw 'Host smoke-test build failed' }

Push-Location $project
try {
    & $output (Resolve-Path $SavePath)
    if ($LASTEXITCODE -ne 0) { throw 'Core smoke test failed' }
} finally {
    Pop-Location
}
