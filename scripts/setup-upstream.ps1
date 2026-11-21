param(
    [string]$UpstreamCommit = "8dde7e7",
    [string]$QuickJsCommit = "dce8c0b",
    [switch]$ForceReset
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$upstreamDir = Join-Path $root 'third_party\wallpaper-scene-renderer'
$quickJsDir = Join-Path $root 'third_party\quickjs-ng'
$patchFile = Join-Path $root 'patches\0001-windows-headless-vulkan-offscreen.patch'

if (-not (Test-Path $patchFile)) {
    throw "Patch file not found: $patchFile"
}

if (-not (Test-Path $quickJsDir)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $quickJsDir) | Out-Null
    Write-Output "Cloning quickjs-ng/quickjs (MIT)..."
    git clone https://github.com/quickjs-ng/quickjs.git $quickJsDir
    if ($LASTEXITCODE -ne 0) { throw "quickjs-ng git clone failed ($LASTEXITCODE)" }
    git -C $quickJsDir checkout $QuickJsCommit
    if ($LASTEXITCODE -ne 0) { throw "quickjs-ng git checkout $QuickJsCommit failed ($LASTEXITCODE)" }
}
elseif ($ForceReset) {
    Write-Output "Resetting quickjs-ng checkout to $QuickJsCommit..."
    git -C $quickJsDir reset --hard $QuickJsCommit
    if ($LASTEXITCODE -ne 0) { throw "quickjs-ng git reset failed ($LASTEXITCODE)" }
}

if (-not (Test-Path $upstreamDir)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $upstreamDir) | Out-Null
    Write-Output "Cloning catsout/wallpaper-scene-renderer..."
    git clone https://github.com/catsout/wallpaper-scene-renderer.git $upstreamDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed ($LASTEXITCODE)" }
    git -C $upstreamDir checkout $UpstreamCommit
    if ($LASTEXITCODE -ne 0) { throw "git checkout $UpstreamCommit failed ($LASTEXITCODE)" }
    git -C $upstreamDir submodule update --init --recursive third_party/SPIRV-Reflect
    if ($LASTEXITCODE -ne 0) { throw "git submodule update failed ($LASTEXITCODE)" }
}
elseif ($ForceReset) {
    Write-Output "Resetting upstream checkout to $UpstreamCommit..."
    git -C $upstreamDir reset --hard $UpstreamCommit
    if ($LASTEXITCODE -ne 0) { throw "git reset failed ($LASTEXITCODE)" }
    git -C $upstreamDir clean -fd
}

# Check whether the patch is already applied before applying.
git -C $upstreamDir apply --reverse --check $patchFile 2>$null
if ($LASTEXITCODE -eq 0) {
    Write-Output "Upstream patch is already applied cleanly."
    exit 0
}

git -C $upstreamDir apply --check $patchFile
if ($LASTEXITCODE -ne 0) {
    throw "Patch cannot be applied cleanly to $upstreamDir. Re-run with -ForceReset if you want to reset upstream to $UpstreamCommit."
}

git -C $upstreamDir apply $patchFile
if ($LASTEXITCODE -ne 0) { throw "git apply failed ($LASTEXITCODE)" }
Write-Output "Applied $patchFile successfully."
