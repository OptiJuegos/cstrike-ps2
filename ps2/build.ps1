param(
    [string]$ToolchainRoot = '',
    [string]$Output = '',
    [switch]$MenuDemo
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot

if (-not $ToolchainRoot) {
    if ($env:PS2DEV) { $ToolchainRoot = $env:PS2DEV }
    else { $ToolchainRoot = Join-Path $project 'psdevwindows' }
}
$ToolchainRoot = [System.IO.Path]::GetFullPath($ToolchainRoot)
$sdk = if ($env:PS2SDK) { $env:PS2SDK } else { Join-Path $ToolchainRoot 'ps2sdk' }
$gskit = if ($env:GSKIT) { $env:GSKIT } else { Join-Path $ToolchainRoot 'gsKit' }
$cc = Join-Path $ToolchainRoot 'ee/bin/mips64r5900el-ps2-elf-gcc.exe'
$linker = Join-Path $sdk 'ee/startup/linkfile'

foreach ($path in @($cc, $linker, (Join-Path $gskit 'lib/libgskit.a'), (Join-Path $gskit 'lib/libdmakit.a'))) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Falta dependencia: $path" }
}

$outDir = Join-Path $project 'ps2/out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$env:PATH = (Join-Path $ToolchainRoot 'ee/bin') + ';' + $env:PATH

if ($MenuDemo) {
    $atlas = Join-Path $outDir 'main_menu.p2sheet'
    $menuHeader = Join-Path $outDir 'menu_layout.h'
    & python (Join-Path $PSScriptRoot 'tools/make_menu_ps2.py') $atlas $menuHeader
    if ($LASTEXITCODE -ne 0) { throw "Conversion de atlas fallida (exit $LASTEXITCODE)" }
    $sourceList = @('ps2/menu_demo.c', 'ps2/src/c2d_ps2.c', 'ps2/src/ctru_ps2_input.c', 'ps2/assets.S')
    $objs = @('menu_demo.o', 'c2d_ps2.o', 'ctru_ps2_input.o', 'assets.o') | ForEach-Object { Join-Path $outDir $_ }
    if (-not $Output) { $Output = Join-Path $outDir 'gdps2-menu.elf' }
} else {
    $sourceList = @('ps2/prototype.c')
    $objs = @(Join-Path $outDir 'prototype.o')
    if (-not $Output) { $Output = Join-Path $outDir 'gdps2-prototype.elf' }
}

for ($i = 0; $i -lt $sourceList.Count; $i++) {
    $compile = @('-D_EE', '-D__PS2__', '-G0', '-O2', '-Wall', '-Wextra',
                 "-I$(Join-Path $sdk 'ee/include')", "-I$(Join-Path $sdk 'common/include')",
                 "-I$(Join-Path $gskit 'include')", "-I$(Join-Path $project 'psp/include')",
                 "-I$(Join-Path $project 'ps2/src')", "-I$outDir", '-c',
                 (Join-Path $project $sourceList[$i]), '-o', $objs[$i])
    & $cc @compile
    if ($LASTEXITCODE -ne 0) { throw "Compilacion EE fallida: $($sourceList[$i]) (exit $LASTEXITCODE)" }
}

$link = @("-T$linker", '-Wl,-zmax-page-size=128',
          "-L$(Join-Path $sdk 'ee/lib')", "-L$(Join-Path $gskit 'lib')",
          '-o', $Output) + $objs + @('-lgskit', '-ldmakit', '-lpad', '-lm', '-lc', '-lkernel')
& $cc @link
if ($LASTEXITCODE -ne 0) { throw "Enlazado EE fallido (exit $LASTEXITCODE)" }
Write-Host "ELF creado: $Output"
