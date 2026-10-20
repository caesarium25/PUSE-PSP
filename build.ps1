param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$workspace = Resolve-Path (Join-Path $project '..\..')
$pspdev = Resolve-Path (Join-Path $workspace 'tools\pspdev')
$gitMsys = 'C:\Program Files\Git\usr\bin'
$env:PATH = "$($pspdev.Path)\bin;$gitMsys;$env:PATH"

$build = Join-Path $project 'build'
$dist = Join-Path $project 'dist\PUSE-PSP'
if ($Clean -and (Test-Path $build)) {
    Get-ChildItem -LiteralPath $build -Force | Remove-Item -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $build, $dist | Out-Null

$sdk = Join-Path $pspdev 'psp\sdk'
$includes = @(
    "-I$project\include",
    "-I$($pspdev.Path)\psp\include",
    "-I$sdk\include"
)
$common = @(
    '-O2', '-G0', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
    '-fno-rtti', '-fno-exceptions', '-std=gnu++17', '-D_PSP_FW_VERSION=660'
) + $includes

$sources = @(
    Get-ChildItem (Join-Path $project 'source\core') -Filter '*.cpp'
    Get-ChildItem (Join-Path $project 'source\io') -Filter '*.cpp'
    Get-ChildItem (Join-Path $project 'source\ui') -Filter '*.cpp'
    Get-Item (Join-Path $project 'source\main.cpp')
)

$objects = @()
foreach ($source in $sources) {
    $group = Split-Path $source.DirectoryName -Leaf
    $object = Join-Path $build "$group-$($source.BaseName).o"
    $objects += $object
    Write-Host "CXX $($source.Name)"
    & psp-g++.exe @common -c $source.FullName -o $object
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($source.FullName)" }
}

$elf = Join-Path $build 'puse-psp.elf'
$prx = Join-Path $build 'puse-psp.prx'
$sfo = Join-Path $build 'PARAM.SFO'
$eboot = Join-Path $dist 'EBOOT.PBP'
$icon0 = Join-Path $project 'assets\ICON0.PNG'
$pic1 = Join-Path $project 'assets\PIC1.PNG'
if (!(Test-Path -LiteralPath $icon0) -or !(Test-Path -LiteralPath $pic1)) {
    throw 'Missing PSP artwork under assets (ICON0.PNG and PIC1.PNG are required)'
}

$linkArgs = @(
    "-specs=$sdk\lib\prxspecs",
    $objects,
    (Join-Path $sdk 'lib\prxexports.o'),
    "-L$($pspdev.Path)\psp\lib",
    "-L$sdk\lib",
    "-Wl,-q,-T$sdk\lib\linkfile.prx", '-Wl,--gc-sections', '-Wl,-zmax-page-size=128',
    '-lSDL2_ttf', '-lharfbuzz', '-lfreetype', '-lbz2', '-lpng16', '-lz',
    '-lSDL2main', '-lSDL2', '-lGL', '-lpspvram', '-lpspaudio', '-lpspvfpu',
    '-lpspdisplay', '-lpspgu', '-lpspge', '-lpsphprm', '-lpspctrl', '-lpsppower',
    '-lpspnet', '-lpspnet_apctl', '-lm',
    '-o', $elf
)
Write-Host 'LINK puse-psp.elf'
& psp-g++.exe @linkArgs
if ($LASTEXITCODE -ne 0) { throw 'Link failed' }

& psp-fixup-imports.exe $elf
if ($LASTEXITCODE -ne 0) { throw 'Import fixup failed' }
& psp-prxgen.exe $elf $prx
if ($LASTEXITCODE -ne 0) { throw 'PRX generation failed' }
& mksfoex.exe -d MEMSIZE=1 'PUSE-PSP' $sfo
if ($LASTEXITCODE -ne 0) { throw 'SFO generation failed' }
& pack-pbp.exe $eboot $sfo $icon0 NULL NULL $pic1 NULL $prx NULL
if ($LASTEXITCODE -ne 0) { throw 'EBOOT packaging failed' }

$distData = Join-Path $dist 'data'
if (Test-Path $distData) {
    Get-ChildItem -LiteralPath $distData -Force | Remove-Item -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $distData | Out-Null
Copy-Item (Join-Path $project 'data\*') $distData -Recurse -Force
Copy-Item (Join-Path $project 'LICENSE-PUSE') $dist -Force
Copy-Item (Join-Path $project 'LICENSE') $dist -Force
Copy-Item (Join-Path $project 'README.md') $dist -Force
Copy-Item (Join-Path $project 'UPSTREAM.md') $dist -Force

Write-Host "Built $eboot"
