param(
    [string]$UpstreamCommit = "8dde7e7",
    [switch]$ForceReset
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$upstreamDir = Join-Path $root 'third_party\wallpaper-scene-renderer'
$patchFile = Join-Path $root 'patches\0001-windows-headless-vulkan-offscreen.patch'

if (-not (Test-Path $patchFile)) {
    throw "Patch file not found: $patchFile"
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
