# One-time setup of a Windows machine for building Monado (modelled on LASRecomp's tools/setup_windows.ps1).
# Installs MSYS2 (UCRT64: SDL3, FFmpeg, Vulkan loader / headers, shaderc, Vulkan Memory Allocator, Opus) and
# Python 3.12, then creates .venv (the recompiler: capstone, unicorn, lz4; the C compiler: ziglang). Safe to re-run.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path C:\msys64\usr\bin\pacman.exe)) {
    Write-Host 'Installing MSYS2 (winget)...'
    winget install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements --silent
}
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
$bash = 'C:\msys64\usr\bin\bash.exe'
Write-Host 'Updating MSYS2 and installing the libraries (pacman)...'
& $bash -lc 'pacman -Syuu --noconfirm' | Out-Null
& $bash -lc 'pacman -Syuu --noconfirm' | Out-Null  # second pass after a core update
$pkgs = 'mingw-w64-ucrt-x86_64-sdl3', 'mingw-w64-ucrt-x86_64-ffmpeg', 'mingw-w64-ucrt-x86_64-vulkan-devel',
        'mingw-w64-ucrt-x86_64-shaderc', 'mingw-w64-ucrt-x86_64-vulkan-memory-allocator', 'mingw-w64-ucrt-x86_64-opus',
        'mingw-w64-ucrt-x86_64-vulkan-validation-layers'
& $bash -lc "pacman -S --needed --noconfirm $($pkgs -join ' ')"

# Python for the tools: the python.org build (prebuilt unicorn / capstone wheels; MSYS2's Python can't build them)
$py = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
if (-not (Test-Path $py)) {
    Write-Host 'Installing Python 3.12 (winget)...'
    winget install --id Python.Python.3.12 -e --scope user --accept-source-agreements --accept-package-agreements --silent
}
if (-not (Test-Path "$root\.venv\Scripts\python.exe")) { & $py -m venv "$root\.venv" }
Write-Host 'Installing the recompiler packages (pip)...'
& "$root\.venv\Scripts\python.exe" -m pip install -q --disable-pip-version-check lz4 capstone unicorn ziglang

Write-Host 'Done. Next: "Build Monado.bat" (or, in Git Bash: scripts/build_all.sh once targets\ holds your game files).'
