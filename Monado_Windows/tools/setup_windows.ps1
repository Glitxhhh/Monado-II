# [XCDE] Explicit project-local setup. Never invokes winget/pacman, upgrades the
# host, accepts vendor terms or changes user/system PATH. Source fallback requires
# a caller-selected native CPython 3.14; the portable GUI packages its interpreter.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PythonPath, [switch]$DownloadDependencies)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath $PythonPath -PathType Leaf)) { throw 'PythonPath must name an existing native CPython executable.' }
& $PythonPath -c 'import sys; assert sys.platform == "win32" and sys.version_info[:2] == (3,14), "Native CPython 3.14 is required"'
if ($LASTEXITCODE -ne 0) { throw 'Unsupported Python interpreter.' }
$venv = Join-Path $root '.venv'
if (-not (Test-Path -LiteralPath $venv)) {
    & $PythonPath -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw 'Creating project-local virtual environment failed.' }
}
$projectPython = Join-Path $venv 'Scripts\python.exe'
& $projectPython -c 'import sys; assert sys.version_info[:2] == (3,14), "Existing venv uses another interpreter; preserve it and select a fresh development installation"'
if ($LASTEXITCODE -ne 0) { throw 'Existing virtual environment was preserved.' }
& $projectPython -m pip install --disable-pip-version-check --require-hashes -r (Join-Path $root 'tools\windows-python.lock.txt')
if ($LASTEXITCODE -ne 0) { throw 'Installing pinned project-local Python tools failed.' }
if ($DownloadDependencies) {
    & $projectPython -B (Join-Path $root 'tools\windows_deps.py') install
    if ($LASTEXITCODE -ne 0) { throw 'Project-local native dependency preparation failed.' }
}
Write-Host 'Local tool setup complete. Run the root Build_Monado_II.bat; it never starts the game.'
