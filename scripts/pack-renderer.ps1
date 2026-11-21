param(
    [string]$Version = "0.1.0",
    [string]$BuildDir = ""
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $root 'build'
}

$exe = Join-Path $BuildDir 'wpe-renderer.exe'
if (-not (Test-Path $exe)) {
    throw "Renderer binary not found: $exe. Build with CMake/Ninja first."
}

$stageParent = Join-Path $env:TEMP 'lcdwpepack'
$stage = Join-Path $stageParent "LcdFusion-WpeRenderer-$Version-win-x64"
if (Test-Path $stageParent) { Remove-Item $stageParent -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'renderer') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'patches') | Out-Null

# Keep GPL-2.0 renderer self-contained and separate from the MIT LcdFusion package.
Copy-Item $exe (Join-Path $stage 'renderer\wpe-renderer.exe')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $root 'README.md') (Join-Path $stage 'README.md')
Copy-Item (Join-Path $root 'patches\*.patch') (Join-Path $stage 'patches')

$zip = Join-Path $root "LcdFusion-WpeRenderer-$Version-win-x64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stageParent, $zip)
Write-Output $zip
